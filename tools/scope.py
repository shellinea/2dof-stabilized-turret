#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
实时波形示波器（pyqtgraph）
============================
采集线程轮询 43 7A -> 预分配环形缓冲 -> Qt 定时器 33ms 刷曲线。
pyqtgraph 只是把 numpy 数组的指针换给曲线，不重画整张图，所以能跑到 30FPS；
matplotlib 每次 setData 都要重建 artist，只能做到几帧，做实时窗口会卡。

为什么要和动作序列共用一个进程：CAN 适配器同一时间只能被一个程序占用。
所以 --motion 让云台动作在这个进程的后台线程跑，示波器前台看曲线。

用法:
    python tools/scope.py                       # 看 1、2 号，10 秒滚动窗
    python tools/scope.py --hz 30 --window 5
    python tools/scope.py --demo                # 假数据，不碰硬件，先看界面长啥样
    python tools/scope.py --save run.csv        # 顺带落盘，之后用 plot.py 复盘

    # 边动边看（--motion 让动作在后台线程跑，示波器前台看）
    python tools/scope.py --motion                        # 俯仰来回 + 自转
    python tools/scope.py --motion --tilt 30 --cycles 3 --spin 170
    python tools/scope.py --motion once --tilt 45         # 单程确认方向
    python tools/scope.py --motion home --home-mode 0     # 回零，看位置曲线扑向零点

    # 无限循环边动边看（--repeat 0）：一直跑，按 X 立即停车、Q 退出
    python tools/scope.py --motion --repeat 0 --tilt 30 --cycles 3 --spin 170

按键: 空格 暂停绘图   X 立即停车   C 清屏   S 存图   Q/Esc 退出

注意：VS Code 的断点**拦不住电机**。调试器在 PC 端暂停时，电机照走（它是自主的）。
所以发现问题就地按 X，不要去够终端——终端可能正被断点占着。断点还会把采集线程一起冻住，
恢复后适配器缓冲里堆的那批过期帧会一股脑涌进来。
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
import threading
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import numpy as np
import pyqtgraph as pg
from PySide6 import QtCore, QtWidgets

from zdt_can import Motor, OP_READ_STATUS, CSV_COLUMNS, parse_status
import config as cfgmod
import gimbal

PANELS = [
    ("pos_deg", "实时位置 (°)"),
    ("speed_rpm", "实时转速 (RPM)"),
    ("phase_ma", "相电流 (mA)"),
    ("err_deg", "位置误差 (°)"),
]
COLORS = ["#4ea1ff", "#ff9040", "#5fd35f", "#ff5fa8", "#c08cff", "#ffe066"]


# --------------------------- 环形缓冲 ---------------------------

class Ring:
    """一个地址的预分配环形缓冲：写满后覆盖最旧的，避免运行久了内存爆掉。"""

    def __init__(self, cap: int):
        self.cap = cap
        self.lock = threading.Lock()
        self.n = 0                  # 累计写入点数（可超过 cap）
        self.miss = 0               # 丢包/超时次数
        self.buf = {k: np.zeros(cap) for k in ("t_s",)
                    + tuple(k for k, _ in PANELS)}

    def push(self, t: float, d: dict):
        with self.lock:
            i = self.n % self.cap
            self.buf["t_s"][i] = t
            for k, _ in PANELS:
                self.buf[k][i] = d[k]
            self.n += 1

    def bump_miss(self):
        with self.lock:
            self.miss += 1

    def clear(self):
        with self.lock:
            self.n = 0
            self.miss = 0

    def last_t(self):
        with self.lock:
            if self.n == 0:
                return None
            return float(self.buf["t_s"][(self.n - 1) % self.cap])

    def series(self, key: str, window: float, t_now: float):
        """窗口内的 (t, y)，时间升序。没数据返回 (None, None)。

        每帧都返回新数组，好让 pyqtgraph 拿到的是快照而不是会被就地改写的视图。
        """
        with self.lock:
            n = min(self.n, self.cap)
            if n == 0:
                return None, None
            tv, yv = self.buf["t_s"], self.buf[key]
            if self.n <= self.cap:
                t, y = tv[:n].copy(), yv[:n].copy()
            else:
                i = self.n % self.cap          # i 处是最旧的一个
                t = np.concatenate((tv[i:], tv[:i]))
                y = np.concatenate((yv[i:], yv[:i]))
        if window and t_now is not None:
            m = t >= (t_now - window)
            t, y = t[m], y[m]
        return t, y


