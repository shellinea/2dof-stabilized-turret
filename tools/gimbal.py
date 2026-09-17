#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
二轴差速齿轮云台 —— 同步动作序列
=================================
托盘姿态分解（差速）:
    俯仰/仰视俯视 = (θ1 + θ2)/2   ->  两电机世界同向同角度
    自转/spin     = (θ1 - θ2)/2   ->  两电机世界反向同角度

用多机同步机制：先分别把两条位置指令以 sync=1 缓存，再广播 00 FF 66 6B 触发同时运动。

标定值（mirror / 符号 / 零点 / 限位 / 运动默认值）读 tools/gimbal_config.json，
CLI 参数可临时覆盖。限位在发命令前检查，越界直接拒绝、不发命令。

用法:
    python tools/gimbal.py                       # 按配置里的默认值跑一轮
    python tools/gimbal.py --show                # 只读当前位置与 pan/tilt
    python tools/gimbal.py --zero                # 把当前姿态标为零点并存盘
    python tools/gimbal.py --tilt 30 --cycles 3 --spin 170 --rpm 60
    python tools/gimbal.py --spin 0              # 只做俯仰
    python tools/gimbal.py --once                # 单程 +tilt 停住，用于确认方向

自转是**按圈换向**的：第 1 圈 +spin，第 2 圈 -spin，第 3 圈 +spin ……
因为 pan 是累加坐标，同向连转会一路累加出去、被软限位永久卡死。

退出码:
    0 = 正常
    1 = 真故障（掉线 / 未到位超时）
    2 = 软限位拦截（保护生效，**不是故障**；VS Code 会把它画成红色，忽略即可）
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from zdt_can import (Motor, OP_POSITION, OP_SYNC, OP_READ_STATUS, OP_STOP,
                     OP_HOMING, OP_HOME_FLAG, CHECKSUM, parse_status)
import config as cfgmod

PULSES_PER_DEG = 3200.0 / 360.0      # 1.8° 步进 / 16 细分 -> 3200 脉冲/圈


def pulses_of(deg: float) -> int:
    return int(round(deg * PULSES_PER_DEG))


def pos_payload(deg: float, rpm: int, acc: int) -> bytes:
    """相对位置指令，sync=1（只缓存，等广播触发）。"""
    direction = 0x01 if deg < 0 else 0x00
    mag = pulses_of(abs(deg))
    return (bytes([OP_POSITION, direction])
            + rpm.to_bytes(2, "big") + bytes([acc & 0xFF])
            + mag.to_bytes(4, "big")
            + bytes([0x00, 0x01]))       # raF=0 相对, sync=1 缓存


def cmd_angles(moves: dict, mirror) -> dict:
    """机构姿态 -> 各电机命令角度（镜像安装的那台取反）。"""
    return {a: (-d if a in mirror else d) for a, d in moves.items()}


def move_sync(m: Motor, moves: dict, rpm: int, acc: int):
    """moves = {addr: 角度}; 缓存后广播同时触发。"""
    for addr, deg in moves.items():
        m.send(addr, pos_payload(deg, rpm, acc))
        time.sleep(0.006)                # 防粘包
    m.send_raw(0x0000, bytes([OP_SYNC, 0x66, CHECKSUM]))
    time.sleep(0.02)


def status_byte(m: Motor, addr: int, wait: float = 0.12):
    """返回 (电机状态字节, 实时转速RPM(带符号), 实时位置角度) 或 (None,)*3。"""
    full, _, _ = m.request(addr, bytes([OP_READ_STATUS, 0x7A]),
                           wait, early=True, func=OP_READ_STATUS)
    d = parse_status(full)
    if d is None:
        return None, None, None
    return d["motor_status"], d["speed_rpm"], d["pos_deg"]


def wait_done(m: Motor, addrs, timeout: float = 10.0):
    """等所有电机 转速=0 且 到位Prf 置位。"""
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.05)
        ok = True
        for a in addrs:
            st, rpm, _ = status_byte(m, a)
            if st is None or rpm != 0 or not (st & 0x02):
                ok = False
                break
        if ok:
            return True
    return False


def emergency_stop(m: Motor, addrs):
    for a in addrs:
        try:
            m.send(a, bytes([OP_STOP, 0x98, 0x00]))
            time.sleep(0.02)
        except Exception:
            pass


# --------------------------- 动作参数组装 ---------------------------

MOTION_KEYS = ("tilt", "cycles", "spin", "rpm", "acc", "dwell")


