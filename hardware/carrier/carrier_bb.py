# -*- coding: utf-8 -*-
r"""载板：给源草图的**面包板视图**摆位（摆位先于入网 ✓ 规则：库仓 `docs/breadboard-routing-rules.md` §A.4/§A.5 ✓）

★ 接在 `carrier_board.py` 后面跑 ✓（后者只排**原理图**视图 ⇒ 面包板视图还是空骨架 ✗）：

    py -3.13 carrier_board.py                     # ① 源草图（含元件、零导线 ✓）
    py -3.13 carrier_bb.py [--dry]                # ② **本脚本**：面包板视图摆位 ＋ 写声明（不上导线 ✓）
    py -3.13 <库仓>\tools\render_bb.py carrier-board.fzz _work\bb.png        # 看摆位 ✓
    py -3.13 <库仓>\tools\bb_precheck.py carrier-board.fzz --nets=carrier_nets.py   # 前置闸门 ✓
    py -3.13 <库仓>\tools\bb_route4.py carrier-board.fzz _work\bb_routed.fzz --nets=carrier_nets.py  # ③ 布线 ✓
    py -3.13 <库仓>\tools\bb_legend3.py _work\bb_routed.fzz _work\bb_legend.fzz 590 12 12 22 -3 --nets=carrier_nets.py  # ④ 图例 ✓

★ **③④ 要靠本脚本塞的两个"模板件"才能跑** ✓（2026-10-16 实测 ✗→✓）：
  · **模板导线**（`WireModuleID` ✓）：`bb_route4` 写文件前要从**输入稿**里捞一条带
    `breadboardView` 的导线当模板 ✗ —— 而且它得**有 `<connect>`**（没有的会被 `--keep-deco`
    当成"图例色条"跳过 ✗）⇒ 零导线的源草图上必然报 `no breadboard wire template` **不写文件** ✗
    （第一次就是这么白算了一遍 ✓）。布线器会把它**删掉**、用它复制出所有新线 ✓
    ⇒ 它只是**牺牲品** ✓、不进交付物 ✓。
  · **图例模板件**（`BreadboardLogoTextModuleID` ✓）：`bb_legend3` 要么从输入稿捞、要么用第 8 个
    参数**外借** ✗ —— 外借就得指着**别的仓库**的文件 ✗（实测那份在
    `f:\git\AuroraTessellation-NFC\…` ✓）⇒ 内联骨架 ✓，整条流水线自洽、换机器也能重跑 ✓。
  ★ 两个模板件的**位置**有硬约束（都踩过 ✗）：
    ① **板子必须排 `<instances>` 第一位** ✓ —— `render_bb` 按**文档次序**画 ✓，板本体不透明、
       排在件后面就把件全盖住 ✗；而 `bb_route4.py` 挑板子的判据是
       `"breadboard" in moduleIdRef.lower()` ✗ —— `BreadboardLogoTextModuleID` **也含**
       `breadboard` ✗ ⇒ 模板件排在板子前面会被**当成板子**去解析 ⇒
       `OSError: ':\resources\parts\core\breadboardlogotext.fzp'` ✗✗（实测 ✓）
       ⇒ **板子首位、模板件末位** ✓。
    ② 图例模板件摆在**图例标题将来要占的那格**（`590+9+3=602`, `y=12` ✓）、形状**空** ✓
       ⇒ 画布上**一个像素也不画** ✓（摆远处会把画布撑大 ✗、写真标题又成了"无缘无故多一行字"✗）。

三条规矩（照库仓 ✓）：
  · **§A.4 摆位先于入网** ✓ —— 每只脚都落在**真实的孔**上 ✓（判据 = 几何 ✓，不是"有没有 `<connect>`" ✗）
  · **§A.5 物理在前、声明在后** ✓ —— 先算几何、再写声明 ✓（声明只是 Fritzing 记下的结果 ✓）
  · **E1/E3** ✓ —— **橡皮筋腿件**（`R0603`：两脚天然相距 4.099 格 ✗ 不是整数格 ✗）靠 `<leg>` 拉到孔上 ✓、
    板侧回指用 `layer="breadboard"`、零件侧用 `layer="breadboardbreadboard"` ✓

几何**一律调库仓同一份实现** ✓（`bb_compare.hole_xy` ✓、`bb_precheck.anchors`/`holes_of` ✓、
`part_box.apply/body_box/resolve_svg` ✓）—— ✗ 本脚本不自己造一套几何 ✗。

★ 摆位表 `PLACE` 是**唯一**的位置来源 ✓（✗ 不许"继承上一个件"✗ —— 那块教训见 §A.4 ✓）。
★ 摆不进孔的件（超容 `BT1`：脚距 **6.00 mm** ✗ = 2.36 格 ✗，而面包板只有 2.54 的整数倍 ✗）
  ⇒ **面包板视图里不画它** ✓（先例：像素板的 `TP1` 也只有 pcb/schematic 两个视图 ✓）；
  它与 `J4` 的 `VBAT` 那一段在**原理图/PCB** 上照旧 ✓。
"""
import io
import os
import re
import sys
import shutil
import pathlib
import zipfile
import xml.etree.ElementTree as ET

