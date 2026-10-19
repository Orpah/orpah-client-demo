# -*- coding: utf-8 -*-
"""`pcb_route.py` 的包装：**允许在 `fine`/`epad` 盘上打同网过孔** ✓（`VIA_AT_PAD = True`）

为什么需要 ✗（2026-10-18 实测 ✓）：`U1`（QFN20，0.4 mm 脚距）的**中央散热盘**
`connector20`（原厂名 `VSS`）被自己那圈脚**围死** ✓ —— 单网跑 GND **0/1** ✗
（四墙都是邻脚净空，出口只有 0.076 mm ✗ < 一格 0.15 ✗）。行业做法 = 盘上打同网热过孔
下到另一层 ✓（库仓 AGENTS §5 明写允许 ✓）。`pcb_route.py` 只对 `epad`/`fine` 盘放行 ✓，
而这只盘**名字叫 `VSS`** ✗ ⇒ 由 `pcb_check.epad_by_geometry()` 按**几何**认它 ✓。

用法： py -3.13 carrier_route_atpad.py <与 pcb_route.py 相同的参数…>
"""
import pathlib
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
LIB = pathlib.Path(r"f:\git\fritzing-parts-langhua\tools")
sys.path.insert(0, str(LIB))
import pcb_route as PR                                            # noqa: E402

PR.VIA_AT_PAD = True
raise SystemExit(PR.main(sys.argv[1:]))
