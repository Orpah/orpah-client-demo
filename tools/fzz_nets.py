# -*- coding: utf-8 -*-
"""核对 b 步夹具接线图（Fritzing `.fzz`）的真实网表。

用法::

    python tools\\fzz_nets.py                 # 核对 hardware/wiring/ 下两张图
    python tools\\fzz_nets.py <file.fzz> ...  # 核对指定工程

为什么需要它：`.fzz` 是个 zip，接线对不对**不该靠肉眼数线**。这里按 Fritzing 自己的
存储方式读连接——① 每个 instance 的 `<connector>/<connects>`；② **每条 wire 自身导通两端**；
③ 部件 `.fzp` 里的**内部 `<bus>`**（同网脚，例如 CH347F 的 11 个 GND、模组的 3 个 GND 焊盘）
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
MOD, MCU, BRIDGE, LED, RES = "MOD", "MCU", "BRIDGE", "LED", "RES"
ROLES = {
    MOD:    lambda mid: mid.startswith("TX-AH-R900PNR"),   # 任意一块 TX-AH 模组
    MCU:    lambda mid: mid == "CH32V203C8T6",            # nanoCH32V203 开发板
    BRIDGE: lambda mid: mid == "CH347F",                  # CH347F-EVT（USB ↔ 2×UART 桥）
    LED:    lambda mid: "ColorLED" in mid,                # Fritzing 核心 LED 部件
    RES:    lambda mid: "Resistor" in mid,                # Fritzing 核心电阻部件
}
ROLE_CN = {MOD: "模组", MCU: "MCU", BRIDGE: "桥", LED: "灯", RES: "电阻"}

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
        owners = {t for t, _m, _n in g}
        if len(owners) > 1:                     # 跨实例却没写进规格 = 多出来的连线
            ok = False
            print("    ✗ 多出来的跨实例网：%s"
                  % "  <->  ".join("%s.%s" % (t, n) for t, _m, n in sorted(g)))
        elif len(g) > 1:                        # 同一实例内部同网 = 部件自己的 <bus>，不是连线
            print("    · 部件内部同网（非图上连线）：%s"
                  % "  <->  ".join("%s.%s" % (t, n) for t, _m, n in sorted(g)))
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
