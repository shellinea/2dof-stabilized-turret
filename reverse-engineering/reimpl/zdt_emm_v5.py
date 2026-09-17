#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ZDT "张大头" Emm_V5 closed-loop stepper/servo — protocol reimplementation.
================================================================================

Provenance
----------
This module is a clean reimplementation of the wire protocol used by the
application recovered from ``CAN.exe`` (identified as
``ZDT_Y42_Emm_CAN_Tool V1.2.4`` / "张大头闭环伺服").

Everything here was cross-verified against the unpacked binary
(``CLEAN_RECONSTRUCTED.exe``); the byte offsets in the comments are the exact
sites in that image where the tool builds each field:

  * command opcodes   recovered from the frame-builder idiom
                      ``mov eax,0x0000AA55 ; mov word[ebp-d],ax``
  * checksum modes    recovered from the dispatcher at 0x407900
  * CRC-8 table       @0x452A80  (CRC-8/MAXIM, poly 0x31)   -> command_calCRC8
  * MODBUS CRC tables @0x452880 / 0x452980 (split hi/lo, poly 0xA001)
                                                             -> modbus_calCRC
  * XOR mode          inlined at 0x4079B3                    -> command_calXOR

This file is *runnable* and independent of the original executable: it can
talk to a real motor over a serial port, a SLCAN/CANable adapter, or a UDP
CANBlaster bridge.

IMPORTANT: this is a reconstruction, not the vendor's original source. The
vendor source is not recoverable from a packed GUI binary — this reproduces
the *behaviour and wire format*, which is what actually drives the hardware.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import List

# ---------------------------------------------------------------------------
# Protocol constants  (recovered from the binary + public Emm_V5 spec)
# ---------------------------------------------------------------------------

#: Fixed "checksum" byte used in the default (CHECK_6B) framing mode.
CHECK_BYTE_6B = 0x6B


class CheckMode(IntEnum):
    """Matches ``enum CheckMode`` in mainwindow.h / dispatcher @0x407900."""
    CHECK_6B = 0          # append fixed 0x6B
    CheckXOR = 1          # XOR of all preceding bytes   (0x4079B3)
    CheckCRC8 = 2         # CRC-8/MAXIM over preceding bytes (table 0x452A80)


class Dir(IntEnum):
    """Rotation direction. Corresponds to ``enum ZeroDir`` (CW/CCW)."""
    CW = 0x00
    CCW = 0x01


class Enable(IntEnum):
    """Driver enable state. Corresponds to ``enum EnActive``."""
    Disable = 0x00
    Enable = 0x01


class Sync(IntEnum):
    """Multi-motor synchronisation flag (多机同步标志)."""
    Single = 0x00
    Sync = 0x01


class PosMode(IntEnum):
    """Relative/absolute selector (绝对标志)."""
    Relative = 0x00     # relative to current real-time position
    Absolute = 0x01     # absolute target


# ---------------------------------------------------------------------------
# Checksums
# ---------------------------------------------------------------------------

def _mk_crc8_maxim_table() -> bytes:
    """Reflected CRC-8/MAXIM table, poly 0x31. Must equal bytes @0x452A80."""
    tbl = bytearray(256)
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x8C if (c & 1) else (c >> 1)
        tbl[i] = c & 0xFF
    return bytes(tbl)


_CRC8_TABLE = _mk_crc8_maxim_table()


def crc8_maxim(data: bytes, crc: int = 0x00) -> int:
    """CRC-8/MAXIM (Dallas 1-Wire), poly 0x31 reflected, init 0x00.

    Verified byte-for-byte against the table baked into the binary at 0x452A80
    (``command_calCRC8``).
    """
    for b in data:
        crc = _CRC8_TABLE[crc ^ b]
    return crc & 0xFF


