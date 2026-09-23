# -*- coding: utf-8 -*-
"""核对 b 步夹具接线图（Fritzing `.fzz`）的真实网表。

用法::

    python tools\\fzz_nets.py                 # 核对 hardware/wiring/ 下已登记规格的各张图
    python tools\\fzz_nets.py <file.fzz> ...  # 核对指定工程

为什么需要它：`.fzz` 是个 zip，接线对不对**不该靠肉眼数线**。这里按 Fritzing 自己的
存储方式读连接——① 每个 instance 的 `<connector>/<connects>`；② **每条 wire 自身导通两端**；
③ 部件 `.fzp` 里的**内部 `<bus>`**（同网脚，例如 CH347F 的 11 个 GND、模组的 3 个 GND 焊盘）；
④ **面包板的同列导通**（见 `_breadboard_columns`）
——把连接并成网（union-find），翻译成「实例.脚名」后与**期望表**逐网比对。

期望表 `EXPECT` 就是这两个夹具的**接线规格**（单一源）：改图时先改这里，
脚本会直接告诉你差在哪一格。退出码 0 = 全部对上；1 = 有差异（信息里带原因）。
"""
import os
import re
import sys
import zipfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

WIRING_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "hardware", "wiring"))

# ---- 角色（**按部件 ID 认，不按实例编号/名字**）-------------------------------
#   理由：实例 title（U2/U3/U4…）**重画就会变**（2026-09-19 两块模组那张图就换过），
#   而“哪块板是什么”由部件本身决定 ⇒ 认 moduleIdRef 才稳。
#   没登记的角色（例如老写法里的 `"U3"`）仍按**实例 title** 精确匹配。
MOD, MCU, BRIDGE, LED, RES, SE = "MOD", "MCU", "BRIDGE", "LED", "RES", "SE"
ROLES = {
    MOD:    lambda mid: mid.startswith("TX-AH-R900PNR"),   # 任意一块 TX-AH 模组
    MCU:    lambda mid: mid == "CH32V203C8T6",            # nanoCH32V203 开发板
    BRIDGE: lambda mid: mid == "CH347F",                  # CH347F-EVT（USB ↔ 2×UART 桥）
    LED:    lambda mid: "ColorLED" in mid,                # Fritzing 核心 LED 部件
    RES:    lambda mid: "Resistor" in mid,                # Fritzing 核心电阻部件
    SE:     lambda mid: mid.startswith("ATECC608B"),      # 安全元件（SOIC-8 转 DIP）
}
ROLE_CN = {MOD: "模组", MCU: "MCU", BRIDGE: "桥", LED: "灯", RES: "电阻", SE: "安全元件"}

