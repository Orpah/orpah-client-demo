# -*- coding: utf-8 -*-
r"""PCB 布线**收尾**：把 `CON_RX` 出 `U1.connector17` 后那一下 0.08 mm 横移**拉回本脚中线** ✓

★ 必须接在 `carrier_pcb_route.py` 之后跑 ✓（它每次重写整份走线 ✗ ⇒ 会把这里的手改冲掉 ✗）。

病（2026-10-18 实测 ✓）：`U1` 是 QFN20、脚距 **0.40 mm**、盘 **0.20×0.60** ✓ ——
  顺着本脚中线出去时铜离隔壁盘是 `0.40/2 − 半宽` = **0.198 mm** ✓（够 0.15 ✓）；
  但栅格布线总会"出脚后先横移一格（0.08 mm）再走" ✗ ⇒ 变成 **0.118 mm** ✗（`pcb_check` ④b 点名 2 条 ✗）。
  调参都试过、都更差 ✗（`TURN_COST=4` ⇒ 0.038 ✗／6 mil ⇒ 0.064 ✗／出口走廊 ⇒ 8/15 ✗）
  ⇒ 按仓规 §0 第 3 条**停手**，改用**定点手改** ✓（就是把那 0.08 挪回去，人也会这么改 ✓）。

只动 3 处 ✓（不碰规则、不碰别的网 ✓）：
  ① 删掉那根 0.08 的竖直小台阶（`x` 不变、`Δy = +0.08` ✓）；
  ② 把 `y ≈ 10.08` 的线整体改成 `y ≈ 10.00`（含它远端那根竖线的起点 ✓）；
  ③ 把被删那根两侧的 `<connect>` **重新对指** ✓（否则就是"接在不存在的东西上" ✗）。

判据（独立跑，不信本脚本自己说 ✓）：`pcb_check.py carrier-pcb.fzz --nets=carrier_nets.py` ⇒ **rc=0** ✓。
用法： py -3.13 carrier_pcb_fixup.py
"""
import pathlib
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
FZZ = pathlib.Path(__file__).resolve().parent / "carrier-pcb.fzz"
SU = 90.0 / 25.4                 # 1 mm = 3.5433 sketch 单位（AGENTS §15）
ORG = 20.0                       # 板左上角（sketch 单位，与 carrier_pcb.py 一致）
Y_OLD = 10.08 * SU + ORG         # 那一下横移后的 y
Y_NEW = 10.00 * SU + ORG         # 本脚盘心的 y
TOL = 2e-3
STEP = (ORG + 27.2799 * SU, 10.00 * SU + ORG, 0.0, 0.08 * SU)   # x, y, dx, dy

with zipfile.ZipFile(FZZ) as z:
    name = [n for n in z.namelist() if n.endswith(".fz")][0]
    text = z.read(name).decode("utf-8")
    others = [(n, z.read(n)) for n in z.namelist() if n != name]

blocks = [m.group(0) for m in
          re.finditer(r'  <instance moduleIdRef="WireModuleID"[\s\S]*?\n  </instance>', text)]
info = []
for b in blocks:
    mi = int(re.search(r'modelIndex="(\d+)"', b).group(1))
    # ★ 取 **pcbView 里**那个 geometry ✓（第一个是面包板/原理图视图的零长占位 ✗，踩过 ✓）
    pv = re.search(r'<pcbView\b[^>]*>([\s\S]*?)</pcbView>', b)
    gm = re.search(r'<geometry\b([^>]*?)/>', pv.group(1)) if pv else None
    if gm is None:
        continue
    at = dict(re.findall(r'(\w+)="([^"]*)"', gm.group(1)))
    try:
        x, y = float(at["x"]), float(at["y"])
        dx, dy = float(at.get("x2", 0)), float(at.get("y2", 0))
    except (KeyError, ValueError):
        continue
    info.append((mi, x, y, dx, dy, b))

step = [t for t in info if abs(t[1] - STEP[0]) < TOL and abs(t[2] - STEP[1]) < TOL
        and abs(t[3]) < TOL and abs(t[4] - STEP[3]) < TOL]
shift = [t[0] for t in info if abs(t[2] - Y_OLD) < TOL]
if not step and not shift:
    print("✓ 没有需要改的（已经改过 ✓）")
    raise SystemExit(0)
if len(step) != 1:
    raise SystemExit("✗ 预期正好 1 根小台阶，实际 %d ⇒ 不写 ✗" % len(step))
mi_del = step[0][0]
peers = {}
for mi, _x, _y, _dx, _dy, b in info:
    for c in re.finditer(r'<connect connectorId="(connector\d)" modelIndex="%d"' % mi_del, b):
        peers.setdefault(mi, []).append(c.group(1))
others_mi = sorted(k for k in peers if k != mi_del)
if len(others_mi) != 2:
    raise SystemExit("✗ 被删那根应当正好 2 个邻居，实际 %s ⇒ 不写 ✗" % others_mi)
a, c = others_mi

new = text
for mi, _x, _y, _dx, _dy, b in info:
    if mi == mi_del:
        new = new.replace(b + "\n", "", 1)
    elif mi in shift:
        new = new.replace(b, b.replace('y="%.6g"' % Y_OLD, 'y="%.6g"' % Y_NEW, 1), 1)
new = new.replace('<connect connectorId="%s" modelIndex="%d"' % (peers[a][0], mi_del),
                  '<connect connectorId="%s" modelIndex="%d"' % (peers[c][0], c))
new = new.replace('<connect connectorId="%s" modelIndex="%d"' % (peers[c][0], mi_del),
                  '<connect connectorId="%s" modelIndex="%d"' % (peers[a][0], a))
ET.fromstring(new)
if 'modelIndex="%d"' % mi_del in new:
    raise SystemExit("✗ 还有地方指着被删的那根 ⇒ 不写 ✗")

tmp = FZZ.with_suffix(".tmp.fzz")
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zo:
    zo.writestr(name, new)
    for n, bb in others:
        zo.writestr(n, bb)
tmp.replace(FZZ)
print("✓ %s：挪回中线 %d 根、删小台阶 1 根、重新对指 2 条（%d 字节）"
      % (FZZ.name, len(shift), FZZ.stat().st_size))
