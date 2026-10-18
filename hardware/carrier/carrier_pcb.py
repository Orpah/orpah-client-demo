# -*- coding: utf-8 -*-
r"""载板 **PCB 视图**：摆位（板框 + 4 个安装孔 + 11 件）⇒ 写 `carrier-pcb.fzz`（**不布线** ✗）

接在 `carrier_board.py` 之后（后者只排原理图 ✓）。**不重跑 `carrier_board.py`** ✗ ——
它会把用户手改定稿的 `carrier-schematic.fzz` 与面包板模板件一起覆盖（见该脚本文件头 ✓）。

★★ 三件必须照做（都踩过/量过 ✓）：
  ① **实例缩进要统一成 2 空格** ✗：源草图里从台架抄来的那 4 个实例是 8 空格缩进 ⇒
     库仓 `pcb_wire.blocks()` 是按「**同缩进配对**」切的 ⇒ **PCB1 被并进上一块** ✗ ⇒
     `board_rect()` 返回 None ⇒ `pcb_check` 跳过"板外检查"、`render_pcb` 报"板框读不出" ✗
     （2026-10-17 实测 ✓）。所以本脚本输出的那份**一律 2 空格** ✓。
  ② **PCB 的旋转写在 `<geometry>` 的**子元素** `<transform m11=…/>`** 里 ✓
     （Fritzing `items/itembase.cpp:341 writeGeometry` → `saveLocAndTransform`
      → `GraphicsUtils::saveTransform`（`utils/graphicsutils.cpp:104`）：identity 才省略 ✓，
      属性写在 `<transform>` 上、不是 `<geometry>` 上 ✗）。
     几何口径：`sketch = loc + M·局部`（平移 0） ⇒ `loc = 目标中心 − M·box中心` ✓
  ③ **安装孔的 `geometry` 不是孔心** ✗：孔心 = `geometry + ring_off`（`part_box.ring_off_mm` ✓，
     "2.2mm,0.0mm" ⇒ +1.66444 mm ✓）。

★ 板尺寸/布局（2026-10-17）：**44 × 32 mm** —— 用户先点头的是 36×28，但**放不下** ✗：
  超容本体 Φ20.5（`body_box` 量出来 20.50×20.50 ✓）＋ 4 个排针（1×4/1×4/1×6/1×7 合计 53.34 mm 长）
  ＋ QFN20 4.10 ＋ SOIC-8 5.88×7.20；36×28 里无论怎么排都会压到超容或角上的安装孔。
  布局：超容占左上大块、芯片在中、**J1/J3 横排贴顶边、J2/J4 横排贴底边**（旋转 90°）、
  4 个 M2 孔在四角（孔心距板边 3 mm）✓

用法： py -3.13 -u -X utf8 carrier_pcb.py [--dry]
"""
import os
import pathlib
import re
import sys
import zipfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = pathlib.Path(__file__).resolve().parent
LIB = pathlib.Path(r"f:\git\fritzing-parts-langhua\tools")
sys.path.insert(0, str(LIB))
import part_box as pbox                                             # noqa: E402

SRC = HERE / "carrier-board.fzz"
DST = HERE / "carrier-pcb.fzz"
LIB_SVG = pathlib.Path(r"f:\git\fritzing-parts-langhua\svg")
SU = 90.0 / 25.4                 # 1 mm = 3.5433 sketch 单位 ✓（AGENTS §15）

# ── 板 ────────────────────────────────────────────────────────────────────────
BOARD_W, BOARD_H = 44.0, 32.0            # mm（见文件头：36×28 放不下 ✓）
BOARD_ORG = (20.0, 20.0)                 # 板左上角（sketch 单位）
HOLE_D, HOLE_RING = 2.2, 0.0             # M2
HOLE_INSET = 3.0                         # 孔心距板边（mm）
Z_BOARD, Z_PART, Z_HOLE = "1.50033", "8.50004", "5.50066"