def make_ctx(cfg, pair, overrides=None, mirror=None, no_limits=False,
             cfg_path=None):
    """配置 + 覆盖值 -> 动作 token（gimbal.main 与 scope.py 共用这一处）。"""
    ov = overrides or {}
    ctx = {
        "pair": pair,
        "mirror": cfgmod.mirror_set(cfg) if mirror is None else set(mirror),
        "kin": cfg["kinematics"],
        "lim": cfg["limits"],
        "no_limits": no_limits,
        "cfg_path": cfg_path or cfgmod.CONFIG_PATH,
        "blocked": None,            # 软限位拦截时记下是哪一次动作
    }
    for k in MOTION_KEYS:
        v = ov.get(k)
        ctx[k] = cfg["motion"][k] if v is None else v
    return ctx


# --------------------------- 姿态解算与限位 ---------------------------

def raw_pose(ang: dict, pair, mirror):
    """两台电机编码器角度 -> 托盘原始姿态 (raw_tilt, raw_pan)，单位度。

    镜像安装的那台，其世界转角 = -编码器角度。同一套式子下，
    俯仰指令 d 恰好给 raw_tilt 加 d，自转指令 s 恰好给 raw_pan 加 s。
    """
    w = {a: (-v if a in mirror else v) for a, v in ang.items()}
    a1, a2 = pair
    return ((w[a1] + w[a2]) / 2.0, (w[a1] - w[a2]) / 2.0)


def shown_pose(raw_tilt, raw_pan, kin):
    """原始姿态 -> 相对标定零点的 pan/tilt（已乘方向符号）。"""
    tilt = (raw_tilt - kin["zero_tilt"]) * kin["tilt_sign"]
    pan = (raw_pan - kin["zero_pan"]) * kin["pan_sign"]
    return tilt, pan


def measure_pose(m: Motor, pair, mirror):
    """读两台编码器，返回 (raw_tilt, raw_pan)；读不到返回 None。"""
    ang = {}
    for a in pair:
        st, rpm, pos = status_byte(m, a)
        if st is None:
            print("  地址 %d 无应答，无法解算姿态。" % a)
            return None
        ang[a] = pos
    return raw_pose(ang, pair, mirror)


def check_limits(tilt, pan, lim):
    """返回越界说明列表（空 = 通过）。"""
    msgs = []
    if not (lim["tilt_min"] <= tilt <= lim["tilt_max"]):
        msgs.append("俯仰 %.2f° 超出 [%g, %g]"
                    % (tilt, lim["tilt_min"], lim["tilt_max"]))
    if not (lim["pan_min"] <= pan <= lim["pan_max"]):
        msgs.append("自转 %.2f° 超出 [%g, %g]"
                    % (pan, lim["pan_min"], lim["pan_max"]))
    return msgs


def spin_capacity(lim):
    """从 pan 零点出发，单程自转最多能走多少度（两个方向取小者）。"""
    return min(lim["pan_max"], -lim["pan_min"])


def guard(m, ctx, new_tilt, new_pan, what):
    """发命令前的软限位闸门。返回 True = 放行。

    拦截时在 ctx 里记一笔 blocked，好让 main 用退出码 2 区分「保护生效」和
    真正的故障（掉线/超时 = 退出码 1）。
    """
    if ctx["no_limits"]:
        return True
    bad = check_limits(new_tilt, new_pan, ctx["lim"])
    if bad:
        ctx["blocked"] = what
        print("  [X] 软限位拦截（%s）：%s —— 本次不发命令。（保护生效，不是故障）"
              % (what, "；".join(bad)))
        print("    目标姿态 俯仰=%.2f° 自转=%.2f°；要放宽请改 %s 的 limits"
              % (new_tilt, new_pan, ctx["cfg_path"]))
        return False
    return True


# --------------------------- 动作序列 ---------------------------

def run_cycle(m: Motor, ctx, verbose=True, spin_dir=+1):
    """一个周期 = 俯仰来回 cycles 次 + 自转 spin 度。返回 False 表示有中止/超时。

    spin_dir 决定本圈自转的方向。pan 是**累加坐标**（不是绕圈取模），所以
    连续同向自转会把 pan 累加出去、被软限位永久卡死。调用方按圈传 ±1 交替，
    pan 就只在 ±spin 之间来回。
    """
    pair, mirror = ctx["pair"], ctx["mirror"]
    a1, a2 = pair
    ok = True
    for i in range(ctx["cycles"]):
        for sign in (+1, -1):
            d = sign * ctx["tilt"]
            pose = measure_pose(m, pair, mirror)
            if pose is None:
                return False
            t, p = shown_pose(pose[0] + d, pose[1], ctx["kin"])
            if not guard(m, ctx, t, p, "俯仰 第%d次 %+g°" % (i + 1, d)):
                return False
            move_sync(m, cmd_angles({a1: d, a2: d}, mirror), ctx["rpm"], ctx["acc"])
            if not wait_done(m, pair):
                ok = False
                if verbose:
                    print("  俯仰 第%d次 %+g° 超时未到位" % (i + 1, d))
    if ctx["spin"]:
        if ctx["dwell"]:
            time.sleep(ctx["dwell"])
        s = spin_dir * ctx["spin"]
        pose = measure_pose(m, pair, mirror)
        if pose is None:
            return False
        t, p = shown_pose(pose[0], pose[1] + s, ctx["kin"])
        if not guard(m, ctx, t, p, "自转 %+.1f°" % s):
            return False
        move_sync(m, cmd_angles({a1: +s, a2: -s}, mirror),
                  ctx["rpm"], ctx["acc"])
        if not wait_done(m, pair, timeout=30.0):
            ok = False
            if verbose:
                print("  自转超时未到位")
    return ok


