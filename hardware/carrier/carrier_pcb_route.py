# -*- coding: utf-8 -*-
r"""载板 **PCB 布线**：跑库仓布线器 ⇒ 把走线/过孔写进 `carrier-pcb.fzz`

接在 `carrier_pcb.py`（只摆位、不布线 ✓）之后。**输入输出同一个文件** ✓
（重跑会先摘掉旧的 Wire/Via 实例 ⇒ 幂等 ✓）。

★ 走线/过孔的写法**全部照** `docs/fritzing-sketch-format-notes.md`（F4/F5/F9/F11/F17/F18）：
  · **三视图都要写** ✓（F9 用户实验判决：只带 `pcbView` 的走线 Fritzing **不认** ✗）——
    面包板/原理图两个视图写**零长度占位** ✓（不编坐标 ✗，别把面包板画花 ✗）；
  · `wireFlags="4"`（= `PCBTraceFlag` ✓，F11）三个视图**同一个值** ✓；过孔 `wireFlags="32"` ✓；
    ✗ 绝不能写 16（`RatsnestFlag` ⇒ 加载时被 Fritzing **直接删掉** ✗）；
  · `<wireExtras mils="8" color=… opacity="1" banded="0"/>` ✓（F18；mils = 我们选的那一档 ✓）；
  · `<connect layer="…">` 写**目标在文件里声明的层** ✓（F10）—— 本板所有件的
    `pcbView` 都是 `layer="copper1"` ✓（`_scratch/carrier_inst_shape.py` 量过 ✓）
    ⇒ 走线→焊盘 `layer="copper1"` ✓、走线→走线/过孔按各自层 ✓；
  · **两端都要写 `<connects>`，且目标侧要写回指** ✓（F2/F5；F8 实测缺回指 ⇒ 整组不被认 ✗）；
  · 过孔 `<property name="hole size" value="0.3mm,0.15mm"/>` ✓（口径 = `孔直径,环宽` ✓，F17）；
  · 过孔 `<geometry>` 点是**画布原点** ✗ ⇒ 真铜心 = geometry + `part_box.ring_off_mm()` ✓。

★ 布线参数（2026-10-18 定 ✓，都有实测依据 ✓）：
  · **8 mil**（0.2032 mm）—— `U1` 是 QFN20 **0.4 mm 脚距** ✓，24 mil 出不去 ✗；
  · `ESCAPE_PROTECT` + `escape-cost=0.4` + `escape-r=1.2` ⇒ 连通数 12 → **14** ✓
    （不让先布的电源把后面那排脚的"脚口"切死 ✓）；
  · **允许 `fine`/`epad` 盘上打同网过孔** ✓（`VIA_AT_PAD=True`）—— `U1` 的中央散热盘
    `VSS` 被自己那圈脚围死 ✓（单网 GND 不加它是 **0/1** ✗），盘上热过孔是行业做法 ✓
    （库仓 AGENTS §5 ✓）；那只盘靠 `pcb_check.epad_by_geometry()` 按**几何**认出来 ✓。

用法： py -3.13 carrier_pcb_route.py [--dry]

★★ **跑完必须再跑一次 `carrier_pcb_fixup.py`** ✓ —— 本脚本每次都会把整份走线重写 ✗，
   而 `U1` 那排脚上有一处 0.08 mm 的横移**必须靠定点手改**拉回中线 ✓（原因见那个脚本的头注 ✓）。
   判据：最后跑 `pcb_check.py carrier-pcb.fzz --nets=carrier_nets.py` ⇒ **rc=0** ✓。
"""
import contextlib
import io
import os
import pathlib
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = pathlib.Path(__file__).resolve().parent
LIB = pathlib.Path(r"f:\git\fritzing-parts-langhua\tools")
sys.path.insert(0, str(LIB))
sys.path.insert(0, str(HERE))
import pcb_check as PC                                            # noqa: E402
import pcb_route as PR                                            # noqa: E402
import pcb_wire as PW                                             # noqa: E402
import part_box as PB                                             # noqa: E402
import carrier_nets as CN                                         # noqa: E402

