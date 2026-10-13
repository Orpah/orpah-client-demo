# -*- coding: utf-8 -*-
r"""造载板的「**含元件、零导线**」原理图源草图（= 画图管线的唯一缺口 ✓）。

做法（两步走 ✓，与像素板那条管线对齐 ✓）：
  ① **粗摆**：所有件先按固定间距放一排 ✓（间距够大 ⇒ 不会重叠 ✓，也不必先知道尺寸 ✓）
  ② **精摆**：跑库仓 `render_sch.py --pins-out` 拿到每件的**本体框** ✓ ⇒ 按 `GAP` 重新排一行
     （本体框中心对齐 ✓）⇒ 再写一版 ✓

骨架从**现成的台架草图**抄（`cstep-ch347f-atecc608b.fzz` ✓）：只保留它的 `<boards>`/板框件 `PCB1` ✓，
其余实例（含所有导线 ⇒ **零导线** ✓）与悬空 `<connects>` 一律删掉 ✓ —— 这样 Fritzing 要的
外层结构一定是**它自己写的形状** ✓（不是我编的 ✗）。

用法（在 `hardware/carrier/` 下）：
    py -3.13 carrier_board.py            # ①粗摆 ②精摆（会自己调 render_sch ✓）⇒ carrier-board.fzz
    py -3.13 carrier_board.py --rough    # 只出粗摆（调试用 ✓）
"""
import os
import pathlib
import re
import subprocess
import sys
import zipfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
SKEL = REPO / "hardware" / "wiring" / "cstep-ch347f-atecc608b.fzz"
SKEL_FZ = "cstep-ch347f-atecc608b.fz"
LIB_TOOLS = pathlib.Path(r"f:\git\fritzing-parts-langhua\tools")
LIB_SVG = pathlib.Path(r"f:\git\fritzing-parts-langhua\svg")
MINE = pathlib.Path(r"C:\Users\shi.jinghai-honor\Documents\Fritzing\parts")
OUT = HERE / "carrier-board.fzz"
WORK = HERE / "_work"
GRID = 7.2                 # Fritzing 原理图网格 0.1 in ✓
GAP = 2 * GRID             # 相邻本体框间隙 ✓（抄像素板定下来的 2 格 ✓）

# ── 元件表 ───────────────────────────────────────────────────────────────────
#   位号 / moduleId / fzp 路径 / 脚数 / 标题文字
#   ★ 库件 = `MINE/user/<moduleId>.fzp` ✓（部署口径见库仓 `check_deploy.py` ✓）
#   ★ 核心件 = `:/resources/parts/core/<file>` ✓
#   ★★ 2026-10-13：`J1–J4` 原来用 `NetLabel-Pad`（1 脚）+ 标题写网名**占位** ✗ ——
#     当时本库**没有**排针件 ✗；现已有 **`PinHeader-1x2…1x10`** ✓（本库新件 ✓，
#     几何照 Fritzing 核心库的 3 脚 0.1in 公排针 ✓）⇒ 换成真件 ✓。
#     ★ 排针是**通用件** ✓（脚名 = `1`…`N` ✓）⇒ **信号名不写脚名** ✗、由连线表达 ✓
#     （对应关系见 `carrier_nets.py` 末尾那张表 ✓）。
U = "C:/Users/shi.jinghai-honor/Documents/Fritzing/parts/user"

# ★★ 2026-10-13 实测定下的口径 ✗：**一律用库件** ✓ ——
#   核心件（`path=":/resources/parts/core/…"` ✗）**我们的工具链解析不了** ✗
#   （实测：`render_sch --pins-out` 对 17 件只认 15 件 ✓，丢的正是那两只核心件 ✗）
#   ⇒ 库件的 moduleId / fzp 名**从部件目录里读** ✓（不写死 ✗，免得改名就漂 ✗）。
LIB_DIRS = ["CH32V203F8U6", "ATECC608B", "Resistor-0603", "Capacitor-0603",
            "SOD-123", "PinHeader-1x4", "PinHeader-1x6", "PinHeader-1x7"]
