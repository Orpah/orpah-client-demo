# -*- coding: utf-8 -*-
"""mkdirs.py — 给固件 Makefile 建/删**构建目录**（`build/` + 它的三个子目录）。

为什么要用 Python 干这件事（用户 2026-10-12 定 ✓）：**建目录本来就不是 C/C++ 的任务** ✗。
本机那套 make + PowerShell 组合下，shell 写法都实测过 ✗：
  · `mkdir -p` ✗ —— 不是 Windows 程序，make 直接 CreateProcess 会报"找不到指定的文件"；
  · `cmd /c "if not exist X mkdir X"` ✗ —— 引号被吃掉：正常环境下**不建目录**（rc=2），
    PATH 不全的终端里还会**起交互式 cmd 卡住**（2026-10-12 实测，5 种写法 × 2 种环境对照）；
  · `cmd /c if not exist X mkdir X`（无引号）✓ 能过，但**路径含空格就废** ✗。
⇒ 交给 Python 最干净 ✓：**完全不传路径**（目录按**本脚本所在目录**算 ✓）⇒ 空格不再是问题 ✓，
  而且 Windows / Linux 一份代码 ✓。

用法（由 Makefile 调用，也可手工跑）：
    py -3 mkdirs.py make     # 建 build/{,Core,Periph,proto}（已存在则跳过 ✓）
    py -3 mkdirs.py clean    # 删掉整个 build/（不存在也算成功 ✓）

输出**只用 ASCII** ✓（make/cmd 的输出不经编码转换，中文在 cp936 控制台上会变乱码 ✗ —— 同
Makefile 里 `.bin` 那条 echo 的口径 ✓）。
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
SUBS = ("Core", "Periph", "proto")          # 除 build/ 本身之外的三个子目录 ✓


def do_make():
    made = []
    for d in (BUILD,) + tuple(os.path.join(BUILD, s) for s in SUBS):
        if not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
            made.append(os.path.relpath(d, HERE))
    print("  mkdirs: %s" % ("created " + ", ".join(made) if made else "already there"))
    return 0


def do_clean():
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD, ignore_errors=True)
        print("  clean : removed build/")
    else:
        print("  clean : build/ not there")
    return 0


def main():
    action = (sys.argv[1] if len(sys.argv) > 1 else "make").lower()
    if action == "make":
        return do_make()
    if action == "clean":
        return do_clean()
    sys.stderr.write("usage: mkdirs.py [make|clean]\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
