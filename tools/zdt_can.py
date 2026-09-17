#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ZDT X42S (Emm 固件, CAN) 直连命令行工具
========================================
绕过官方 GUI，直接用 candleLight(gs_usb) 适配器收发 CAN 帧来控制电机。

帧格式（实测确认 2026-09-16，并逐字节核对过官方 Emm_V5.c 的 can_SendCmd）:
    扩展帧 ID = (地址 << 8) | 包号    包号从 0 开始
    数据     = 功能码 + 参数 + 校验(默认 0x6B)
    * 地址只在 ID 里，不要放进数据里（放进去电机会回 EE 命令格式错误）

长命令（>8 字节）按包拆分，每包开头都重复功能码，7 字节一包：
    完整 = FD dir spdH spdL acc p3 p2 p1 p0 raF sync 6B      (12 字节)
    第0包 = [FD] + 前 7 字节
    第1包 = [FD] + 剩余字节

依赖: python-can, pyusb, libusb-package, gs_usb
    pip install -i https://pypi.tuna.tsinghua.edu.cn/simple python-can pyusb libusb-package gs_usb

用法: python zdt_can.py [--addr 1,2] <子命令> [参数]

    python zdt_can.py --addr 1 status           # 读系统状态
    python zdt_can.py --addr 1,2 scan           # 扫 1..8 号谁在线
    python zdt_can.py --addr 1,2 sample --secs 10 --log s.csv   # 采样写 CSV
    python zdt_can.py --addr 1 latency --n 100              # 量往返延迟分布
    python zdt_can.py --addr 1 latency --op all --n 50      # 横向对比 6 条读命令