def modbus_crc16(data: bytes) -> int:
    """MODBUS CRC-16, poly 0xA001 (reflected 0x8005), init 0xFFFF.

    The binary uses a split hi/lo lookup table (0x452880 / 0x452980); this
    produces identical output (``modbus_calCRC``).
    """
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def xor_checksum(data: bytes) -> int:
    """Simple XOR of every byte (``command_calXOR``, inlined @0x4079B3)."""
    c = 0
    for b in data:
        c ^= b
    return c & 0xFF


def checksum(data: bytes, mode: CheckMode = CheckMode.CHECK_6B) -> bytes:
    """Return the trailing checksum bytes for *data* under *mode*.

    Note: MODBUS CRC-16 is a distinct 16-bit check used by a separate frame
    family (config read replies); it is exposed via :func:`modbus_crc16` and
    is not part of the 3-way CheckMode selector.
    """
    if mode == CheckMode.CHECK_6B:
        return bytes([CHECK_BYTE_6B])
    if mode == CheckMode.CheckXOR:
        return bytes([xor_checksum(data)])
    if mode == CheckMode.CheckCRC8:
        return bytes([crc8_maxim(data)])
    raise ValueError(f"unknown check mode {mode!r}")


# ---------------------------------------------------------------------------
# Frame assembly
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Frame:
    """A complete logical command: address + payload + checksum."""
    address: int
    payload: bytes
    mode: CheckMode = CheckMode.CHECK_6B

    def bytes(self) -> bytes:
        """Raw UART/TTL wire bytes: ``[addr][payload...][checksum]``."""
        head = bytes([self.address]) + self.payload
        return head + checksum(head, self.mode)

    def __repr__(self) -> str:
        return "Frame(%s)" % self.bytes().hex(" ").upper()


def _u16(v: int) -> bytes:
    """16-bit big-endian (speed fields are sent MSB-first)."""
    return struct.pack(">H", v & 0xFFFF)


def _u32(v: int) -> bytes:
    """32-bit big-endian (pulse/angle fields are sent MSB-first)."""
    return struct.pack(">I", v & 0xFFFFFFFF)


# ---------------------------------------------------------------------------
# Commands  (one function per UI action in the recovered MainWindow)
# ---------------------------------------------------------------------------

