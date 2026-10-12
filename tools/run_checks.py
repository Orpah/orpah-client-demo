# -*- coding: utf-8 -*-
"""run_checks.py — **一键回归**：把本仓「不需要台架」的闸门全跑一遍。

为什么要有它（AGENTS §4 说的那件事）：本仓现在有 5~6 道闸门，但它们散着 ——
改完东西靠"记得跑哪几个"是**会漏的**（隔壁仓就是这么漏出来的：两份 `run_checks` 各漏一半）。
⇒ 这里把无台架的那几道串成一条命令，判定口径与隔壁仓一致：
    **退出码 0 且输出里没有 `FAIL` / `Traceback`**（有些脚本自己吞异常还会往下跑 ⇒ 只看退出码会漏）。

跑什么（全部**离线**，不需要板子）：
  · `proto/run_cross_test.py`      —— C ↔ Python 交叉测试（协议内核 + ATECC 报文层，16 组）
  · `tools/fzz_nets.py`            —— 9 张接线图的**网表 vs 规格表**（`EXPECT` 是单一源）
  · `tools/fzz_annot_scan.py --svg`—— 标注扫描（**量尺不是闸门**，退出码恒 0 ⇒ 只记 info）
  · `make -C firmware`             —— 固件编译（要求 **零 warning**；缺 `sh` 就**明确跳过** ✗ 不静默当通过）
  · `python -m compileall`         —— `tools/`、`proto/` 与 `firmware/`（后者的 `mkdirs.py` 是构建依赖 ✓）的 Python 语法
  · 报告写到仓库根 `checks_report.md`（入库，同隔壁仓惯例 ⇒ "上次是不是全绿"一眼可查）

需要台架的**不在**默认里（要跑就加 `--bench`，那时请先确认台架接线与 `hardware/wiring/README.md` 一致）：
  · `tools/check_c4g2_bench.py`    —— SE：唤醒/Random/**签名**/`nonce_src`/`sign_src`/上游收帧

用法：
    python tools\\run_checks.py                 # 全离线闸门 + 写报告
    python tools\\run_checks.py --bench         # 连台架判据一起跑（要接板子）
    python tools\\run_checks.py --no-report     # 只打印、不写 checks_report.md
    python tools\\run_checks.py --quick         # 跳过固件编译（很快）

⚠ 本文件**不重写任何判据** —— 它只负责"调起来 + 按统一口径判"；判据还在各自那个脚本里（单一源）。
"""
import argparse
import datetime
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PY = sys.executable or "python"

# ★★ 2026-10-12（CI 三系统矩阵第一次跑出来的教训 ✓）：判“红”的同时**必须把“为什么红”留在日志里** ✗
#   起因：Windows / macOS 那两栏只报「FAIL 1 项未过：['编译 ID 流水线…']」✗ —— 真正的编译报错行
#   被原来的“尾部 8 行”裁掉了 ✗ ⇒ 手上有完整日志也**说不清为什么红** ✗（只能再跑一次去猜 ✗）。
#   所以：失败时额外把**报错行**（关键词命中）也打出来 ✓ —— 判据会喊“红了”，也得会说“因为什么”。
ERR_PAT = re.compile(r"(error|Error|ERROR|FAIL|Traceback|not found|undefined reference|"
                     r"cannot find|unrecognized|无法|找不到)")


def show_slices(out, n_err=20, n_tail=25):
    """把闸门输出切成“值得留存/打印的那些行”：报错行 + 尾部。

    返回 list[str]。**打红的时候用它**；绿的时候只需要短短一眼（尾部 8 行）。
    """
    lines = out.splitlines()
    errs = [l for l in lines if ERR_PAT.search(l)]
    sel = []
    if errs:
        sel.append("(报错行，最多 %d 条)" % n_err)
        sel += errs[:n_err]
    sel.append("(尾部 %d 行)" % n_tail)
    sel += lines[-n_tail:]
    return sel

# 缺 sh 时的候选（隔壁仓的教训：`shutil.which("sh")` 会找到 WSL 的 bash.exe，它看不到 F:\ ✗）
SH_CANDS = [r"D:\Program Files\Git\bin\sh.exe",
            r"C:\Program Files\Git\bin\sh.exe",
            r"D:\Program Files\Git\usr\bin\sh.exe"]


def find_sh():
    # ★★ 2026-10-12：**POSIX（Linux / CI）上直接用 `sh`** ✓ —— 原来只认 Windows 上那几个 Git
    #   的 `sh.exe` ✗ ⇒ 在 Linux 上会返回 None ⇒ 固件那栏被"明确跳过" ✗（CI 里就少一条闸门 ✗）。
    #   ⚠ 但**不**在 Windows 上用 `shutil.which("sh")`：隔壁仓的教训 —— 它会找到 WSL 的 bash.exe，
    #     而那个 sh 看不到 `F:\` ✗（所以 Windows 仍只认下面这几个绝对路径 ✓）。
    if os.name != "nt":
        for c in ("/bin/sh", "/usr/bin/sh"):
            if os.path.isfile(c):
                return c
        return None
    for c in SH_CANDS:
        if os.path.isfile(c):
            return c
    return None


