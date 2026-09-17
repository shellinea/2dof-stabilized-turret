#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
采样 CSV 画图
==============
读 zdt_can.py sample 产出的 CSV，画 位置 / 转速 / 相电流 / 位置误差 四张子图。
多个文件叠在一起，就是多组实验对比。

用法:
    python tools/plot.py sample.csv
    python tools/plot.py before.csv after.csv          # 叠在一起对比
    python tools/plot.py sample.csv --save fig.png     # 存图不开窗
    python tools/plot.py sample.csv --addr 1           # 只看 1 号

列名与单位约定（与 MCU 侧日志保持一致，方便两边曲线直接叠图）:
    t_s 秒 / *_deg 度 / bus_mv 毫伏 / phase_ma 毫安 / speed_rpm 转每分(带符号)
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import matplotlib
import matplotlib.pyplot as plt

# Windows 上让中文标签正常显示（没有微软雅黑就退回黑体/默认）
matplotlib.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei",
                                          "DejaVu Sans"]
matplotlib.rcParams["axes.unicode_minus"] = False

PANELS = [
    ("pos_deg", "实时位置 (°)"),
    ("speed_rpm", "实时转速 (RPM)"),
    ("phase_ma", "相电流 (mA)"),
    ("err_deg", "位置误差 (°)"),
]
LINESTYLES = ["-", "--", ":", "-."]


def read_csv(path):
    """返回 (元信息行列表, 字段名列表, 行列表)。跳过 '#' 注释行。"""
    path = Path(path)
    if not path.exists():
        raise SystemExit("找不到文件：%s" % path)
    meta, data = [], []
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith("#"):
            meta.append(ln[1:].strip())
        elif ln.strip():
            data.append(ln)
    if not data:
        raise SystemExit("%s 里没有数据行" % path)
    reader = csv.DictReader(data)
    return meta, reader.fieldnames, list(reader)


def as_floats(rows, key):
    out = []
    for r in rows:
        try:
            out.append(float(r[key]))
        except (KeyError, TypeError, ValueError):
            out.append(float("nan"))
    return out


def main():
    ap = argparse.ArgumentParser(description="采样 CSV 画图")
    ap.add_argument("csv", nargs="+", help="一个或多个采样 CSV")
    ap.add_argument("--addr", default=None, help="只看某个地址 (如 1)")
    ap.add_argument("--save", default=None, help="存成图片而不是开窗")
    ap.add_argument("--title", default=None, help="图标题")
    args = ap.parse_args()

    only = None if args.addr is None else int(args.addr)

    fig, axes = plt.subplots(len(PANELS), 1, figsize=(11, 9), sharex=True)
    if len(PANELS) == 1:
        axes = [axes]

    total = 0
    for fi, path in enumerate(args.csv):
        meta, fields, rows = read_csv(path)
        missing = [k for k, _ in PANELS if k not in (fields or [])]
        if missing:
            print("  跳过 %s：缺少列 %s" % (path, missing))
            continue
        addrs = sorted({int(float(r["addr"])) for r in rows})
        if only is not None:
            addrs = [a for a in addrs if a == only]
        for ai, a in enumerate(addrs):
            sub = [r for r in rows if int(float(r["addr"])) == a]
            t = as_floats(sub, "t_s")
            label = "%s / 地址%d%s" % (Path(path).stem, a,
                                       "" if len(args.csv) == 1 else "")
            for ax, (key, _) in zip(axes, PANELS):
                ax.plot(t, as_floats(sub, key), LINESTYLES[fi % len(LINESTYLES)],
                        linewidth=1.2, label=label)
            total += len(sub)
        if meta:
            print("  %s: %s" % (Path(path).name, meta[0]))

    if total == 0:
        raise SystemExit("没有可画的数据（检查 --addr 或 CSV 列名）")

    for ax, (key, ylabel) in zip(axes, PANELS):
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
    axes[0].legend(loc="upper right", fontsize=8, ncol=2)
    axes[-1].set_xlabel("时间 (s)")
    fig.suptitle(args.title or "ZDT X42S 采样波形")
    fig.tight_layout()

    if args.save:
        fig.savefig(args.save, dpi=130)
        print("已存图：%s" % args.save)
        plt.close(fig)
    else:
        print("共 %d 点，开窗显示..." % total)
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
