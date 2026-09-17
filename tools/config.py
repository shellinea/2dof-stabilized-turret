#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
云台标定参数（JSON）
====================
把 mirror / 运动学符号 / 零点 / 限位 / 运动默认值落盘，gimbal.py 自动读取，
CLI 参数仍可临时覆盖。将来搬 MCU 时，这份 JSON 就是参数表的原型。

用法:
    python tools/config.py            # 打印当前配置（缺省 + 文件覆盖）
    python tools/config.py --init     # 写出缺省配置到 gimbal_config.json
    python tools/config.py --path     # 只打印配置文件路径
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

CONFIG_PATH = Path(__file__).resolve().with_name("gimbal_config.json")

# 缺省值 = 2026-09-16 实机标定结果（2 号电机镜像安装）
DEFAULTS = {
    "motors": {
        "1": {"mirror": False},
        "2": {"mirror": True},
    },
    "kinematics": {
        "pan_sign": 1,
        "tilt_sign": 1,
        "zero_pan": 0.0,
        "zero_tilt": 0.0,
    },
    "limits": {
        "tilt_min": -45.0,
        "tilt_max": 45.0,
        "pan_min": -180.0,
        "pan_max": 180.0,
    },
    "motion": {
        "rpm": 45,
        "acc": 80,
        "tilt": 30.0,
        "cycles": 3,
        "spin": 170.0,
        "dwell": 0.0,
    },
    "pulses_per_rev": 3200,
}


def load(path=None) -> dict:
    """读配置：缺省值打底，文件里的键覆盖之（按 section 递归合并）。"""
    cfg = json.loads(json.dumps(DEFAULTS))
    p = Path(path) if path else CONFIG_PATH
    if p.exists():
        try:
            user = json.loads(p.read_text(encoding="utf-8"))
        except Exception as e:
            raise SystemExit("配置文件 %s 解析失败：%s" % (p, e))
        if not isinstance(user, dict):
            raise SystemExit("配置文件 %s 顶层必须是 JSON 对象" % p)
        _merge(cfg, user)
    return cfg


def _merge(base: dict, over: dict):
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v


def save(cfg: dict, path=None) -> Path:
    p = Path(path) if path else CONFIG_PATH
    p.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n",
                 encoding="utf-8")
    return p


def mirror_set(cfg) -> set:
    """返回需要取反角度的电机地址集合（镜像安装的那台）。"""
    return {int(a) for a, m in cfg["motors"].items()
            if isinstance(m, dict) and m.get("mirror")}


def main():
    ap = argparse.ArgumentParser(description="云台标定参数")
    ap.add_argument("--config", default=None, help="配置文件路径")
    ap.add_argument("--init", action="store_true", help="写出缺省配置")
    ap.add_argument("--path", action="store_true", help="只打印配置文件路径")
    args = ap.parse_args()

    p = Path(args.config) if args.config else CONFIG_PATH
    if args.path:
        print(p)
        return 0
    if args.init:
        if p.exists():
            print("已存在，未覆盖：%s" % p)
            return 0
        save(DEFAULTS, p)
        print("已写出缺省配置：%s" % p)
        return 0

    cfg = load(p)
    print("配置文件：%s  %s" % (p, "(已存在)" if p.exists() else "(不存在，用的是缺省值)"))
    print(json.dumps(cfg, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