# ---- 接线规格（期望的网表）--------------------------------------------------
#   每个集合 = 一个网；元素写作 `(角色, 脚名)`，脚名照部件里的 `connectorname`
#   （= 板上丝印/模组脚名）。
#   ★ 关键点是**端口 ↔ 角色**，不是“哪一块板放在左边”：
#     · `MOD` = 模组；`BRIDGE` = CH347F（P2 = UART0、P3 = UART1）；`MCU` = nano。
#   ⚠ `.fzz` 只内嵌**图上用到的自定义部件**（模组/nano/CH347F），Fritzing **核心部件**
#     （LED、电阻）的 `.fzp` 不在包里 ⇒ 它们的脚名读不到，只能落回 `connectorN`：
#     `603ColorLED…` 的 `connector0 = cathode`、`connector1 = anode`（核过核心 .fzp）。
EXPECT = {
    "bstep-ch347f-txah-evb.fzz": [
        # 单模块：桥 ↔ 模组（P2 数据口 + P3 AT/打印口）
        {(MOD, "A10"), (BRIDGE, "TXD0")},
        {(MOD, "A11"), (BRIDGE, "RXD0")},
        {(MOD, "A12"), (BRIDGE, "TXD1")},
        {(MOD, "A13"), (BRIDGE, "RXD1")},
        {(MOD, "GND"), (BRIDGE, "GND")},
    ],
    "bstep-ch347f-2txah-evb.fzz": [
        # 两块模块：**P2 那一路 = 客户端(STA)**、**P3 那一路 = 对端(AP)**；三块板共地
        {(MOD, "A10"), (BRIDGE, "TXD0")},
        {(MOD, "A11"), (BRIDGE, "RXD0")},
        {(MOD, "A10"), (BRIDGE, "TXD1")},
        {(MOD, "A11"), (BRIDGE, "RXD1")},
        {(MOD, "GND"), (BRIDGE, "GND")},
    ],
    "bstep-ch347f-txah-evb-thrj45.fzz": [
        # 对端换成 T-Halow-RJ45（**无线相连、图上与任何部件都没有连线**，只看它的 USB）；
        # 客户端侧只接数据口 3 根（AT/打印口走它自己的 USB，不过桥）
        {(MOD, "A10"), (BRIDGE, "TXD0")},
        {(MOD, "A11"), (BRIDGE, "RXD0")},
        {(MOD, "GND"), (BRIDGE, "GND")},
    ],
    # ---- c 步：客户端台架（MCU 参与，模组 UART0 = 数据口、UART1 = AT/打印口）----
    #   ⚠ 早先那张两板图 `cstep-nanoch32v203-txah-evb.fzz` 已作废（2026-09-20 用户删图）：
    #     它缺 PC 侧观察口；下面这张是它的取代版 —— 同样两块板各自供电 + 共地 + 数据口，
    #     外加 CH347F 提供的**两个 PC 窗口**。旧图若从 git 历史里翻出来，请只当历史看。
    "cstep-ch347f-txah-evb-nanoch32.fzz": [
        # 加了 CH347F 当**两个 PC 窗口**：UART0(P2) = 我们的 console、UART1(P3) = 模组 AT/打印口。
        # 三块板各自 USB 供电、共地；CH347F 的 3V3/VIO **不接**（避免两个 3.3V 源并联）。
        {(MOD, "A10"), (MCU, "PA2/ADC2")},      # 数据口：nano USART2_TX → 模组 UART0_RX
        {(MOD, "A11"), (MCU, "PA3/ADC3")},      # 数据口：模组 UART0_TX → nano USART2_RX
        {(BRIDGE, "TXD0"), (MCU, "PA10")},      # 窗口①：CH347F TXD0 → nano USART1_RX
        {(BRIDGE, "RXD0"), (MCU, "PA9")},       # 窗口①：nano USART1_TX → CH347F RXD0
        {(MOD, "A12"), (BRIDGE, "TXD1")},       # 窗口②：CH347F TXD1 → 模组 UART1_RX（发 AT）
        {(MOD, "A13"), (BRIDGE, "RXD1")},       # 窗口②：模组 UART1_TX → CH347F RXD1（看日志）
        {(MOD, "GND"), (MCU, "GND"), (BRIDGE, "GND")},   # 三块板共地
    ],
    # ---- B 方案：**独立 I²C 主机**（CH347F 直连 SE，nano 不在图上）----------------
    #   规格出处 `docs/ch347f-i2c-crosscheck.md` §2；目的见 `docs/atecc608b-se.md` §9：
    #   把 nano 固件排除在外，判「器件不讲 CryptoAuth」还是「我们固件还有毛病」。
    #   · CH347F 的 **P5 = I²C**（板子丝印 `P5` + 那 4 个脚名；脚位**未核实**）；
    #   · SE 的 pin4/5/6/8 = GND/SDA/SCL/VCC；
    #   · **两只 4.7k 上拉**（一端接 SDA/SCL、另一端接 3V3）——电源由 CH347F 的 3V3 出；
    #   · **图上没有 nano** = 两个主机不同时驱动同一对线（nano 要断电或按住 RST）。
    #   ⚠ 上拉的**阻值**（4.7k）本脚本不断言，只断言它接在哪个网上。
    "cstep-ch347f-atecc608b.fzz": [
        {(BRIDGE, "SDA"), (SE, "SDA"), (RES, "connector1")},   # 数据线 + 一只上拉
        {(BRIDGE, "SCL"), (SE, "SCL"), (RES, "connector1")},   # 时钟线 + 一只上拉
        {(BRIDGE, "3V3"), (SE, "VCC"), (RES, "connector0")},   # 3V3 = 两只上拉的另一端
        {(BRIDGE, "GND"), (SE, "GND")},                        # 共地（SE 的 GND 只跟 CH347F 的地在一起）
    ],
}