# ── 摆位：(位号, 本体中心 x mm, y mm, 旋转°) ─────────────────────────────────
PLACE = [
    ("BT1", 14.0, 16.0, 0),      # 超容 Φ20.5：x 3.75~24.25 / y 5.75~26.25
    # ★★ U1 **旋转 270°** ✓（2026-10-18 摆位搜索定的 ✓，证据 = `_scratch/carrier_place_search.py`）：
    #   不转时布线器稳定卡在 **14/15** ✗（`CON_RX`＝`U1.connector17` 与 `SWDIO/SWCLK`
    #   轮流布不通 ✓ —— 它们是 QFN **0.4 mm 脚距**那一排，出口只有朝外一个方向 ✓，
    #   被上面的 `3V3` 干线封住就再也出不去 ✗）。转 270° 后那排脚朝**下**、出口换到
    #   U1↔U2 之间的空档 ⇒ **15/15** ✓（其余摆位一个数没动 ✓）。
    ("U1", 29.0, 10.0, 270),     # QFN20 4.10×4.10
    ("U2", 29.0, 20.0, 0),       # SOIC-8 5.88×7.20
    ("R1", 35.0, 9.0, 0),        # 0603 2.16×0.86
    ("R2", 35.0, 11.5, 0),
    ("R3", 35.0, 14.0, 0),
    ("D1", 39.0, 10.0, 0),       # LED-0603 0.92×2.30
    # ★ 排针整排右移 0.87（2026-10-17 用户「离安装孔太近了」✓，实测量过 ✓）：
    #   左孔壁在 x=4.1、右孔壁在 x=39.9；底排 J2(15.24)+J4(17.78)=33.02 只能占 35.8 ⇒
    #   两侧各 0.28 / 2.01 极不均衡 ⇒ 右移 0.87 后 = 1.15 / 1.14（J1 0.82→1.69）✓
    #   ✗ 再往**左**会撞孔（J2 立刻 <0）✗；想两侧都 ≥2 mm 只能把板加宽到 ≈48 ✓
    ("J1", 10.87, 3.3, 90),      # 1×4 → 横排贴顶边
    ("J3", 22.87, 3.3, 90),
    ("J2", 12.87, 28.7, 90),     # 1×6 → 横排贴底边
    ("J4", 29.87, 28.7, 90),     # 1×7 → 横排贴底边
    # ★ 2026-10-18：**电源开关** SW1（`SK-12D02VG3` 拨动 SPDT，只用一个掷 ✓；额定 0.3A/24V ✓）
    #   本体 DIP3+2PIN ≈ 8.6×4.4 mm ✓ ⇒ 放右下空区（U2 右 / J4 上）✓，离安装孔 (41,29) 够远 ✓
    #   拨向**板内** = 通电 ✓（`1` 接板内 3V3）
    ("SW1", 37.0, 22.0, 90),
]
HOLES = [(HOLE_INSET, HOLE_INSET), (BOARD_W - HOLE_INSET, HOLE_INSET),
         (HOLE_INSET, BOARD_H - HOLE_INSET), (BOARD_W - HOLE_INSET, BOARD_H - HOLE_INSET)]

ROT = {0: (1, 0, 0, 1), 90: (0, 1, -1, 0), 180: (-1, 0, 0, -1), 270: (0, -1, 1, 0)}


def num(v):
    s = ("%.6f" % float(v)).rstrip("0").rstrip(".")
    return s if s else "0"


def pcb_svg(dirname):
    """按部件的 pcbView 的 `image=` 找 svg（库件在仓库里：`svg/<件>/<svg.pcb.<件>_pcb.svg>`）"""
    fp = sorted((LIB_SVG / dirname).glob("part.*.fzp"))[0]
    txt = fp.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"<pcbView[^>]*>([\s\S]*?)</pcbView>", txt)
    img = re.search(r'<layers[^>]*image="([^"]+)"', m.group(1)) if m else None
    if not img:
        return None
    cand = LIB_SVG / dirname / img.group(1).replace("/", "___")
    if cand.is_file():
        return cand
    # 库件的 svg 名字是 `svg.pcb.<件名>_pcb.svg`
    for f in sorted((LIB_SVG / dirname).glob("svg.pcb.*_pcb.svg")):
        return f
    return None


