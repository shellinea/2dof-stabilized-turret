#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
zdt_gui — 上位机 GUI (Tkinter)
================================================================
镜像原程序 "ZDT_Y42_Emm_CAN_Tool V1.2.4" 的界面，复用逆向得到的协议层。
按钮与中文标签取自原 exe 的字符串表；逻辑对应各槽函数。

运行:
    python zdt_gui.py

链路: 仿真(无需硬件) / UART / SLCAN(CANable) / UDP(CANBlaster)
真实链路需要 pyserial:  pip install pyserial
"""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import ttk, scrolledtext

from zdt_emm_v5 import (
    CheckMode, Dir, PosMode, Sync,
    enable, velocity, position, stop, sync_motion, clear_position,
    release_stall, read_sys_status, read_driver_params, read_homing_params,
    set_zero_point, trigger_homing, exit_homing,
)

# 复用 CLI 里已经写好的链路实现与应答解析
from zdt_tool import (
    SimTransport, SerialTransport, UdpTransport, decode_status, cmd_status_code,
)

MODE_MAP = {"0x6B 固定": CheckMode.CHECK_6B,
            "XOR 异或": CheckMode.CheckXOR,
            "CRC8": CheckMode.CheckCRC8}


class ZdtApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("张大头闭环伺服 上位机 — 复现版 (ZDT Emm_V5)")
        root.geometry("980x640")

        self.link = None
        self.q: "queue.Queue[tuple[str, object]]" = queue.Queue()

        self.v_link = tk.StringVar(value="仿真")
        self.v_port = tk.StringVar(value="COM5")
        self.v_baud = tk.StringVar(value="115200")
        self.v_addr = tk.StringVar(value="1")
        self.v_check = tk.StringVar(value="0x6B 固定")

        self.v_rpm = tk.StringVar(value="300")
        self.v_acc = tk.StringVar(value="0")
        self.v_dir = tk.StringVar(value="cw")
        self.v_pulses = tk.StringVar(value="32000")
        self.v_abs = tk.BooleanVar(value=True)
        self.v_home_mode = tk.StringVar(value="0")

        self._build()
        self.root.after(80, self._drain)

    # ---------------- UI ----------------
    def _build(self):
        top = ttk.LabelFrame(self.root, text="端口设置")
        top.pack(fill="x", padx=8, pady=6)

        ttk.Label(top, text="链路").grid(row=0, column=0, padx=4, pady=4)
        ttk.Combobox(top, textvariable=self.v_link, width=8, state="readonly",
                     values=["仿真", "UART", "SLCAN", "UDP"]).grid(row=0, column=1)
        ttk.Label(top, text="端口/地址").grid(row=0, column=2, padx=4)
        ttk.Entry(top, textvariable=self.v_port, width=10).grid(row=0, column=3)
        ttk.Label(top, text="波特率").grid(row=0, column=4, padx=4)
        ttk.Entry(top, textvariable=self.v_baud, width=8).grid(row=0, column=5)
        ttk.Label(top, text="电机地址").grid(row=0, column=6, padx=4)
        ttk.Entry(top, textvariable=self.v_addr, width=5).grid(row=0, column=7)
        ttk.Label(top, text="通讯校验方式").grid(row=0, column=8, padx=4)
        ttk.Combobox(top, textvariable=self.v_check, width=11, state="readonly",
                     values=list(MODE_MAP)).grid(row=0, column=9)
        self.btn_conn = ttk.Button(top, text="连接", command=self.on_connect)
        self.btn_conn.grid(row=0, column=10, padx=8)
        self.lbl_conn = ttk.Label(top, text="未连接", foreground="#b00")
        self.lbl_conn.grid(row=0, column=11, padx=4)

        mid = ttk.Frame(self.root)
        mid.pack(fill="both", expand=True, padx=8)

        # 左列
        left = ttk.Frame(mid)
        left.pack(side="left", fill="y")

        g = ttk.LabelFrame(left, text="驱动板使能")
        g.pack(fill="x", pady=4)
        ttk.Button(g, text="使能驱动板", width=16,
                   command=lambda: self.cmd(enable(self.addr(), True, mode=self.mode()))).pack(padx=6, pady=3)
        ttk.Button(g, text="关闭驱动板", width=16,
                   command=lambda: self.cmd(enable(self.addr(), False, mode=self.mode()))).pack(padx=6, pady=3)

        g = ttk.LabelFrame(left, text="运动控制")
        g.pack(fill="x", pady=4)
        ttk.Label(g, text="转速(RPM)").grid(row=0, column=0, sticky="e", padx=3, pady=2)
        ttk.Entry(g, textvariable=self.v_rpm, width=8).grid(row=0, column=1)
        ttk.Label(g, text="加速度").grid(row=0, column=2, sticky="e", padx=3)
        ttk.Entry(g, textvariable=self.v_acc, width=5).grid(row=0, column=3)
        ttk.Label(g, text="方向").grid(row=1, column=0, sticky="e", padx=3)
        ttk.Combobox(g, textvariable=self.v_dir, width=6, state="readonly",
                     values=["cw", "ccw"]).grid(row=1, column=1)
        ttk.Label(g, text="脉冲数").grid(row=2, column=0, sticky="e", padx=3)
        ttk.Entry(g, textvariable=self.v_pulses, width=8).grid(row=2, column=1)
        ttk.Checkbutton(g, text="绝对位置", variable=self.v_abs).grid(row=2, column=2, columnspan=2, sticky="w")
        row = ttk.Frame(g)
        row.grid(row=3, column=0, columnspan=4, pady=4)
        ttk.Button(row, text="位置模式", width=10,
                   command=lambda: self.cmd(position(
                       self.addr(), self.dir(), int(self.v_rpm.get()), int(self.v_acc.get()),
                       int(self.v_pulses.get()),
                       PosMode.Absolute if self.v_abs.get() else PosMode.Relative,
                       mode=self.mode()))).pack(side="left", padx=2)
        ttk.Button(row, text="速度模式", width=10,
                   command=lambda: self.cmd(velocity(
                       self.addr(), self.dir(), int(self.v_rpm.get()),
                       int(self.v_acc.get()), mode=self.mode()))).pack(side="left", padx=2)
        row2 = ttk.Frame(g)
        row2.grid(row=4, column=0, columnspan=4, pady=2)
        ttk.Button(row2, text="立即停止", width=10,
                   command=lambda: self.cmd(stop(self.addr(), mode=self.mode()))).pack(side="left", padx=2)
        ttk.Button(row2, text="清零位置角度", width=12,
                   command=lambda: self.cmd(clear_position(self.addr(), mode=self.mode()))).pack(side="left", padx=2)
        row3 = ttk.Frame(g)
        row3.grid(row=5, column=0, columnspan=4, pady=2)
        ttk.Button(row3, text="多机同步运动", width=12,
                   command=lambda: self.cmd(sync_motion(self.addr(), mode=self.mode()))).pack(side="left", padx=2)
        ttk.Button(row3, text="解除堵转保护", width=12,
                   command=lambda: self.cmd(release_stall(self.addr(), mode=self.mode()))).pack(side="left", padx=2)

        # 右列
        right = ttk.Frame(mid)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))

        g = ttk.LabelFrame(right, text="原点回零")
        g.pack(fill="x", pady=4)
        ttk.Button(g, text="设置单圈零点位置", width=18,
                   command=lambda: self.cmd(set_zero_point(self.addr(), mode=self.mode()))).pack(side="left", padx=4, pady=4)
        ttk.Label(g, text="回零模式值").pack(side="left", padx=(8, 2))
        ttk.Entry(g, textvariable=self.v_home_mode, width=6).pack(side="left")
        ttk.Button(g, text="触发回零", width=10,
                   command=lambda: self.cmd(trigger_homing(
                       self.addr(), int(self.v_home_mode.get()), mode=self.mode()))).pack(side="left", padx=4)
        ttk.Button(g, text="强制退出回零", width=12,
                   command=lambda: self.cmd(exit_homing(self.addr(), mode=self.mode()))).pack(side="left", padx=4)

        g = ttk.LabelFrame(right, text="参数")
        g.pack(fill="x", pady=4)
        ttk.Button(g, text="读取系统状态", width=14,
                   command=lambda: self.cmd(read_sys_status(self.addr(), mode=self.mode()))).pack(side="left", padx=4, pady=4)
        ttk.Button(g, text="读取驱动参数", width=14,
                   command=lambda: self.cmd(read_driver_params(self.addr(), mode=self.mode()))).pack(side="left", padx=4)
        ttk.Button(g, text="读取回零参数", width=14,
                   command=lambda: self.cmd(read_homing_params(self.addr(), mode=self.mode()))).pack(side="left", padx=4)

        g = ttk.LabelFrame(right, text="系统状态")
        g.pack(fill="both", expand=True, pady=4)
        self.status = ttk.Label(g, text="(未读取)", justify="left", anchor="nw",
                                font=("Consolas", 10))
        self.status.pack(fill="both", expand=True, padx=6, pady=6)

        g = ttk.LabelFrame(self.root, text="提示")
        g.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.log = scrolledtext.ScrolledText(g, height=8, font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, padx=4, pady=4)
        self.log.configure(state="disabled")

    # ---------------- helpers ----------------
    def addr(self) -> int:
        return int(self.v_addr.get())

    def mode(self) -> CheckMode:
        return MODE_MAP[self.v_check.get()]

    def dir(self) -> Dir:
        return Dir.CW if self.v_dir.get() == "cw" else Dir.CCW

    def logln(self, s: str):
        self.log.configure(state="normal")
        self.log.insert("end", s + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    # ---------------- link ----------------
    def on_connect(self):
        if self.link is not None:
            self.link.close()
            self.link = None
            self.lbl_conn.configure(text="未连接", foreground="#b00")
            self.btn_conn.configure(text="连接")
            self.logln("[断开]")
            return
        kind = self.v_link.get()
        try:
            if kind == "仿真":
                self.link = SimTransport()
            elif kind == "UART":
                self.link = SerialTransport(self.v_port.get(), int(self.v_baud.get()))
            elif kind == "SLCAN":
                self.link = SerialTransport(self.v_port.get(), int(self.v_baud.get()), slcan=True)
            elif kind == "UDP":
                self.link = UdpTransport(self.addr())
        except Exception as e:
            self.logln("[连接失败] %s" % e)
            return
        self.lbl_conn.configure(text="已连接", foreground="#080")
        self.btn_conn.configure(text="断开")
        self.logln("[连接成功] 链路=%s" % kind)

    # ---------------- command dispatch ----------------
    def cmd(self, frame):
        if self.link is None:
            self.logln("[提示] 请先连接")
            return
        threading.Thread(target=self._worker, args=(frame,), daemon=True).start()

    def _worker(self, frame):
        try:
            reply = self.link.send(frame)
        except Exception as e:
            self.q.put(("log", "[错误] %s" % e))
            return
        self.q.put(("log", "-> %s" % frame))
        if reply:
            self.q.put(("log", "<- %s" % reply.hex(" ").upper()))
        if len(reply) >= 3 and reply[1] == 0x43 and reply[2] == 0x7A:
            self.q.put(("status", reply))
            self.q.put(("log", "   读取系统状态"))
        else:
            self.q.put(("log", "   " + cmd_status_code(reply)))

    def _drain(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "log":
                    self.logln(payload)
                elif kind == "status":
                    self.status.configure(text=decode_status(payload).strip())
        except queue.Empty:
            pass
        self.root.after(80, self._drain)


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except Exception:
        pass
    ZdtApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