def read_fzz(path):
    """返回 (fz 文本, {moduleId: {connectorId: 脚名}}, {modelIndex: (moduleId, title, body)},
    {moduleId: [同网脚集合, ...]})。"""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        fz = z.read([n for n in names if n.endswith(".fz")][0]).decode("utf-8", "replace")
        fzps = {n: z.read(n).decode("utf-8", "replace") for n in names if n.endswith(".fzp")}

    pins, buses = {}, {}
    for txt in fzps.values():
        mid = re.search(r'<module[^>]*moduleId="([^"]+)"', txt)
        if not mid:
            continue
        mid = mid.group(1)
        pins[mid] = dict(re.findall(r'<connector id="([^"]+)" name="([^"]*)"', txt))
        buses[mid] = [set(re.findall(r'connectorId="([^"]+)"', body))
                      for _bid, body in re.findall(r'<bus id="([^"]*)"\s*>(.*?)</bus>', txt, re.S)]

    insts = {}
    for m in re.finditer(r'<instance moduleIdRef="([^"]+)"([^>]*)>(.*?)</instance>', fz, re.S):
        midref, attrs, body = m.groups()
        mi = re.search(r'modelIndex="([^"]+)"', attrs)
        title = re.search(r"<title>([^<]*)</title>", body)
        if mi:
            insts[mi.group(1)] = (midref, title.group(1) if title else "?", body)
    return fz, pins, insts, buses


# ---- 面包板内部的「同列导通」--------------------------------------------------
#   面包板上一个「列」是两段各 5 孔：**A–E 同列互连、F–J 同列互连**（列内 0.1 in 间距）。
#   出处：标准面包板结构。Fritzing 把这件事写在**核心部件**的 `.fzp`（`<bus>`）里，
#   而**核心部件不内嵌进 `.fzz`** ⇒ 读不到，只能按这条规则补上（孔名就是列号，不需要外部文件）。
#   ⚠ 覆盖上限：**不建模电源轨** —— 若某张图用 `+`/`-`/`TP…` 这类孔，它们不会与任何东西并网，
#     会显示成「断开的网」。那是本规则的边界，不是图错了。
HALF = {"A": 0, "B": 0, "C": 0, "D": 0, "E": 0,
        "F": 1, "G": 1, "H": 1, "I": 1, "J": 1}


def _breadboard_columns(insts, pins):
    """给**核心**面包板实例补「同列导通」的边；内嵌部件自己有 `<bus>`，不猜。"""
    edges = []
    for mi, (midref, _t, body) in insts.items():
        if midref == "WireModuleID" or midref in pins:
            continue
        cols = {}
        for cid in re.findall(r'<connector connectorId="([A-J]\d+)"', body):
            cols.setdefault((HALF[cid[0]], int(cid[1:])), []).append(cid)
        for grp in cols.values():
            for cid in grp[1:]:
                edges.append(((mi, grp[0]), (mi, cid)))
    return edges


def nets(path):
    fz, pins, insts, buses = read_fzz(path)
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        r = x
        while parent[r] != r:
            r = parent[r]
        while parent[x] != r:
            parent[x], x = r, parent[x]
        return r

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for mi, (midref, _t, body) in insts.items():
        if midref == "WireModuleID":
            union((mi, "connector0"), (mi, "connector1"))     # wire 自己导通两端
        for cm in re.finditer(r'<connector connectorId="([^"]+)"[^>]*>(.*?)</connector>',
                              body, re.S):
            cid, blk = cm.groups()
            find((mi, cid))
            for cc in re.finditer(r'<connect connectorId="([^"]+)"[^>]*modelIndex="([^"]+)"', blk):
                union((mi, cid), (cc.group(2), cc.group(1)))
    for mi, (midref, _t, _b) in insts.items():                # 部件内部 bus（同网脚）
        if midref == "WireModuleID":
            continue
        for grp in buses.get(midref, []):
            grp = sorted(grp)
            for cid in grp[1:]:
                union((mi, grp[0]), (mi, cid))
    for a, b in _breadboard_columns(insts, pins):             # 面包板：同列两半各自导通
        union(a, b)

    groups = {}
    for k in list(parent):
        groups.setdefault(find(k), []).append(k)
    out = []
    for members in groups.values():
        lbl = set()
        for mi, cid in members:
            midref, title, _ = insts.get(mi, ("?", "?", ""))
            if midref == "WireModuleID":
                continue
            lbl.add((title, midref, pins.get(midref, {}).get(cid, cid)))
        if lbl:
            out.append(lbl)
    return out, insts