def run_once(m: Motor, ctx, verbose=True):
    """单程：两轴世界同向走 +tilt 后停住（确认方向用，不来回、不自转）。

    返回 True = 到位。被软限位拦截会写 ctx["blocked"]。
    """
    pair, a1, a2 = ctx["pair"], ctx["pair"][0], ctx["pair"][1]
    pose = measure_pose(m, pair, ctx["mirror"])
    if pose is None:
        return False
    t, p = shown_pose(pose[0] + ctx["tilt"], pose[1], ctx["kin"])
    if not guard(m, ctx, t, p, "单程俯仰 %+.1f°" % ctx["tilt"]):
        return False
    move_sync(m, cmd_angles({a1: ctx["tilt"], a2: ctx["tilt"]}, ctx["mirror"]),
              ctx["rpm"], ctx["acc"])
    done = wait_done(m, pair)
    if verbose and not done:
        print("  单程 %+.1f° 超时未到位" % ctx["tilt"])
    return done


def run_homing(m: Motor, ctx, mode: int = 0, timeout: float = 30.0,
               verbose=True):
    """触发回零并等它真正结束（轮询 3B 的 Org_SF/Org_CF 位）。

    回零是找参考点的动作，**故意不过软限位闸门**——否则从限位外就没法回零了。
    返回 True = 回零成功。
    """
    addrs = ctx["pair"]
    for a in addrs:
        m.send(a, bytes([OP_HOMING, mode & 0xFF, 0x00]))
        time.sleep(0.02)
    if verbose:
        print("  已触发回零（模式 %d），等结束 ..." % mode)
    end = time.time() + timeout
    started = False
    last = {}
    while time.time() < end:
        time.sleep(0.1)
        flags = {}
        for a in addrs:
            full, _, _ = m.request(a, bytes([OP_HOME_FLAG]), 0.12,
                                   early=True, func=OP_HOME_FLAG)
            flags[a] = (full[1] if full and full[0] == OP_HOME_FLAG
                        and len(full) >= 3 else None)
        if any(v is None for v in flags.values()):
            continue                    # 读不到，下一轮再试
        last = flags
        if any(v & 0x04 for v in flags.values()):
            started = True              # 至少一台进入「正在回零」
            continue
        if started:
            ok = all(not (v & 0x08) for v in flags.values())
            if verbose:
                print("  回零结束：%s（标志 %s）"
                      % ("成功" if ok else "失败",
                         " ".join("%d=0x%02X" % (a, v) for a, v in flags.items())))
            return ok
    if verbose:
        print("  回零超时（%.0fs）标志 %s"
              % (timeout, " ".join("%d=0x%02X" % (a, v) for a, v in last.items())
                 or "无"))
    return False


def show_pose(m, ctx, title="当前姿态"):
    print("%s：" % title)
    for a in ctx["pair"]:
        st, rpm, pos = status_byte(m, a)
        if st is None:
            print("  地址 %d 无应答" % a)
            return None
        print("  地址 %d 状态=0x%02X 转速=%d 位置=%.2f°" % (a, st, rpm, pos))
    pose = measure_pose(m, ctx["pair"], ctx["mirror"])
    if pose is None:
        return None
    tilt, pan = shown_pose(pose[0], pose[1], ctx["kin"])
    print("  托盘姿态（相对零点）：俯仰 %+.2f°  自转 %+.2f°" % (tilt, pan))
    return pose