# --------------------------- 采集线程 ---------------------------

class Sampler(threading.Thread):
    """轮询 43 7A 把状态推进环形缓冲；可选边采边写 CSV。"""

    def __init__(self, m, addrs, rings, hz, wait, stop_evt,
                 demo=False, csv_path=None):
        super().__init__(daemon=True)
        self.m = m
        self.addrs = addrs
        self.rings = rings
        self.hz = hz
        self.wait = wait
        self.stop_evt = stop_evt
        self.demo = demo
        self.csv_path = csv_path
        self.rate = {a: 0.0 for a in addrs}
        self._f = self._writer = None

    def run(self):
        if self.csv_path:
            self._f = open(self.csv_path, "w", newline="", encoding="utf-8")
            self._f.write("# scope 实时采样  %s\n"
                          % time.strftime("%Y-%m-%d %H:%M:%S"))
            self._f.write("# 电机=%s  目标=%gHz\n" % (self.addrs, self.hz))
            self._f.write("# 角度 度 / 电压 mV / 电流 mA / speed_rpm 带符号\n")
            self._writer = csv.DictWriter(self._f, fieldnames=CSV_COLUMNS)
            self._writer.writeheader()
        period = 1.0 / self.hz if self.hz > 0 else 0.0
        payload = bytes([OP_READ_STATUS, 0x7A])
        t0 = time.time()
        recent = {a: [] for a in self.addrs}        # 最近若干个时刻，估速率用
        try:
            while not self.stop_evt.is_set():
                tick = time.time()
                for a in self.addrs:
                    d = (self._fake(a, tick - t0) if self.demo
                         else self._read(a, payload))
                    t = time.time() - t0
                    if d is None:
                        self.rings[a].bump_miss()
                        continue
                    self.rings[a].push(t, d)
                    if self._writer:
                        row = {"t_s": round(t, 6), "addr": a}
                        row.update(d)
                        self._writer.writerow(row)
                    r = recent[a]
                    r.append(time.time())
                    if len(r) > 20:
                        del r[0]
                if self._f:
                    self._f.flush()
                for a in self.addrs:
                    r = recent[a]
                    if len(r) > 1 and r[-1] > r[0]:
                        self.rate[a] = (len(r) - 1) / (r[-1] - r[0])
                if period:
                    dt = time.time() - tick
                    if dt < period:
                        self.stop_evt.wait(period - dt)
        finally:
            if self._f:
                self._f.close()

    def _read(self, a, payload):
        full, _, _ = self.m.request(a, payload, self.wait, early=True,
                                    func=OP_READ_STATUS)
        return parse_status(full)

    def _fake(self, a, t):
        """假数据：俯仰正弦，两台差个相位，用来先确认界面是活的。"""
        w = 2 * math.pi / 8.0
        base = 30.0 if a == self.addrs[0] else -30.0
        ang = base * math.sin(w * t + 0.25 * self.addrs.index(a))
        spd = base * w * math.cos(w * t + 0.25 * self.addrs.index(a))
        return {
            "bus_mv": 11020 + 12 * math.sin(37 * t),
            "phase_ma": 320 + 300 * abs(spd) / 25.0 + 8 * np.random.randn(),
            "encoder": 0,
            "target_deg": ang + 0.3 * math.sin(60 * t),
            "speed_rpm": spd / 6.0 + 0.4 * np.random.randn(),
            "pos_deg": ang,
            "err_deg": 0.25 * math.sin(60 * t),
            "homing_flags": 0x0B,
            "motor_status": 0x03,
        }