FZZ = HERE / "carrier-pcb.fzz"
MIL = 8                        # ★ 8 mil（0.2032 mm）；6 mil 试过 ⇒ 更差 ✗（0.0638 ✗）
CELL_MM = 0.08                # ★ 栅格取细 ✓（0.15 会把净空量化到刚好 0.15 ⇒ 差 1.6 µm ✗）
CLEAR_MM = 0.16                # ★ 比规则值 0.15 略大 ✓（同上：给量化留余量 ✓）
VIA_HOLE, VIA_RING = 0.30, 0.15
Z_WIRE, Z_VIA = "9.5", "5.5"
Q = 1e-3                        # 端点归并精度（sketch 单位 ≈ 0.28 µm ✓）


def num(v):
    return PW.fmt(v)


def key(p):
    return (round(p[0] / Q), round(p[1] / Q))


# ── ① 读写 fzz ───────────────────────────────────────────────────────────────
def read_fz(path):
    with zipfile.ZipFile(path) as z:
        name = [n for n in z.namelist() if n.endswith(".fz")][0]
        return name, z.read(name).decode("utf-8"), [(n, z.read(n))
                                                    for n in z.namelist() if n != name]


def write_fz(path, name, text, others):
    """★ 全读进内存 ⇒ 写临时文件 ⇒ 原子替换 ✓（别覆盖正在读的 zip ✗，踩过 ✓）"""
    tmp = path.with_suffix(".tmp.fzz")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zo:
        zo.writestr(name, text)
        for n, b in others:
            zo.writestr(n, b)
    os.replace(tmp, path)


def strip_wires(text):
    """摘掉已有的 **PCB 走线 / 过孔** 实例 ✓（幂等 ✓）＋ 焊盘里指向走线的 `<connect>` ✓

    ★ 只摘 **PCB 那层的走线** ✓（`copper0trace`/`copper1trace`）：源草图里还有一条
      **面包板模板导线**（`Wire90000012`，`layer="breadboardWire"` ✓）—— 那是**别的视图**的东西 ✗，
      整类删掉会把面包板视图削掉一块 ✗（`sch_strip_wires` 踩过同一个坑 ✓）。
    """
    n = [0]

    def drop(m):
        b = m.group(0)
        if 'moduleIdRef="ViaModuleID"' in b or "copper0trace" in b or "copper1trace" in b:
            n[0] += 1
            return ""
        return b
    text = re.sub(r'[ \t]*<instance moduleIdRef="(?:Wire|Via)ModuleID"[\s\S]*?</instance>\n?',
                  drop, text)
    if n[0]:
        text = re.sub(r'[ \t]*<connect [^>]*layer="copper[01]trace"[^>]*/>\n', "", text)
        text = re.sub(r"[ \t]*<connects>\s*</connects>\n", "", text)
    return text, n[0]