def main():
    ap = argparse.ArgumentParser(description="二轴差速云台同步动作序列")
    ap.add_argument("--config", default=None, help="配置文件路径")
    ap.add_argument("--addrs", default="1,2", help="两个电机地址, 逗号分隔")
    ap.add_argument("--mirror", default=None,
                    help="镜像安装、角度需取反的电机地址，逗号分隔；"
                         "不给则用配置里的（空串=都不取反）")
    ap.add_argument("--tilt", type=float, default=None, help="俯仰幅度/度")
    ap.add_argument("--cycles", type=int, default=None, help="俯仰来回次数")
    ap.add_argument("--spin", type=float, default=None, help="自转角度/度 (0=跳过)")
    ap.add_argument("--rpm", type=int, default=None, help="转速 RPM")
    ap.add_argument("--acc", type=int, default=None, help="加速度档位 0-255")
    ap.add_argument("--dwell", type=float, default=None, help="俯仰结束到自转之间的停顿秒数")
    ap.add_argument("--repeat", type=int, default=1,
                    help="整体周期重复次数 (默认 1；0=无限循环，Ctrl+C 停车)")
    ap.add_argument("--once", action="store_true",
                    help="只走单程 +tilt 一次并停在终点(不来回、不自转)，用于确认方向")
    ap.add_argument("--show", action="store_true", help="只读当前位置与 pan/tilt 后退出")
    ap.add_argument("--zero", action="store_true",
                    help="把当前姿态记为 pan/tilt 零点并写回配置文件")
    ap.add_argument("--no-limits", action="store_true", help="本次跳过软限位检查")
    args = ap.parse_args()

    cfg = cfgmod.load(args.config)
    cfg_path = args.config or cfgmod.CONFIG_PATH
    a1, a2 = (int(x) for x in args.addrs.split(","))
    pair = (a1, a2)
    mirror = (None if args.mirror is None
              else {int(x) for x in args.mirror.split(",") if x.strip()})
    ctx = make_ctx(cfg, pair,
                   {"tilt": args.tilt, "cycles": args.cycles, "spin": args.spin,
                    "rpm": args.rpm, "acc": args.acc, "dwell": args.dwell},
                   mirror=mirror, no_limits=args.no_limits, cfg_path=cfg_path)

    m = Motor()
    try:
        print("配置：%s%s" % (cfg_path, "" if Path(cfg_path).exists()
                             else "（不存在，用缺省值；可跑 python tools/config.py --init 生成）"))
        print("镜像取反的地址：%s | 软限位：%s"
              % (sorted(ctx["mirror"]) or "无",
                 "关（--no-limits）" if args.no_limits else
                 "俯仰[%g, %g] 自转[%g, %g]" % (cfg["limits"]["tilt_min"],
                                                cfg["limits"]["tilt_max"],
                                                cfg["limits"]["pan_min"],
                                                cfg["limits"]["pan_max"])))
        if show_pose(m, ctx, "上电/当前") is None:
            return 1

        if args.show:
            return 0

        if args.zero:
            pose = measure_pose(m, pair, ctx["mirror"])
            if pose is None:
                return 1
            cfg["kinematics"]["zero_pan"] = round(pose[1], 4)
            cfg["kinematics"]["zero_tilt"] = round(pose[0], 4)
            p = cfgmod.save(cfg, cfg_path)
            print("\n已把当前姿态记为 pan/tilt 零点 -> %s" % p)
            print("   zero_pan=%.4f  zero_tilt=%.4f（原始编码器坐标系）"
                  % (pose[1], pose[0]))
            return 0

        if args.once:
            print("\n单程测试: 两轴世界同向 %+.1f°, %d RPM (走完停住)"
                  % (ctx["tilt"], ctx["rpm"]))
            done = run_once(m, ctx)
            print("  到位=%s" % ("是" if done else "否(超时)"))
            show_pose(m, ctx, "结束姿态")
            if ctx["blocked"]:
                print("\n退出码 2：软限位保护生效（%s），不是故障。"
                      % ctx["blocked"])
                return 2
            return 0 if done else 1

        print("\n动作: 俯仰 ±%.1f° × %d 次来回 → 自转 ±%.1f°（按圈换向）, %d RPM, 共 %s 个周期"
              % (ctx["tilt"], ctx["cycles"], ctx["spin"], ctx["rpm"],
                 "无限" if args.repeat == 0 else str(args.repeat)))

        cap = spin_capacity(ctx["lim"])
        if ctx["spin"] > cap:
            print("  [!] 自转 %.1f° 超过限位能容纳的 ±%.1f°：会走到一半被软限位拦下。"
                  "把它缩到 %.1f 以内，或改 %s 的 limits。"
                  % (ctx["spin"], cap, cap, ctx["cfg_path"]))

        n = 0
        stopped = False
        try:
            while args.repeat == 0 or n < args.repeat:
                n += 1
                print("--- 周期 %d%s ---" % (n, "" if args.repeat else " (无限循环, Ctrl+C 停)"))
                if not run_cycle(m, ctx, spin_dir=(+1 if n % 2 else -1)):
                    stopped = True
                    break
        except KeyboardInterrupt:
            print("\n收到 Ctrl+C，立即停车 ...")
            emergency_stop(m, pair)
            stopped = True

        show_pose(m, ctx, "结束姿态")
        if ctx["blocked"]:
            print("\n退出码 2：软限位保护生效（%s），不是故障。" % ctx["blocked"])
            return 2
        return 1 if stopped else 0
    finally:
        m.close()


if __name__ == "__main__":
    sys.exit(main())