# --------------------------- 动作线程 ---------------------------

MODE_TEXT = {"cycle": "俯仰+自转", "once": "单程", "home": "回零"}


class MotionThread(threading.Thread):
    """后台跑云台动作，和采集线程共用一个 Motor（靠 Motor.lock 分时）。

    mode: cycle=俯仰来回+自转  once=单程确认方向  home=触发回零并等结束
    """

    def __init__(self, m, ctx, mode, repeat, stop_evt, home_mode=0):
        super().__init__(daemon=True)
        self.m, self.ctx, self.mode = m, ctx, mode
        self.repeat, self.stop_evt, self.home_mode = repeat, stop_evt, home_mode
        self.n = 0
        self.ok = None
        self.done = False
        self.aborted = False        # 用户在窗口里按了 X 急停
        self.blocked = None
        self.error = None

    def run(self):
        try:
            if self.mode == "once":
                self.n, self.ok = 1, gimbal.run_once(self.m, self.ctx)
            elif self.mode == "home":
                self.n, self.ok = 1, gimbal.run_homing(self.m, self.ctx,
                                                       self.home_mode)
            else:
                self.ok = True
                while (not self.stop_evt.is_set()
                       and (self.repeat == 0 or self.n < self.repeat)):
                    self.n += 1
                    # 自转按圈换向，否则 pan 累加出去会被软限位卡死
                    if not gimbal.run_cycle(self.m, self.ctx,
                                            spin_dir=(+1 if self.n % 2 else -1)):
                        self.ok = False
                        break
        except Exception as e:      # 动作崩了也不能连累示波器
            self.error = repr(e)
        finally:
            self.blocked = self.ctx.get("blocked")
            self.done = True


# --------------------------- 界面 ---------------------------