HERE = pathlib.Path(__file__).resolve().parent
LIB = pathlib.Path(r"f:\git\fritzing-parts-langhua\tools")
sys.path.insert(0, str(LIB))
import part_box as pbox                                              # noqa: E402
import bb_compare as BC                                              # noqa: E402
import bb_precheck as BP                                             # noqa: E402

SK_MM = 25.4 / 90.0
TOL_MM = 0.6                             # 与 bb_precheck 缺省同值 ✓
SRC = HERE / "carrier-board.fzz"
BB_FZP = str(pathlib.Path(r"C:\Users\shi.jinghai-honor\AppData\Local\Programs\Fritzing"
                          r"\fritzing-parts\core\breadboard2.fzp"))
BB_MID = "Breadboard-RSR03MB102-ModuleID"    # ★ 核心 `breadboard2.fzp` 自己的 moduleId ✓（实测 ✓）
BB_MI = "90000011"
WIRE_MI = "90000012"                     # ★ 模板导线（牺牲品 ✓，见文末那段说明 ✓）
LOGO_MI = "90000013"                     # ★ 图例模板件（画布外 ✓，同见文末 ✓）
BB_LOC = (-4.6484, 0.0)                  # ★ 抄像素板交付件那块板的位置 ✓（实测：插孔与
#   `bb_compare.hole_xy` 逐点对上 ✓；换一个位置渲染就对不上孔列 ✗）
Z_BB, Z_PART = "1.50059", "2.50002"

ROT = {0: (1, 0, 0, 1), 90: (0, 1, -1, 0), 180: (-1, 0, 0, -1), 270: (0, -1, 1, 0)}

