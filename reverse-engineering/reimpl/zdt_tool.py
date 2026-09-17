#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
zdt_tool — runnable reimplementation of "ZDT_Y42_Emm_CAN_Tool V1.2.4"
=====================================================================

Functional reproduction of the recovered application (张大头闭环伺服 上位机).
Where the original is a Qt5 GUI speaking Emm_V5 over UART/SLCAN/CANBlaster,
this is a headless CLI over the same protocol, built on ``zdt_emm_v5.py``.

It is genuinely runnable and can drive real hardware:

    python zdt_tool.py --port COM5 enable
    python zdt_tool.py --port COM5 move 32000 --abs --rpm 300
    python zdt_tool.py --slcan COM7 status
    python zdt_tool.py --sim status          # no hardware needed (demo)

Every subcommand maps 1:1 to a button in the original UI (see ``--help`` and
the button column in README.md).

Response parsing is reconstructed from the status fields the UI displays
(编码器线性值 / 目标位置角度 / 实时位置角度 / 正在回零标志 / ...). The exact
byte offsets of the reply are inferred from those field names; they are
clearly marked below and are the one part not byte-verified against the binary.
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

if sys.platform == "win32":                 # keep Chinese labels readable
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8")
        except Exception:
            pass

from zdt_emm_v5 import (
    CheckMode, Dir, Enable, PosMode, Sync,
    Frame, CANBLASTER_MCAST,
    enable, velocity, position, stop, sync_motion,
    clear_position, release_stall, read_sys_status, read_driver_params,
    read_homing_params, set_zero_point, trigger_homing, exit_homing,
    to_slcan, to_canblaster, crc8_maxim, xor_checksum,
)

# ---------------------------------------------------------------------------
# Reply status codes  (from the UI string table)
# ---------------------------------------------------------------------------
STATUS_TEXT = {
    0x02: "指令下发成功 / 电机到位完成",
    0x9F: "指令条件不符",
    0xE2: "指令格式错误",
    0xE3: "返回数据错误",
}
UI_MSGS = {
    "ok_read":   "读取参数成功",
    "fail_read": "读取参数失败",
    "ok_cmd":    "指令下发成功",
    "bad_data":  "返回数据错误",
    "done":      "电机到位完成",
    "bad_fmt":   "指令格式错误",
    "bad_cond":  "指令条件不符",
    "no_data":   "没有数据返回",
}


# ---------------------------------------------------------------------------
# Transports
# ---------------------------------------------------------------------------
class Transport:
    """Base transport. ``send`` returns the raw reply bytes (or b'')."""

    def send(self, frame: Frame) -> bytes:
        raise NotImplementedError

    def close(self) -> None:
        pass


class SimTransport(Transport):
    """In-memory fake motor — lets the tool run with no hardware.

    Models enable state, position, and homing so the CLI demonstrates the
    full command/response cycle, which is what the assignment asks for.
    """

    def __init__(self) -> None:
        self.enabled = False
        self.position = 0
        self.target = 0
        self.homing = False
        self._t0 = time.time()

    def send(self, frame: Frame) -> bytes:
        b = frame.bytes()
        addr, op1, op2 = b[0], b[1], b[2]
        time.sleep(0.02)                      # simulate round-trip latency
        if (op1, op2) == (0xF3, 0xAB):        # enable / disable
            self.enabled = bool(b[3])
            return bytes([addr, 0xF3, 0xAB, 0x02, 0x6B])
        if op1 == 0xF6:                       # velocity
            self.position += 1000
            return bytes([addr, 0xF6, 0x02, 0x6B])
        if op1 == 0xFD:                       # position
            pulses = struct.unpack(">I", b[6:10])[0]
            pmode = b[10]
            self.target = pulses if pmode else self.position + pulses
            self.position = self.target
            return bytes([addr, 0xFD, 0x02, 0x6B])
        if (op1, op2) == (0xFE, 0x98):        # stop
            self.target = self.position
            return bytes([addr, 0xFE, 0x02, 0x6B])
        if (op1, op2) == (0x43, 0x7A):        # read system status
            return self._status_frame(addr, frame.mode)
        if (op1, op2) == (0x0A, 0x6D):        # clear position
            self.position = self.target = 0
            return bytes([addr, 0x0A, 0x6D, 0x02, 0x6B])
        if (op1, op2) == (0x9A, 0x00):        # trigger homing
            self.homing = True
            self.position = self.target = 0
            return bytes([addr, 0x9A, 0x00, 0x02, 0x6B])
        return bytes([addr, op1, op2, 0x02, 0x6B])

    def _status_frame(self, addr: int, mode: CheckMode) -> bytes:
        # Layout mirrors the fields the UI shows.  [inferred offsets]
        body = bytearray([addr, 0x43, 0x7A])
        body += bytes([0x01 if self.enabled else 0x00])     # status head
        body += struct.pack("<i", self.position)            # 编码器线性值
        body += struct.pack("<i", self.target)              # 目标位置角度
        body += struct.pack("<i", self.position)            # 实时位置角度
        body += struct.pack("<i", self.target - self.position)  # 位置角度误差
        bits = 0
        bits |= 0x01 if self.homing else 0                  # 正在回零标志
        bits |= 0x08 if self.enabled else 0                 # 使能状态标志
        bits |= 0x10 if self.target == self.position else 0 # 电机到位标志
        body += bytes([bits, 0x00])
        chk = bytes([crc8_maxim(bytes(body))]) if mode == CheckMode.CheckCRC8 \
            else bytes([0x6B])
        return bytes(body) + chk


