#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 README 配图
==================
把实测数据画成图存进 docs/assets/。数据是硬编码的实测值（来源见下），
不依赖硬件，随时可以重跑。

用法:
    python measurements/make_figures.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

matplotlib.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei",
                                          "DejaVu Sans"]
matplotlib.rcParams["axes.unicode_minus"] = False

OUT = Path(__file__).resolve().parent.parent / "docs" / "assets"

# 2026-09-17 实测，zdt_can.py latency --op all --n 50（地址 1）
# 修 drain() 之前 / 之后，单位 ms（mean）
CMDS = ["43 7A\n读系统状态\n(31B/5帧)", "36\n读位置", "35\n读转速",
        "27\n读相电流", "37\n读位置误差", "3A\n读状态标志"]
BEFORE = [15.0, 15.0, 15.2, 15.2, 15.1, 15.1]
AFTER = [1.91, 0.75, 0.70, 0.71, 0.76, 0.69]


def fig_latency():
    fig, ax = plt.subplots(figsize=(11, 5.2))
    x = np.arange(len(CMDS))
    w = 0.38

    b1 = ax.bar(x - w / 2, BEFORE, w, label="修之前（每次都调 drain）",
                color="#c94f4f", edgecolor="white", linewidth=0.8)
    b2 = ax.bar(x + w / 2, AFTER, w, label="修之后（只在缓冲脏时清）",
                color="#3f8f6f", edgecolor="white", linewidth=0.8)

    for bars in (b1, b2):
        for r in bars:
            ax.annotate("%.2f" % r.get_height(),
                        (r.get_x() + r.get_width() / 2, r.get_height()),
                        ha="center", va="bottom", fontsize=9.5)

    ax.set_yscale("log")
    ax.set_ylim(0.4, 40)
    ax.set_ylabel("单条读往返耗时 (ms，对数轴)")
    ax.set_title("CAN 往返延迟：一个 drain() 白吃 15ms，与应答长度无关\n"
                 "ZDT X42S @ 500 kbps，地址 1，各 50 次",
                 fontsize=13, pad=14)
    ax.set_xticks(x)
    ax.set_xticklabels(CMDS, fontsize=9)
    ax.grid(True, axis="y", alpha=0.3, which="both")
    ax.set_axisbelow(True)
    ax.legend(fontsize=10)

    ax.annotate("", xy=(5.45, 0.75), xytext=(5.45, 15.0),
                arrowprops=dict(arrowstyle="<->", color="#444", lw=1.4))
    ax.text(5.55, 3.4, "20 倍", fontsize=13, color="#444",
            ha="left", va="center", rotation=90)

    fig.tight_layout()
    fig.savefig(OUT / "latency.png", dpi=150)
    plt.close(fig)
    print("  -> docs/assets/latency.png")


def fig_waveform():
    """直接把 sample.csv 画成一张精简版（只画位置和转速，README 用）。"""
    import csv
    src = Path(__file__).resolve().parent / "sample.csv"
    if not src.exists():
        print("  跳过 waveform.png：找不到 %s" % src)
        return
    rows = []
    with src.open(encoding="utf-8") as f:
        data = [ln for ln in f if not ln.startswith("#") and ln.strip()]
    for r in csv.DictReader(data):
        try:
            rows.append((int(float(r["addr"])), float(r["t_s"]),
                         float(r["pos_deg"]), float(r["speed_rpm"])))
        except (KeyError, ValueError):
            pass
    if not rows:
        print("  跳过 waveform.png：sample.csv 无有效行")
        return

    fig, (a1, a2) = plt.subplots(2, 1, figsize=(11, 5.6), sharex=True)
    for addr, color in ((1, "#2f6fbf"), (2, "#d07a2a")):
        sub = [r for r in rows if r[0] == addr]
        if not sub:
            continue
        t = [r[1] for r in sub]
        a1.plot(t, [r[2] for r in sub], color=color, lw=1.2,
                label="地址 %d" % addr)
        a2.plot(t, [r[3] for r in sub], color=color, lw=1.2,
                label="地址 %d" % addr)

    a1.set_ylabel("实时位置 (°)")
    a1.set_title("静止基线（两台都未使能动作）——位置漂移 ±0.03°，转速恒 0",
                 fontsize=12)
    a2.set_ylabel("实时转速 (RPM)")
    a2.set_xlabel("时间 (s)")
    for ax in (a1, a2):
        ax.grid(True, alpha=0.3)
    a1.legend(loc="upper right", fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT / "waveform.png", dpi=150)
    plt.close(fig)
    print("  -> docs/assets/waveform.png")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    print("生成 README 配图:")
    fig_latency()
    fig_waveform()
    return 0


if __name__ == "__main__":
    sys.exit(main())