# ── 摆位表 ✓（**唯一**位置来源 ✓）────────────────────────────────────────────────
#   `ref`/`pin`/`hole` = 参考脚 → 参考孔（其余脚由**同一变换**推出来，脚本逐脚自检 ✓）
#   `holes` = **整件**橡皮筋腿：逐脚点名的孔 ✓（腿拉过去 ✓）—— 例如 `R0603`（两脚天然相距 4.099 格 ✗）
#   `legs`  = **单脚**橡皮筋腿：只给点名的脚一根腿、其余脚照常落最近孔 ✓
#     ★ 用途 = **裸焊盘**（`U1` 的 `EPAD` = 部件里的 `VSS` ✓）：它本来不插孔 ✗，
#       而画板时**必须特意连到 GND** ✓（库仓 §5b 第 8 条 ✓）⇒ 给它一根腿拉到 GND 轨的空孔 ✓
#       —— 这样"看得出特意接地" ✓、腿端落孔能让 `bb_precheck` ①② 过 ✓、
#       而且 **重跑布线也不会丢** ✓（它属于摆位、不属于导线 ✓；单加一根导线会被布线器删掉 ✗）。
#   `away`  = 摆不进孔、又还没想好怎么处理的件（本表里为空 ✓）
PLACE = [
    dict(ref="J1", rot=270, pin="connector0", hole="pin3J"),      # 模组口 1×4 ⇒ 3..6 列 / J 行
    dict(ref="U1", rot=270, pin="connector0", hole="pin8E",
         #      ★ 裸焊盘 `connector20`（部件里叫 `VSS` ✓）= **EPAD** ✓ ⇒ 不插孔 ✗，
         #        给它一根腿拉到 **行 Z 的 GND 轨空孔** ✓（`pin18Z`：离这颗焊盘最近 ✓，
         #        且行 Z 本来就是本板 GND 轨 ✓ ⇒ 挂上去就与 GND 同一条铜片 ✓，不必多绕线 ✓）。
         #        ★ 先例：像素板 `U1` 的 EPAD → `pin31Z` ✓（库仓 `AGENTS.md` §5b 第 8 条 ✓）。
         legs={"connector20": "pin18Z"}),            # MCU 转接板（8..17 列 / E+F 行）
    dict(ref="U2", rot=270, pin="connector0", hole="pin20E"),     # SE 转接板 ⇒ 20..23 列 / E+F 行
    dict(ref="D1", rot=270, pin="connector0", hole="pin28I"),     # 心跳灯：两脚在 I 行、**25与28列** ✓
    #      ★ 为什么是 25/28 ✗：它的两脚**沿 local −y** ⇒ 转 270° 后第二脚在**左边 3 列** ✓
    #      （与 `U1`/`U2` 相反 ✗）⇒ 第一版放到 23 列 ⇒ 与 `U2` 的 SDA 脚（`pin23F`）同 bus = 短路 ✗
    #      ⇒ 布线器当场报 `[SHORT] bus 2/3/J carries LED_HB, SDA` ✓（✗ 不许自己"看着没问题"✗）
    dict(ref="R1", rot=0, holes={"connector0": "pin31J", "connector1": "pin36I"}),
    dict(ref="R2", rot=0, holes={"connector0": "pin31G", "connector1": "pin38G"}),
    #      ★ 两只上拉的第一脚（`R1.c0`/`R2.c0`）同在 **31 列**的 F–J bus 上 ✓ —— 都是 **3V3** ✓
    #      （EXPECT 里 `3V3` = R1/R2/R3 的 connector0 ✓）⇒ 同 bus 不构成"两网相连" ✓✓
    #      ★ 而第二脚是**三张不同的网**（R1=SCL ✓ / R2=SDA ✓ / R3=LED_R ✓）⇒ 必须分列 ✓
    #      （第一版 R1/R2 第二脚都在 36 列 ⇒ 同 bus = SCL+SDA 短接 ✗ 布线器报
    #       `[SHORT] bus 3/6/J carries SCL, SDA` ✓）
    dict(ref="R3", rot=0, holes={"connector0": "pin31D", "connector1": "pin38C"}),
    dict(ref="J2", rot=270, pin="connector0", hole="pin40J"),     # 台架/产线 1×6
    dict(ref="J3", rot=270, pin="connector0", hole="pin40C"),     # SWD 1×4
    dict(ref="J4", rot=270, pin="connector0", hole="pin48J"),     # 前端 1×7
    dict(ref="BT1", no_bb=True),                                  # 超容：脚距 6.00 mm ✗ 插不进 ⇒ 不画 ✓
]


def num(v):
    f = float(v)
    return str(int(round(f))) if abs(f - round(f)) < 1e-9 else ("%.4f" % f).rstrip("0").rstrip(".")


def tf_text(rot, box):
    """Fritzing 的旋转写法 ✓：`m31 = 盒左+盒右`、`m32 = 盒上+盒下`（= 2×中心 ✓）"""
    m11, m12, m21, m22 = ROT[rot]
    return (m11, m12, m21, m22, box[0] + box[2], box[1] + box[3])


