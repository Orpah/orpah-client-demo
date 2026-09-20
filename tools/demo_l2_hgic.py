#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
demo_l2_hgic.py — b 步真链路**完整闭环**：PC 同时扮 Router + Server（含 Orpah ID 验签）。

台架（2026-09-20 实测连好的那套）：

    PC(客户端侧) ──CH347F/UART0(HGIC)── 客户端 TX-AH(STA) ──空口── TH-RJ45(AP，L2 透明桥)
                                                                    └──RJ45── 有线网 ── PC(有线侧，Npcap)
    ↑ tools/hgic_bus.py                                                          ↑ tools/l2bus.py

**设备与业务逻辑一行不改**：
  · 设备侧 = 上游 `client_sim.DeviceSim`（`cycle()` = REQ-CONNECT → REPORT → 已签 ID-REPORT，
    顺序与 `ui_server._report_loop` 一致）——报文/选级/签名/自限频全走它的单一源；
  · Router = 上游 `router.RouterBridge`（`bus=` 注入 `L2Bus`）；Server = 上游 `server.OrpahServer`；
  · 密钥 = 上游 `orpah_id.Device`（同 `(sn, gen)` 派生），密钥库 = 上游 `orpah_id.KeyStore`。
本脚本只做两件事：**把两端传输换成真机** + **判定**。

判据（退出码 0 = 全过）：
  ① 周期上报：N 拍 REPORT 都被 Server 收下
  ② 服务端回执：客户端陆续收到 `ACCESS-INFO` 与 `TRACKING-STATUS`
  ③ 下行真到达：下行报文经空口 + CH347F 回到设备（`DeviceSim.down_count`）
  ④ 上游零丢弃：Router / Server 的限频丢弃 = 0
  ⑤ **ID 验签通过**：已签 `ORPAH-ID-REPORT` 过空口 + 有线到 Server，`accepted=True`
  ⑥ **负对照**：把已签报文的 payload 改掉（连 nonce 一起换）⇒ 必须**被拒**
     —— 证明验签真在跑，不是橡皮图章

soak 模式（`--minutes > 0`）另加四条：⑨ 上游零丢弃 / ⑩ 数据口无重开无坏字节 /
  ⑪ 期间**无真验签失败** / ⑫ 无链路中断（每次都有回执）。
  ★ ⑪ 按**理由码**分类（`BENIGN_ID_ERRORS`）：`replay_detected` = 我们自己重发出去的
    **第二份**（见 `hgic_bus.send_frame` 的确认重试），协议靠 nonce 去重正确吸收 ⇒
    **不算失败**，但速率照实打印；**其它任何理由码 ⇒ 判失败**。

用法：
  python tools\demo_l2_hgic.py                          # 有线侧网卡自动挑
  python tools\demo_l2_hgic.py --iface "WLAN 4" --reports 3
  python tools\demo_l2_hgic.py --data-port COM23 --at-port COM6 --sn CN-WH01-9AF3C1D2