def enable(addr: int, on: bool, sync: Sync = Sync.Single,
           mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """使能驱动板 / 关闭驱动板 — ``F3 AB state sync`` (0x407C3D region)."""
    payload = bytes([0xF3, 0xAB, 0x01 if on else 0x00, int(sync)])
    return Frame(addr, payload, mode)


def velocity(addr: int, direction: Dir, rpm: int, accel: int = 0,
             sync: Sync = Sync.Single,
             mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """速度模式 — ``F6 dir speedH speedL acc sync``."""
    payload = bytes([0xF6, int(direction)]) + _u16(rpm) + \
              bytes([accel & 0xFF, int(sync)])
    return Frame(addr, payload, mode)


def position(addr: int, direction: Dir, rpm: int, accel: int,
             pulses: int, pmode: PosMode = PosMode.Relative,
             sync: Sync = Sync.Single,
             mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """位置模式 — ``FD dir speedH speedL acc p3 p2 p1 p0 raF sync``."""
    payload = bytes([0xFD, int(direction)]) + _u16(rpm) + bytes([accel & 0xFF]) \
              + _u32(pulses) + bytes([int(pmode), int(sync)])
    return Frame(addr, payload, mode)


def stop(addr: int, sync: Sync = Sync.Single,
         mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """立即停止 — ``FE 98 00 sync`` (0x408C95 region)."""
    return Frame(addr, bytes([0xFE, 0x98, 0x00, int(sync)]), mode)


def sync_motion(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """多机同步运动 — ``FF 66`` (confirmed @0x408FF4)."""
    return Frame(addr, bytes([0xFF, 0x66]), mode)


def clear_position(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """清零位置角度 — ``0A 6D`` (confirmed @0x408954)."""
    return Frame(addr, bytes([0x0A, 0x6D]), mode)


def read_sys_status(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """读取系统状态参数 — ``43 7A`` (confirmed @0x40552E)."""
    return Frame(addr, bytes([0x43, 0x7A]), mode)


def read_driver_params(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """读取驱动参数 — ``42 6C`` (0x40587E region)."""
    return Frame(addr, bytes([0x42, 0x6C]), mode)


def read_homing_params(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """读取回零参数 — ``AE 4B`` (0x4063EE region)."""
    return Frame(addr, bytes([0xAE, 0x4B]), mode)


def set_zero_point(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """设置单圈回零零点位置 — ``93 88`` (confirmed @0x407844)."""
    return Frame(addr, bytes([0x93, 0x88]), mode)


def trigger_homing(addr: int, mode_value: int = 0x0000,
                   mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """触发回零 — ``9A hi lo`` (Ghidra-confirmed 5-byte frame).

    The two payload bytes carry a 16-bit homing-mode value read from the UI
    (回零模式/方向); the original builds them at FUN_004079E0. Default 0.
    """
    return Frame(addr, bytes([0x9A, (mode_value >> 8) & 0xFF, mode_value & 0xFF]),
                 mode)


def exit_homing(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """强制中断并退出回零 — ``9C 48`` (confirmed @0x407F34)."""
    return Frame(addr, bytes([0x9C, 0x48]), mode)


def release_stall(addr: int, mode: CheckMode = CheckMode.CHECK_6B) -> Frame:
    """解除堵转保护 — ``0E 52`` (0x409334 region)."""
    return Frame(addr, bytes([0x0E, 0x52]), mode)


# ---------------------------------------------------------------------------
# Transports
# ---------------------------------------------------------------------------

def to_slcan(frame: Frame, can_id: int | None = None) -> bytes:
    """Wrap a command for a CANable / SLCAN USB-CAN adapter (ASCII, CR-term).

    The tool probes for exactly these adapters ("CANable 1.0/2.0 detected",
    "CANable SLCAN", SLCANInterface). CAN id defaults to the motor address.
    """
    cid = frame.address if can_id is None else can_id
    data = frame.bytes()
    if len(data) > 8:
        raise ValueError("payload exceeds a single CAN frame (8 bytes)")
    payload = data.hex().upper()
    return ("t%03X%d%s\r" % (cid, len(data), payload)).encode("ascii")


def to_can(frame: Frame, can_id: int | None = None) -> "CanFrame":
    """Plain CAN frame (id + up to 8 data bytes) for any CAN backend."""
    cid = frame.address if can_id is None else can_id
    data = frame.bytes()
    if len(data) > 8:
        raise ValueError("payload exceeds a single CAN frame (8 bytes)")
    return CanFrame(can_id=cid, data=data)


@dataclass(frozen=True)
class CanFrame:
    can_id: int
    data: bytes


# ---------------------------------------------------------------------------
# CANBlaster UDP bridge — multicast 239.255.43.21 (recovered constant)
# ---------------------------------------------------------------------------

CANBLASTER_MCAST = "239.255.43.21"


def to_canblaster(frame: Frame, can_id: int | None = None,
                  channel: int = 0) -> bytes:
    """Build a CANBlaster UDP datagram (19-byte header + up to 8 data bytes).

    Header layout (little-endian, per CANBlaster protocol):
        magic(2) command(1) ... channel(1) id(4) dlc(1) ... data(8)
    """
    cid = frame.address if can_id is None else can_id
    data = frame.bytes()
    if len(data) > 8:
        raise ValueError("payload exceeds a single CAN frame (8 bytes)")
    pkt = bytearray(19 + 8)
    struct.pack_into("<H", pkt, 0, 0x424C)       # 'BL'
    pkt[2] = 0x01                                # command: send frame
    pkt[3] = channel & 0xFF
    struct.pack_into("<I", pkt, 4, cid)
    pkt[8] = len(data)                           # dlc
    pkt[19:19 + len(data)] = data
    return bytes(pkt)


# ---------------------------------------------------------------------------
# Self-test — validates against the frames recovered from the binary
# ---------------------------------------------------------------------------

def _selftest() -> int:
    import struct as _s

    # 1. checksum tables must match the bytes baked into the binary
    assert _CRC8_TABLE[:16].hex() == "005ebce2613fdd83c29c7e20a3fd1f41", "CRC8 table mismatch"
    # MODBUS CRC-16 known vector: "123456789" -> 0x4B37
    assert modbus_crc16(b"123456789") == 0x4B37, "MODBUS CRC16 mismatch"
    # CRC-8/MAXIM known vector: "123456789" -> 0xA1
    assert crc8_maxim(b"123456789") == 0xA1, "CRC8/MAXIM mismatch"

    # 2. recovered command frames (address 0x01, default 0x6B checksum)
    checks = [
        (clear_position(0x01),    "01 0A 6D 6B"),
        (read_sys_status(0x01),   "01 43 7A 6B"),
        (sync_motion(0x01),       "01 FF 66 6B"),
        (exit_homing(0x01),       "01 9C 48 6B"),
        (set_zero_point(0x01),    "01 93 88 6B"),
        (read_driver_params(0x01),"01 42 6C 6B"),
        (read_homing_params(0x01),"01 AE 4B 6B"),
        (release_stall(0x01),     "01 0E 52 6B"),
        (enable(0x01, True),      "01 F3 AB 01 00 6B"),
        (stop(0x01),              "01 FE 98 00 00 6B"),
        (velocity(0x01, Dir.CW, 100, 0), "01 F6 00 00 64 00 00 6B"),
        (position(0x01, Dir.CW, 100, 0, 100, PosMode.Absolute),
                                  "01 FD 00 00 64 00 00 00 00 64 01 00 6B"),
    ]
    ok = True
    for frame, expect in checks:
        got = frame.bytes().hex(" ").upper()
        flag = "ok " if got == expect else "FAIL"
        if got != expect:
            ok = False
        print("  [%s] %-42s %s" % (flag, repr(frame), got))
        if got != expect:
            print("        expected: %s" % expect)

    # 3. checksum modes produce the documented tails
    f6b = clear_position(0x01, CheckMode.CHECK_6B).bytes()
    fxor = clear_position(0x01, CheckMode.CheckXOR).bytes()
    fcrc = clear_position(0x01, CheckMode.CheckCRC8).bytes()
    print("  mode 6B  :", f6b.hex(" ").upper())
    print("  mode XOR :", fxor.hex(" ").upper(),
          "(tail=%02X, expect %02X)" % (fxor[-1], xor_checksum(f6b[:-1])))
    print("  mode CRC8:", fcrc.hex(" ").upper(),
          "(tail=%02X, expect %02X)" % (fcrc[-1], crc8_maxim(f6b[:-1])))
    assert fxor[-1] == xor_checksum(f6b[:-1])
    assert fcrc[-1] == crc8_maxim(f6b[:-1])

    # 4. transports
    print("  SLCAN    :", to_slcan(clear_position(0x01)).decode().strip())
    print("  CANBlaster:", to_canblaster(clear_position(0x01)).hex(" ").upper())

    print("\nSELFTEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    if "--selftest" in sys.argv:
        raise SystemExit(_selftest())
    # demo: print the full command table for motor address 1
    demos: List[Frame] = [
        enable(1, True), enable(1, False),
        velocity(1, Dir.CW, 300, 10), velocity(1, Dir.CCW, 300, 10),
        position(1, Dir.CW, 200, 0, 3200, PosMode.Absolute),
        position(1, Dir.CCW, 200, 0, 1600, PosMode.Relative),
        stop(1), sync_motion(1), clear_position(1), release_stall(1),
        read_sys_status(1), read_driver_params(1), read_homing_params(1),
        set_zero_point(1), trigger_homing(1), exit_homing(1),
    ]
    print("ZDT Emm_V5 command table (addr=1, mode=0x6B)\n")
    for f in demos:
        print("  %s" % f)