"""

from __future__ import annotations

import argparse
import csv
import sys
import threading
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# --- libusb-package 自带 DLL, 让 pyusb 用上它 (否则 python-can 找不到 libusb) ---
import libusb_package
import usb.backend.libusb1 as _libusb1

if not getattr(_libusb1, "_zdt_patched", False):
    _backend = libusb_package.get_libusb1_backend()
    _libusb1.get_backend = lambda *a, **k: _backend
    _libusb1._zdt_patched = True

import can  # noqa: E402

CHECKSUM = 0x6B
BITRATE = 500000
CHANNEL = 0

# 功能码（来源：官方 Emm_V5.c + 用户手册 V1.0.4，均核对过）
OP_ENABLE = 0xF3        # F3 AB <state> <snF>
OP_VELOCITY = 0xF6      # F6 dir velH velL acc snF
OP_POSITION = 0xFD      # FD dir velH velL acc p3 p2 p1 p0 raF snF
OP_STOP = 0xFE          # FE 98 <snF>
OP_SYNC = 0xFF          # FF 66
OP_CLEAR = 0x0A         # 0A 6D   当前位置清零
OP_READ_STATUS = 0x43   # 43 7A   读系统状态参数
OP_READ_DRIVER = 0x42   # 42 6C   读驱动参数
OP_RELEASE_STALL = 0x0E  # 0E 52  解除堵转保护
OP_SET_ID = 0xAE        # AE 4B <svF> <id>   修改电机地址

# --- 原点回零（手册 5.4 / Emm_V5_Origin_*）---
OP_HOMING = 0x9A        # 9A <mode> <snF>     触发回零
OP_EXIT_HOMING = 0x9C   # 9C 48              强制中断并退出回零
OP_HOME_FLAG = 0x3B     # 3B                 读回零状态标志
OP_HOME_STATUS = 0x3C   # 3C                 读回零状态标志 + 电机状态标志
OP_READ_HOMING = 0x22   # 22                 读回零参数（注意：不是 AE 4B！）
OP_SET_ZERO = 0x93      # 93 88 <svF>        设置单圈回零零点
OP_AUTO_RETURN = 0x11   # 11 18 <func> <msH> <msL>  定时返回信息命令

HOMING_MODE_TEXT = {
    0: "单圈就近回零",
    1: "单圈方向回零",
    2: "无限位碰撞回零",
    3: "限位回零",
    4: "回绝对位置零点",
    5: "回上次掉电位置",
}

STATUS_TEXT = {
    0x02: "执行正确",
    0x12: "已在零点/限位，电机不动",
    0x9F: "动作完成/条件不符",
    0xE2: "参数错误",
    0xEE: "命令格式错误",
}

STATUS_FLAG_BITS = ["使能Ens", "到位Prf", "堵转Cgi", "堵转保护Cgp",
                    "左限位EsiL", "右限位EsiR", "保留", "掉电标志Oac"]
HOMING_FLAG_BITS = ["编码器就绪Enc", "校准就绪Cal", "正在回零OrgS",
                    "回零失败OrgC", "过热Otp", "过流Ocp"]

# 采样 CSV 的列（PC 与 MCU 共用同一套列名/单位，便于把两边曲线叠在一张图上）
CSV_COLUMNS = ["t_s", "addr", "bus_mv", "phase_ma", "encoder",
               "target_deg", "speed_rpm", "pos_deg", "err_deg",
               "homing_flags", "motor_status"]


def build_frames(addr: int, payload: bytes, checksum: int = CHECKSUM):
    """把逻辑命令拆成 (扩展帧ID, 数据) 列表。"""
    data = payload + bytes([checksum])
    if len(data) <= 8:
        return [((addr << 8) | 0, data)]
    func = data[0]
    out = [((addr << 8) | 0, data[0:8])]
    rest, pkt = data[8:], 1
    for i in range(0, len(rest), 7):
        out.append(((addr << 8) | pkt, bytes([func]) + rest[i:i + 7]))
        pkt += 1
    return out


def reassemble(chunks: dict):
    """把按包号收到的多包应答拼回完整数据（去重每包开头的功能码）。"""
    if 0 not in chunks:
        return None
    out = bytearray(chunks[0])
    pkt = 1
    while pkt in chunks:
        out += chunks[pkt][1:]      # 丢掉后续包重复的功能码
        pkt += 1
    return bytes(out)


class Motor:
    def __init__(self, channel: int = CHANNEL, bitrate: int = BITRATE):
        self.bus = can.Bus(interface="gs_usb", channel=channel, bitrate=bitrate)
        self._sent = []             # [(id, data, t)] 用于过滤适配器回显
        self.lock = threading.RLock()   # 读操作是「发命令+收应答」的原子事务
        self._dirty = True          # 缓冲可能脏（上次没收全 / 别的程序留的）

    def close(self):
        self.bus.shutdown()

    def send_raw(self, can_id: int, data: bytes, settle: float = 0.0):
        with self.lock:
            msg = can.Message(arbitration_id=can_id, is_extended_id=True, data=data)
            self.bus.send(msg)
            self._sent.append((can_id, bytes(data), time.time()))
        if settle:
            time.sleep(settle)

    def send(self, addr: int, payload: bytes, gap: float = 0.003):
        with self.lock:
            for i, (cid, data) in enumerate(build_frames(addr, payload)):
                self.send_raw(cid, data)
                if i == 0 and len(payload) + 1 > 8:
                    time.sleep(gap)     # 分包之间留点间隔，防粘包

    def _is_echo(self, can_id: int, data: bytes):
        now = time.time()
        self._sent = [s for s in self._sent if now - s[2] < 1.0]
        return any(s[0] == can_id and s[1] == data for s in self._sent)

    def drain(self):
        """丢掉接收缓冲里的残留帧，避免把上一次的漏网应答算到这次头上。

        代价很高：Windows 上 libusb 的短超时会凑到系统定时器节拍，
        **空缓冲一次 recv 就要 ~15ms**（实测 min 13.2 / p50 15.1）。
        所以只在缓冲可能脏时才调（见 request）。
        """
        with self.lock:
            while self.bus.recv(timeout=0) is not None:
                pass

    def collect(self, addr: int, wait: float = 0.4,
                early: bool = False, func: int = None):
        """收 addr 的应答，按包号重组。

        返回 (完整数据, 所有原始帧, 该地址的包号->数据)。
        early=True 时，一旦重组结果以校验码收尾就立刻返回（采样时用）。
        """
        with self.lock:
            chunks, raw = {}, []
            end = time.time() + wait
            while time.time() < end:
                m = self.bus.recv(timeout=0.05)
                if m is None:
                    continue
                if self._is_echo(m.arbitration_id, bytes(m.data)):
                    continue
                raw.append((m.arbitration_id, bytes(m.data)))
                if (m.arbitration_id >> 8) == addr:
                    chunks[m.arbitration_id & 0xFF] = bytes(m.data)
                    if early:
                        full = reassemble(chunks)
                        if (full and full[-1] == CHECKSUM
                                and (func is None or full[0] == func)):
                            return full, raw, chunks
            return reassemble(chunks), raw, chunks

    def request(self, addr: int, payload: bytes, wait: float = 0.4,
                early: bool = False, func: int = None):
        """原子地「发一条命令 + 收它的应答」。

        多线程下必须整段占住总线，否则两个线程各自的 send 和 collect 会交错，
        互相把对方的应答收走。返回 (完整数据, 原始帧, 包号->数据)。

        **成功路径不调 drain()**：collect(early=True) 恰好收全就返回、不留残帧，
        而 drain 一次要 ~15ms，放在每次读前面等于把读取速度砍到 1/16。
        只有上一次没收全（可能留了半截或迟到的帧）时才在下次开头清一次。
        """
        with self.lock:
            if self._dirty:
                self.drain()
                self._dirty = False
            self.send(addr, payload)
            full, raw, chunks = self.collect(addr, wait, early=early, func=func)
            if full is None:
                self._dirty = True
            return full, raw, chunks

    def listen(self, secs: float = 3.0):
        with self.lock:
            raw, end = [], time.time() + secs
            while time.time() < end:
                m = self.bus.recv(timeout=0.2)
                if m is None:
                    continue
                if self._is_echo(m.arbitration_id, bytes(m.data)):
                    continue
                raw.append((m.arbitration_id, bytes(m.data)))
            return raw


# --------------------------- 应答与失败诊断 ---------------------------

class Reply:
    """一次命令的完整结果：数据 + 失败原因（None 表示成功）。"""

    __slots__ = ("full", "raw", "chunks", "attempts", "reason")

    def __init__(self, full, raw, chunks, attempts, reason):
        self.full = full
        self.raw = raw
        self.chunks = chunks
        self.attempts = attempts
        self.reason = reason


def diagnose(addr, func, full, chunks, raw):
    """把「没收到 / 收不全 / 收到别人的」区分开，返回人类可读原因或 None。"""
    mine = [(cid, d) for cid, d in raw if (cid >> 8) == addr]
    if full is None:
        if not mine:
            others = sorted({cid >> 8 for cid, _ in raw})
            if others:
                return ("无应答（但总线上有地址 %s 的帧：地址不匹配，"
                        "或该电机开了定时返回）" % others)
            return "无应答（电机掉线 / 地址不对 / 24V 未上电 / 总线未接）"
        return ("无应答（收到地址 %d 的帧，但包号里没有 0：%s）"
                % (addr, sorted(chunks)))
    if full[0] != func:
        return ("应答功能码 %02X 与请求 %02X 不符（应答错乱 / 总线干扰）"
                % (full[0], func))
    if full[-1] != CHECKSUM:
        return ("应答不完整：只收到包号 %s，末字节 %02X != 校验 %02X"
                "（丢包 / 总线拥塞）" % (sorted(chunks), full[-1], CHECKSUM))
    return None


def cmd(addr, payload, wait=0.4, m: Motor = None, retries=2, verbose=False):
    """发命令 + 收应答，失败自动重试。返回 Reply。"""
    last = None
    for attempt in range(retries + 1):
        full, raw, chunks = m.request(addr, payload, wait)
        last = Reply(full, raw, chunks, attempt + 1,
                     diagnose(addr, payload[0], full, chunks, raw))
        if last.reason is None:
            return last
        if verbose and attempt < retries:
            print("    重试 %d/%d：%s" % (attempt + 1, retries, last.reason))
        if attempt < retries:
            time.sleep(0.05)
    return last


# --------------------------- 显示与解析 ---------------------------

def format_reply(r: Reply, addr: int):
    lines = []
    for cid, data in r.raw:
        lines.append("   RX  id=%#06x  %s" % (cid, data.hex(" ").upper()))
    if r.reason:
        head = "  -> %s" % r.reason
        if r.attempts > 1:
            head += "（已重试 %d 次仍失败）" % (r.attempts - 1)
        return "\n".join(lines + [head])
    full = r.full
    body = full[:-1] if len(full) >= 2 else b""
    res = full[1] if len(full) >= 2 else None
    tail = ""
    if res in STATUS_TEXT:
        tail = "   结果码 %02X: %s" % (res, STATUS_TEXT[res])
    out = "\n".join(lines) + "\n  <- 完整 %d 字节: %s%s" % (
        len(full), full.hex(" ").upper(), tail)
    if r.attempts > 1:
        out += "\n  （重试 %d 次后成功）" % (r.attempts - 1)
    return out


def _bits(v, names):
    return " ".join(n for i, n in enumerate(names) if v & (1 << i))


def parse_status(full: bytes):
    """43 7A 应答 -> 数值字典（手册 5.8.2）。供显示 / CSV / 画图共用。

    CAN 数据 = 43 1F 09 <26字节> 6B。角度单位: 数值*360/65536，符号字节 00=正 01=负。
    """
    if not full or full[0] != OP_READ_STATUS or len(full) < 29:
        return None
    b = full[3:-1]
    if len(b) < 26:
        return None

    def u16(i): return int.from_bytes(b[i:i + 2], "big")
    def u32(i): return int.from_bytes(b[i:i + 4], "big")
    def deg(sign, val): return (val * 360.0 / 65536.0) * (-1.0 if sign else 1.0)

    return {
        "bus_mv": u16(0),
        "phase_ma": u16(2),
        "encoder": u16(4),
        "target_deg": deg(b[6], u32(7)),
        "speed_rpm": u16(12) * (-1 if b[11] else 1),
        "pos_deg": deg(b[14], u32(15)),
        "err_deg": deg(b[19], u32(20)),
        "homing_flags": b[24],
        "motor_status": b[25],
    }


def decode_status(full: bytes):
    """把 parse_status 的结果格式化成一行行可读文本。"""
    d = parse_status(full)
    if d is None:
        return None
    return {
        "总线电压": "%.2f V (%d mV)" % (d["bus_mv"] / 1000.0, d["bus_mv"]),
        "相电流": "%d mA" % d["phase_ma"],
        "线性化编码器": d["encoder"],
        "目标位置": "%.2f°" % d["target_deg"],
        "实时转速": "%d RPM" % d["speed_rpm"],
        "实时位置": "%.2f°" % d["pos_deg"],
        "位置误差": "%.4f°" % d["err_deg"],
        "回零状态": "0x%02X [%s]" % (d["homing_flags"],
                                     _bits(d["homing_flags"], HOMING_FLAG_BITS)),
        "电机状态": "0x%02X [%s]" % (d["motor_status"],
                                     _bits(d["motor_status"], STATUS_FLAG_BITS)),
    }


def decode_homing_params(full: bytes):
    """22 应答 -> 可读字段（手册 5.4.5）。

    CAN 数据 = 22 <15字节> 6B，共 17 字节。
    """
    if not full or full[0] != OP_READ_HOMING or len(full) < 17:
        return None
    b = full[1:-1]
    if len(b) < 15:
        return None

    def u16(i): return int.from_bytes(b[i:i + 2], "big")
    def u32(i): return int.from_bytes(b[i:i + 4], "big")

    mode = b[0]
    return {
        "回零模式": "%d (%s)" % (mode, HOMING_MODE_TEXT.get(mode, "未知")),
        "回零方向": "CW" if b[1] == 0 else "CCW",
        "回零速度": "%d RPM" % u16(2),
        "回零超时": "%d ms" % u32(4),
        "碰撞检测转速": "%d RPM" % u16(8),
        "碰撞检测电流": "%d mA" % u16(10),
        "碰撞检测时间": "%d ms" % u16(12),
        "上电自动回零": "使能" if b[14] else "不使能",
    }


def decode_home_flags(full: bytes):
    """3B 应答 -> 回零状态标志文本。"""
    if not full or full[0] != OP_HOME_FLAG or len(full) < 3:
        return None
    f = full[1]
    state = f & 0x0C
    return {
        "回零状态标志": "0x%02X [%s]" % (f, _bits(f, HOMING_FLAG_BITS)),
        "回零状态": {0x04: "正在回零", 0x08: "回零失败", 0x00: "回零成功/未开始"}
                    .get(state, "?")
        + ("（过流保护已触发）" if f & 0x20 else "")
        + ("（过热保护已触发）" if f & 0x10 else ""),
    }


# --------------------------- 命令构造 ---------------------------

def payload_for(args):
    """按子命令构造逻辑命令负载（不含校验字节）。"""
    if args.cmd == "status":
        return bytes([OP_READ_STATUS, 0x7A])
    if args.cmd == "read-driver":
        return bytes([OP_READ_DRIVER, 0x6C])
    if args.cmd == "read-homing":
        return bytes([OP_READ_HOMING])                    # 22
    if args.cmd == "home":
        return bytes([OP_HOMING, args.mode & 0xFF,
                      0x01 if args.sync else 0x00])       # 9A <mode> <snF>
    if args.cmd == "home-exit":
        return bytes([OP_EXIT_HOMING, 0x48])              # 9C 48
    if args.cmd == "home-status":
        return bytes([OP_HOME_FLAG])                      # 3B
    if args.cmd == "enable":
        return bytes([OP_ENABLE, 0xAB, 0x00 if args.off else 0x01, 0x00])
    if args.cmd == "move":
        return (bytes([OP_POSITION, 0x01 if args.ccw else 0x00])
                + args.rpm.to_bytes(2, "big") + bytes([args.acc & 0xFF])
                + args.pulses.to_bytes(4, "big")
                + bytes([0x01 if args.abs else 0x00, 0x01 if args.sync else 0x00]))
    if args.cmd == "speed":
        return (bytes([OP_VELOCITY, 0x01 if args.ccw else 0x00])
                + args.rpm.to_bytes(2, "big")
                + bytes([args.acc & 0xFF, 0x01 if args.sync else 0x00]))
    if args.cmd == "stop":
        return bytes([OP_STOP, 0x98, 0x00])
    if args.cmd == "clear":
        return bytes([OP_CLEAR, 0x6D])
    if args.cmd == "set-zero":
        return bytes([OP_SET_ZERO, 0x88, 0x00 if args.no_save else 0x01])
    if args.cmd == "release-stall":
        return bytes([OP_RELEASE_STALL, 0x52])
    if args.cmd == "set-id":
        return bytes([OP_SET_ID, 0x4B, 0x01, args.newaddr])
    raise SystemExit("未知子命令: %s" % args.cmd)


# --------------------------- CSV 采样 ---------------------------

def do_sample(m: Motor, addrs, secs, hz, log, wait, retries):
    """定时轮询 43 7A，边跑边写 CSV，Ctrl+C 也能留下已采到的数据。"""
    period = 1.0 / hz if hz > 0 else 0.0
    payload = bytes([OP_READ_STATUS, 0x7A])
    t0 = time.time()
    n = {a: 0 for a in addrs}
    miss = {a: 0 for a in addrs}

    f = open(log, "w", newline="", encoding="utf-8")
    f.write("# ZDT 采样  %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    f.write("# 电机=%s  目标=%gHz  轮询等待=%gs\n" % (addrs, hz, wait))
    f.write("# 角度 度 / 电压 mV / 电流 mA / speed_rpm 带符号 / *_flags 为位域原件\n")
    writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
    writer.writeheader()

    try:
        while True:
            tick = time.time()
            if tick - t0 >= secs:
                break
            for a in addrs:
                full, raw, chunks = m.request(a, payload, wait, early=True,
                                              func=OP_READ_STATUS)
                d = parse_status(full)
                if d is None:
                    miss[a] += 1
                    # 丢包也重试一次，采样对连续性要求高
                    if retries:
                        full, raw, chunks = m.request(a, payload, wait,
                                                      early=True,
                                                      func=OP_READ_STATUS)
                        d = parse_status(full)
                    if d is None:
                        continue
                row = {"t_s": round(time.time() - t0, 6), "addr": a}
                row.update(d)
                writer.writerow(row)
                n[a] += 1
            f.flush()
            if period:
                dt = time.time() - tick
                if dt < period:
                    time.sleep(period - dt)
    except KeyboardInterrupt:
        print("\n收到 Ctrl+C，停止采样（数据已落盘）。")
    finally:
        f.close()

    el = time.time() - t0
    print("已写入 %s" % log)
    print("  用时 %.1fs" % el)
    for a in addrs:
        print("  地址 %d：%d 点（%.1f Hz），丢/超时 %d 次"
              % (a, n[a], n[a] / el if el else 0, miss[a]))
    return 0


# --------------------------- 往返延迟测量 ---------------------------

# 对比「胖命令」和「5.5 单量命令」的往返耗时，用来判断提速该走哪条路。
# 注意：只有 5.5 的功能码能用于 11 18 定时推送；43 7A 在 5.8，推不了。
LATENCY_OPS = {
    "status":  (bytes([OP_READ_STATUS, 0x7A]),
                "43 7A  读系统状态 (5.8.2，31字节/5帧；不在 5.5，不能推送)"),
    "pos":     (bytes([0x36]), "36     读实时位置 (5.5.13) <- 可 11 18 推送"),
    "speed":   (bytes([0x35]), "35     读实时转速 (5.5.11) <- 可 11 18 推送"),
    "phase":   (bytes([0x27]), "27     读相电流   (5.5.6)  <- 可 11 18 推送"),
    "err":     (bytes([0x37]), "37     读位置误差 (5.5.14) <- 可 11 18 推送"),
    "mstatus": (bytes([0x3A]), "3A     读状态标志 (5.5.15) <- 可 11 18 推送"),
}


def _pct(sorted_vals, q):
    if not sorted_vals:
        return float("nan")
    i = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[i]


def do_latency(m: Motor, addrs, ops, n, wait):
    """量「发一条命令 -> 收全应答」的往返耗时分布。

    这是判断提速手段的唯一依据：
      min 就接近平均值 -> 每次都要花这么久，是电机的硬成本，只能换协议
                          （改用 11 18 推 5.5 单量命令）才有意义；
      min 远小于中位数  -> 慢的那几次是排队/竞争，调参数可能就能改善，
                          不必动协议。
    """
    for a in addrs:
        for op in ops:
            payload, desc = LATENCY_OPS[op]
            print("\n地址 %d | %s" % (a, desc))
            print("  跑 %d 次 ..." % n)
            lat, frames, bad = [], [], 0
            t_start = time.perf_counter()
            for i in range(n):
                t0 = time.perf_counter()
                full, raw, chunks = m.request(a, payload, wait, early=True,
                                              func=payload[0])
                dt = time.perf_counter() - t0
                if full is None:
                    bad += 1
                    continue
                lat.append(dt)
                frames.append(len(chunks))
                if (i + 1) % 25 == 0:
                    el = time.perf_counter() - t_start
                    print("    ... %d/%d  已用 %.1fs  预计共 %.1fs"
                          % (i + 1, n, el, el * n / (i + 1)))
            if not lat:
                print("  全部无应答 —— 查供电/地址/总线，测量作废。")
                continue
            s = sorted(lat)
            mean = sum(s) / len(s)
            print("  成功 %d/%d   最坏收到 %d 帧/次"
                  % (len(s), n, max(frames) if frames else 0))
            print("  min  %6.2f ms" % (s[0] * 1e3))
            print("  p50  %6.2f ms" % (_pct(s, 0.50) * 1e3))
            print("  p90  %6.2f ms" % (_pct(s, 0.90) * 1e3))
            print("  max  %6.2f ms" % (s[-1] * 1e3))
            print("  mean %6.2f ms  -> 单台上限 %.0f Hz（两台轮流约 %.0f Hz/台）"
                  % (mean * 1e3, 1.0 / mean, 0.5 / mean))
            ratio = s[0] / mean if mean else 0
            print("  min/mean = %.2f（越接近 1 越稳）" % ratio)
            if mean * 1e3 > 5.0:
                print("  单条超过 5ms，偏慢：查供电/总线干扰，或看是不是多了额外往返"
                      "（曾经 drain() 就白吃 15ms）。")
            if ratio < 0.5:
                print("  min 远小于 mean：有等待/阻塞成分，值得查（不是电机算得慢）。")
    return 0


# --------------------------- 主流程 ---------------------------

def main():
    ap = argparse.ArgumentParser(description="ZDT X42S CAN 直连工具")
    ap.add_argument("--addr", default="1", help="电机地址，逗号分隔可指定多个 (如 1,2)")
    ap.add_argument("--wait", type=float, default=0.4, help="等待应答秒数")
    ap.add_argument("--retries", type=int, default=2,
                    help="应答失败后的重试次数 (默认 2；0=不重试)")
    ap.add_argument("--verbose", action="store_true", help="打印重试过程")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="读取系统状态")
    sub.add_parser("read-driver", help="读取驱动参数")
    sub.add_parser("read-homing", help="读取回零参数 (功能码 22)")

    p = sub.add_parser("home", help="触发回零 (9A 模式 同步标志)")
    p.add_argument("mode", type=int, nargs="?", default=0,
                   help="回零模式: 0单圈就近 1单圈方向 2无限位碰撞 "
                        "3限位 4回绝对零点 5回上次掉电位置 (默认 0)")
    p.add_argument("--sync", action="store_true", help="先缓存，等广播触发")

    sub.add_parser("home-exit", help="强制中断并退出回零 (9C 48)")
    sub.add_parser("home-status", help="读取回零状态标志 (3B)")

    p = sub.add_parser("enable", help="使能驱动板")
    p.add_argument("--off", action="store_true", help="改为失能（松手，可手动摆位）")

    p = sub.add_parser("move", help="位置模式")
    p.add_argument("pulses", type=int, help="脉冲数")
    p.add_argument("--rpm", type=int, default=100)
    p.add_argument("--acc", type=int, default=0)
    p.add_argument("--ccw", action="store_true")
    p.add_argument("--abs", action="store_true", help="绝对定位")
    p.add_argument("--sync", action="store_true", help="多机同步标志=1 (只缓存)")

    p = sub.add_parser("speed", help="速度模式")
    p.add_argument("rpm", type=int)
    p.add_argument("--acc", type=int, default=0)
    p.add_argument("--ccw", action="store_true")
    p.add_argument("--sync", action="store_true")

    sub.add_parser("stop", help="立即停止")
    sub.add_parser("sync-all", help="广播触发多机同步 (ID=0x0, FF 66 6B)")
    sub.add_parser("clear", help="当前位置清零")

    p = sub.add_parser("set-zero", help="设置单圈零点")
    p.add_argument("--no-save", action="store_true", help="不存储到电机（掉电丢）")

    sub.add_parser("release-stall", help="解除堵转保护")

    p = sub.add_parser("scan", help="扫描 1..8 号地址谁在线")
    p.add_argument("--max", type=int, default=8)

    p = sub.add_parser("listen", help="被动监听总线")
    p.add_argument("--secs", type=float, default=3.0)

    p = sub.add_parser("raw", help="发送原始帧")
    p.add_argument("--id", type=lambda x: int(x, 0), required=True, help="扩展帧 ID, 如 0x0100")
    p.add_argument("--data", required=True, help="十六进制字节, 如 '43 7A 6B'")

    p = sub.add_parser("set-id", help="修改电机地址并保存")
    p.add_argument("newaddr", type=int)

    p = sub.add_parser("sample", help="定时轮询状态并写 CSV (画波形用)")
    p.add_argument("--secs", type=float, default=10.0, help="采样时长秒 (默认 10)")
    p.add_argument("--hz", type=float, default=20.0,
                   help="轮询频率 Hz (默认 20；0=尽可能快)")
    p.add_argument("--log", default="sample.csv", help="输出 CSV 路径")
    p.add_argument("--swait", type=float, default=0.08,
                   help="每点等待应答秒数 (默认 0.08)")

    p = sub.add_parser("latency", help="量各读命令的往返延迟分布 (判断要不要上 11 18)")
    p.add_argument("--n", type=int, default=100, help="每条测多少次 (默认 100)")
    p.add_argument("--op", default="status",
                   help="测哪条: %s，或 all (默认 status)" % " / ".join(LATENCY_OPS))
    p.add_argument("--lwait", type=float, default=0.2,
                   help="单次等待应答秒数 (默认 0.2；测的是快慢，短一点好)")

    args = ap.parse_args()
    try:
        addrs = [int(x) for x in args.addr.replace(",", " ").split()] or [1]
    except ValueError:
        ap.error("--addr 必须是数字，可用逗号分隔多个，如 --addr 1,2")

    lat_ops = None
    if args.cmd == "latency":
        lat_ops = (list(LATENCY_OPS) if args.op.strip().lower() == "all"
                   else [x.strip() for x in args.op.split(",") if x.strip()])
        bad = [o for o in lat_ops if o not in LATENCY_OPS]
        if bad:
            ap.error("--op 不认识: %s（可选 %s，或 all）"
                     % (",".join(bad), " / ".join(LATENCY_OPS)))

    m = Motor()
    code = 0
    try:
        if args.cmd == "sync-all":
            m.send_raw(0x0000, bytes([OP_SYNC, 0x66, CHECKSUM]))
            time.sleep(0.2)
            raw = m.listen(0.3)
            print("   广播 00 FF 66 6B 已发送")
            for cid, data in raw:
                print("   RX  id=%#06x  %s" % (cid, data.hex(" ").upper()))
        elif args.cmd == "scan":
            for ad in range(1, args.max + 1):
                r = cmd(ad, bytes([OP_READ_STATUS, 0x7A]), 0.25, m, retries=0)
                print("  地址 %d : %s" % (ad, "在线  <- " + r.full.hex(" ").upper()
                                          if not r.reason else r.reason))
        elif args.cmd == "listen":
            print("  监听 %.1fs ..." % args.secs)
            for cid, data in m.listen(args.secs):
                print("   RX  id=%#06x  %s" % (cid, data.hex(" ").upper()))
        elif args.cmd == "raw":
            data = bytes.fromhex(args.data.replace(" ", "").replace(",", ""))
            m.send_raw(args.id, data)
            time.sleep(0.05)
            for cid, d in m.listen(args.wait):
                print("   RX  id=%#06x  %s" % (cid, d.hex(" ").upper()))
        elif args.cmd == "sample":
            code = do_sample(m, addrs, args.secs, args.hz, args.log,
                             args.swait, args.retries)
        elif args.cmd == "latency":
            code = do_latency(m, addrs, lat_ops, args.n, args.lwait)
        else:
            payload = payload_for(args)
            for a in addrs:
                if len(addrs) > 1:
                    print("-- 地址 %d --" % a)
                r = cmd(a, payload, args.wait, m,
                        retries=args.retries, verbose=args.verbose)
                print(format_reply(r, a))
                if r.reason:
                    code = 1
                    continue
                if args.cmd == "status":
                    d = decode_status(r.full)
                elif args.cmd == "read-homing":
                    d = decode_homing_params(r.full)
                elif args.cmd == "home-status":
                    d = decode_home_flags(r.full)
                else:
                    d = None
                if d:
                    for k, v in d.items():
                        print("   %-12s %s" % (k, v))
    finally:
        m.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