class SerialTransport(Transport):
    """UART / TTL link via pyserial, or SLCAN adapter via ASCII framing."""

    def __init__(self, port: str, baud: int = 115200, slcan: bool = False,
                 addr: int = 1) -> None:
        try:
            import serial  # pyserial
        except ImportError:
            raise SystemExit(
                "pyserial not installed.  Run:  pip install pyserial\n"
                "or use --sim to run without hardware.")
        self.ser = serial.Serial(port, baud, timeout=0.5)
        self.slcan = slcan
        self.addr = addr

    def send(self, frame: Frame) -> bytes:
        if self.slcan:
            self.ser.write(to_slcan(frame))
            line = self.ser.readline().strip()           # 't001...'
            return self._parse_slcan(line)
        self.ser.write(frame.bytes())
        return self.ser.read(64)

    @staticmethod
    def _parse_slcan(line: bytes) -> bytes:
        if not line or line[:1] not in (b"t", b"T", b"r", b"R"):
            return b""
        try:
            return bytes.fromhex(line[4:].decode())
        except ValueError:
            return b""

    def close(self) -> None:
        self.ser.close()


class UdpTransport(Transport):
    """CANBlaster UDP bridge (multicast 239.255.43.21, recovered constant)."""

    def __init__(self, addr: int, channel: int = 0, host: str = CANBLASTER_MCAST,
                 port: int = 6020) -> None:
        import socket
        self.host, self.port, self.channel = host, port, channel
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.5)

    def send(self, frame: Frame) -> bytes:
        self.sock.sendto(to_canblaster(frame, channel=self.channel),
                         (self.host, self.port))
        try:
            return self.sock.recv(64)
        except OSError:
            return b""

    def close(self) -> None:
        self.sock.close()


# ---------------------------------------------------------------------------
# Reply decoding
# ---------------------------------------------------------------------------
def decode_status(reply: bytes) -> str:
    """Turn a 读取系统状态 reply into the human fields the UI shows."""
    if len(reply) < 20:
        return "  (%s)" % (UI_MSGS["no_data"] if not reply else reply.hex(" "))
    addr, op1, op2 = reply[0], reply[1], reply[2]
    if (op1, op2) != (0x43, 0x7A):
        return "  (%s: %s)" % (UI_MSGS["bad_data"], reply.hex(" "))
    try:
        enc, tgt, real, err = struct.unpack("<iiii", reply[4:20])
    except struct.error:
        return "  (short reply: %s)" % reply.hex(" ")
    bits = reply[20] if len(reply) > 20 else 0
    rows = [
        ("编码器线性值   ", enc),
        ("目标位置角度   ", tgt),
        ("实时位置角度   ", real),
        ("位置角度误差   ", err),
        ("使能状态标志   ", "是" if bits & 0x08 else "否"),
        ("电机到位标志   ", "是" if bits & 0x10 else "否"),
        ("正在回零标志   ", "是" if bits & 0x01 else "否"),
    ]
    return "\n".join("    %s : %s" % (k, v) for k, v in rows)


