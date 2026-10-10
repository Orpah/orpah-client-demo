# -*- coding: utf-8 -*-
"""扫 `.fzz`（zip）里哪些条目含某段中文标注 —— 判「这句标注在工程里，还是只在导出的 `.svg` 上」。

用法::

    python tools\\fzz_annot_scan.py                     # 默认：hardware\\wiring\\*.fzz × 默认三段字
    python tools\\fzz_annot_scan.py --needle 供电
    python tools\\fzz_annot_scan.py --dir hardware\\wiring --svg   # 顺便看同名 .svg（对照）

为什么需要它
------------
本仓接线图上的**橙色/蓝色说明文字是贴在「导出的 `.svg`」上**的（见 `hardware/wiring/README.md`）：
`.fzz` 工程里**一个中文字都没有**。不知道这件事的人会「在 Fritzing 里找不到那句话」，
或者改完图重新导出后发现字没了/字回来了。

`--svg` 那一栏是**对照**：`.fzz` 说「工程里没有」、`.svg` 说「导出图上有」⇒ 这句字属导出图。
只读，不改任何文件；**退出码恒为 0**（它是量尺、不是闸门）——要看结论读输出。

出处：2026-10-06 判 `cstep-ch347f-atecc608b.svg` / `cstep-nanoch32-atecc608b.svg`
上那句「打印日志」到底在哪一层时写的（实跑：8 个 `.fzz` 三段字全无命中）。
"""
import argparse
import sys
import zipfile
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO = Path(__file__).resolve().parent.parent
DEFAULT_NEEDLES = ["打印日志", "供电", "I2C通讯"]


def scan_zip(path: Path, needles: list) -> dict:
    """→ {needle: [zip 内条目名, …]}（只收命中项）。"""
    hits = {n: [] for n in needles}
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            data = z.read(name)
            for n in needles:
                if n.encode("utf-8") in data:
                    hits[n].append(name)
    return {n: v for n, v in hits.items() if v}


def main() -> int:
    ap = argparse.ArgumentParser(description="扫 .fzz 里的中文标注（判它在工程里还是在导出图上）")
    ap.add_argument("--dir", default=str(REPO / "hardware" / "wiring"), help="待扫目录（默认 hardware/wiring）")
    ap.add_argument("--needle", action="append", default=None, help="要找的字符串（可给多次；默认三段字）")
    ap.add_argument("--svg", action="store_true", help="顺便扫同名 .svg（对照）")
    args = ap.parse_args()

    needles = args.needle or DEFAULT_NEEDLES
    folder = Path(args.dir)
    if not folder.is_dir():
        print(f"[X] 目录不存在: {folder}")
        return 2

    fzzs = sorted(folder.glob("*.fzz"))
    if not fzzs:
        print(f"[X] {folder} 下没有 .fzz")
        return 2

    print(f"扫 {len(fzzs)} 个 .fzz，找：{' / '.join(needles)}")
    total = 0
    for f in fzzs:
        hits = scan_zip(f, needles)
        total += sum(len(v) for v in hits.values())
        tag = ", ".join(f"{n}×{len(v)}" for n, v in hits.items()) or "（都没有）"
        print(f"  {f.name}: {tag}")
        for n, entries in hits.items():
            for e in entries:
                print(f"      {n} -> {e}")

    if args.svg:
        print("--- 同名 .svg 对照 ---")
        for f in sorted(folder.glob("*.svg")):
            hits = {n: True for n in needles if n in f.read_text(encoding="utf-8", errors="replace")}
            if hits:
                print(f"  {f.name}: {' / '.join(hits)}")

    print(f"合计命中 {total} 处（.fzz 内）。0 = 这些字都只在导出的 .svg 上。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
