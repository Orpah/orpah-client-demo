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

★★ **出交付件**（`carrier-schematic.*`）的**整条命令** ✓（2026-10-14 ✓，= 本次交付的 `c1` 版 ✓）：
    # ① 源草图（含元件、零导线 ✓）；它顺带写出 `_work/pins_final.py`（脚位几何 ✓）
    py -3.13 carrier_board.py
    # ② 出“尺子”（`render_sch` 自己出的 svg = **sketch 单位** ⇒ 布线器要 `--ratio 1.0` ✓；
    #    对 Fritzing 自己导出的 svg 才是 1.25 ✓）
    py -3.13 <库仓>\\tools\\render_sch.py carrier-board.fzz _work\\ruler.png
    # ③ 布线 ⇒ 交付件（★ 下面那几个开关**一个都不能省** ✗，理由见紧跟的两条 ★）
    py -3.13 <库仓>\\tools\\sch_route.py carrier-board.fzz _work\\ruler.svg carrier-schematic.fzz `
        --ratio 1.0 --nets=carrier_nets.py --pins=_work\\pins_final.py `
        --orig <本仓>\\hardware\\wiring\\cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz `
        --rails=3V3,GND --text-gap --choffs 7.3,12,22,34 `
        --no-mst-merge --no-mst-shape
    # ④ 出图 + 逐网核对 ＋ **线色闸门**（`check_netlist` / `file_sanity` **都必须 rc=0** ✓）
    #    ★ `file_sanity` 查**两个视图**的线色 ✓（①面包板 / ①b原理图 ✓ —— 2026-10-16 补 ✓，
    #      用户定「原理图线色闸门必须补」✓；色表唯一出处 = 库仓 `tools/fz_colors.py` ✓）
    py -3.13 <库仓>\\tools\\render_sch.py carrier-schematic.fzz carrier-schematic.png
    py -3.13 <库仓>\\tools\\check_netlist.py carrier-schematic.fzz --nets=carrier_nets.py

★ **`--choffs 7.3,12,22,34` 不能省** ✗（2026-10-14 量出来的 ✓）：库里的缺省第一档是
  `CH_OFFS[0] = CLEAR = 6.0` ✗，它 **< `CLEAR_PIN` 7.2** ✗；而包围盒**含引脚引线** ⇒
  用第一档走廊跑的主干离旁边的引脚端点**正好 6.00** ✗ ⇒ **必然贴脚** ✓。实测同一画布：
  缺省 `--choffs` ⇒ 贴脚 **8** ✗；`7.3,12,22,34` ⇒ 贴脚 **1** ✓。
  （✗ 没有去改库里的缺省值 ✗ —— 那会连带影响别的板子 ✓。）

★ **`--no-mst-merge --no-mst-shape` 是显式关** ✗（2026-10-14 实测这两个开关**不能开** ✓）：
  开着 ⇒ MST 把 `J4` 的 `3/5/6` 并成一簇，两条并线沿 `J4` 自己的引脚列跑 ⇒ 代表脚
  `J4.connector3` **自己接轨时一条合法候选都没有** ✗ ⇒ 严格模式拒画 4 处 ⇒ `3V3` **断** ✗；
  同批还把 `GND` 的 `BT1` 支线挤成**折返** ✗ ⇒ **不同的导线重叠 3 对** ✗（硬规则要 0 ✗）⇒ 退出码 1 ✗。
  两个开关**当前缺省就是关** ✓，这里显式写出来是为了**把交付件钉死** ✗（将来谁改了缺省也不受影响 ✓）；
  完整证据 / 对比数据在库仓 `sch_route.py` 的 `MST_MERGE` 定义处 ✓。