# ★★ **核心件**（Fritzing 自带 ✓）2026-10-13 实测定口径 ✗：
#   · 核心件的 `path` **必须写绝对路径** ✓（`…/Fritzing/fritzing-parts/core/<文件>.fzp` ✓）——
#     ✗ 写成 Fritzing 内部记号 `:/resources/parts/core/…` 我们的工具链**读不到** ✗
#     （`part_measure.py` 明写：core 件不在 .fzz 里 ⇒ 按实例给的**绝对路径**读它的 .fzp ✓）。
#   · 但 Fritzing 那只**泛用排针/Generic IC 是运行时生成的** ✗ —— `fritzing-parts/core/` 里
#     **没有它的 .fzp** ✗ ⇒ 工具链读不到 ✗（只能用真存在的核心件 ✓，或自己在库里做 ✓）。
FRITZ_CORE = pathlib.Path(r"C:\Users\shi.jinghai-honor\AppData\Local\Programs\Fritzing\fritzing-parts\core")


def core_part(filename):
    """核心件 ⇒ `(moduleId, 绝对路径)` ✓（moduleId 从它的 fzp 里读 ✓）"""
    f = FRITZ_CORE / filename
    if not f.exists():
        raise SystemExit("✗ 核心件不存在：%s" % f)
    m = re.search(r'moduleId="([^"]+)"', f.read_text(encoding="utf-8", errors="replace"))
    if not m:
        raise SystemExit("✗ %s 里没读到 moduleId" % filename)
    return m.group(1), str(f).replace("\\", "/")


def lib_part(dirname):
    """按库件目录名 ⇒ `(moduleId, fzp 路径)` ✓（moduleId = fzp 里的 `moduleId` ✓，与部署口径一致 ✓）"""
    fzps = sorted((LIB_SVG / dirname).glob("part.*.fzp"))
    if not fzps:
        raise SystemExit("✗ 库里没有部件：%s" % dirname)
    f = fzps[0]
    m = re.search(r'moduleId="([^"]+)"', f.read_text(encoding="utf-8", errors="replace"))
    if not m:
        raise SystemExit("✗ %s 里没读到 moduleId" % f.name)
    return m.group(1), "%s/%s.fzp" % (U, m.group(1)), dirname


# (位号, 部件目录, 实例标题) —— ★ 标题**必须是纯位号** ✗（`sch_route.py` 拿 `<title>` 当**位号**
#   去 `insts[ref]` 查实例 ✗ —— 写成 `BT1 5.5V 超容` 会 KeyError ✗，2026-10-13 实测 ✓）
#   ⇒ 说明文字只留在注释里 ✓。
SPEC = [
    ("U1", "CH32V203F8U6", "U1"),                  # MCU ✓
    ("U2", "ATECC608B", "U2"),                     # 安全元件 ✓
    ("R1", "Resistor-0603", "R1"),                 # 4.7k，I2C SDA 上拉 ✓
    ("R2", "Resistor-0603", "R2"),                 # 4.7k，I2C SCL 上拉 ✓
    ("R3", "Resistor-0603", "R3"),                 # 10k，心跳灯限流 ✓
    # D1 心跳灯：用 Fritzing 的 **SMD 0603 LED 核心件** ✓（真件 ✓，不再是占位 ✗）
    ("D1", "core:SMD_LED_generic_0603.fzp", "D1"),
    # BT1 = 5.5 V 超容 ✓ ⇒ 用核心件里的 **goldcap（超容）** ✓（真件 ✓）
    ("BT1", "core:capacitor_elko_goldcap_4.0F_92eef0d1d2f548b9db9918a1769107d3_2.fzp",
     "BT1"),
    # ── 外部接口（★ 2026-10-13 起用**真排针** ✓，不再是 NetLabel-Pad 占位 ✗）──────────
    #   脚号 ↔ 信号名见 `carrier_nets.py` 末尾 ✓（出处 `carrier-connections.md` §2 ✓）
    ("J1", "PinHeader-1x4", "J1"),                 # 模组口 ✓
    ("J2", "PinHeader-1x6", "J2"),                 # 台架/产线 ✓
    ("J3", "PinHeader-1x4", "J3"),                 # SWD ✓
    ("J4", "PinHeader-1x7", "J4"),                 # CJMCU-2557 前端模块 ✓
]