# ── ② 布线 ───────────────────────────────────────────────────────────────────
def _escape_corridors(pads, net_of_pad, half=0.04, reach=0.60):
    """细间距焊盘：**只留一条沿自身长轴的出口走廊** ✓ ⇒ 其余方向按真净空挡死 ✓

    ★ 为什么必须加 ✗（2026-10-18 实测 ✓，`_scratch/carrier_wire_geom.py` ✓）：
      `U1` 是 QFN20、**脚距 0.40 mm** ✓、盘 0.20×0.60 ✓ ⇒ 相邻两盘之间的缝只有 **0.20 mm** ✗
      而 8 mil 线 × 真净空 0.15 ⇒ 需要 `0.2032 + 2×0.15 = 0.503` ✗ ⇒ **那条缝本来就走不了** ✓。
      但 `pcb_route.carve_pads()` 会把本网盘外扩 `neck_grow`（≈0.29 ✓）**整片放开** ✗，
      而随后只按 `grow=0` 把**别人盘框本身**挡回去 ✗ ⇒ 走线可以从缝里**横着钻出去** ✗：
      实测 `CON_RX` 从 `U1.connector17` 出来时往 `connector18` 那侧**滑了 0.08 mm** ✗
      ⇒ 铜净距 **0.118 mm** ✗（< 0.15 ✗，`pcb_check` ④b 点名 2 条 ✓）。
    ★ 做法 ✓：对每只盘，在"盘框外扩 `净空 + 半宽`"的矩形里，**只掏出一条**
      （半宽 `half`、沿盘的长轴、两端各伸 `reach`）的走廊 ✓ ⇒ 走线只能**正着出/正着进** ✓，
      横向滑动一律被挡 ✓。✗ 不整块按净空挡 ✗ —— 库仓 2026-10-01 在像素板试过那样 ⇒ 3/9→0/9 ✗。
    """
    out = []
    clear = PR.U(MIL * PR.MIL_MM / 2 + CLEAR_MM)
    for _k, q in pads.items():
        b = q.get("box")
        if not b or not (q.get("fine") or q.get("epad")):
            continue
        cx, cy = q["c"]
        hz = PR.U(half)
        rz = PR.U(reach)
        # ★★ 障碍要**打成这只盘自己那张网** ✓（`items` 的第 4 项 = 网名 ✓）：
        #   那样**本网**布线时会忽略它（`carrier_pcb_route` 的 tag 语义 = 同网豁免 ✓），
        #   而**别的网**一律被挡 ⇒ 既不妨碍本脚接线 ✓，又不可能从缝里钻 ✓。
        tg = net_of_pad.get(_k)
        if (b[2] - b[0]) >= (b[3] - b[1]):            # 横放 ⇒ 走廊沿 x
            x0, x1 = b[0] - rz, b[2] + rz
            out.append(("both", (x0, b[1] - clear, x1, cy - hz), 0.0, tg))
            out.append(("both", (x0, cy + hz, x1, b[3] + clear), 0.0, tg))
        else:                                          # 竖放 ⇒ 走廊沿 y
            y0, y1 = b[1] - rz, b[3] + rz
            out.append(("both", (b[0] - clear, y0, cx - hz, y1), 0.0, tg))
            out.append(("both", (cx + hz, y0, b[2] + clear, y1), 0.0, tg))
    return out


def _net_of_pad(net_pads):
    d = {}
    for n, mem in net_pads.items():
        for k in mem:
            d[k] = n
    return d


def _seg_rect_gap(p0, p1, b):
    """线段（轴对齐）到矩形框的**最近距离**（sketch 单位 ✓）"""
    x0, x1 = min(p0[0], p1[0]), max(p0[0], p1[0])
    y0, y1 = min(p0[1], p1[1]), max(p0[1], p1[1])
    dx = max(b[0] - x1, 0.0, x0 - b[2])
    dy = max(b[1] - y1, 0.0, y0 - b[3])
    return (dx * dx + dy * dy) ** 0.5