def _match(want, got):
    """want: {(角色, 脚名)}；got: {(title, moduleIdRef, 脚名)}。每个 want 都要能对上。

    角色优先按 `ROLES`（部件 ID）认；没登记的角色按实例 title 认（老写法兼容）。
    """
    used = []
    for role, pin in want:
        pred = ROLES.get(role)
        for t, mid, nm in got:
            if nm != pin:
                continue
            if pred(mid) if pred else (role == t):
                used.append((t, nm))
                break
        else:
            return None
    return used


def check(path):
    name = os.path.basename(path)
    got, insts = nets(path)
    exp = EXPECT.get(name)
    print("=" * 88)
    print(name, "->", path)
    print("  实例：", ", ".join("%s=%s" % (t, m) for m, (mr, t, _b) in insts.items()
                               if mr != "WireModuleID"))
    print("  实际网表（%d 个网）：" % len(got))
    for g in sorted(got, key=lambda s: sorted(s)[0]):
        print("    " + "  <->  ".join("%s.%s" % (t, n) for t, _m, n in sorted(g)))
    if exp is None:
        print("  （EXPECT 里没有这张图的规格，仅列出网表）")
        return True
    # 面包板实例的孔（`A4`/`J11` 这种名字）：它只是个「插座」，**孔里有脚 ≠ 有线**。
    # 判「多出来的连线」时要把孔排除掉，否则「NC 脚插在空孔上」会被误报成多余连线。
    hole_owner = {t for _m, (mr, t, body) in insts.items()
                  if mr != "WireModuleID"
                  and re.search(r'<connector connectorId="[A-J]\d+"', body)}
    ok = True
    print("  --- 逐网核对（模块侧不限实例编号）---")
    matched = []
    for want in exp:
        hit = None
        for g in got:
            used = _match(want, g)
            if used:
                hit = (g, used)
                break
        desc = "  <->  ".join("%s.%s" % (ROLE_CN.get(r, r), p) for r, p in sorted(want))
        if hit:
            matched.append(hit[0])
            real = "  ".join("%s.%s" % (t, n) for t, n in sorted(hit[1]))
            print("    ✓ %-42s  （图上 = %s）" % (desc, real))
        else:
            ok = False
            print("    ✗ %s ：图上找不到这个网" % desc)
    for g in got:
        if g in matched:
            continue
        parts = sorted((t, n) for t, _m, n in g if t not in hole_owner)
        holes = sorted(n for t, _m, n in g if t in hole_owner)
        if len({t for t, _n in parts}) > 1:     # 跨实例却没写进规格 = 多出来的连线
            ok = False
            print("    ✗ 多出来的跨实例网：%s"
                  % "  <->  ".join("%s.%s" % (t, n) for t, n in parts))
        elif len(g) > 1:                        # 非连线：部件内部同网 / 只插在面包板空孔上
            print("    · 非连线（部件内部同网 / 空孔）：%s%s"
                  % ("  <->  ".join("%s.%s" % (t, n) for t, n in parts) or "(只有孔)",
                     ("  @" + ",".join(holes)) if holes else ""))
    print("  结论：" + ("全部对上 ✓" if ok else "有差异 ✗"))
    return ok


def main(argv):
    files = argv[1:] or [os.path.join(WIRING_DIR, n) for n in sorted(EXPECT)]
    bad = 0
    for f in files:
        if not os.path.exists(f):
            print("!! 找不到", f)
            bad += 1
            continue
        if not check(f):
            bad += 1
    print("=" * 88)
    print("一共 %d 张图，%d 张有问题" % (len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