"""
import math
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
# ★★ 2026-10-13 实测调大 ✗：原来是 `2 * GRID`（= 14.4 单位 ≈ 4 mm，抄像素板 ✓）——
#   像素板元件少、通道够 ✓；本板 **11 件、15 张网**（`3V3` 11 脚 / `GND` 7 脚）⇒ 太挤 ✗，
#   布线器反复报「**没有一条候选**能…」✗，硬约束「线↔线净距 ≥ `--wire-gap`（1.000 mm）」
#   实测 **37 对不足**（其中 3 对 0.0000 mm **完全贴** ✗）⇒ rc=1 ✗。
#   ⇒ 间隙放大到 **4 格 ≈ 8.1 mm** 给布线留通道 ✓（一次只改这一个数 ✓）。
# ★★ 2026-10-13 再调到 **6 格 ≈ 12.2 mm** ✓（用户报「`D1.C` 接上了、**另一端悬空**」✗ 之后 ✓）：
#   查明是 **`LED_R`（`R3.1 → D1.0`）被严格模式拒画** ✓ ——
#   「**没有一条候选**能与已布的线净距 ≥ 1.000 mm」✗ ⇒ 那条走廊凑不出 1 mm ✗。
#   而 `D1` 刚因为 **px 口径修正**（`sch_box` 25.4/90 → 25.4/72 ✓）**变大 25%** ✓
#   ⇒ 原来刚够的空档变成了不够 ✗。⇒ 先把最通用的旋钮（**间隙**）加大 ✓，
#   与像素板当年「1 格 → 2 格 ⇒ 穿体 10→6 ✓」同一手法 ✓（✗ 不动判据、不放松闸门 ✗）。
# ★★ 2026-10-14 再调到 **12 格 ≈ 24.4 mm** ✓ —— 这一次是**逐档量出来的**（不是拍的 ✓）：
#   量法 = 只动 `GAP` 这一个变量，每档跑一遍完整流水线（重出画布 → 出尺子 → 布线 →
#   `check_netlist.py`），三档结果：
#     · 6 格（原值）⇒ 4 张网对不上 ✗（`3V3`/`SDA`/`SCL`/`VBAT_OK`），拒画 12 处；
#     · 8 / 10 格 ⇒ 拒画降到 8 处、`3V3`/`VBAT_OK` 通了 ✓，但 `SDA`/`SCL` 仍少脚 ✗；
#     · **12 格 ⇒ 拒画 8→0**（再配合下面右列次序）✓。
#   ★ 什么被撑开了 ✗：`R1`（下排最右）与 `J2`（右列）原本只隔 **0.79 单位 = 0.22 mm** ✗ ——
#     `R1` 的 3V3 脚**朝左**，那条出脚走廊窄得没路 ✗（布线器报「没有一条候选能避开别人的本体」✓）。
#   ★ 13 / 14 格也量过 ✓：13 格虽把贴脚压到 0 ✓，但 `GND` 断 ✗；14 格交叉降到 47 ✓，但 `SDA` 断 ✗ ⇒
#     当时结论 = **只有 12 格是 14/14 全对的** ✓（非单调，所以是逐档量出来的 ✓）。
#   ★★ 2026-10-14 ✓ **`--choffs` 一加，这个结论就变了** ✗（上面那批是**没有** `--choffs` 时量的 ✓）：
#     真因是布线器**走廊首档** `CH_OFFS[0] = CLEAR = 6.0 < CLEAR_PIN 7.2` ✗ ⇒ 用首档走廊跑的
#     主干**必然贴脚** ✓（详见用法里那条 ★ ✓）。**补上 `--choffs 7.3,12,22,34` 之后重量** ✓：
#       · **11 格 ⇒ 贴脚 1 ｜ 交叉 54 ｜ 擦身 51 ｜ 拒画 0 ✓ ｜ 重叠 0 ✓ ｜ 网表 ✓ 14/14 ✓
#         ｜ 总长 7590 ✓（= 本次交付的 `c1` ✓）**
#       · 12 格 ⇒ 贴脚 1 ｜ 交叉 **53** ✓ ｜ 擦身 **41** ✓ ｜ 拒画 0 ✓ ｜ 重叠 0 ✓ ｜ 网表 ✓ 14/14 ✓
#         ｜ 总长 7840 ✗（**多 250 单位 ≈ 70 mm** ✗）
#     ⇒ 两档都**达标** ✓、**贴脚那 1 处与 `GAP` 无关** ✓（同一根 `SDA` 从 `J2.connector2`
#       **45° 斜着出脚** ✗ ⇒ 到相邻脚 `9.0 × cos45° = 6.364 < 7.15` ✗ —— 它只与**出脚角度**有关 ✓）。
#     ★ 本文件取 **11 格** ✓（= 用户手上那版 `c1` ✓，两边对得上 ✓）；要换成 12 格只需改回这一行 ✓
#       （可读性两项更少 ✓，代价是总长 +70 mm ✗）。
GAP = 11 * GRID            # 相邻本体框间隙 ✓

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
            "SOD-123", "LED-0603", "PinHeader-1x4", "PinHeader-1x6", "PinHeader-1x7"]
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
    # D1 心跳灯 = **本库自己的 `LED-0603`** ✓（2026-10-15 从 core 件换过来 ✓）
    #   ★★ 为什么换 ✗（用户 2026-10-15 截图报 ✓）：core 那只 SparkFun 件的 schematic 里，
    #     **两个端子画在引脚线的中点** ✗ ⇒ Fritzing 把从它接出去的导线端**吸在引线中间** ✗
    #     （用户的图：从 2 脚出去的线端正落在**阴极杠**上 ✓）。
    #   ✓ 本库新件 `LED-0603` 的 schematic 是**自绘**的 ✓：两端子显式落在引脚线的**外端** ✓
    #     （生成器出处 `svg/LED-0603/gen_part.py`；那条 `min(x0, x1)` 踩坑也已修 ✓）。
    #   ★ 脚序依旧：`connector0 name=A`（**阳极** ✓）/ `connector1 name=C`（**阴极** ✓）
    #     ⇒ 与 `carrier_nets.py` 的 `LED_R→#1`（限流电阻之后 = 阳极 ✓）/ `LED_HB→#2`
    #     （MCU 脚 = 阴极，低电平点亮 ✓）对得上 ✓。
    #   ★ 历史（2026-10-13，已作废 ✗）：先前用过 `SMD_LED_generic_0603.fzp`（**泛用壳** ✗ ——
    #     icon/breadboard 指向 5mm 直插 ✗、schematic 的脚包在 `<g>` 里 ⇒ `pins_of` 一只都认不出 ✗）；
    #     随后换成 core 的 SparkFun 件 ✓ —— 它的**端子位置**就是本次换掉的原因 ✓。
    ("D1", "LED-0603", "D1"),
    # BT1 = 5.5 V 超容 ✓ ⇒ 用**我们库里**的真超容件 ✓
    #   ★★ 2026-10-13 用户定 ✓（原话：「**BT1 应该带电容值**」✗）：
    #     ✗ 原先用**核心库**的 goldcap（`core:capacitor_elko_goldcap_4.0F_…` ✗）——
    #       它的 `.fzp` 里 `capacitance` **没设 `showInLabel=\"yes\"`** ✗（实测 ✓）
    #       ⇒ Fritzing **不把容值画进标签** ✗；而核心件在 Fritzing 安装目录里 ✗
    #       （不内嵌进 `.fzz` ✓、我们**改不了** ✗）⇒ 只能**换件** ✓。
    #     ✓ 换成 `Supercap-1.5F` ✓（用户 2026-10-13 选 ✓）—— 它是**我们库件** ✓、
    #       `capacitance` 已带 `showInLabel=\"yes\"` ✓ ⇒ 标签里会显示 `1.5F` ✓；
    #       而且它是**库件** ⇒ 会随 `.fzz` 内嵌 ✓（不再依赖 Fritzing 安装目录 ✓）。
    ("BT1", "Supercap-1.5F", "BT1"),
    # ── 外部接口（★ 2026-10-13 起用**真排针** ✓，不再是 NetLabel-Pad 占位 ✗）──────────
    #   脚号 ↔ 信号名见 `carrier_nets.py` 末尾 ✓（出处 `carrier-connections.md` §2 ✓）
    ("J1", "PinHeader-1x4", "J1"),                 # 模组口 ✓
    ("J2", "PinHeader-1x6", "J2"),                 # 台架/产线 ✓
    ("J3", "PinHeader-1x4", "J3"),                 # SWD ✓
    ("J4", "PinHeader-1x7", "J4"),                 # CJMCU-2557 前端模块 ✓
]

# ★★ 元件标签（实例属性）—— 2026-10-13 用户截图点名后定 ✓
#   病（原话 ✓）：「**电阻阻值、电容容量，都缺失；元件号仍然在，应该删除；芯片名被遮挡**」✗
#   真因（**拿 Fritzing 自己写的台架草图当标准答案**量出来的 ✓，不是猜 ✗）：
#     · Fritzing 的元件标签 = **`<title>`（位号）** ＋ **实例里写了值的 `showInLabel="yes"` 属性** ✓；
#     · 台架草图的电阻实例写的是 `<property name="tolerance" …/>` / `power` / **`resistance`** ✓，
#       **没有** `<property name="part number">` ✗ —— 那是**我们多写的** ✗，值又是 moduleId ✗
#       ⇒ 图上显示一长串 `R0603_36137a0f…-ResistorModuleID` ✗，还把 `U1` 的名字**遮住**了 ✗。
#   ⇒ 三条一起改 ✓：① **删 `part number`** ✗；② 属性**从 `.fzp` 读** ✓（`showInLabel="yes"` 的那些 ✓
#     —— 唯一来源 ✓ 不手抄 ✗），本表只写**与默认不同**的 ✓；③ **位号只给芯片** ✓
#     （用户 2026-10-13 定 ✓：「**芯片元件号正确；其它元件不需要元件号**」✗ ⇒ 见 `CHIP_REFS` ✓）。
PROPS = {
    "R1": {"resistance": "4.7k"},                # I2C SDA 上拉 ✓
    "R2": {"resistance": "4.7k"},                # I2C SCL 上拉 ✓
    "R3": {"resistance": "10k"},                 # 心跳灯限流 ✓（`.fzp` 默认是 4.7k ⇒ 这里覆写 ✓）
}
# ★ 谁显示位号 ✗：**所有件都显示** ✓（用户 2026-10-14 定 ✓：「应该显示 D1/J1/J2/J3/J4」✗）。
CHIP_REFS = ("U1", "U2")
# ★★ 谁**额外**显示“型号”那一行 ✗（= `part number` ✓）—— 用户 2026-10-14 定 ✓：
#   · **芯片**：截图 `U1 / CH32V003F4U6` ✓ ⇒ 位号 + 型号 ✓；
#   · **接口**：截图 `J2 / SH1.0-3P-LT` ✓ ⇒ 位号 + 型号 ✓；
#   · **二极管**：同一手先定“有 D1 就够了”✗ ⇒ 随即更正为「**二极管也应该有元件型号**」✓
#     ⇒ 也进本表 ✓（它的型号 = core 件的 `part number` = `SparkFun-LED-LED-0603` ✓）。
SHOW_MODEL = ("U1", "U2", "D1", "J1", "J2", "J3", "J4")
# ★ 型号那一行的**覆写** ✓（用户 2026-10-14 问：「**D1 的型号应该是 LED-0603 吧**？」✓）
#   ★ 为什么需要它 ✗：`D1` 用的是 **core 件** `sparkfun-led-led-0603.fzp` ✓ ——
#     它的 `.fzp` 里**根本没有 `part number`** ✗（实测：只有 `package`/`family`/`variant` ✓）
#     ⇒ 即使 `displayKey` 列了这一行，`displayTexts()` 也会因**值为空**而跳过 ✗
#     ⇒ 所以**必须在这里显式给一个值** ✓。
#   ✓ 其余件（`U1` = `CH32V203F8U6` ✓、`J1` = `PinHeader-1x4` ✓）用件自带的 ✓，不写 ✓。
#   ★★ 注意 ✗：这本表**先装覆写** ✓，然后 `build_parts()` 用 `setdefault` **只补件自带的** ✓
#     —— ✗ 早先写成 `PARTNUM = {}` 把它**整个覆盖** ✗ ⇒ `D1` 的型号静默丢了 ✗（实测 ✓）。
#     为什么必须**在实例里写出值** ✗（不能指望回落 `.fzp` ✓）：实测实例里没有该属性时
#     `check_displaykeys` 读到的是 `'—'` ✗；而标签那一行取的是 `m_owner->getProperty(key)` ✓
#     —— 写出来就**不依赖**它的回落行为 ✓（两种实现下结果一致 ✓）。
PARTNUM = {"D1": "LED-0603"}
LABPROPS = {}          # {位号: [(属性名, 值), …]} ✓ —— 由 `build_parts()` 填 ✓（读 `.fzp` ✓）


def lab_props(fzp_text):
    """从 `.fzp` 里取**要进标签**的属性 ⇒ `[(name, 默认值), …]` ✓

    ★ 判据 = `showInLabel="yes"` ✓（Fritzing 认的就是这个 ✓，出处见库仓 `AGENTS.md` §11 B6.3 ✓）。
    ★ 属性元素的**属性次序不固定** ✗（核心件与库件写法不同 ✗）⇒ 先切元素再解析 ✓，
      ✗ 不用“一条正则吃到底” ✗（换个次序就静默漏属性 ✗）。
    """
    out = []
    for m in re.finditer(r"<property\b([^>]*)>([^<]*)</property>", fzp_text):
        attrs, val = m.group(1), m.group(2).strip()
        if 'showInLabel="yes"' not in attrs:
            continue
        nm = re.search(r'name="([^"]+)"', attrs)
        if nm:
            out.append((nm.group(1), val))
    return out


def build_parts():
    """⇒ `[(位号, moduleId, fzp 路径, 脚数, 标题, 部件目录), …]` ✓（脚数 = fzp 里 `<connector>` 个数 ✓）

    ★ 顺手把每件的**标签属性**收进 `LABPROPS` ✓（`showInLabel="yes"` 的那些 ✓ —— 见上面那条 ✓）。
    """
    out = []
    for ref, d, title in SPEC:
        if d.startswith("core:"):               # ★ 核心件：绝对路径 ✓、不内嵌 ✓
            mid, path = core_part(d.split(":", 1)[1])
            txt = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
            LABPROPS[ref] = lab_props(txt)
            _pn = re.search(r'<property name="part number"[^>]*>([^<]*)</property>', txt)
            PARTNUM.setdefault(ref, _pn.group(1).strip() if _pn else "")   # ★ 只补不覆盖 ✗
            out.append((ref, mid, path, len(re.findall(r"<connector\b", txt)), title, None))
            continue
        mid, path, dirname = lib_part(d)
        txt = sorted((LIB_SVG / dirname).glob("part.*.fzp"))[0].read_text(
            encoding="utf-8", errors="replace")
        LABPROPS[ref] = lab_props(txt)
        _pn = re.search(r'<property name="part number"[^>]*>([^<]*)</property>', txt)
        PARTNUM.setdefault(ref, _pn.group(1).strip() if _pn else "")       # ★ 只补不覆盖 ✗
        out.append((ref, mid, path, len(re.findall(r"<connector\b", txt)), title, dirname))
    return out


PARTS = build_parts()

# 精摆顺序 ✓（粗摆 + 兜底用 ✓）
ORDER = ["U1", "U2", "R1", "R2", "R3", "D1", "BT1", "J1", "J2", "J3", "J4"]

# ★★★ 2026-10-13 ✓ **分段摆位**（用户定 ✓：「先画电源天地轨，然后**按信号流分段**，
#   每段围绕一个**核心 IC/元件**来画」✓）—— 依据 = **量出来的脚位** ✗ 不是猜 ✗：
#   `U1`（`CH32V203F8U6`，21 脚）的脚分在**四条边**上 ✓（`_work/pins_final.py` ✓）：
#     · 左 c0–c4 ⇒ `VBAT_OK`(c0) / `MOD_RX`(c2) / `MOD_TX`(c3) ✓
#     · 下 c5–c9 ⇒ `LED_HB`(c7) / `SCL`(c9) ✓
#     · 右 c10–c14 ⇒ `SDA`(c10) / `CON_TX`(c14) ✓
#     · 上 c15–c20 ⇒ `SWDIO`(c15) / `SWCLK`(c16) / `CON_RX`(c17) / `3V3`(c18) / `GND`(c20) ✓
#   ⇒ **谁接在哪条边上，就摆到那条边外侧** ✓ —— 线是直的 ✓，也不必跨过 `U1` 本体 ✓。
#   ★ 实测过为什么必须这样 ✗（2026-10-13 ✓）：两排平铺那一版，9 段“穿体”里 **5 段**是
#     “从老远横穿 `U1`” ✗（`VBAT` 那根 `(519.8,35.5)→(113.1,35.5)` ✓、`3V3` 的
#     `(52.2,-47.5)→(132.5,-47.5)` ✓……）—— 因为 `BT1` 在最右、`R3/D1` 也在最右，
#     而它们要接的脚在 `U1` 的**左/下**边 ✗。
#   ★ 对齐口径照库仓 `sch_layout.py` 的规则 ③④ ✓（**相邻脚同高/同列** ⇒ 那根线是直的 ✓），
#     但**依据换成载板自己的脚位** ✓（那个脚本的规则是按像素板写死的 ✗，不能直接用 ✗）。
SEGS = [
    # (件, 边, 本件要对齐的脚, 对面 `U1` 的脚) —— L/R 对齐 **y**（同高 ✓）、T/B 对齐 **x**（同列 ✓）
    ("J1", "L", "connector2", "connector2"),      # `MOD_RX` ⇒ U1 左边 c2 ✓
    ("J3", "T", "connector2", "connector15"),     # `SWDIO` ⇒ U1 上边 c15 ✓
    ("J2", "R", "connector2", "connector10"),     # `SDA` ⇒ U1 右边 c10 ✓
    ("U2", "R", "connector4", "connector10"),     # `SDA` ⇒ U1 右边 c10 ✓（SE 就贴着 I2C ✓）
    ("R2", "R", "connector1", "connector10"),     # `SDA` 上拉 ✓
    ("D1", "B", "connector1", "connector7"),      # `LED_HB` ⇒ U1 下边 c7 ✓（阴极 ✓）
    ("R1", "B", "connector1", "connector9"),      # `SCL` ⇒ U1 下边 c9 ✓
]
# 同一条边上的**先后次序** ✓（`None` 对齐的件 ⇒ 跟着前面那个走：`BT1` 贴 `J4`（`VBAT` 两端
#   在一起 ✓）、`R3` 贴 `D1`（`LED_R` 是它俩之间的网 ✓））
#   ★★ 实测调整 ✗（2026-10-13 ✓）：`J4`（1x7 ⇒ 本体高 **63.6** ✓）**不能**摆在 `U1` 左边 ✗
#     —— 它会横跨 `U1` 左边整段可接脚区（c0–c4 只占 y −18…18 ✓）⇒ `J1` 被顶开 **112.6 单位**
#     （≈ 32 mm ✗）⇒ 对齐全丢了 ✓。⇒ `J4` 改上排 ✓（它的 3V3/GND 正好上轨近 ✓、只留
#     `VBAT_OK` 一根绕左上角下来 ✓）。
#   ★★ 2026-10-14 实测调整右列次序 ✗✓（用户 2026-10-14 选「先把腾地方解决掉」✓）：
#     原来是 `["U2", "J2", "R2"]` ✗ ⇒ 只有**第一个**能保住“与 `U1` 的脚同高”的对齐 ✓，
#     其余被“同段依次顶开” ✗ —— 实测 **`J2` 被顶开 93 单位、`R2` 被顶开 185 单位** ✗
#     ⇒ `J2` 的 `SDA`(c2)/`SCL`(c3) 离 `U1.c10`/`c9` 足有 **120 多单位**，那两根线要横跨
#     大半个画布 ⇒ 被已布的线挤死 ✗（严格模式报 4 对“没找到不碰本体的路径”✗）。
#     ✓ 改成 `["J2", "R2", "U2"]` ✓：`J2` 保住对齐（它的 `c2`＝`SDA` 与 `U1.c10` **同高** ✓
#     ⇒ 那根线是**直的** ✓）；`R2`（`SDA` 上拉）次之（它也接 `U1.c10` ✓）；`U2` 最后 ✓。
SIDE_SEQ = {"L": ["J1"], "T": ["J3", "J4", "BT1"],
            "R": ["J2", "R2", "U2"], "B": ["D1", "R3", "R1"]}


def place_segmented(box_of, pin_of, gap):
    r"""分段摆位 ⇒ `{位号: (x, y)}` ✓（几何全部来自 `render_sch --pins-out` ✓，没有手拍的数 ✓）

    口径 ✓：
      ① `U1` = 核心 ✓ ⇒ 本体**左缘落 x=0、框心落 y=0** ✓；
      ② 每段：先把该件**指定的脚**摆到与 `U1` 指定脚**同高**（L/R ✓）/ **同列**（T/B ✓）；
      ③ 再把该件**整体推到 `U1` 外面** `gap` 处 ✓（L 段：本件右缘 = `U1` 左缘 − `gap` ✓…）；
      ④ 同一条边上按 `SIDE_SEQ` **依次排开、不许重叠** ✓（后面那个被顶开 ⇒ 只有它自己的
         对齐会偏一点 ✓ —— 会打印出来 ✓）。
    """
    u, pu = box_of["U1"], pin_of["U1"]
    place = {"U1": (-u[0], -(u[1] + u[3]) / 2.0)}
    a = place["U1"]
    ux0, uy0, ux1, uy1 = a[0] + u[0], a[1] + u[1], a[0] + u[2], a[1] + u[3]
    want = {}
    for ref, side, c_self, c_u in SEGS:
        b, ps = box_of.get(ref), pin_of.get(ref) or {}
        if not b:
            continue
        if ps.get(c_self) and pu.get(c_u):
            tx, ty = a[0] + pu[c_u][0], a[1] + pu[c_u][1]
            want[ref] = (tx - ps[c_self][0], ty - ps[c_self][1])
        else:
            print("   !! %s 的对齐脚缺失（%s / %s）⇒ 退化成贴边摆 ✓" % (ref, c_self, c_u))
    drift = []
    for side, seq in SIDE_SEQ.items():
        prev = None
        for ref in seq:
            b = box_of.get(ref)
            if not b:
                continue
            if ref in want:
                x, y = want[ref]
            elif prev is not None:                      # 无对齐 ⇒ 跟着同段前一件 ✓
                pb, pp = box_of[prev], place[prev]
                if side in "LR":
                    x, y = pp[0], pp[1] + pb[3] + gap - b[1]     # 同一列、排在下面 ✓
                else:
                    x, y = pp[0] + pb[2] + gap - b[0], pp[1]     # 同一行、排在右边 ✓
            else:
                x, y = 0.0, 0.0
            # ③ 推到 `U1` 外面 `gap` ✓
            if side == "L":
                x += (ux0 - gap) - (x + b[2])
            elif side == "R":
                x += (ux1 + gap) - (x + b[0])
            elif side == "T":
                y += (uy0 - gap) - (y + b[3])
            elif side == "B":
                y += (uy1 + gap) - (y + b[1])
            # ④ 同段不许重叠 ✓（按排开方向顶开 ✓）
            if prev is not None:
                pb, pp = box_of[prev], place[prev]
                if side in "LR":
                    lim = pp[1] + pb[3] + gap
                    if y < lim:
                        drift.append((ref, lim - y))
                        y = lim
                else:
                    lim = pp[0] + pb[2] + gap
                    if x < lim:
                        drift.append((ref, lim - x))
                        x = lim
            place[ref] = (x, y)
            prev = ref
    for ref, d in drift:
        print("   · %s 被同段前一件顶开 %.1f 单位（它的对齐偏了这么多 ✓）" % (ref, d))
    return place

VIEWS = [("breadboardView", "breadboard", "2.50002"),
         ("schematicView", "schematic", "2.50002"),
         ("pcbView", "copper1", "8.50004")]

# ★★ 2026-10-14 ✓ **`ROT`：把某件绕自己的局部原点转任意角** ✓（能力 ✓；**默认空 = 都不转** ✗）
#   ★ 起因：用户原话「**如果我手调的话，肯定会反转 J1 和 D1**」✓ —— 他的直觉是这两件
#     脚都背对着要接的脚（`J1` 在 `U1` 左边、排针脚在本体远端；`D1` 在 `U1` 下边、两极朝下 ✓）。
#   ★★ **实测（量出来的 ✓，别照直觉 ✗）：`ROT = {"J1":180,"D1":180}` 反而更差** ✗ ——
#     · `J1` 的 4 只脚**本来就在本体朝 `U1` 的那一侧** ✓（摆位时已按“脚朝向 `U1`”选中 ✓）
#       ⇒ 转 180° 是把它**转背过去** ✗；
#     · `D1` 的两极是**上 + 下** ✓（LED 符号天生如此 ✓）⇒ 转 180° 只是把 `LED_R`/`LED_HB`
#       **左右对调** ✗，两头仍是一上一下 ✓。
#     同档实测（GAP=13 格）：不转 = 贴脚 8 / 交叉 56 / **网表 ✓ 14/14** ✓；
#     反转 = 贴脚 **12** ✗ / 交叉 37 / **网表 ✗**（`SDA`/`SCL`/`LED_HB` 三张网拒画 ✗）。
#   ⇒ **默认留空** ✓；要试某个角度就填这一行 ✓（改前先跑一遍基线 ✓，别把它当“顺手优化” ✗）。
#   ★ 格式 = **Fritzing 自己的** ✓（库仓 `set_rot.py` 从**用户转过的文件**里挖出来的 ✓）：
#     旋转是 `schematicView/geometry` 的**子元素 `<transform m11…m32>`** ✗ **不是属性** ✗；
#     平移不动（= 绕零件自己局部原点转 ✓）；**角度算得出来** ✓ —— 它在 Fritzing 里是
#     一个**通用 2×3 矩阵** ✗ 不是枚举值 ✗ ⇒ ±90° / 180° / 45° / 135° / 任意角都写得出 ✓
#     （库仓 `set_rot.py` 里有**实测**：用户按 Ctrl+R 转 45° ⇒ `m11=m22=-0.7071…`、
#       `m12=-m21=-0.7071…` ✓）。
#     ⇒ 工具链**本来就认它** ✓（`part_box.tf_of()` ✓ ⇒ 渲染器 / 量测 / 布线器全跟着转 ✓）。
#   ★★ 必须写在**生成阶段** ✗（不能拿 `set_rot.py` 事后转 ✗）：`place_segmented()` 的对齐
#     用的是 `render_sch --pins-out` 量到的**脚位** ✓ —— 先转再摆，对齐才是按**新朝向**算的 ✓
#     （库仓 `set_rot.py` 文档串里那次实验就是因为“先摆后转”而失败 ✗）。
#   ★★ 角度矩阵与库仓 `set_rot.rotate()` **同一个式子** ✓（`m11=cosθ` / `m12=sinθ` /
#     `m21=-sinθ` / `m22=cosθ` ✓，正角 = **顺时针在屏幕上** ✓）—— 两处必须一致 ✗，
#     否则同一个角度在两个工具里会转出两个朝向 ✓。
ROT = {}    # {位号: 角度°} ✓；空字典 = 都不转 ✓（**当前缺省** ✗ —— 见上：转 J1/D1 实测更差 ✓）


def inst_xml(mi, mid, path, npins, title, x, y, ref=""):
    """一只元件的实例 XML ✓（三个视图各一份 ✓；★ 脚的 `<geometry x="0" y="0"/>` ✓ ——
    脚位由**部件 svg + transform** 决定 ✓，不用我算 ✓；**零 `<connects>`** ✓）

    ★★ 2026-10-13 补 ✗：**schematicView 还必须带 `<titleGeometry>`** ✓ ——
      没它 `render_sch` 就算不出位号文字（它的口径 = 实例 `title` ＋ fzp 的 `showInLabel` 字段 ✓，
      位置取 `titleGeometry` ✓）⇒ 图上**一个位号都没有** ✗（实测：`LAB = {}` / “位号 0 个” ✗），
      布线器的 `relabel()`（避让重摆 ✓）也**无事可做** ✗。
      · 格式照 Fritzing 自己写的 ✓（挂在**实例的 schematicView 里** ✓，与 `<geometry>` 同级 ✓）。
      · 初位置给“本体框右上角外一点” ✓（`Fritzing` 的 `relabel()` 会把它避让重摆 ✓）；
        是相对**实例锚点**的 ✓（与 `<geometry>` 同一坐标系 ✓）。
    """
    def conns(layer):
        return "\n".join(
            '      <connector connectorId="connector%d" layer="%s">\n'
            '        <geometry x="0" y="0" />\n'
            '      </connector>' % (i, layer)
            for i in range(npins))

    def view(v, layer, z):
        tg = ''
        # ★★★ 2026-10-13 用户定 ✓（原话）：「**应该显示 D1/J1/J2/J3/J4**，R1/R2/R3 应该带阻值，
        #   BT1 应该带电容值，U2 标签不应与元件有位置冲突」✗
        #   ⇒ **每个件都给 `<titleGeometry>`** ✓（位号要显示 ✓）。
        #   ★ 那“元件号不该有”是指什么 ✗：指**那串 moduleId** ✗ —— 它来自我们多写的
        #     `<property name="part number" value="<moduleId>">` ✗，**已删** ✓（见 `inst_xml` ✓）。
        #   ★ 标签里到底出现几行 ✗（Fritzing 自己拼 ✓，我们只管给它数据 ✓）：
        #     · 第 1 行 = `<title>` = **位号** ✓（每件都有 ✓）；
        #     · 之后 = 该件 `.fzp` 里 `showInLabel="yes"` 的属性 ✓（只有 `Resistor-*` 有 ✓）；
        #       `D1`/`J*`/`U*` 的 `.fzp` **没有**这个标记 ✗ ⇒ 它们**只显示位号** ✓✓（实测 ✓）。
        if v == "schematicView":
            # ★★★ 2026-10-13 ✓ **必须带 `visible="true"`** ✗✗（用户报的「**所有元件都没有显示
            #   元件编号**」✓ 就是这一条 ✓）—— 拿 **Fritzing 自己保存的台架草图**逐字段比出来的 ✓：
            #     `<titleGeometry visible="true" x=… y=… z=… xOffset=… yOffset=…
            #        textColor="#000000" fontSize="5"/>` ✓
            #   ✗ 原来只写 `x/y/z` ⇒ **缺 `visible` ⇒ Fritzing 当作“不显示”** ✗
            #     （`<title>` 里的文字它照收 ✓，画不画由这个属性说了算 ✓）。
            #   ★ `xOffset/yOffset` = 位号**相对零件锚点**的偏移 ✓（`x/y` 是**绝对**坐标 ✓）——
            #     两个都要给 ✓，否则 Fritzing 拖零件时位号不跟着走 ✗。
            # ★★★ 2026-10-14 ✓ **标签的每一行必须显式写出来** ✗✗（用户定的规范 ✓，原话：
            #   「画图的**第一步是摆出元件**，元件会**带着标签**等，如截图所示的标签，**才是
            #    合规的、正确的**……以前传递的只是**图纸**，没有软件；以后留存的，**纸上信息
            #    也必须完整**」✗）
            #   ⇒ 判据（**Fritzing 源码** ✓ 不是猜 ✗）：
            #     · `partlabel.cpp:336 saveInstance()` 写的就是**这个形状** ✓：
            #       `<titleGeometry …>` **不自闭合** ✗，里面**每个 key 一个
            #       `<displayKey key="…"/>`** ✓，**顺序 = 显示顺序** ✓；
            #     · `partlabel.cpp:414-418 restoreLabel()` 读回来时**先 `clear()`** ✓
            #       ⇒ 文件里这份列表 = **权威** ✓；
            #     · 列表**为空**才回落默认（位号 + `part number` ✗ —— 就是那个
            #       `PinHeader-1x4` 的来源 ✗，`partlabel.cpp:421-425` ✓）。
            #   ★ 顺序照**用户截图的标杆** ✓：电阻 `R1 / ±5% / 1/10W / 220Ω` ✓、
            #     电容 `C1 / 16V / 100 nF` ✓（**电压在前、容值在后** ✓，电容**不带公差** ✓）。
            #   ★ 这样连 `part number` 都不用管了 ✓（**不在列表里就不显示** ✓）——
            #     实例里那个清空保留着当双保险 ✓。
            keys = [""]
            lp = dict(LABPROPS.get(ref) or [])
            if "resistance" in lp:
                keys += ["tolerance", "power", "resistance"]      # 照截图 R1/±5%/1/10W/220Ω ✓
            elif "capacitance" in lp:
                keys += ["voltage", "capacitance"]                # 照截图 C1/16V/100 nF ✓
            elif ref in SHOW_MODEL:
                keys += ["part number"]                            # 芯片/接口第二行 = 型号 ✓
                #   （用户 2026-10-14 截图标杆：`U1` / `CH32V203F8U6` ✓、`J2` / `SH1.0-3P-LT` ✓；
                #    二极管同样要型号 ✓ —— 用户当天更正「**二极管也应该有元件型号**」✗）
            dk = "".join('\n        <displayKey key="%s" />' % k for k in keys)
            tg = ('\n      <titleGeometry visible="true" z="2.50002" x="%s" y="%s"'
                  ' xOffset="%s" yOffset="%s" textColor="#000000" fontSize="5">'
                  '%s\n      </titleGeometry>'
                  % (fmt(x + 3.0), fmt(y - 3.0), fmt(3.0), fmt(-3.0), dk))
        geo = '      <geometry z="%s" x="%s" y="%s" />' % (z, fmt(x), fmt(y))
        if v == "schematicView" and ref in ROT:
            # ★ 任意角度 ✓：`m11=cosθ` / `m12=sinθ` / `m21=-sinθ` / `m22=cosθ` ✓
            #   （与库仓 `set_rot.rotate()` 同式 ✓；平移 m31/m32 不动 ✓ = 绕局部原点转 ✓）
            th = math.radians(ROT[ref])
            ca, sa = math.cos(th), math.sin(th)
            geo = ('      <geometry z="%s" x="%s" y="%s">\n'
                   '        <transform m11="%s" m12="%s" m13="0" m21="%s" m22="%s"'
                   ' m23="0" m31="0" m32="0" m33="1" />\n'
                   '      </geometry>'
                   % (z, fmt(x), fmt(y), fmt(ca), fmt(sa), fmt(-sa), fmt(ca)))
        return ('    <%s layer="%s">\n'
                '%s%s\n'
                '      <connectors>\n%s\n      </connectors>\n'
                '    </%s>' % (v, layer, geo, tg, conns(layer), v))

    views = "\n".join(view(v, layer, z) for v, layer, z in VIEWS)
    # ★★★ 2026-10-14 ✓ **必须把 `part number` 清空** ✗✗（用户原话：「**J1 在图上不要带
    #   `PinHeader-1x4`，其它 J2 等也是同样要求**」✗）—— 真因（**Fritzing 源码** ✓ 不是猜 ✗）：
    #     · `partlabel.cpp:109-112` 构造标签时**硬加两行** ✓：
    #         `m_displayKeys.append(LabelTextKey);`   ← 第 1 行 = `<title>` = **位号** ✓
    #         `m_displayKeys.append(propertyName);`   ← 第 2 行 = **`part number`** ✗
    #       而 `propertyName` 就是 `"part number"` ✓（`itembase.cpp:240`
    #       `TranslatedPropertyNames.insert("part number", …)` ✓；`properties.xml` 里**没有**它 ✓
    #       ⇒ 它是**内置**的第二行 ✗）。
    #     · ⇒ 只要该件的 `.fzp` 里 `part number` 非空 ✗（`PinHeader-*` = `PinHeader-1x4` ✓、
    #       `CH32V203F8U6` ✓、`ATECC608B` ✓、`SparkFun-LED-LED-0603` ✓ …），
    #       Fritzing **一定会把它画出来** ✗ —— 与我们写不写 `<property>` **无关** ✗。
    #     · ✓ 解法：实例里**覆写**成空值 ✓ ⇒ `partlabel.cpp:286 if (!t.isEmpty())` 会**跳过**它 ✓；
    #       而 `resistance`/`capacitance` 走的是 `modelPart()->displayKeys()` ✓
    #       （= `.fzp` 里 `showInLabel="yes"` 的那些 ✓，见 `partlabel.cpp:145` ✓）⇒ **照旧显示** ✓✓。
    #   ★ 芯片的第二行（型号）走 `part number` ✓ ⇒ **芯片保留它的值** ✓（`PARTNUM` ✓，只对芯片 ✓）；
    #     其余件的 `part number` **清空** ✗（用户 2026-10-13 点名 `PinHeader-1x4` ✗）。
    #   ★★ 2026-10-14 改口径 ✓（用户三条规范 ✓）：「**接口**要像截图 `J2 / SH1.0-3P-LT`」✗
    #     ⇒ **芯片与接口本来就要显示型号那一行** ✓ ⇒ **`part number` 不必清空、实例也不写它** ✓
    #     ⇒ 值直接取自件的 `.fzp` ✓（`PinHeader-1x4` ✓ / `CH32V203F8U6` ✓ / `ATECC608B` ✓）；
    #     而「谁显示这一行」由 `<displayKey key="part number"/>` 说了算 ✓（见 `SHOW_MODEL` ✓；
    #     二极管不列 ⇒ 只位号 ✓ —— 用户原话「**有 D1 就够了**」✗）。
    props = []
    if ref in SHOW_MODEL:                     # ★ 只有要显示型号的件才写它 ✓（其余件不写 ✓）
        props.append('    <property name="part number" value="%s" />' % PARTNUM.get(ref, ""))
    for nm, dflt in LABPROPS.get(ref) or []:
        val = (PROPS.get(ref) or {}).get(nm, dflt)      # 本项目覆写优先 ✓，否则用 `.fzp` 默认 ✓
        props.append('    <property name="%s" value="%s" />' % (nm, val))
    prop_xml = ("\n" + "\n".join(props)) if props else ""
    # ★★★ **不再写 `<property name="part number" value="<moduleId>">`** ✗✗（2026-10-13 用户报 ✓）：
    #   Fritzing 会把 `part number` **画进元件标签** ✗ ⇒ 图上显示一长串 moduleId ✗
    #   （截图里 `U1` 下面那行 `CH32V203F8U6` 就是它 ✗ —— 用户说“**芯片名被遮挡**” ✓）。
    #   ★ 证据：**Fritzing 自己写的**台架草图里**没有**这个属性 ✓（只有 tolerance/power/resistance/pin spacing ✓）。
    # ★★★ 2026-10-13 ✓ **`<title>` 就是“位号行”** ✗✗ —— 两个事实叠在一起 ✓：
    #   ① **显示**：Fritzing `partlabel.cpp:275 displayTexts()` 里
    #      `if (key.compare(LabelTextKey) == 0) t = m_text;` ⇒ 位号行 = `<title>` ✓；
    #      而 `LabelTextKey = ""` 是构造时**硬编码**进 `m_displayKeys` 的 ✗（`:109` ✓）
    #      ⇒ **位号行无法单独关掉** ✗，只有让它**内容为空**才会被 `if (!t.isEmpty())` 跳过 ✓；
    #   ② **认件**：库仓 `sch_route.py` 拿 `<title>` **当实例字典的键** ✓（网表也按位号查脚 ✗）
    #      ⇒ `<title>` 一留空，9 件撞名 ⇒ 布线器**认不出件** ✗（实测 `KeyError: 'U1'` ✗）。
    #   ⇒ ★ 两条冲突 ✗ ⇒ **本次先保“跑得通”** ✓：`<title>` **一律给位号** ✓（R/C 的位号也会显示 ✗，
    #     这是 Fritzing 的限制 ✗）；真要“只显示参数、不要位号” ✗ 得把**显示与认件拆成两个字段** ✗
    #     （改工具链全链 ✗）—— 先与用户对齐再动 ✓。
    title_xml = title
    return ('  <instance moduleIdRef="%s" modelIndex="%d" path="%s">%s\n'
            '    <title>%s</title>\n'
            '    <views>\n%s\n    </views>\n'
            '  </instance>' % (mid, mi, path, prop_xml, title_xml, views))


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
        blocks.append(inst_xml(mi, mid, path, npins, title, x, y, ref))
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


def read_pins(pins):
    """从 pins_ref.py 抠出 {modelIndex: {connectorN: (dx,dy), …}} ✓（同样是纯数据 ✓）

    ★ 本板要靠它做“**相邻脚同高/同列**”的对齐 ✓（库仓 `sch_layout.py` 规则③④ ✓）。
    """
    txt = pins.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^PINS = (\{.*?\})\s*$", txt, re.M | re.S)
    if not m:
        return {}
    return eval(m.group(1))                      # noqa: S307


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

    # ② 分段摆位 ✓（口径写在 `place_segmented()` 的文档串里 ✓；几何来自工具 ✓）
    _t, _e, mi_of = emit(place, WORK / "x.fzz")
    pind = read_pins(pins)
    box_of, pin_of = {}, {}
    for ref in ORDER:
        mi = mi_of.get(ref)
        b = boxes.get(str(mi)) or boxes.get(mi)
        ps = pind.get(str(mi)) or pind.get(mi) or {}
        if not b:
            print("   !! 没拿到 %s 的本体框（mi=%s）" % (ref, mi))
            continue
        box_of[ref] = b
        pin_of[ref] = {k: v for k, v in ps.items() if k.startswith("connector")}
    if "U1" not in box_of:
        print("   !! 核心件 U1 没有几何 ⇒ 不摆了 ✓")
        return 1
    place2 = place_segmented(box_of, pin_of, GAP)
    emit(place2, OUT)
    print("② 分段摆位 → %s（%d 件 ✓；按 U1 四条边分段 ✓）" % (OUT, len(place2)))
    for ref in ORDER:
        if ref in place2 and ref in box_of:
            b, p = box_of[ref], place2[ref]
            print("   %-4s 锚 (%7.1f,%7.1f) ⇒ 本体 (%7.1f,%7.1f)→(%7.1f,%7.1f)"
                  % (ref, p[0], p[1], p[0] + b[0], p[1] + b[1], p[0] + b[2], p[1] + b[3]))

    rc, out, err, pins2 = pins_out(OUT, "final")
    print("   复核 render_sch rc=%d" % rc)
    for l in (out or "").splitlines():
        if any(k in l for k in ("本体盒", "画布", "位号", "无法判定", "悬空")):
            print("   " + l.strip()[:160])
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