def run(label, argv, cwd=None, timeout=1800):
    """跑一条闸门。返回 dict：rc / out / ok（口径 = rc==0 且无 FAIL/Traceback）。"""
    print("-" * 78)
    print("== %s" % label)
    print("   $ %s" % " ".join(argv))
    sys.stdout.flush()
    try:
        p = subprocess.run(argv, cwd=cwd or REPO, capture_output=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        out = (p.stdout or "") + (p.stderr or "")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        out, rc = "（超时 %ds）" % timeout, 124
    except OSError as e:                                        # noqa: BLE001
        out, rc = "（起不来：%r）" % (e,), 127
    bad = ("FAIL" in out) or ("Traceback" in out)
    ok = (rc == 0) and not bad
    print("   -> rc=%d%s" % (rc, "" if ok else "   ✗（%s）" % ("rc≠0" if rc else "输出里有 FAIL/Traceback")))
    # ★ 绿：几行就够（别把日志灌满 ✗）；红：**把报错行也打出来** ✓（见 ERR_PAT 那段注释）
    for ln in (out.splitlines()[-8:] if ok else show_slices(out)):
        print("      | " + ln)
    sys.stdout.flush()
    return {"label": label, "argv": argv, "rc": rc, "ok": ok, "out": out}


def main():
    ap = argparse.ArgumentParser(description="一键回归（离线闸门；--bench 才碰台架）")
    ap.add_argument("--bench", action="store_true", help="连台架判据（check_c4g2_bench.py）一起跑")
    ap.add_argument("--quick", action="store_true", help="跳过固件编译")
    ap.add_argument("--no-report", action="store_true", help="不写 checks_report.md")
    ap.add_argument("--port", default="COM23", help="台架串口（--bench 时用）")
    args = ap.parse_args()

    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                       # noqa: BLE001
            pass

    res = []
    skipped = []

    # ① 协议内核：C ↔ Python 交叉测试（最重要的一道）
    res.append(run("协议内核交叉测试（C ↔ Python）",
                   [PY, os.path.join(REPO, "proto", "run_cross_test.py")]))

    # ② 接线图网表（规格表 = 单一源）
    res.append(run("接线图网表 vs 规格表（9 张）",
                   [PY, os.path.join(HERE, "fzz_nets.py")]))

    # ③ 标注扫描（量尺：退出码恒 0；只把它当 info）
    inf = run("标注扫描（量尺，不作闸门）", [PY, os.path.join(HERE, "fzz_annot_scan.py"), "--svg"])
    inf["ok"] = True                                            # 量尺 ✓ 不判红
    res.append(inf)

    # ④ Python 语法（tools/ / proto/ / firmware ←2026-10-12 加上：firmware/mkdirs.py 是构建
    #    依赖的脚本，它坏了会让"干净树上 make"静静挂在建目录那一步 ✗（构建只在 build/ 不存在时
    #    才调它 ⇒ 普通跑不一定会执行到 ⇒ 光靠 make 盖不住它 ✗）
    res.append(run("Python 语法（tools/ + proto/ + firmware/）",
                   [PY, "-m", "compileall", "-q", HERE, os.path.join(REPO, "proto"),
                    os.path.join(REPO, "firmware")]))

    # ⑤ 固件编译（零 warning；缺 sh 就明确跳过）
    if args.quick:
        skipped.append("固件编译（--quick）")
    else:
        sh = find_sh()
        if sh is None:
            skipped.append("固件编译（本机找不到 Git 的 sh.exe —— **明确跳过**，别当通过 ✗）")
        else:
            # ★★ 2026-10-12：改成 **`make rebuild`（从零）** ✓ —— 增量 `make` 时这条只保证
            #   “make 认为该编的那些”零 warning ✗（而“改 .h 不重编”就是这么漏过去的 ✓）；
            #   从零后 “零 warning” = **整棵树都编过** ✓，且不靠增量依赖写得全不全 ✓。
            #   代价约 3 s（本机 / 26 个编译单元 ✓）。
            # ★ 另外：环境里有 `RISCV_PREFIX` 就透给 make ✓ —— CI（Linux）要用**通用**工具链
            #   跑这条闸门 ✓（本机（Windows）不设它 ⇒ 用 Makefile 默认的 MounRiver 路径 ✓）。
            fw = ["make", "-C", os.path.join(REPO, "firmware"), "rebuild", "SHELL=" + sh]
            pre = os.environ.get("RISCV_PREFIX")
            if pre:
                fw.append("RISCV_PREFIX=" + pre)
            # ★★ 2026-10-12（**首次 CI 实测**）：通用工具链与 MounRiver 有两处口径不同，
            #   都用环境变量**显式**传进来（**不当默认值** ✗ —— 默认必须留给**烧录那条路** ✓）：
            #     `ISA=rv32imac_zicsr`：GCC 12+ 不再让 `rv32imac` 隐含 zicsr（汇编 `csrw` 会挂 ✗）；
            #     `INTFAST=0`：WCH 的 `interrupt("WCH-Interrupt-fast")` 通用工具链不认 ⇒ 逐处 warning ✗
            #   ⇒ 这里只**透传**（谁调谁负责 ✓），并把非默认值**打进标签** —— 免得把
            #     “可移植配置编过了” 误读成 “烧录固件编过了” ✗。
            extra = []
            for k in ("ISA", "INTFAST"):
                v = os.environ.get(k)
                if v:
                    fw.append("%s=%s" % (k, v))
                    extra.append("%s=%s" % (k, v))
            res.append(run("固件从零编译（%s%s，要求零 warning）"
                           % (os.path.basename(pre) if pre else "riscv-none-embed-gcc",
                              ("；" + " ".join(extra)) if extra else ""), fw))

    # ⑥ 台架判据（默认不跑）
    if args.bench:
        res.append(run("台架判据：SE 唤醒/Random/签名/nonce_src/sign_src/上游收帧",
                       [PY, os.path.join(HERE, "check_c4g2_bench.py"), "--port", args.port,
                        # ★★ 2026-10-12：**默认就用密钥库里的钥验** ✓ —— 原先它默认用
                        #   `check_c4g2_bench.py` 里**硬编码的 Slot 0 公钥常量** ✗，而那常量
                        #   只属于**被量过的那一颗** ⇒ 换一颗（或跑 HS256）必然
                        #   `signature_invalid`：**那是尺子错、不是签名错** ✗
                        #   （2026-10-12 跑这条闸门时真的抓到了 ✓，见 docs §9.0 第 5 条）。
                        "--from-keystore"]))
    else:
        skipped.append("台架判据（check_c4g2_bench.py，要接板子 ⇒ 用 --bench）")

    # ---- 汇总 ----
    bad = [r for r in res if not r["ok"]]
    print("=" * 78)
    for r in res:
        print("  %-6s %s" % ("PASS" if r["ok"] else "FAIL", r["label"]))
    for s in skipped:
        print("  skip   %s" % s)
    print("=" * 78)
    print("%s：%d 道闸门，%d 道未过%s"
          % ("PASS" if not bad else "FAIL", len(res), len(bad),
             "" if not bad else " -> " + "；".join(r["label"] for r in bad)))

    if not args.no_report:
        path = os.path.join(REPO, "checks_report.md")
        lines = ["# checks_report.md —— `tools/run_checks.py` 的输出（**入库**，同隔壁仓惯例）", "",
                 "跑的时间：%s" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                 "",
                 "判定口径：**退出码 0 且输出里没有 `FAIL` / `Traceback`**（只看退出码会漏 —— 有些脚本自己吞异常）。",
                 "",
                 "| 结果 | 闸门 | 命令 | rc |", "|---|---|---|---|"]
        for r in res:
            lines.append("| %s | %s | `%s` | %d |"
                         % ("✅ PASS" if r["ok"] else "❌ FAIL", r["label"],
                            " ".join(os.path.basename(a) if a.startswith(REPO) else a
                                     for a in r["argv"]), r["rc"]))
        for s in skipped:
            lines.append("| ⏭ skip | %s | — | — |" % s)
        lines += ["", "## 各闸门输出的尾部（便于「上次为什么红」一眼可查）", ""]
        for r in res:
            body = (r["out"].splitlines()[-25:] if r["ok"] else show_slices(r["out"]))
            lines += ["### %s（rc=%d，%s）" % (r["label"], r["rc"], "PASS" if r["ok"] else "FAIL"),
                      "", "```", "\n".join(body), "```", ""]
        lines += ["## 需要台架的（不在默认里）", "",
                  "`--bench` 会另外跑 `tools/check_c4g2_bench.py`（SE 唤醒/Random 真随机数/"
                  "**Slot 0 签名 + 上位机用 Slot 0 公钥独立验签**/`nonce_src`/`sign_src`/"
                  "上游 `verify_report` 收帧）。跑之前先确认台架与 `hardware/wiring/README.md` 一致。", ""]
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
        print("报告已写入：%s" % path)

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