"""
import argparse
import copy
import os
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hgic_bus import HgicBus                      # noqa: E402
from l2bus import L2Bus                          # noqa: E402

DEFAULT_ORPAH_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "orpah-over-halow"))

# 判据 ⑪ 的分类依据（**单一源**）：这些理由码 = 「协议正确防住了我们自己的重复投递」，
# 不是验签失败。来历（2026-09-20 4h soak 取证）：`hgic_bus.send_frame` 写帧后只等 1.5s
# 模组日志 `[mbus rx] <8+len> byte(s)`，等不到就**重发同一条帧**（第一份其实已经出去了）
# ⇒ 空口上出现两份；服务端 nonce 去重（SPEC §5.5）拒掉第二份。实测 ≈0.5% 的拍，且只见于
# 483B 的 ID 报文（3635 条 REPORT 收包零重复）。**除本表外的任何理由码都要让 ⑪ 判失败。**
BENIGN_ID_ERRORS = ("replay_detected",)


class Rec:
    """收集两侧看到的报文（判据都用它，不做隐式判断）。"""

    def __init__(self):
        self.access = []
        self.tracking = []
        self.reports = 0
        self.reports_in = []          # 服务端收下的 REPORT（含 sn/seq）
        self.client_recv = []         # 客户端 on_recv 收到的下行
        self.id_recs = []             # 服务端验签结果（on_id_report 回调）

    def on_id(self, rec):
        self.id_recs.append(rec)

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
    ap.add_argument("--reports", "--cycles", type=int, default=3,
                    help="闭环拍数（每拍 = REQ-CONNECT + REPORT + 已签 ID-REPORT）")
    ap.add_argument("--gap", type=float, default=1.8,
                    help="拍间隔秒（默认 1.8s：远快于设计常态 60s，又慢于设备自限频 1.67 条/秒）")
    ap.add_argument("--minutes", type=float, default=0.0,
                    help=">0 = 跑 soak（先跑完前面的短流程确立判据，再持续跑这么久）")
    ap.add_argument("--log-every", type=float, default=60.0, help="soak 每隔多少秒打一行进度")
    ap.add_argument("--rtc", choices=["none", "true", "false"], default="false",
                    help="cap.rtc 三态声明（默认 false：真机 CH32V203 无 RTC）")
    ap.add_argument("--real-ts", action="store_true",
                    help="发真实时间戳（默认为真机形态发 ts=0）")
    ap.add_argument("--battery-mv", type=int, default=None,
                    help="设备自报储能电压 mV（默认 = 能量模型的 mv_of(CHARGE0_MJ, STORE_MJ)）")
    ap.add_argument("--udp-port", type=int, default=19447, help="本机 Server 的 UDP 端口")
    ap.add_argument("--orpah-dir", default=DEFAULT_ORPAH_DIR)
    args = ap.parse_args()

    if not os.path.isdir(args.orpah_dir):
        print("找不到 %s —— 用 --orpah-dir 指定 orpah-over-halow 仓库" % args.orpah_dir)
        return 1
    sys.path.insert(0, args.orpah_dir)
    try:
        import client_sim
        import downlink
        import energy as en
        import orpah_id as oid
        import orpah_proto as op
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

    # ---------------- 1) 客户端设备 + 密钥库（Server 要先认识这个 SN） ----------------
    print("=" * 84)
    print("1) 建客户端设备（上游 DeviceSim）+ 密钥库登记")
    bus = HgicBus(args.data_port, at_port=args.at_port)
    host = ClientHost(0, sn=args.sn, bus=bus)
    # ★ 设备侧用上游 `DeviceSim`（`cycle()` = REQ-CONNECT → REPORT → 已签 ID-REPORT，
    #   顺序与 `ui_server._report_loop` 一致）——报文/选级/签名/自限频全走它的单一源，
    #   本脚本只把传输换成真机（`client=` 注入）。
    # ★ 设备声明按**真机形态**（无 RTC 的 CH32V203）来：ts=0 + cap.rtc=false，并如实报储能电压。
    cap_rtc = {"none": None, "true": True, "false": False}[args.rtc]
    batt_mv = (args.battery_mv if args.battery_mv is not None
               else en.mv_of(en.CHARGE0_MJ, en.STORE_MJ))    # 演示标定，不是实测
    sim = client_sim.DeviceSim(sn=args.sn, id_report=True, client=host, log=print,
                               cap_rtc=cap_rtc, ts_zero=not args.real_ts,
                               battery_mv=batt_mv)
    print("   设备声明：cap.rtc=%s / ts=%s / battery_mv=%d mV（%s）"
          % (args.rtc, "真实时间" if args.real_ts else "0", batt_mv,
             "命令行给定" if args.battery_mv is not None else "energy.mv_of 演示映射"))
    dev = sim._ensure_device()      # ★ 登记**同一个**设备对象（不重跑一遍 (sn, gen) 派生）
    ks = oid.KeyStore()
    ks.register(dev, model="bench-l2-hgic", firmware="bench")
    print("   设备 sn=%s 已登记进内存密钥库（演示密钥由 (sn, gen) 派生，非真机做法）" % args.sn)

    # ---------------- 2) Server（权威走失库，含 ID 验签）+ Router（L2 侧） ----------------
    print("=" * 84)
    print("2) 起 Server（UDP %d）+ Router（有线侧 = Npcap 裸以太帧）" % args.udp_port)
    down_priv, down_pub = downlink.demo_pair()
    srv = OrpahServer(port=args.udp_port, on_report=rec.on_report,
                      on_id_report=rec.on_id, down_key=down_priv, keystore=ks)
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

    # ---------------- 3) 打开客户端数据口（真机这一侧） ----------------
    print("=" * 84)
    print("3) 打开客户端数据口（HGIC %s / AT %s）" % (args.data_port, args.at_port))
    if not client_sim.connect(sim, retries=10):        # = host.connect() + 下行接到 DeviceSim
        print("   数据口打不开：%s" % args.data_port)
        rb.stop()
        srv.stop()
        return 1
    # 上面 `connect` 把下行接到了 `DeviceSim.on_down`；本脚本判据也要留一份
    host.on_recv = lambda m: (rec.on_client_recv(m), sim.on_down(m))
    ok("HGIC 数据口一问一答（MACBUS_UART 固件在线）", bus.ping(), "GET_UART_FIXLEN")

    # 抓一份「设备真正发出去的报文」——第 6 节的负对照要用它（不在夹具里重造报文）
    sent_ids = []
    _orig_send_id = host.send_id_report

    def _spy_send_id(rep, force=False):
        sent_ids.append(rep)
        return _orig_send_id(rep, force=force)

    host.send_id_report = _spy_send_id

    # ---------------- 7) soak（--minutes > 0）：长跑看链路/端口稳定 ----------------
    def soak(minutes, gap, log_every, ran_cycles):
        """持续跑同一套闭环，周期打点；连续无回执就**如实早停**。

        为什么这么做：b 步在真机上已经能跑单场闭环，但「能不能一直跑」是另一件事 ——
        CH347F 的 VCP、USB、AP、家用 LAN 任一抽风都会现形，而**只有长跑才看得见**。
        """
        t0 = time.time()
        end = t0 + minutes * 60.0
        cycles = ran_cycles
        # 负对照（故意篡改的那一条）发生在 soak **之前** ⇒ 它的“被拒”不能算进 soak 判据
        id_seen_before = len(rec.id_recs)
        miss = 0
        stall = 0
        max_stall = 0
        next_log = t0 + log_every

        def stat_ids():
            """soak 期间的 ID 验签结果分类（进度行与收尾判据**共用**这一处）。

            返回 (总数, 被拒, 重复份, 真失败)：重复份 = 理由码落在 `BENIGN_ID_ERRORS`
            里的那些（我们自己重发造成的第二份）；真失败 = 其余被拒。
            """
            seen = rec.id_recs[id_seen_before:]
            bad = [r for r in seen if not r["accepted"]]
            d = sum(1 for r in bad if r.get("error") in BENIGN_ID_ERRORS)
            return len(seen), len(bad), d, len(bad) - d

        print("=" * 84)
        print("7) soak：%g 分钟（拍间隔 %.2fs）—— 设计常态是 60s/拍，这里为演示加速 ≈ %.0f 倍"
              % (minutes, gap, 60.0 / gap))
        print("   ⚠ 期间别用 COM23/COM6，也别关这个 VS Code 窗口（后台任务活在这里）")
        while time.time() < end:
            n_t, n_i = len(rec.tracking), len(rec.id_recs)
            sim.cycle()
            cycles += 1
            got = wait_for(lambda: len(rec.tracking) > n_t, timeout=8.0)
            wait_for(lambda: len(rec.id_recs) > n_i, timeout=8.0)
            if got:
                stall = 0
            else:
                miss += 1
                stall += 1
                max_stall = max(max_stall, stall)
                if stall >= 5:
                    print("   ✗ 连续 %d 拍没有回执 —— 判定链路已断，提前结束 soak" % stall)
                    break
            if time.time() >= next_log:
                next_log = time.time() + log_every
                st = bus.stats()
                n_id, n_bad, n_dup, n_real = stat_ids()
                print("   [%6.0fs] 拍=%d 上/回=%d/%d ID=%d(重复份=%d 真失败=%d) "
                      "丢(rtr/srv)=%d/%d 延后=%d 重开=%d 坏字节=%d 缺回执=%d"
                      % (time.time() - t0, cycles, rec.reports, len(rec.tracking),
                         n_id, n_dup, n_real, rb.rl_dropped, len(srv.rl_drops),
                         host.self_held, st["reopens"], st["bad_bytes"], miss))
            time.sleep(gap)

        el = max(time.time() - t0, 1e-6)
        st = bus.stats()
        n_id, n_bad_id, n_dup_id, n_real_bad = stat_ids()
        print("   soak 结束：%.1f 分钟 / 共 %d 拍（%.2f 拍/秒）"
              % (el / 60.0, cycles, cycles / el))
        if n_bad_id:
            reasons = {}
            for r in rec.id_recs[id_seen_before:]:
                if not r["accepted"]:
                    k = r.get("error")
                    reasons[k] = reasons.get(k, 0) + 1
            print("   ID 被拒 %d/%d，按理由码：%s（单列 %s = 重发的重复份，其余都算真失败）"
                  % (n_bad_id, n_id, reasons, "/".join(BENIGN_ID_ERRORS)))
            if n_dup_id:
                print("   ⚠ 重复投递率 ≈ %.2f%%（%d/%d 条）—— 来源 = host 侧发送确认重试"
                      "（`hgic_bus.send_frame` 1.5s 内没看到模组 `[mbus rx] n byte(s)` 就重发，"
                      "而第一份已经出去了）；协议侧由 nonce 去重吸收（SPEC §5.5）。"
                      % (100.0 * n_dup_id / max(n_id, 1), n_dup_id, n_id))
            if n_real_bad:
                r0 = next(r for r in rec.id_recs[id_seen_before:]
                          if not r["accepted"] and r.get("error") not in BENIGN_ID_ERRORS)
                print("   ✗ 真失败样例：error=%s trust=%s alg=%s level=%s"
                      % (r0.get("error"), r0.get("trust"), r0.get("alg"), r0.get("level")))
        ok("⑨ soak：上游零丢弃（Router/Server 限频）",
           rb.rl_dropped == 0 and len(srv.rl_drops) == 0,
           "router_dropped=%d server_dropped=%d" % (rb.rl_dropped, len(srv.rl_drops)))
        ok("⑩ soak：数据口无重开、无坏字节",
           st["reopens"] == 0 and st["bad_bytes"] == 0,
           "reopens=%d bad_bytes=%d tx_retry=%d"
           % (st["reopens"], st["bad_bytes"], st.get("tx_retry", -1)))
        ok("⑪ soak：期间无真验签失败（重发的重复份单列，不算失败）",
           n_real_bad == 0,
           "soak 期间 ID=%d：被拒=%d（重复份=%d / 真失败=%d）；soak 前另有 %d 条"
           % (n_id, n_bad_id, n_dup_id, n_real_bad, id_seen_before))
        ok("⑫ soak：无链路中断（每次都有回执）",
           miss == 0 and max_stall < 5,
           "缺回执=%d 次，最长连续=%d 拍" % (miss, max_stall))

    try:
        # ---------------- 4) 闭环：N 拍（每拍 = REQ-CONNECT → REPORT → 已签 ID-REPORT） ----------------
        print("=" * 84)
        print("4) 跑 %d 拍（顺序与 ui_server 一致；拍间隔 %.1fs —— 设计常态是 60s，这里为演示加速）"
              % (args.reports, args.gap))
        for i in range(1, args.reports + 1):
            n_t, n_i, n_d = len(rec.tracking), len(rec.id_recs), sim.down_count
            sim.cycle()          # ← 设备侧全走上游实现（含已签 ID 上报）
            wait_for(lambda: len(rec.tracking) > n_t, timeout=8.0)
            wait_for(lambda: len(rec.id_recs) > n_i, timeout=8.0)
            st = rec.tracking[-1].get("status") if len(rec.tracking) > n_t else "✗ 未收到"
            idr = rec.id_recs[-1] if len(rec.id_recs) > n_i else None
            print("   第 %d 拍：回执=%s  ID 验签=%s  本拍下行=%d"
                  % (i, st,
                     ("✓ accepted" if (idr and idr["accepted"])
                      else ("✗ %s" % (idr.get("error") if idr else "未收到"))),
                     sim.down_count - n_d))
            time.sleep(args.gap)

        n_ok_id = sum(1 for r in rec.id_recs if r["accepted"])
        if any(not r["accepted"] for r in rec.id_recs):
            r0 = next(r for r in rec.id_recs if not r["accepted"])
            print("   ⚠ 有 ID 被拒：error=%s trust=%s alg=%s level=%s%s"
                  % (r0.get("error"), r0.get("trust"), r0.get("alg"), r0.get("level"),
                     "（= 重发的重复份，协议用 nonce 去重正确拒掉；不是验签失败）"
                     if r0.get("error") in BENIGN_ID_ERRORS else "（**非**重复份 → 要查）"))
        ok("① 周期上报：Server 都收下", rec.reports >= args.reports,
           "reports=%d/%d" % (rec.reports, args.reports))
        ok("② 服务端回执：收到 TRACKING-STATUS", len(rec.tracking) >= args.reports,
           "tracking=%d" % len(rec.tracking))
        ok("③ 下行真到达（经空口回到设备）", sim.down_count >= args.reports + 1,
           "down=%d（ACCESS-INFO %d / TRACKING-STATUS %d）"
           % (sim.down_count, len(rec.access), len(rec.tracking)))
        ok("⑤ ID 验签通过（已签报文过空口 + 有线到 Server）", n_ok_id >= args.reports,
           "accepted=%d/%d  level=%s ts_src=%s battery_mv=%s"
           % (n_ok_id, args.reports,
              rec.id_recs[-1].get("level") if rec.id_recs else "-",
              rec.id_recs[-1].get("ts_src") if rec.id_recs else "-",
              rec.id_recs[-1].get("battery_mv") if rec.id_recs else "-"))

        # ⑦⑧：时间基准与电量声明（都靠已签事实说话）
        last_id = rec.id_recs[-1] if rec.id_recs else {}
        ok("⑦ 无 RTC 声明生效：服务端改用接收时刻（ts_src=server 且记录时刻≈现在）",
           bool(last_id) and last_id.get("ts_src") == "server"
           and abs(float(last_id.get("ts_eff") or 0) - time.time()) < 120,
           "ts_src=%s ts_eff=%s cap_rtc=%s ts_ok=%s"
           % (last_id.get("ts_src"), last_id.get("ts_eff"),
              last_id.get("cap_rtc"), last_id.get("ts_ok")))
        ok("⑧ 电量报进签名里（battery_mv 原样到达 Server）",
           bool(last_id) and last_id.get("battery_mv") == batt_mv,
           "battery_mv=%s（设备报 %d）level=%s degraded_reason=%s"
           % (last_id.get("battery_mv"), batt_mv,
              last_id.get("level"), last_id.get("degraded_reason")))

        # ---------------- 5) mark 走失 → 再跑一拍应变 TRACKED ----------------
        print("=" * 84)
        print("5) Server 标记走失 → 再跑一拍，期望 TRACKED（走失表也随之下发到 Router）")
        srv.mark_tracked(args.sn, note="bench-l2")
        time.sleep(0.8)
        n0 = len(rec.tracking)
        sim.cycle()
        wait_for(lambda: len(rec.tracking) > n0, timeout=8.0)
        last = rec.tracking[-1].get("status") if len(rec.tracking) > n0 else "-"
        ok("走失命中后回执 = TRACKED", last == op.ST_TRACKED, "status=%s" % last)
        print("   Router 走失缓存：%s" % rb.lost_cache)
        time.sleep(args.gap)

        # ---------------- 6) 负对照：篡改 payload 的已签报文必须被拒 ----------------
        print("=" * 84)
        print("6) 负对照：把一条**已签**报文的 payload 改掉（连 nonce 一起换，免得先被 nonce 去重拦下）")
        if not sent_ids:
            ok("⑥ 负对照：篡改 payload 的已签报文被拒", False, "没抓到已签报文（spy 未生效）")
        else:
            bad = copy.deepcopy(sent_ids[-1])
            pl = bad.setdefault("payload", {})
            pl["nonce"] = os.urandom(16).hex().upper()     # 新 nonce ⇒ 不让去重先拦
            pl["battery_mv"] = int(pl.get("battery_mv") or 3000) + 123
            n_i = len(rec.id_recs)
            host.send_id_report(bad, force=True)           # 演示注入 ⇒ force（非本机业务上报）
            wait_for(lambda: len(rec.id_recs) > n_i, timeout=8.0)
            r = rec.id_recs[-1] if len(rec.id_recs) > n_i else None
            ok("⑥ 负对照：篡改 payload 的已签报文被拒",
               r is not None and not r["accepted"],
               "error=%s" % (r.get("error") if r else "未收到结果"))

        ok("④ 上游零丢弃（Router / Server 限频）",
           rb.rl_dropped == 0 and len(srv.rl_drops) == 0,
           "router_dropped=%d server_dropped=%d" % (rb.rl_dropped, len(srv.rl_drops)))
        if host.self_held:
            print("   ⚠ 设备侧自限频**延后**了 %d 条（不是丢弃，下一拍会再发）——"
                  "说明拍间隔比设备自限频（0.6s / 1.67 条每秒）还快，把 --gap 调大即可"
                  % host.self_held)

        # ---------------- 7) soak（可选，--minutes > 0） ----------------
        if args.minutes > 0:
            soak(args.minutes, args.gap, args.log_every, sim.tick)
    finally:
        print("=" * 84)
        print("计数（供核对）：")
        print("   有线侧：%s" % l2.stats())
        print("   HGIC  ：%s" % bus.stats())
        print("   Router：up=%d down=%d dropped=%d" % (rb.up_count, rb.down_count, rb.rl_dropped))
        print("   Server：reports=%d id_reports=%d（验签通过 %d）rl_drops=%d"
              % (rec.reports, srv.id_report_total,
                 sum(1 for r in rec.id_recs if r["accepted"]), len(srv.rl_drops)))
        print("   设备侧：cycle=%d req=%d report=%d id=%d held=%d"
              % (sim.tick, sim.req_sent, sim.report_sent, sim.id_sent, host.self_held))
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