class ScopeWindow(QtWidgets.QMainWindow):
    def __init__(self, rings, addrs, window, sampler, motion=None, stopper=None):
        super().__init__()
        self.rings, self.addrs, self.window = rings, addrs, window
        self.sampler, self.motion = sampler, motion
        self.stopper = stopper      # 回调：窗口里按 X 时立即停车
        self.paused = False
        self.setWindowTitle("ZDT X42S 实时波形")
        self.resize(1180, 860)

        central = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(central)
        lay.setContentsMargins(4, 4, 4, 4)
        self.pw = pg.GraphicsLayoutWidget()
        lay.addWidget(self.pw)
        self.setCentralWidget(central)

        self.plots, self.curves = {}, {}
        first = None
        for row, (key, label) in enumerate(PANELS):
            p = self.pw.addPlot(row=row, col=0)
            p.showGrid(x=True, y=True, alpha=0.25)
            p.setLabel("left", label)
            p.getViewBox().setAutoVisible(y=True)   # Y 只按可见窗口自适应
            if first is None:
                first = p
                p.addLegend(offset=(-20, 12))
            else:
                p.setXLink(first)
            self.plots[key] = p
            for a in addrs:
                c = p.plot(pen=pg.mkPen(COLORS[addrs.index(a) % len(COLORS)],
                                        width=1.4),
                           name="地址 %d" % a)
                # 只画窗口内的点；点多到超过像素密度时按峰谷抽点（真示波器也这么干）
                c.setClipToView(True)
                c.setDownsampling(auto=True, method="peak")
                self.curves[(key, a)] = c
        self.plots[PANELS[-1][0]].setLabel("bottom", "时间 (s)")

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(33)                    # ~30 FPS

        self.ctl = QtCore.QTimer(self)
        self.ctl.timeout.connect(self.tick_status)
        self.ctl.start(500)

    # --- 绘图 ---

    def refresh(self):
        if self.paused:
            return
        t_now = None
        for a in self.addrs:
            lt = self.rings[a].last_t()
            if lt is not None and (t_now is None or lt > t_now):
                t_now = lt
        if t_now is None:
            return
        for key, _ in PANELS:
            for a in self.addrs:
                t, y = self.rings[a].series(key, self.window, t_now)
                if t is not None and len(t):
                    self.curves[(key, a)].setData(t, y)
        x0 = max(0.0, t_now - self.window)
        self.plots[PANELS[0][0]].setXRange(x0, max(t_now, self.window), padding=0)

    def tick_status(self):
        bits = ["地址%d %.1fHz 丢%d" % (a, self.sampler.rate.get(a, 0.0),
                                      self.rings[a].miss)
                for a in self.addrs]
        if self.paused:
            bits.append("已暂停（空格继续）")
        if self.motion is not None:
            bits.append(self._motion_text())
        bits.append("空格暂停  X急停  C清屏  S存图  Q退出")
        self.statusBar().showMessage("   |   ".join(bits))

    def _motion_text(self):
        mo = self.motion
        what = MODE_TEXT.get(mo.mode, "动作")
        if mo.error:
            return "动作异常：%s" % mo.error
        if mo.aborted:
            return "已手动急停"
        if mo.blocked:
            return "被软限位拦截：%s" % mo.blocked
        if mo.done:
            if mo.mode == "cycle":
                if mo.repeat == 0:      # 无限循环只会被 X / Q / 出错叫停
                    return "已停止（共跑 %d 轮%s）" % (
                        mo.n, "，正常" if mo.ok else "，有超时")
                return "动作已完成 %d 轮（%s）" % (mo.n, "正常" if mo.ok else "有超时")
            return "%s %s" % (what, "成功" if mo.ok else "失败/超时")
        if mo.mode == "cycle":
            return "%s 进行中（第 %d 轮%s）" % (
                what, mo.n, "，无限循环" if mo.repeat == 0 else "")
        return "%s 进行中" % what

    # --- 交互 ---

    def keyPressEvent(self, ev):
        k = ev.key()
        if k == QtCore.Qt.Key_Space:
            self.paused = not self.paused
        elif k == QtCore.Qt.Key_X:
            # 看波形时发现不对，不用去动终端（那里可能还被断点占着）
            if self.motion is not None:
                self.motion.aborted = True
            if self.stopper:
                self.stopper()
        elif k == QtCore.Qt.Key_C:
            for r in self.rings.values():
                r.clear()
        elif k == QtCore.Qt.Key_S:
            p = time.strftime("scope_%Y%m%d_%H%M%S.png")
            self.pw.grab().save(p)
            print("已存图：%s" % p)
        elif k in (QtCore.Qt.Key_Q, QtCore.Qt.Key_Escape):
            self.close()

    def closeEvent(self, ev):
        self.timer.stop()
        self.ctl.stop()
        self.sampler.stop_evt.set()
        super().closeEvent(ev)


# --------------------------- 入口 ---------------------------