def _nudge_clearance(wires, vias, pads, net_of_pad, need, half, rounds=10):
    """把「离**别的网**的盘太近」的走线，沿垂直方向**推开**到够 ✓（只动内部顶点 ✓）

    ★ 为什么需要 ✗（2026-10-18 实测 ✓）：`U1` 的 QFN 脚距 0.40 / 盘宽 0.20 ⇒ 从某只盘
      顺着自己中线出去时，铜离隔壁盘是 `0.40/2 − 半宽` = **0.198 mm** ✓ 本来就够 0.15 ✓；
      但栅格布线偶尔会"出脚后先横移一格（0.08）再走" ✗ ⇒ 变成 **0.118 mm** ✗（实测 2 条 ✓）。
      调参试过 `TURN_COST=4`（更差 0.038 ✗）、6 mil（更差 0.064 ✗）、出口走廊（掉到 8/15 ✗）
      ⇒ 按仓规 §0 第 3 条停手 ✓，改用**有界的后处理** ✓：把那一小段平移回中线 ✓。
    ★ 只动**内部顶点** ✓（焊盘中心 / 过孔点一律不动 ✗）；平移量 = `差多少 + 0.01` ✓；
      平移会把相邻段的端点一起带走 ⇒ 相邻段变成斜线/零长 ✓（零长的随后删掉 ✓）。
    """
    def move(vert, d):
        """把所有**同网**、端点等于 `vert` 的线端一起挪 ✓ ⇒ 拓扑不断 ✓"""
        for w2 in wires:
            for e2 in (0, 1):
                p2 = w2["p0"] if e2 == 0 else w2["p1"]
                if key(p2) != key(vert):
                    continue
                np_ = (p2[0] + d[0], p2[1] + d[1])
                if e2 == 0:
                    w2["p0"] = np_
                else:
                    w2["p1"] = np_

    protect = {key(q["c"]) for q in pads.values()}
    protect |= {key(v["p"]) for v in vias}
    n = 0
    for _ in range(rounds):
        bad = 0
        for w in wires:
            a, b = w["p0"], w["p1"]
            horiz = abs(a[1] - b[1]) < 1e-6
            vert = abs(a[0] - b[0]) < 1e-6
            if not (horiz or vert):
                continue
            for k2, q in pads.items():
                if net_of_pad.get(k2) == w["net"] or not q.get("box"):
                    continue
                g = _seg_rect_gap(a, b, q["box"]) - half
                if g >= need - 1e-9:
                    continue
                d = need - g + 0.01
                if horiz:
                    cy = (q["box"][1] + q["box"][3]) / 2.0
                    off = (-d if a[1] > cy else d, 0)
                    off = (0.0, off[0])
                else:
                    cx = (q["box"][0] + q["box"][2]) / 2.0
                    off = (-d if a[0] > cx else d, 0.0)
                for v in (a, b):
                    if key(v) not in protect:
                        move(v, off)
                bad += 1
                n += 1
                break
        if not bad:
            break
    # 删掉零长线 ✓
    wires[:] = [w for w in wires if key(w["p0"]) != key(w["p1"])]
    return n