def main(argv):
    dry = "--dry" in argv
    z = zipfile.ZipFile(SRC)
    fz_name = [n for n in z.namelist() if n.endswith(".fz")][0]
    text = z.read(fz_name).decode("utf-8", "replace")

    # ── ① 统一缩进（8 → 2），并去掉 CRLF（后面所有正则都按 \n 写）──
    text = text.replace("\r\n", "\n")
    text = re.sub(r"(?m)^        (<instance\b|</instance>)", r"  \1", text)

    # ── ② 板框：<board> + PCB1 实例（★ 源草图只在 <boards> 里登记了板，
    #      **<instances> 里没有 PCB1 实例** ✗ ⇒ `pcb_wire.board_rect()` 找不到 loc ⇒ 返回 None
    #      ⇒ `pcb_check` 跳过板外检查、`render_pcb` 报“板框读不出” ✗（2026-10-17 实测 ✓）。
    #      ⇒ 本脚本**补一个** PCB1 实例，并放在 `<instances>` **第一位** ✓
    #        （渲染按文档次序画，板本体不透明 ⇒ 排后面把件盖住 ✗，与 `carrier_bb.py` 同一条经验 ✓）。
    text = re.sub(r'(<board\b[^>]*?)\bwidth="[^"]*"', r'\1width="%scm"' % num(BOARD_W / 10.0), text)
    text = re.sub(r'(<board\b[^>]*?)\bheight="[^"]*"', r'\1height="%scm"' % num(BOARD_H / 10.0), text)
    # ★ 不依赖“有没有”的检测：**先删掉任何已有 PCB1 实例，再插一个新的** ✓
    #   （实测：源草图 `<instances>` 里根本没有 PCB1 实例，而 `<boards>` 里那个是
    #    `moduleId=` 不是 `moduleIdRef=` ⇒ 按字符串搜会误判成“已存在” ✗）
    text = re.sub(r'  <instance moduleIdRef="TwoLayerRectanglePCBModuleID"[\s\S]*?</instance>\n?',
                  "", text)
    pcb1 = (
        '  <instance moduleIdRef="TwoLayerRectanglePCBModuleID" modelIndex="5783" '
        'path=":/resources/parts/core/rectangle_pcb_two_layers.fzp">\n'
        '    <property name="layers" value="2" />\n'
        '    <property name="width" value="%s" />\n'
        '    <property name="height" value="%s" />\n'
        '    <title>PCB1</title>\n'
        '    <views>\n      <pcbView layer="board">\n'
        '                    <geometry z="%s" x="%s" y="%s" />\n'
        '      </pcbView>\n    </views>\n  </instance>\n'
        % (num(BOARD_W * SU), num(BOARD_H * SU), Z_BOARD,
           num(BOARD_ORG[0]), num(BOARD_ORG[1])))
    text = text.replace("<instances>\n", "<instances>\n" + pcb1, 1)
    print("② 板框 → %.1f × %.1f mm @ sketch (%.1f, %.1f)（写入 PCB1 实例，排 `<instances>` 首位 ✓）"
          % (BOARD_W, BOARD_H, BOARD_ORG[0], BOARD_ORG[1]))

    # ── ③ 11 件摆位 ──
    dirs = {}
    for m in re.finditer(r'<instance moduleIdRef="([^"]+)"[^>]*>\s*<title>([^<]+)</title>', text):
        dirs[m.group(2).strip()] = m.group(1)
    # 位号 → 库目录（与 carrier_board.SPEC 一致；moduleId 里带后缀，按前缀匹配）
    D2 = {"U1": "CH32V203F8U6", "U2": "ATECC608B", "BT1": "Supercap-1.5F",
          "D1": "LED-0603", "SW1": "SK-12D02VG3"}
    for ref in ("R1", "R2", "R3"):
        D2[ref] = "Resistor-0603"
    for ref in ("J1", "J2", "J3", "J4"):
        D2[ref] = "PinHeader-1x" + {"J1": "4", "J2": "6", "J3": "4", "J4": "7"}[ref]

    # ★★ 2026-10-18：**源草图里没有的件**要先补一个实例 ✓（例：电源开关 `SW1`）——
    #   ① 必须插在 `blocks = text.split(...)` **之前** ✗（否则 `"<instance ".join(blocks)`
    #     会把新插的那个冲掉 ✓；实测：字节数一点没变 ✓）；
    #   ② 写法照 Fritzing 自己的件实例 ✓：`<pcbView layer="copper1">` ＋ `<geometry>` ＋
    #     `<connectors>`（每脚一个、`<geometry x="0" y="0"/>` ✓，与 `BT1` 那些件同形 ✓）。
    for ref, _cx, _cy, _rot in PLACE:
        if re.search(r'<title>%s</title>' % re.escape(ref), text):
            continue
        mid2 = D2[ref]
        fz2 = sorted((LIB_SVG / mid2).glob("part.*.fzp"))[0]
        ncon = len(re.findall(r'<connector id="connector\d+"',
                              fz2.read_text(encoding="utf-8", errors="replace")))
        cons = "\n".join(
            '        <connector connectorId="connector%d" layer="copper1">\n'
            '          <geometry x="0" y="0" />\n        </connector>' % i
            for i in range(ncon))
        blk = (
            '  <instance moduleIdRef="%s" modelIndex="%d" '
            'path="C:/Users/shi.jinghai-honor/Documents/Fritzing/parts/user/%s.fzp">\n'
            '    <title>%s</title>\n'
            '    <views>\n      <pcbView layer="copper1">\n'
            '                    <geometry z="%s" x="0" y="0" />\n'
            '        <connectors>\n%s\n        </connectors>\n'
            '      </pcbView>\n    </views>\n  </instance>'
            % (mid2, 90000098, mid2, ref, Z_PART, cons))
        text = text.replace("</instances>", blk + "\n</instances>")
        print("③ 补实例：%s（%s，%d 脚）" % (ref, mid2, ncon))

    blocks = text.split("<instance ")

    ok = 0
    for ref, cx, cy, rot in PLACE:
        svg = pcb_svg(D2[ref])
        if not svg:
            print("   ✗ %s 找不到 pcb svg（%s）" % (ref, D2[ref]))
            continue
        box = pbox.body_box(svg)
        if not box:
            print("   ✗ %s 的 body_box 量不出" % ref)
            continue
        c = ((box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0)
        m11, m12, m21, m22 = ROT[rot]
        # M·c
        mx = m11 * c[0] + m21 * c[1]
        my = m12 * c[0] + m22 * c[1]
        tx = BOARD_ORG[0] + cx * SU
        ty = BOARD_ORG[1] + cy * SU
        loc = (tx - mx, ty - my)
        tf = "" if rot == 0 else (
            '\n      <transform m11="%d" m12="%d" m13="0" m21="%d" m22="%d" m23="0" '
            'm31="0" m32="0" m33="1" />' % (m11, m12, m21, m22))
        geom = '<geometry z="%s" x="%s" y="%s"%s' % (Z_PART, num(loc[0]), num(loc[1]),
                                                      (">" + tf + "\n     </geometry>")
                                                      if tf else " />")
        # 替换该位号的 pcbView 里的 geometry
        for i, b in enumerate(blocks):
            t = re.search(r"<title>([^<]+)</title>", b)
            if not t or t.group(1).strip() != ref:
                continue
            nb, n = re.subn(r"(<pcbView\b[^>]*>)\s*<geometry\b[^>]*?(/>|>[\s\S]*?</geometry>)",
                            r"\1\n                    " + geom.replace("\\", "\\\\"), b, count=1)
            if n:
                blocks[i] = nb
                ok += 1
                rot_s = (" %.0f°" % rot) if rot else ""
                print("   %-4s 中心 (%5.1f, %5.1f) mm%s  本体 %.2f×%.2f   loc=(%.1f, %.1f)"
                      % (ref, cx, cy, rot_s, (box[2] - box[0]) / SU, (box[3] - box[1]) / SU,
                         loc[0], loc[1]))
            else:
                print("   ✗ %s 没找到 pcbView/geometry" % ref)
            break

    text = "<instance ".join(blocks)

    # ── ④ 4 个安装孔 ──
    off = pbox.ring_off_mm(HOLE_D, HOLE_RING) * SU        # geometry ≠ 孔心 ✓
    holes = []
    for k, (hx, hy) in enumerate(HOLES, 1):
        x = BOARD_ORG[0] + hx * SU - off
        y = BOARD_ORG[1] + hy * SU - off
        holes.append(
            '  <instance moduleIdRef="HoleModuleID" modelIndex="%d" '
            'path=":/resources/parts/core/hole.fzp">\n'
            '    <property name="hole size" value="%.1fmm,%.1fmm" />\n'
            '    <title>H%d</title>\n'
            '    <views>\n      <pcbView layer="copper0">\n'
            '                    <geometry z="%s" x="%s" y="%s" />\n'
            '      </pcbView>\n    </views>\n  </instance>'
            % (90014000 + k, HOLE_D, HOLE_RING, k, Z_HOLE, num(x), num(y)))
    text = text.replace("</instances>", "\n".join(holes) + "\n</instances>")
    print("④ 安装孔 4 × Φ%.1f mm（孔心 %s）" % (HOLE_D, HOLES))

    if dry:
        print("（--dry：不写文件）")
        return 0
    with zipfile.ZipFile(DST, "w", zipfile.ZIP_DEFLATED) as zo:
        zo.writestr(fz_name, text)
        for n in z.namelist():
            if n != fz_name:
                zo.writestr(n, z.read(n))
    print("✓ 写出 %s（%d 字节）" % (DST.name, os.path.getsize(DST)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