def main():
    ap = argparse.ArgumentParser(description="ZDT X42S 实时波形示波器")
    ap.add_argument("--addr", default="1,2", help="要看的电机地址，逗号分隔")
    ap.add_argument("--hz", type=float, default=20.0,
                    help="每台电机的轮询频率 (默认 20)。"
                         "实测两台一起读全量 43 7A 也能到 ~130Hz，放心往上开")
    ap.add_argument("--window", type=float, default=10.0, help="滚动窗口秒数 (默认 10)")
    ap.add_argument("--secs", type=float, default=0.0,
                    help="环形缓冲长度秒数 (默认 = 窗口×6)")
    ap.add_argument("--swait", type=float, default=0.08, help="每点等待应答秒数")
    ap.add_argument("--save", default=None, help="同时写 CSV（之后可用 plot.py 复盘）")
    ap.add_argument("--demo", action="store_true", help="不用硬件，生成假数据先看界面")
    ap.add_argument("--motion", nargs="?", const="cycle",
                    choices=["cycle", "once", "home"],
                    help="同时跑哪个动作（必须同进程：适配器只能被一个程序占用）："
                         "cycle=俯仰来回+自转(默认) once=单程确认方向 home=回零")
    ap.add_argument("--home-mode", type=int, default=0,
                    help="home 用哪种回零模式 (默认 0 单圈就近)")
    ap.add_argument("--repeat", type=int, default=1, help="动作重复轮数 (0=无限)")
    ap.add_argument("--tilt", type=float, default=None)
    ap.add_argument("--cycles", type=int, default=None)
    ap.add_argument("--spin", type=float, default=None)
    ap.add_argument("--rpm", type=int, default=None)
    ap.add_argument("--acc", type=int, default=None)
    ap.add_argument("--no-limits", action="store_true")
    args = ap.parse_args()

    addrs = [int(x) for x in args.addr.replace(",", " ").split()] or [1]
    span = args.secs or (args.window * 6.0)
    cap = max(600, int(span * max(args.hz, 1.0)) + 10)

    rings = {a: Ring(cap) for a in addrs}
    stop_evt = threading.Event()
    m = motion = None

    if args.demo:
        print("演示模式：假数据，不碰硬件")
    else:
        try:
            m = Motor()
        except Exception as e:
            print("打不开 CAN 适配器：%r" % (e,))
            print("  —— 先关掉官方上位机 / cangaroo；或加 --demo 先看界面。")
            return 1
        print("已连上 CAN 适配器（500 kbps）")

    sampler = Sampler(m, addrs, rings, args.hz, args.swait, stop_evt,
                      demo=args.demo, csv_path=args.save)
    sampler.start()

    def do_stop():
        """窗口里按 X：立即停车（不发到终端，终端可能被断点占着）。"""
        if m is None:
            print("演示模式：没有硬件可停。")
            return
        pair = (tuple(addrs[:2]) if len(addrs) >= 2 else (1, 2))
        gimbal.emergency_stop(m, pair)
        print("已发立即停车：%s" % (pair,))

    if args.motion:
        if args.demo:
            print("--demo 下不跑动作（没有硬件）")
        else:
            pair = tuple(addrs[:2]) if len(addrs) >= 2 else (1, 2)
            cfg = cfgmod.load(None)
            ctx = gimbal.make_ctx(cfg, pair,
                                  {"tilt": args.tilt, "cycles": args.cycles,
                                   "spin": args.spin, "rpm": args.rpm,
                                   "acc": args.acc},
                                  no_limits=args.no_limits)
            motion = MotionThread(m, ctx, args.motion, args.repeat, stop_evt,
                                  home_mode=args.home_mode)
            motion.start()
            print("动作线程已启动：%s（%s）%s"
                  % (pair, MODE_TEXT.get(args.motion, args.motion),
                     "，无限轮" if args.repeat == 0 and args.motion == "cycle"
                     else ""))

    app = QtWidgets.QApplication([sys.argv[0]])
    win = ScopeWindow(rings, addrs, args.window, sampler, motion,
                      stopper=do_stop)
    win.show()
    QtCore.QTimer(app).start(200)       # 空转，让 Qt 事件循环能把 Ctrl+C 交给 Python

    try:
        app.exec()
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()
        if motion is not None:
            try:
                gimbal.emergency_stop(m, motion.ctx["pair"])
            except Exception:
                pass
            motion.join(timeout=2.0)
        sampler.join(timeout=1.0)
        if m is not None:
            m.close()

    if args.save:
        print("数据已存：%s（复盘：python tools/plot.py %s）" % (args.save, args.save))
    return 0


if __name__ == "__main__":
    sys.exit(main())