def build_parts():
    """⇒ `[(位号, moduleId, fzp 路径, 脚数, 标题, 部件目录), …]` ✓（脚数 = fzp 里 `<connector>` 个数 ✓）"""
    out = []
    for ref, d, title in SPEC:
        if d.startswith("core:"):               # ★ 核心件：绝对路径 ✓、不内嵌 ✓
            mid, path = core_part(d.split(":", 1)[1])
            n = len(re.findall(r"<connector\b",
                               pathlib.Path(path).read_text(encoding="utf-8", errors="replace")))
            out.append((ref, mid, path, n, title, None))
            continue
        mid, path, dirname = lib_part(d)
        n = len(re.findall(r"<connector\b",
                           sorted((LIB_SVG / dirname).glob("part.*.fzp"))[0]
                           .read_text(encoding="utf-8", errors="replace")))
        out.append((ref, mid, path, n, title, dirname))
    return out


PARTS = build_parts()

# 精摆的排布顺序 ✓（左→右：电源/储能 → MCU → SE → 上拉 → 灯 → 四个外部接口 ✓）
ORDER = ["BT1", "U1", "U2", "R1", "R2", "R3", "D1", "J1", "J2", "J3", "J4"]

VIEWS = [("breadboardView", "breadboard", "2.50002"),
         ("schematicView", "schematic", "2.50002"),
         ("pcbView", "copper1", "8.50004")]


def inst_xml(mi, mid, path, npins, title, x, y):
    """一只元件的实例 XML ✓（三个视图各一份 ✓；★ 脚的 `<geometry x="0" y="0"/>` ✓ ——
    脚位由**部件 svg + transform** 决定 ✓，不用我算 ✓；**零 `<connects>`** ✓）"""
    def conns(layer):
        return "\n".join(
            '      <connector connectorId="connector%d" layer="%s">\n'
            '        <geometry x="0" y="0" />\n'
            '      </connector>' % (i, layer)
            for i in range(npins))

    views = "\n".join(
        '    <%s layer="%s">\n'
        '      <geometry z="%s" x="%s" y="%s" />\n'
        '      <connectors>\n%s\n      </connectors>\n'
        '    </%s>' % (v, layer, z, fmt(x), fmt(y), conns(layer), v)
        for v, layer, z in VIEWS)
    return ('  <instance moduleIdRef="%s" modelIndex="%d" path="%s">\n'
            '    <property name="part number" value="%s" />\n'
            '    <title>%s</title>\n'
            '    <views>\n%s\n    </views>\n'
            '  </instance>' % (mid, mi, path, mid, title, views))


def fmt(v):
    s = ("%.4f" % float(v)).rstrip("0").rstrip(".")
    return s if s else "0"


def build_skeleton():
    """从台架草图抄骨架 ✓：留下 `<boards>` 与板框件 `PCB1` ✓，删掉别的实例与全部 `<connects>` ✓"""
    z = zipfile.ZipFile(SKEL)
    raw = z.read(SKEL_FZ).decode("utf-8", errors="replace")
    raw = re.sub(r"\n\s*<connects>.*?</connects>", "", raw, flags=re.S)   # 悬空连接 ✓
    keep_pcb = re.search(r"<instance[^>]*moduleIdRef=\"TwoLayerRectanglePCBModuleID\".*?</instance>",
                         raw, re.S)
    pcb = keep_pcb.group(0) if keep_pcb else ""
    head = raw[:raw.index("<instances")] if "<instances" in raw else raw
    return head, pcb