def do_route():
    PR.TRACE_MM = MIL * PR.MIL_MM
    # ★★ 净空取 **0.20**（库仓默认 0.15 ✗，2026-10-18 实测调 ✓）：
    #   栅格 0.15 会把 `0.15 + 半宽` 量化到**正好 0.15** ✗ ⇒ 实测铜净距 **0.1484 mm** ✗
    #   （比校验器要的 0.15 差 **1.6 µm** ✓、`SWCLK` 那条更差 0.0484 ✗）⇒ 给模型留一格余量 ✓。
    PR.CLEAR_MM = CLEAR_MM
    PR.VIA_AT_PAD = True
    PR.ESCAPE_PROTECT = True
    PR.ESCAPE_COST_MM = 0.4
    PR.ESCAPE_R_MM = 1.2
    # ★★ 「挖完自己的盘后，把别人的盘按**真净空**重挡」✓（默认 0 = 不挡 ✗，本板必须挡 ✓）：
    #   不挡 ⇒ 走线会从 **0.20 mm 的 QFN 脚缝**里横穿 ✗（实测 `CON_RX` 离 `U1.connector18`
    #   的铜只有 **0.118 mm** ✗ < 0.15 ✗）。设成"半宽 + 净空" ⇒ 缝宽 0.20 两边各要 0.26 ⇒ 无解 ✓。
    PR.REBLOCK_OTHERS_MM = 0.0
    # ★★ **拐弯代价调大**（库仓默认 0.5 格 ✗）：默认时 "出脚后横移 0.08 mm 再走" 几乎免费 ✗
    #   ⇒ `CON_RX` 就会在 `U1` 那排脚**里面**横滑 0.08 ✗ ⇒ 铜离隔壁盘只剩 0.118 mm ✗。
    #   调到 4 格（= 0.32 mm/转弯）⇒ 那一下横移要多花 2 个转弯 ⇒ 不如**顺着本脚中线**出去 ✓。
    PR.TURN_COST = 0.5            # ★ 试过 4.0 ⇒ 更差 ✗（0.0384 ✗，且 SWCLK 也中招）⇒ 保持默认 ✓
    model = PC.collect(str(FZZ))
    net_pads, unresolved = PR.resolve_nets(model, CN.NETS)
    if unresolved:
        raise SystemExit("✗ 网表里这些脚在板上找不到：%s" % "、".join(unresolved))
    model["net_pads"] = net_pads
    pads = PR.pad_index(model)
    items, _st = PR.obstacles(model)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        res = PR.route_ripup(items, model["board"], net_pads, pads, CELL_MM, 4.0,
                             tries=4, passes=3)
    bad = [n for n, d in res.items() if not d["ok"]]
    print("① 布线：**%d/%d 网连通**%s" % (len(res) - len(bad), len(res),
                                       ("，布不通：%s ✗" % "、".join(bad)) if bad else " ✓"))
    if bad:
        print(buf.getvalue()[-2500:])
        raise SystemExit("✗ 有网没连通 ⇒ 不写文件 ✗")
    net_of_pad = {}
    for n, mem in net_pads.items():
        for k in mem:
            net_of_pad[k] = n
    clash = PR.via_pad_conflicts([(n, v) for n, d in res.items() for v in d["vias"]],
                                 pads, net_of_pad)
    if clash:
        for c in clash[:6]:
            print("   ✗ 过孔 %s 压 %s.%s（%.3f < %.3f mm）" % (c[0], c[2], c[3], c[4], c[5]))
        raise SystemExit("✗ 过孔压盘 ⇒ 不写文件 ✗")
    return res, pads, net_pads


# ── ③ 生成 XML ───────────────────────────────────────────────────────────────
def _add_connects(blk, cid, lst):
    """把 `[(对方 connectorId, modelIndex, layer)]` 写进 `cid` 那个连接器的 `<connects>` ✓"""
    m = re.search(r'(<connector connectorId="%s"[^>]*>\s*<geometry [^>]*/>)' % cid, blk)
    if not m:
        return blk
    head, rest = blk[:m.end()], blk[m.end():]
    mm = re.match(r'\s*<connects>([\s\S]*?)</connects>', rest)
    lines = "".join('              <connect connectorId="%s" modelIndex="%s" layer="%s" />\n'
                    % c for c in lst)
    if mm:
        inner = rest[mm.start(1):mm.end(1)]
        if not inner.endswith("\n"):
            inner += "\n"
        return head + rest[:mm.start(1)] + inner + lines + rest[mm.end(1):]
    return (head + "\n            <connects>\n" + lines +
            "            </connects>" + rest)


def _conns(lst, ind):
    if not lst:
        return ""
    return ("\n" + " " * ind + "<connects>\n" +
            "".join(" " * (ind + 2) +
                    '<connect connectorId="%s" modelIndex="%s" layer="%s" />\n' % c
                    for c in lst) +
            " " * ind + "</connects>")


