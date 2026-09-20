#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
demo_l2_hgic.py — b 步真链路**完整闭环**：PC 同时扮 **Router + Server**。

台架（2026-09-20 实测连好的那套）：

    PC(客户端侧) ──CH347F/UART0(HGIC)── 客户端 TX-AH(STA) ──空口── TH-RJ45(AP，L2 透明桥)
                                                                    └──RJ45── 有线网 ── PC(有线侧，Npcap)
    ↑ tools/hgic_bus.py                                                          ↑ tools/l2bus.py

**业务逻辑一行不改**：客户端 = 上游 `client.ClientHost`；Router = 上游 `router.RouterBridge`
（用 `bus=` 注入 `L2Bus`）；Server = 上游 `server.OrpahServer`；报文/签名/走失表全用上游实现。

判据（退出码 0 = 全过）：
  ① 周期上报：客户端发 N 次 REPORT，Server 都收下（`on_report` 计数）
  ② 服务端处理通过：客户端陆续收到 `ACCESS-INFO` 与 `TRACKING-STATUS`
  ③ 下行真到达：客户端 `on_recv` 收到下行报文（经空口 + CH347F 回到 PC 这一侧）
  ④ 上游零丢弃：Router / Server 的限频丢弃都为 0，客户端自限频延后为 0

用法：
  python tools\demo_l2_hgic.py                          # 有线侧网卡自动挑
  python tools\demo_l2_hgic.py --iface "WLAN 4" --reports 3
  python tools\demo_l2_hgic.py --data-port COM23 --at-port COM6 --sn CN-WH01-9AF3C1D2