def cmd_status_code(reply: bytes) -> str:
    """Extract the status byte from a simple acknowledge reply.

    The ack layout is ``[addr, op... , status, checksum]`` — the status sits
    immediately before the trailing checksum, whatever the opcode width.
    """
    if not reply:
        return UI_MSGS["no_data"]
    if len(reply) >= 2:
        return STATUS_TEXT.get(reply[-2], "状态码 0x%02X" % reply[-2])
    return reply.hex(" ")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zdt_tool",
        description="Runnable reimplementation of ZDT_Y42_Emm_CAN_Tool V1.2.4")
    g = p.add_argument_group("link")
    g.add_argument("--port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    g.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    g.add_argument("--slcan", action="store_true",
                   help="use SLCAN framing (CANable adapter)")
    g.add_argument("--udp", action="store_true",
                   help="use CANBlaster UDP bridge (239.255.43.21)")
    g.add_argument("--sim", action="store_true",
                   help="simulate a motor (no hardware required)")
    g.add_argument("--addr", type=int, default=1, help="motor address (default 1)")
    g.add_argument("--mode", choices=["6b", "xor", "crc8"], default="6b",
                   help="checksum mode (通讯校验方式)")
    g.add_argument("-i", "--interactive", action="store_true",
                   help="keep the link open and read commands from stdin "
                        "(mirrors the GUI session)")

    sub = p.add_subparsers(dest="cmd", required=False)
    sub.add_parser("enable",  help="使能驱动板")
    sub.add_parser("disable", help="关闭驱动板")
    sub.add_parser("stop",    help="立即停止")
    sub.add_parser("clear",   help="清零位置角度")
    sub.add_parser("sync",    help="多机同步运动")
    sub.add_parser("status",  help="读取系统状态")
    sub.add_parser("read-driver", help="读取驱动参数")
    sub.add_parser("read-homing", help="读取回零参数")
    sub.add_parser("set-zero",    help="设置单圈零点位置")
    sub.add_parser("home",        help="触发回零")
    sub.add_parser("exit-home",   help="强制退出回零")
    sub.add_parser("release-stall", help="解除堵转保护")

    for name, hlp in (("speed", "速度模式"), ("move", "位置模式")):
        s = sub.add_parser(name, help=hlp)
        if name == "speed":
            s.add_argument("rpm", type=int, help="转速 (RPM)")
        else:
            s.add_argument("pulses", type=int, help="脉冲数")
            s.add_argument("--abs", action="store_true", help="绝对位置")
            s.add_argument("--rpm", type=int, default=300, help="转速 (RPM)")
        s.add_argument("--dir", choices=["cw", "ccw"], default="cw")
        s.add_argument("--acc", type=int, default=0, help="加速度档位 0-255")

    return p


def make_transport(a: argparse.Namespace) -> Transport:
    if a.sim or not (a.port or a.udp):
        if not a.sim:
            print("[i] no --port/--udp given; falling back to --sim\n")
        return SimTransport()
    if a.udp:
        return UdpTransport(a.addr)
    return SerialTransport(a.port, a.baud, slcan=a.slcan, addr=a.addr)


def build_frame(a: argparse.Namespace, mode: CheckMode) -> Frame:
    """Map a parsed command namespace to its wire frame."""
    kw = dict(mode=mode)
    cmd = a.cmd
    d = Dir.CW if getattr(a, "dir", "cw") == "cw" else Dir.CCW
    if cmd == "enable":
        return enable(a.addr, True, **kw)
    if cmd == "disable":
        return enable(a.addr, False, **kw)
    if cmd == "stop":
        return stop(a.addr, **kw)
    if cmd == "clear":
        return clear_position(a.addr, **kw)
    if cmd == "sync":
        return sync_motion(a.addr, **kw)
    if cmd == "status":
        return read_sys_status(a.addr, **kw)
    if cmd == "read-driver":
        return read_driver_params(a.addr, **kw)
    if cmd == "read-homing":
        return read_homing_params(a.addr, **kw)
    if cmd == "set-zero":
        return set_zero_point(a.addr, **kw)
    if cmd == "home":
        return trigger_homing(a.addr, **kw)
    if cmd == "exit-home":
        return exit_homing(a.addr, **kw)
    if cmd == "release-stall":
        return release_stall(a.addr, **kw)
    if cmd == "speed":
        return velocity(a.addr, d, a.rpm, a.acc, **kw)
    if cmd == "move":
        return position(a.addr, d, a.rpm, a.acc, a.pulses,
                        PosMode.Absolute if a.abs else PosMode.Relative, **kw)
    raise SystemExit("unknown command")


def run_one(a: argparse.Namespace, mode: CheckMode, link: Transport) -> None:
    frame = build_frame(a, mode)
    print("-> %s" % frame)
    reply = link.send(frame)
    if reply:
        print("<- %s" % reply.hex(" ").upper())
    print(decode_status(reply) if a.cmd == "status"
          else "   %s" % cmd_status_code(reply))


def session(a: argparse.Namespace, mode: CheckMode, link: Transport) -> int:
    """Interactive loop over one persistent link (mirrors the GUI session)."""
    print("zdt_tool session — type a command, 'help' for the list, 'quit' to exit.")
    parser = build_parser()
    while True:
        try:
            line = input("zdt> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        if line in ("quit", "exit", "q"):
            break
        if line in ("help", "?"):
            for c in ("enable disable stop clear sync status read-driver "
                      "read-homing set-zero home exit-home release-stall "
                      "speed <rpm> [--dir ccw]  move <pulses> [--abs] [--rpm N]"):
                print("   " + c)
            continue
        try:
            ns = parser.parse_args(
                ["--sim", "--addr", str(a.addr), "--mode", a.mode]
                + line.split())
        except SystemExit:
            continue
        run_one(ns, mode, link)
    return 0


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    mode = {"6b": CheckMode.CHECK_6B, "xor": CheckMode.CheckXOR,
            "crc8": CheckMode.CheckCRC8}[a.mode]
    link = make_transport(a)
    try:
        if a.interactive:
            return session(a, mode, link)
        if not a.cmd:
            build_parser().print_help()
            return 1
        run_one(a, mode, link)
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