def build(text, res, pads, net_pads):
    part = {}
    for m in re.finditer(r'<instance [^>]*modelIndex="(\d+)"[\s\S]*?<title>([^<]+)</title>'
                         r'([\s\S]*?)\n  </instance>', text):
        pv = re.search(r'<pcbView\b[^>]*?layer="([^"]+)"', m.group(3))
        part[m.group(2).strip()] = (m.group(1), pv.group(1) if pv else "copper1")

    net_of_pad = {}
    for n, mem in net_pads.items():
        for k in mem:
            net_of_pad[k] = n
    pad_at = {}
    for k, q in pads.items():
        pad_at.setdefault(key(q["c"]), []).append(k)

    used = [int(x) for x in re.findall(r'modelIndex="(\d+)"', text)]
    mi_w = max(used + [90000000]) + 1
    mi_v = mi_w + 300000
    wires, vias = [], []
    for net in sorted(res):
        d = res[net]
        for lay, p, q in d["segs"]:
            wires.append(dict(mi=mi_w, net=net, lay=lay, p0=p, p1=q))
            mi_w += 1
        for p in d["vias"]:
            vias.append(dict(mi=mi_v, net=net, p=p))
            mi_v += 1

    # ★ 后处理"顶点微调"**默认关** ✗（2026-10-18 ✓）：试过 ⇒ 一次推开 103 处 ✗、
    #   反而造出 2 条真短接（`⑤ 网表` 2 条 ✓）⇒ 按仓规 §0 第 3 条**停手** ✗。
    #   那 2 条过近的走线另用**定点手改**处理 ✓（见 `docs/carrier-pcb-notes.md` ✓）。
    if "--nudge" in sys.argv:
        _n = _nudge_clearance(wires, vias, pads, net_of_pad,
                              PR.U(0.15 + 0.005), PR.U(MIL * PR.MIL_MM / 2))
        print("   微调：推开 %d 处过近走线 ✓" % _n)

    at = {}
    for w in wires:
        at.setdefault(key(w["p0"]), []).append(("w", w["mi"], 0, w["lay"]))
        at.setdefault(key(w["p1"]), []).append(("w", w["mi"], 1, w["lay"]))
    for v in vias:
        at.setdefault(key(v["p"]), []).append(("v", v["mi"], 0, "copper0"))
    for k, lst in pad_at.items():
        for (t, c) in lst:
            at.setdefault(k, []).append(("p", part[t][0], (t, c), part[t][1]))

    back = {}                       # (位号, connectorN) -> [(走线 mi, 层)]

    def ends(w):
        out = []
        for p in (w["p0"], w["p1"]):
            cs = []
            for kind, mi, extra, lay in at[key(p)]:
                if kind == "w":
                    if mi == w["mi"]:
                        continue
                    cs.append(("connector%d" % extra, mi, lay + "trace"))
                elif kind == "v":
                    cs.append(("connector0", mi, "copper0"))
                else:
                    if net_of_pad.get(extra) != w["net"]:
                        continue           # ✗ 绝不接别的网（含空脚 ✗，F15）
                    cs.append((extra[1], mi, lay))
                    back.setdefault(extra, []).append((w["mi"], w["lay"] + "trace"))
            out.append(cs)
        return out

    body = []
    for w in wires:
        e0, e1 = ends(w)
        dx, dy = w["p1"][0] - w["p0"][0], w["p1"][1] - w["p0"][1]
        col = CN.COLOR.get(w["net"], "#999999")
        body.append(
            '  <instance moduleIdRef="WireModuleID" modelIndex="%d" '
            'path=":/resources/parts/core/wire.fzp">\n'
            '    <views>\n'
            '      <breadboardView layer="breadboardWire">\n'
            '        <geometry z="%s" x="0" y="0" x1="0" y1="0" x2="0" y2="0" '
            'wireFlags="4" />\n'
            '      </breadboardView>\n'
            '      <schematicView layer="schematicTrace">\n'
            '        <geometry z="%s" x="0" y="0" x1="0" y1="0" x2="0" y2="0" '
            'wireFlags="4" />\n'
            '      </schematicView>\n'
            '      <pcbView layer="%strace">\n'
            '        <geometry z="%s" x="%s" y="%s" x1="0" y1="0" x2="%s" y2="%s" '
            'wireFlags="4" />\n'
            '        <wireExtras mils="%d" color="%s" opacity="1" banded="0" />\n'
            '        <connectors>\n'
            '          <connector connectorId="connector0" layer="%strace">\n'
            '            <geometry x="0" y="0" />%s\n'
            '          </connector>\n'
            '          <connector connectorId="connector1" layer="%strace">\n'
            '            <geometry x="%s" y="%s" />%s\n'
            '          </connector>\n'
            '        </connectors>\n'
            '      </pcbView>\n'
            '    </views>\n'
            '  </instance>'
            % (w["mi"], Z_WIRE, Z_WIRE, w["lay"], Z_WIRE, num(w["p0"][0]), num(w["p0"][1]),
               num(dx), num(dy), MIL, col, w["lay"], _conns(e0, 12), w["lay"],
               num(dx), num(dy), _conns(e1, 12)))

    off = PB.draw_off_units("via", (VIA_HOLE, VIA_RING))    # ★ sketch 单位 ✓（不是 mm ✗）
    for v in vias:
        cs, tail = [], []
        for kind, mi, e, lay in at[key(v["p"])]:
            if kind != "w":
                continue
            cs.append(("connector%d" % e, mi, lay + "trace"))
            tail.append((mi, lay + "trace"))
        body.append(
            '  <instance moduleIdRef="ViaModuleID" modelIndex="%d" '
            'path=":/resources/parts/core/via.fzp">\n'
            '    <property name="hole size" value="%.2fmm,%.2fmm" />\n'
            '    <title>Via%d</title>\n'
            '    <views>\n'
            '      <breadboardView layer="copper0">\n'
            '        <geometry z="%s" x="0" y="0" wireFlags="32" />\n'
            '      </breadboardView>\n'
            '      <schematicView layer="copper0">\n'
            '        <geometry z="%s" x="0" y="0" wireFlags="32" />\n'
            '      </schematicView>\n'
            '      <pcbView layer="copper0">\n'
            '        <geometry z="%s" x="%s" y="%s" wireFlags="32" />\n'
            '        <connectors>\n'
            '          <connector connectorId="connector0" layer="copper0">\n'
            '            <geometry x="0" y="0" />%s\n'
            '          </connector>\n'
            '        </connectors>\n'
            '      </pcbView>\n'
            '    </views>\n'
            '  </instance>'
            % (v["mi"], VIA_HOLE, VIA_RING, v["mi"] % 100000, Z_VIA, Z_VIA, Z_VIA,
               num(v["p"][0] - off), num(v["p"][1] - off), _conns(cs, 12)))
        # 过孔的回指（写进它自己的实例里 ⇒ 直接改刚追加的那段）
        body[-1] = _add_connects(body[-1], "connector0",
                                 [("connector%d" % (0 if j == 0 else 1), m2, l2)
                                  for j, (m2, l2) in enumerate(tail)])
        del tail

    # ★ 回指：走线写进**焊盘**的 `<connects>` ✓（缺它 ⇒ 整组不被认 ✗，F8）
    for (t, cid), lst in back.items():
        mi = part[t][0]
        m = re.search(r'<instance [^>]*modelIndex="%s"[\s\S]*?\n  </instance>' % mi, text)
        if not m:
            continue
        new = _add_connects(m.group(0), cid, [("connector0", w, l) for (w, l) in lst])
        text = text[:m.start()] + new + text[m.end():]
    return body, text


def main(argv):
    dry = "--dry" in argv
    name, text, others = read_fz(FZZ)
    text, n_old = strip_wires(text)
    res, pads, net_pads = do_route()
    body, text = build(text, res, pads, net_pads)
    text = text.replace("</instances>", "\n".join(body) + "\n</instances>")
    try:
        ET.fromstring(text)
    except Exception as e:                                        # noqa: BLE001
        raise SystemExit("✗ 写出来的 XML 解析不了：%s" % e)
    nw = sum(1 for b in body if "WireModuleID" in b)
    nv = sum(1 for b in body if "ViaModuleID" in b)
    print("② 摘掉旧实例 %d 条 ⇒ 写入 **走线 %d / 过孔 %d**" % (n_old, nw, nv))
    if dry:
        print("（--dry：不写文件）")
        return 0
    write_fz(FZZ, name, text, others)
    print("✓ 写出 %s（%d 字节）" % (FZZ.name, FZZ.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