def emit(place, out_path):
    head, pcb = build_skeleton()
    blocks, mi_of = [], {}
    for i, (ref, mid, path, npins, title, _d) in enumerate(PARTS):
        mi = 90000000 + i
        mi_of[ref] = mi
        x, y = place.get(ref, (0.0, 0.0))
        blocks.append(inst_xml(mi, mid, path, npins, title, x, y))
    body = ("<instances>\n"
            + (pcb + "\n" if pcb else "")
            + "\n".join(blocks) + "\n</instances>\n</module>\n")
    # ★ 外层结构一字不动 ✓，只把 `<instances>` 换成我们自己那份 ✓
    head = re.sub(r"<instances.*\Z", "", head, flags=re.S)
    text = head + body
    # 内嵌用到的**库件**（fzp + 4 个视图 svg ✓）——与 Fritzing 自己打包的口径一致 ✓
    used = sorted({d for *_x, d in PARTS if d})
    emb = [f for d in used for f in sorted((LIB_SVG / d).glob("*"))
           if f.is_file() and (f.suffix == ".fzp" or f.name.startswith("svg."))]
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zo:
        zo.writestr("carrier-board.fz", text)
        for f in emb:
            zo.write(f, f.name)
    return text, emb, mi_of


def pins_out(fzz, tag):
    WORK.mkdir(exist_ok=True)
    pins = WORK / ("pins_%s.py" % tag)
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.run([r"C:\Python313\python.exe", str(LIB_TOOLS / "render_sch.py"), str(fzz),
                        str(WORK / ("%s.png" % tag)), "--pins-out", str(pins)],
                       cwd=str(LIB_TOOLS), capture_output=True, encoding="utf-8",
                       errors="replace", env=env)
    return r.returncode, r.stdout or "", r.stderr or "", pins


def read_box(pins):
    """从 pins_ref.py 抠出 {modelIndex: (x0,y0,x1,y1)}（**纯数据文件** ✓）"""
    txt = pins.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^BOX = (\{.*?\})\s*$", txt, re.M | re.S)
    if not m:
        return {}
    return eval(m.group(1))                      # noqa: S307 —— 它就是我们自己工具生成的纯数据 ✓


def main(argv):
    rough = "--rough" in argv
    if WORK.exists() is False:
        WORK.mkdir(parents=True, exist_ok=True)

    # ① 粗摆：固定间距一排 ✓
    place = {}
    x = 0.0
    for ref in ORDER:
        place[ref] = (x, 0.0)
        x += 120.0
    tmp = WORK / "rough.fzz"
    emit(place, tmp)
    print("① 粗摆 → %s" % tmp)

    rc, out, err, pins = pins_out(tmp, "rough")
    print("   render_sch --pins-out rc=%d  pins=%s" % (rc, pins.exists()))
    if rc != 0:
        print("   !! rc≠0，stderr 尾 20 行：\n%s" % "\n".join(err.splitlines()[-20:]))
        return 1
    boxes = read_box(pins)

    if rough:
        return 0

    # ② 精摆：按本体框中心对齐 + GAP 间隙 ✓（几何来自**工具** ✓，不是我拍的数 ✓）
    _t, _e, mi_of = emit(place, WORK / "x.fzz")
    place2, x, bx = {}, 0.0, 0.0
    for ref in ORDER:
        mi = mi_of.get(ref)
        b = boxes.get(str(mi)) or boxes.get(mi)
        if not b:
            # ★ 网标签件**没有“本体框”** ✓（它的旗标本体见库仓 `sch_net.py` ✓）
            #   ⇒ 摆到**第二排** ✓（粗摆那排是 x=120 递增 ✗ ⇒ 会把画布拉到 2000 单位 ✗）
            #   ★ 旗标从锚点**向左上**伸 ✓ ⇒ 间距给足 ✓、整排放在第一排**下面** ✓
            place2[ref] = (bx, -100.0)
            bx += 60.0
            continue
        if not b:
            print("   !! 没拿到 %s 的本体框（mi=%s）⇒ 摆到第二排" % (ref, mi))
            place2[ref] = (bx, -100.0)
            bx += 60.0
            continue
        w = b[2] - b[0]
        cy = (b[1] + b[3]) / 2.0
        place2[ref] = (x - b[0], -cy)          # 本体框左缘落在 x ✓、框心对 0 行 ✓
        x += w + GAP
    emit(place2, OUT)
    print("② 精摆 → %s（%d 件）" % (OUT, len(PARTS)))

    rc, out, err, pins2 = pins_out(OUT, "final")
    print("   复核 render_sch rc=%d" % rc)
    for l in (out or "").splitlines():
        if any(k in l for k in ("本体盒", "画布", "位号", "无法判定", "悬空")):
            print("   " + l.strip()[:160])
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