"""
import argparse
import os
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hgic_bus import HgicBus                      # noqa: E402
from l2bus import L2Bus                          # noqa: E402

DEFAULT_ORPAH_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "orpah-over-halow"))


class Rec:
    """收集两侧看到的报文（判据都用它，不做隐式判断）。"""

    def __init__(self):
        self.access = []
        self.tracking = []
        self.reports = 0
        self.reports_in = []          # 服务端收下的 REPORT（含 sn/seq）
        self.client_recv = []         # 客户端 on_recv 收到的下行

    def on_report(self, msg):
        self.reports += 1
        self.reports_in.append(msg)

    def on_client_recv(self, msg):
        self.client_recv.append(msg)
        t = msg.get("type")
        if t and t.endswith("ACCESS-INFO"):
            self.access.append(msg)
        elif t and t.endswith("TRACKING-STATUS"):
            self.tracking.append(msg)


def wait_for(cond, timeout=6.0, interval=0.05):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(interval)
    return False


def main():
    ap = argparse.ArgumentParser(description="b 步真链路完整闭环（PC 同时扮 Router+Server）")
    ap.add_argument("--iface", default=None, help="有线侧网卡（名字或 MAC；不给就自动挑）")
    ap.add_argument("--data-port", default="COM23", help="客户端数据口（CH347F UART0）")
    ap.add_argument("--at-port", default="COM6", help="客户端 AT/打印口（发送确认）")
    ap.add_argument("--sn", default="CN-WH01-9AF3C1D2")
    ap.add_argument("--reports", type=int, default=3, help="周期上报次数")
    ap.add_argument("--udp-port", type=int, default=19447, help="本机 Server 的 UDP 端口")
    ap.add_argument("--orpah-dir", default=DEFAULT_ORPAH_DIR)
    args = ap.parse_args()

    if not os.path.isdir(args.orpah_dir):
        print("找不到 %s —— 用 --orpah-dir 指定 orpah-over-halow 仓库" % args.orpah_dir)
        return 1
    sys.path.insert(0, args.orpah_dir)
    try:
        import orpah_proto as op
        import downlink
        from client import ClientHost
        from router import RouterBridge
        from server import OrpahServer
    except Exception as e:                            # noqa: BLE001
        print("导入上游失败：%r" % (e,))
        return 1

    rec = Rec()
    results = []

    def ok(name, cond, extra=""):
        results.append((name, bool(cond), extra))
        print("   [%s] %s%s" % ("✓" if cond else "✗", name, ("  " + extra) if extra else ""))

    # ---------------- 1) Server（权威走失库）+ Router（L2 侧） ----------------
    print("=" * 84)
    print("1) 起 Server（UDP %d）+ Router（有线侧 = Npcap 裸以太帧）" % args.udp_port)
    down_priv, down_pub = downlink.demo_pair()
    srv = OrpahServer(port=args.udp_port, on_report=rec.on_report, down_key=down_priv)
    srv.start()

    l2 = L2Bus(iface=args.iface)
    if not l2.connect():
        print("   有线侧网卡打不开；用 `python tools\\l2bus.py list` 看可选项，再 --iface 指定")
        srv.stop()
        return 1
    print("   有线侧：iface=%s mac=%s" % (l2.iface, l2.local_mac()))
    mac_b = bytes(int(x, 16) for x in l2.local_mac().split(":"))
    # ★ self_mac 就用这块网卡的 MAC：Router 的下行帧源地址 = 本机 NIC，
    #   配合 L2Bus 的 drop_own，避免 Router 把自己发出去的帧当成上行收回来（回路）。
    rb = RouterBridge(None, server_port=args.udp_port, down_pub=down_pub,
                      self_mac=mac_b, bus=l2)
    if not rb.start():
        print("   Router 起不来（有线侧 connect 失败）")
        srv.stop()
        return 1

    # ---------------- 2) 客户端（CH347F/UART0 = HGIC） ----------------
    print("=" * 84)
    print("2) 起客户端（HGIC 数据口 %s / AT 口 %s）" % (args.data_port, args.at_port))
    bus = HgicBus(args.data_port, at_port=args.at_port)
    if not bus.connect():
        print("   数据口打不开：%s" % args.data_port)
        rb.stop()
        srv.stop()
        return 1
    ok("HGIC 数据口一问一答（MACBUS_UART 固件在线）", bus.ping(), "GET_UART_FIXLEN")
    host = ClientHost(0, sn=args.sn, bus=bus, on_recv=rec.on_client_recv)
    if not host.connect():
        print("   ClientHost 连不上")
        rb.stop()
        srv.stop()
        return 1

    try:
        # ---------------- 3) REQ-CONNECT → ACCESS-INFO ----------------
        print("=" * 84)
        print("3) REQ-CONNECT → 期望收到 ACCESS-INFO")
        host.send_req_connect()
        got_access = wait_for(lambda: bool(rec.access), timeout=8.0)
        ok("客户端收到 ACCESS-INFO（下行已到）", got_access,
           "tracked=%s" % (rec.access[-1].get("tracked") if rec.access else "-"))

        # ---------------- 4) 周期上报 ----------------
        print("=" * 84)
        print("4) 周期上报 %d 次（每次都要被 Server 收下并回 TRACKING-STATUS）" % args.reports)
        for i in range(1, args.reports + 1):
            n0 = len(rec.tracking)
            host.report_once()
            got = wait_for(lambda: len(rec.tracking) > n0, timeout=6.0)
            print("   第 %d 次：Server 收下 %s / 回执 %s"
                  % (i, "✓" if rec.reports >= i else "✗",
                     rec.tracking[-1].get("status") if len(rec.tracking) > n0 else "✗ 未收到"))
            time.sleep(0.3)
        ok("① 周期上报：Server 都收下", rec.reports >= args.reports,
           "reports=%d/%d" % (rec.reports, args.reports))
        ok("② 服务端处理通过：收到 TRACKING-STATUS", len(rec.tracking) >= args.reports,
           "tracking=%d" % len(rec.tracking))

        # ---------------- 5) mark 走失 → 再报一次应变 TRACKED ----------------
        print("=" * 84)
        print("5) Server 标记走失 → 再报一次，期望 TRACKED（走失表也随之下发到 Router）")
        srv.mark_tracked(args.sn, note="bench-l2")
        time.sleep(0.8)
        n0 = len(rec.tracking)
        host.report_once()
        wait_for(lambda: len(rec.tracking) > n0, timeout=6.0)
        last = rec.tracking[-1].get("status") if len(rec.tracking) > n0 else "-"
        ok("走失命中后回执 = TRACKED", last == op.ST_TRACKED, "status=%s" % last)
        print("   Router 走失缓存：%s" % rb.lost_cache)

        ok("③ 下行真到达客户端（on_recv 累计）", len(rec.client_recv) >= 2,
           "client_recv=%d（ACCESS-INFO %d / TRACKING-STATUS %d）"
           % (len(rec.client_recv), len(rec.access), len(rec.tracking)))
        ok("④ 上游零丢弃（Router/Server 限频 + 客户端自限频延后）",
           rb.rl_dropped == 0 and len(srv.rl_drops) == 0 and host.self_held == 0,
           "router_dropped=%d server_dropped=%d client_held=%d"
           % (rb.rl_dropped, len(srv.rl_drops), host.self_held))
    finally:
        print("=" * 84)
        print("计数（供核对）：")
        print("   有线侧：%s" % l2.stats())
        print("   HGIC  ：%s" % bus.stats())
        print("   Router：up=%d down=%d dropped=%d" % (rb.up_count, rb.down_count, rb.rl_dropped))
        print("   Server：reports=%d rl_drops=%d" % (rec.reports, len(srv.rl_drops)))
        try:
            host.close()
        except Exception:                             # noqa: BLE001
            pass
        rb.stop()
        srv.stop()
        l2.close()

    bad = [n for n, c, _ in results if not c]
    print("=" * 84)
    for n, c, e in results:
        print("   %s %s" % ("✓" if c else "✗", n))
    print("   => %s" % ("全部通过 ✓" if not bad else "有 %d 项没过：%s" % (len(bad), bad)))
    return 0 if not bad else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