def inv_apply(m, x, y):
    """逆变换：给 sketch 位移 (x,y) ⇒ 局部点 ✓（旋转变换是正交的，直接解 2×2 ✓）"""
    a, b, c, d = m[0], m[1], m[2], m[3]          # A = [[a, c], [b, d]]
    det = a * d - b * c
    u, v = x - m[4], y - m[5]
    return ((d * u - c * v) / det, (-b * u + a * v) / det)


def load(path):
    z = zipfile.ZipFile(path)
    name = [n for n in z.namelist() if n.endswith(".fz")][0]
    return z, name, z.read(name).decode("utf-8")


def svg_of(fzp_path):
    root = ET.parse(fzp_path).getroot()
    lay = root.find(".//breadboardView/layers")
    img = lay.get("image") if lay is not None else None
    return pbox.resolve_svg(fzp_path, img) if img else None


def bb_pins(fzp_path):
    """⇒ {脚名…}：这只脚**在面包板视图里有图形**（fzp 里写了 `<breadboardView><p …/>`）✓
    ★ 为什么要有它 ✗：**裸焊盘**（`U1` 的 `EPAD` ✓）在面包板视图里**根本没有图形** ✗
      ⇒ 它**不插孔** ✓（判据 = 库里那条"裸焊盘不插孔"✓）⇒ 不参与落孔检查 ✓、
      也不写孔声明 ✓ —— 它会由**布线器**接一根线出去 ✓（规则 §A.5 第 5 条：导线靠端点声明 ✓）。"""
    root = ET.parse(fzp_path).getroot()
    out = set()
    for c in root.iter("connector"):
        for p in c.findall(".//breadboardView/p"):
            out.add(c.get("id"))
            break
    return out


def parts_of(text):
    out = {}
    for m in re.finditer(r"(?s)<instance\b.*?</instance>", text):
        b = m.group(0)
        head = b[:b.index(">") + 1]
        if "Wire" in (re.search(r'moduleIdRef="([^"]+)"', head) or ["", ""])[1]:
            continue
        t = re.search(r"<title>([^<]*)</title>", b)
        if not t:
            continue
        out[t.group(1)] = dict(blk=b, s=m.start(), e=m.end(),
                               fzp=(re.search(r'path="([^"]*)"', head) or [None, ""])[1].replace("/", os.sep),
                               mi=(re.search(r'modelIndex="([^"]+)"', head) or [None, ""])[1])
    return out


def nearest_hole(p, holes):
    best, bd = None, None
    for h, (x, y) in holes.items():
        d = ((p[0] - x) ** 2 + (p[1] - y) ** 2) ** 0.5
        if bd is None or d < bd:
            best, bd = h, d
    return best, bd


def main(argv):
    src, dst, dry = SRC, SRC, False
    for a in argv:
        if a == "--dry":
            dry = True
        elif a.startswith("--in="):
            src = pathlib.Path(a.split("=", 1)[1])
        elif a.startswith("--out="):
            dst = pathlib.Path(a.split("=", 1)[1])
    z, name, text = load(src)
    got = BP.holes_of(text)
    if got is None:
        # 草图里还没有面包板实例（**第一次跑就是这种** ✓）⇒ 直接按核心件的 `.fzp` 取孔集 ✓
        # （与 `bb_precheck.holes_of` 同一份 `hole_xy` ✓ —— ✗ 不是自己再造一套 ✗）
        broot = ET.parse(BB_FZP).getroot()
        holes = {c.get("id"): BC.hole_xy(c.get("id")) for c in broot.iter("connector")
                 if re.fullmatch(r"pin\w+", c.get("id") or "")}
        holes = {k: v for k, v in holes.items() if v}
    else:
        _board, holes, _bus = got
    parts = parts_of(text)
    print("真实孔集：%d 颗 ✓" % len(holes))

    plan, bad = {}, []
    for spec in PLACE:
        ref = spec["ref"]
        if ref not in parts:
            print("✗ %s 不在草图里 ✗" % ref)
            bad.append(ref)
            continue
        if spec.get("no_bb"):
            plan[ref] = dict(no_bb=True, mi=parts[ref]["mi"])
            print("  %-4s 不上面包板 ✓（脚距 6.00 mm ✗ 插不进孔 ✗）" % ref)
            continue
        fzp = parts[ref]["fzp"]
        anc = BP.anchors(fzp)
        svg = svg_of(fzp)
        if not anc or not svg:
            print("✗ %s 取不到几何（anc=%d svg=%s）✗" % (ref, len(anc), svg))
            bad.append(ref)
            continue
        box = pbox.body_box(svg)
        m = tf_text(spec.get("rot", 0), box)
        if "hole" in spec:
            h = holes.get(spec["hole"])
            if h is None:
                print("✗ %s 的参考孔 %s 不存在 ✗" % (ref, spec["hole"]))
                bad.append(ref)
                continue
            lx, ly = anc[spec["pin"]]
            p = pbox.apply(m, lx, ly)
            loc = (h[0] - p[0], h[1] - p[1])
        else:
            c0 = sorted(spec["holes"], key=lambda s: int(re.findall(r"\d+", s)[0]))[0]
            h = holes.get(spec["holes"][c0])
            lx, ly = anc[c0]
            p = pbox.apply(m, lx, ly)
            loc = (h[0] - p[0], h[1] - p[1])
        conns = []
        bbp = bb_pins(fzp)
        for cid in sorted(anc, key=lambda s: int(re.findall(r"\d+", s)[0])):
            lx, ly = anc[cid]
            # ★★ 单脚腿**必须排在最前** ✗（2026-10-16 ✓）：`bb_pins` 只收"fzp 的
            #   `breadboardView` 里有图形"的脚 ✗ ⇒ 裸焊盘（EPAD）常常**没有图形** ✗
            #   ⇒ 它会在下面那个"裸焊盘"分支就被判掉、永远走不到腿 ✗。
            LEG1 = spec.get("legs", {})
            if cid in LEG1:                           # 点名的脚 ⇒ 腿拉到那颗孔 ✓
                H = LEG1[cid]
                if H not in holes:
                    print("✗ %s.%s 的腿孔 %s 不存在 ✗" % (ref, cid, H))
                    bad.append("%s.%s" % (ref, cid))
                    continue
                hxy = holes[H]
                q = inv_apply(m, hxy[0] - loc[0], hxy[1] - loc[1])
                conns.append(dict(cid=cid, hole=H, geo=(lx, ly),
                                  leg=(q[0] - lx, q[1] - ly)))
                continue
            if cid not in bbp or cid in spec.get("no_hole", ()):   # ★ 裸焊盘（不插孔 ✓）
                conns.append(dict(cid=cid, hole=None, geo=(lx, ly), leg=None, bare=True))
                continue
            p = pbox.apply(m, lx, ly)
            P = (loc[0] + p[0], loc[1] + p[1])
            if "holes" in spec:                       # 腿件：孔**点名** ✓ + 写腿 ✓
                H = spec["holes"].get(cid)
                if H is None:
                    conns.append(dict(cid=cid, hole=None, geo=(lx, ly), leg=None))
                    continue
                hxy = holes[H]
                q = inv_apply(m, hxy[0] - loc[0], hxy[1] - loc[1])
                leg = (q[0] - lx, q[1] - ly)
                conns.append(dict(cid=cid, hole=H, geo=(lx, ly), leg=leg))
            else:                                     # 普通件：落在**最近的真实孔**上 ✓
                H, d = nearest_hole(P, holes)
                if d * SK_MM > TOL_MM:
                    bad.append("%s.%s→%s 差 %.3f mm" % (ref, cid, H, d * SK_MM))
                conns.append(dict(cid=cid, hole=H, geo=(0, 0), leg=None, d=d))
        nd = max([c["d"] for c in conns if c.get("d") is not None] or [0])
        nb = len([1 for c in conns if c.get("bare")])
        print("  %-4s rot=%-3s loc=(%9.3f,%9.3f) 脚=%2d 落孔=%2d 裸焊盘=%d 最大偏差=%.4f mm"
              % (ref, spec.get("rot", 0), loc[0], loc[1], len(conns),
                 len(conns) - nb, nb, nd * SK_MM))
        plan[ref] = dict(loc=loc, m=m, conns=conns, mi=parts[ref]["mi"])

    if bad:
        print("\n✗ 没到位 %d 处 ⇒ **不写文件** ✗" % len(bad))
        for b in bad:
            print("   -", b)
        return 1

    # ── 写回：逐件重建 breadboardView ✓（其它视图一个字不动 ✓）────────────────────
    new = text
    for ref, pl in plan.items():
        s, e = parts[ref]["s"], parts[ref]["e"]
        blk = parts[ref]["blk"]
        if pl.get("no_bb"):
            blk2 = re.sub(r"(?s)\s*<breadboardView\b.*?</breadboardView>", "", blk)
        else:
            lines = ['<breadboardView layer="breadboard">',
                     '                    <geometry z="%s" x="%s" y="%s">' % (Z_PART, num(pl["loc"][0]), num(pl["loc"][1]))]
            m = pl["m"]
            if m[0:4] != (1, 0, 0, 1) or m[4] or m[5]:
                lines.append('                        <transform m11="%s" m12="%s" m13="0" '
                             'm21="%s" m22="%s" m23="0" m31="%s" m32="%s" m33="1" />'
                             % (num(m[0]), num(m[1]), num(m[2]), num(m[3]), num(m[4]), num(m[5])))
            lines += ['                    </geometry>', '                    <connectors>']
            for c in pl["conns"]:
                lines.append('                        <connector connectorId="%s" layer="breadboard">' % c["cid"])
                lines.append('                            <geometry x="%s" y="%s" />'
                             % (num(c["geo"][0]), num(c["geo"][1])))
                if c["leg"]:
                    lines += ['                            <leg>',
                              '                                <point x="0" y="0" />',
                              '                                <bezier />',
                              '                                <point x="%s" y="%s" />' % (num(c["leg"][0]), num(c["leg"][1])),
                              '                                <bezier />',
                              '                            </leg>']
                if c["hole"]:
                    lines += ['                            <connects>',
                              '                                <connect connectorId="%s" modelIndex="%s" layer="breadboardbreadboard" />'
                              % (c["hole"], BB_MI),
                              '                            </connects>']
                lines.append('                        </connector>')
            lines += ['                    </connectors>', '                </breadboardView>']
            blk2 = "\n".join(lines)
            old_bv = re.search(r"(?s)<breadboardView\b.*?</breadboardView>", blk)
            if old_bv:
                blk2 = blk[:old_bv.start()] + blk2 + blk[old_bv.end():]
            else:                                     # 没这块 ⇒ 插在 `</views>` 之前 ✓
                i = blk.rindex("</views>")
                blk2 = blk[:i] + blk2 + "\n                " + blk[i:]
        new = new[:s] + blk2 + new[e:]
        # ★ 偏移跟着变 ✗ ⇒ 每改一件就重新解析一次位置 ✓（简单可靠 ✓）
        parts = parts_of(new)

    # 先摘掉旧的面包板实例（重跑幂等 ✓），再插新的 ✓
    new = re.sub(r"(?s)\s*<instance\b[^>]*moduleIdRef=\"%s\".*?</instance>" % re.escape(BB_MID), "", new)
    bl = ['        <instance moduleIdRef="%s" modelIndex="%s" path="%s">' % (BB_MID, BB_MI, BB_FZP.replace(os.sep, "/")),
          '            <title>Breadboard1</title>', '            <views>',
          '                <breadboardView layer="breadboard">',
          '                    <geometry z="%s" x="%s" y="%s" />' % (Z_BB, num(BB_LOC[0]), num(BB_LOC[1])),
          '                    <connectors>']
    for ref, pl in plan.items():
        if pl.get("no_bb"):
            continue
        for c in pl["conns"]:
            if not c["hole"]:
                continue
            bl += ['                        <connector connectorId="%s" layer="breadboardbreadboard">' % c["hole"],
                   '                            <geometry x="0" y="0" />', '                            <connects>',
                   '                                <connect connectorId="%s" modelIndex="%s" layer="breadboard" />'
                   % (c["cid"], pl["mi"]),
                   '                            </connects>', '                        </connector>']
    bl += ['                    </connectors>', '                </breadboardView>', '            </views>',
           '        </instance>']
    # ★★ 面包板实例要插在 `<instances>` 的**最前面** ✗✓（2026-10-16 实测 ✓）：
    #   `render_bb` 的绘制次序 = **文档次序** ✓（`body = [背景框] + 逐实例` ✓）⇒
    #   板子若排在**件后面**，它那层不透明本体就把件**盖住** ✗（第一版就插在 `</instances>`
    #   前 ⇒ 预览只见板子、件全不见 ✗）；像素板那份的板子本来就在最前 ✓ ⇒ 照它 ✓。
    i = new.index(">", new.index("<instances")) + 1
    new = new[:i] + "\n" + "\n".join(bl) + new[i:]

    # ★★ 模板导线（牺牲品 ✓）：`bb_route4` 写文件前要从输入里捞一条**带 `breadboardView` 的
    #   Wire 实例**当模板 ✗ —— 它需要**有 `<connect>`**（没有的会被 `--keep-deco` 当成图例色条
    #   而**不当模板** ✗，见 `bb_route4.py:178-187` ✓）。我们的源草图是**零导线**的 ✗
    #   ⇒ 不加这条就会 `no breadboard wire template` ⇒ **不写文件** ✗（2026-10-16 实测 ✓）。
    #   ⇒ 这里插一条**真导线**当模板 ✓：布线器会把它**删掉**、用它复制出所有新线 ✓
    #     （它声明的那两个孔随后会被"悬空声明"清理一起收拾掉 ✓）。
    #   ★ 孔的写法照**实发**过的那条（像素板 `pixel-breadboard100.fzz` 的 `pin12A`/`pin12X` ✓）：
    #     `pin<列><行>` ✓、列取**真轨孔列**（3..61 ✓）、行取 `A` 与电源轨 `X` ✓；
    #     位置挑**本板不会用到**的右侧空列 60 ✓（就算它万一幸存也不会压到我们的线 ✓）。
    wire = ('        <instance moduleIdRef="WireModuleID" modelIndex="%(mi)s" '
            'path=":/resources/parts/core/wire.fzp">\n'
            '            <title>Wire%(mi)s</title>\n'
            '            <views>\n'
            '                <breadboardView layer="breadboardWire">\n'
            '                    <geometry x="540.0000" y="144.0000" x1="0" y1="0" '
            'x2="0.0000" y2="27.0000" wireFlags="64" z="3.50002" />\n'
            '                    <wireExtras mils="22.2222" color="#404040" opacity="1" banded="0" />\n'
            '                    <connectors>\n'
            '                        <connector connectorId="connector0" layer="breadboardWire">\n'
            '                            <geometry x="0" y="0" />\n'
            '                            <connects><connect connectorId="pin60A" modelIndex="%(bb)s" '
            'layer="breadboardbreadboard" /></connects>\n'
            '                        </connector>\n'
            '                        <connector connectorId="connector1" layer="breadboardWire">\n'
            '                            <geometry x="0" y="0" />\n'
            '                            <connects><connect connectorId="pin60X" modelIndex="%(bb)s" '
            'layer="breadboardbreadboard" /></connects>\n'
            '                        </connector>\n'
            '                    </connectors>\n'
            '                </breadboardView>\n'
            '            </views>\n'
            '        </instance>' % dict(mi=WIRE_MI, bb=BB_MI))
    new = re.sub(r"(?s)\s*<instance\b[^>]*modelIndex=\"%s\".*?</instance>" % WIRE_MI, "", new)

    # ★★ 图例模板件（`BreadboardLogoText` ✓）：`bb_legend3` 要么从**输入稿**里捞一个、
    #   要么用第 8 个参数外借一个 ✗ —— 外借就得依赖**别的仓库**的文件 ✗
    #   （实测那份在 `f:\git\AuroraTessellation-NFC\…` ✓，本仓交付流水线不该指着它 ✗）
    #   ⇒ **内联骨架** ✓（同模板导线那一手 ✓），整条流水线自洽 ✓、换机器也能重跑 ✓。
    #   ★ 放**图例标题将来要占的那一格**（`x0+色条 9+间隙 3 = 602`, `y=12` ✓）：
    #     形状是**空**的 ⇒ **画布上一个像素也不画** ✓（摆到远处会把画布撑大 ✗、
    #     写个真标题又在跑图例之前成了"无缘无故多一行字"✗ —— §0 第 9 条 ✓）。
    #   ★ 五个属性（logo/color/width/height/shape）图例工具**都会覆盖** ✓
    #     ⇒ `shape` 只放一个**合法占位**（比例与盒子一致 ✓，免得 Fritzing 说渲染不了 ✗）：
    #     SVG 属性用单引号 ✓ ⇒ 写进 `value=` 里不用再夹一层转义 ✓。
    logo = ('        <instance moduleIdRef="BreadboardLogoTextModuleID" modelIndex="%(mi)s" '
            'path=":/resources/parts/core/breadboardlogotext.fzp">\n'
            '            <property name="logo" value="图例：" />\n'
            '            <property name="color" value="#333333" />\n'
            '            <property name="width" value="7.620000" />\n'
            '            <property name="height" value="3.302000" />\n'
            '            <property name="shape" value="&lt;svg xmlns=\'http://www.w3.org/2000/svg\' '
            'height=\'0.130000in\' viewBox=\'0 0 30 13\' width=\'0.300000in\'&gt;'
            '&lt;g id=\'breadboard\'&gt;&lt;/g&gt;&lt;/svg&gt;" />\n'
            '            <property name="lastfilename" '
            'value=":/resources/parts/svg/core/breadboard/breadboardlogo.svg" />\n'
            '            <title>TXT%(mi)s</title>\n'
            '            <views>\n'
            '                <breadboardView layer="breadboard">\n'
            '                    <geometry z="2.50057" x="602.0000" y="12.0000" />\n'
            '                </breadboardView>\n'
            '            </views>\n'
            '        </instance>' % dict(mi=LOGO_MI))
    new = re.sub(r"(?s)\s*<instance\b[^>]*modelIndex=\"%s\".*?</instance>" % LOGO_MI, "", new)
    # ★★ 两个模板件都插在 `<instances>` 的**末尾** ✗✓（2026-10-16 实测 ✓）：
    #   `bb_route4.py:108-110` 挑面包板时是 `"breadboard" in moduleIdRef.lower()` ✗ ——
    #   `BreadboardLogoTextModuleID` **也含 `breadboard`** ✗ ⇒ 它排在板子前面就会被
    #   **当成板子**去解析 ⇒ `:/resources/parts/core/breadboardlogotext.fzp` 直接报
    #   `OSError: Invalid argument` ✗；而渲染次序（`render_bb` 按文档次序画 ✓）又
    #   **要求板子在最先**（板本体不透明，压后面的件 ✗）⇒ 两头都指向同一个写法：
    #   **板子在第一位、模板件在最后** ✓。
    j = new.index("</instance>", new.index('modelIndex="%s"' % BB_MI)) + len("</instance>")
    new = new[:j] + "\n" + wire + "\n" + logo + new[j:]

    try:
        ET.fromstring(new)
    except Exception as ex:
        print("✗✗ 写出来的 XML 解析不了（%s）⇒ **不写文件** ✗" % ex)
        return 1
    if dry:
        print("\n（--dry ⇒ 不写 ✓）")
        return 0
    if dst == src:
        shutil.copy2(src, str(src) + ".bak")
    tmp = str(dst) + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zo:
        for n in z.namelist():
            zo.writestr(n, new.encode("utf-8") if n == name else z.read(n))
    z.close()
    os.replace(tmp, dst)
    print("\n写入 %s ✓（原件备份 %s.bak ✓）" % (dst, src))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
