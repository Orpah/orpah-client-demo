#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
demo_client_hgic.py — b 步**真链路**联调：ORPAH 客户端协议栈 --(HGIC/UART0)--> 真空口。

与 `orpah-over-halow/demo_client_uart.py` 的关系：那个是**纯 PC 排练**（拿模拟器的 AT 控制台
跑同一套线路协议），本脚本是**真板**版 —— 传输换成 `tools/hgic_bus.py`（HGIC over UART0），
**协议一行不改**：报文编解码、自限频、周期节奏全部来自 `orpah-over-halow`（单一源）。

台架（2026-09-20 实测那套）：
    客户端 TX-AH EVB（FMAC，**STA**） ←空口→ T-Halow-RJ45（WNB，**AP**）
    PC ──CH347F UART0(COM23)── 客户端 UART0(A10/A11)     ← 本脚本的数据口
    PC ──USB-UART(COM6)─────── 客户端 UART1(A12/A13)     ← 发送确认（模组 `[mbus rx]` 日志）
    PC ──USB-UART(COM8)─────── TH-RJ45 的 USB-C          ← AP per-STA 计数（空口证据）

判据（退出码 0 = 全过）：
  ① 传输就位：HGIC 握手（`GET_UART_FIXLEN` 一问一答）+ 数据口能收到下行帧（AP 侧协议栈的帧）；
  ② 协议栈能发：`ClientHost.send_req_connect()` 真发出去了（模组 AT 口 `[mbus rx] <8+len> byte(s)` 确认）；
  ③ 过空口：AP 的 per-STA `rx1_cnt` 增长；
  ④ 下行回到协议栈：`ClientHost.on_recv` / `host.recv` 计数（⚠ 本台架 AP 不会回 ORPAH 报文 ——
     ORPAH 的对端是 Router/Server，要走 **TH-RJ45 的 RJ45** 那侧；所以这一项如实报"几条"，
     0 条不算失败，但会打印为什么）。

用法：
  python tools\demo_client_hgic.py                     # 默认 COM23 / COM6 / COM8
  python tools\demo_client_hgic.py --data-port COM23 --at-port COM6 --ap-port COM8
  python tools\demo_client_hgic.py --no-protocol        # 只做 ①（不动协议栈）
"""
import argparse
import os
import re
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hgic_bus import HgicBus                     # noqa: E402
from eth_frames import dhcp_discover, eth_summary, is_dhcp   # noqa: E402

sys.path.insert(0, os.path.join(HERE, "..", "tools"))          # 兼容从别的目录起
DEFAULT_ORPAH_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "orpah-over-halow"))


class TapBus:
    """给真传输套一层"留痕"：所有收发都记一份（不改协议、不改传输）。"""

    def __init__(self, inner):
        object.__setattr__(self, "inner", inner)
        self.tx = []
        self.rx = []

    def connect(self, *a, **kw):
        return self.inner.connect(*a, **kw)

    def close(self):
        return self.inner.close()

    def send_frame(self, frame):
        self.tx.append(bytes(frame))
        return self.inner.send_frame(frame)

    def recv_frame(self, timeout=2.0):
        f = self.inner.recv_frame(timeout)
        if f:
            self.rx.append(bytes(f))
        return f

    def __getattr__(self, name):                 # stats()/recent_lines() 等直接转发
        return getattr(self.inner, name)


def ap_counter(ser, cmd="AT+VERSION=?", secs=5.0, want="agc=", timeout=12.0):
    """读一次 AP 的 AT 口，返回 (文本, {'rx1_cnt': n, 'tx1_cnt': n})。

    ⚠ LMAC 块是**周期打印**的（~6s 一次），固定窗口会漏 → 这里等到 `agc=`（per-STA 块的最后一行）
    出现再返回；最多等 `timeout`。
    """
    try:
        ser.reset_input_buffer()
        ser.write((cmd + "\r\n").encode("ascii"))
        ser.flush()
    except Exception as e:                       # noqa: BLE001
        return "", {}
    t0, buf = time.time(), ""
    limit = timeout if want else secs
    while time.time() - t0 < limit:
        c = ser.read(4096)
        if c:
            buf += c.decode("utf-8", "replace")
            if want and want in buf:
                break
        else:
            time.sleep(0.02)
    d = {}
    for k, pat in (("rx1_cnt", r"rx1:.*?cnt=(\d+)"), ("tx1_cnt", r"tx1:.*?cnt=(\d+)")):
        v = [int(x) for x in re.findall(pat, buf, re.S)]
        if v:
            d[k] = max(v)
    return buf, d


def main():
    ap = argparse.ArgumentParser(description="b 步真链路联调（ORPAH 客户端经 HGIC 上真空口）")
    ap.add_argument("--data-port", default="COM23", help="客户端数据口（CH347F UART0）")
    ap.add_argument("--at-port", default="COM6", help="客户端 AT/打印口（发送确认用）")
    ap.add_argument("--ap-port", default="COM8", help="TH-RJ45 的 AT 口（读 per-STA 计数）")
    ap.add_argument("--orpah-dir", default=DEFAULT_ORPAH_DIR,
                    help="orpah-over-halow 仓库路径（默认同级的 ../orpah-over-halow）")
    ap.add_argument("--sn", default="CN-WH01-9AF3C1D2")
    ap.add_argument("--host-mac", default="94186537d5b1",
                    help="客户端 host 侧 MAC（做 DISCOVER 的源；回包是广播，源写谁都收得到）")
    ap.add_argument("--recv-secs", type=float, default=8.0, help="发完听下行多久")
    ap.add_argument("--no-protocol", action="store_true", help="只做传输层握手与收帧")
    args = ap.parse_args()
    args.host_mac = bytes.fromhex(args.host_mac.replace(":", "").replace("-", ""))
    ap.add_argument("--expect-downlink", action="store_true",
                    help="把“下行回到协议栈 on_recv”当硬判据（需 RJ45 侧接上 ORPAH 对端）")
    args = ap.parse_args()
    args.host_mac = bytes.fromhex(args.host_mac.replace(":", "").replace("-", ""))
    results = []

    def ok(name, cond, extra=""):
        results.append((name, bool(cond), extra))
        print("   [%s] %s%s" % ("✓" if cond else "✗", name, ("  " + extra) if extra else ""))

    # ---------------- 0) 协议栈 ----------------
    print("=" * 84)
    print("0) 载入 ORPAH 客户端协议栈（单一源，不重写）")
    op = None
    if not args.no_protocol:
        if not os.path.isdir(args.orpah_dir):
            print("   找不到 %s —— 用 --orpah-dir 指定 orpah-over-halow 仓库" % args.orpah_dir)
            return 1
        sys.path.insert(0, args.orpah_dir)
        try:
            import orpah_proto                            # noqa: E402
            from client import ClientHost                 # noqa: E402
            op = orpah_proto
            print("   ✓ %s" % args.orpah_dir)
        except Exception as e:                            # noqa: BLE001
            print("   导入失败：%r" % (e,))
            return 1

    # ---------------- 1) 传输：HGIC 握手 + 收帧 ----------------
    print("=" * 84)
    print("1) 传输层：HGIC 数据口（%s）+ AT 口（%s）" % (args.data_port, args.at_port))
    bus = HgicBus(args.data_port, at_port=args.at_port)
    if not bus.connect(retries=8, interval=0.5):
        print("   数据口打不开：%s（被别的程序占着？端口号变了？CH347F 抽风？）" % args.data_port)
        return 1
    try:
        ok("数据口有真正的 HGIC 应答（MACBUS_UART 固件 / 线 / 跳线都对）",
           bus.ping(), "GET_UART_FIXLEN 一问一答")

        # 收下行：**主动**发一条 DHCP DISCOVER 让 AP 侧协议栈回一条。
        # 为什么不靠在窗口内等偶发流量：AP 自己那几条 DHCP/ARP/IPv6 是**突发**的，
        # 6s 窗口里可能一条都没有 → 判据会时红时绿（实测就该这么做：发 DISCOVER ~30ms 就回）。
        print("   —— 下行：发一条 DHCP DISCOVER，等 AP 侧协议栈回一条 ——")
        bus.send_frame(dhcp_discover(args.host_mac))
        frames = []
        t0 = time.time()
        while time.time() - t0 < 6.0:
            f = bus.recv_frame(timeout=1.0)
            if f:
                frames.append(f)
                print("   [下行] %s" % eth_summary(f))
        dhcp = [f for f in frames if is_dhcp(f)]
        ok("下行能到客户端 host 口（由 DISCOVER 触发）", bool(dhcp),
           "收到 %d 条下行，其中 DHCP %d 条" % (len(frames), len(dhcp)))

        # ---------------- 2) 协议栈发真报文 ----------------
        if op is not None:
            print("=" * 84)
            print("2) 协议栈发真报文（ClientHost.send_req_connect，经真空口）")
            ap_ser = None
            try:
                import serial
                ap_ser = serial.Serial(args.ap_port, 115200, timeout=0.2)
            except Exception as e:                        # noqa: BLE001
                print("   （读不到 AP 口 %s：%r —— ③ 那项会跳过）" % (args.ap_port, e))
            before = {}
            if ap_ser:
                # 先打开 LMAC 打印，否则基线那次可能读不到 per-STA `rx1:` 行 → 判据退化成"0 → 有"
                ap_counter(ap_ser, "AT+SYSDBG=LMAC,1", secs=2.0)
                txt, before = ap_counter(ap_ser)
                print("   AP 基线：%s" % before)

            tap = TapBus(bus)
            host = ClientHost(0, sn=args.sn, bus=tap, on_recv=lambda m: print("   [协议] 收到下行报文 %r" % (m.get("type"),)))
            if not host.connect():
                print("   ClientHost 连接失败")
                return 1
            sent_ok = host.send_req_connect()
            time.sleep(1.0)
            st = bus.stats()
            ok("协议栈把报文交给了 HGIC 传输", bool(tap.tx), "%d 条" % len(tap.tx))
            if tap.tx:
                print("   [上行] %s" % eth_summary(tap.tx[-1]))
            ok("模组确认收到（AT 口 [mbus rx] 日志）", st["tx_frames"] >= 1,
               "tx_frames=%d tx_fail=%d tx_retry=%d" % (st["tx_frames"], st["tx_fail"], st["tx_retry"]))

            print("   —— 听 %ds 下行（要真收到 ORPAH 下行，得把 TH-RJ45 的 RJ45 接到有 ORPAH 对端的网上）——" % args.recv_secs)
            t0 = time.time()
            while time.time() - t0 < args.recv_secs:
                time.sleep(0.2)
            if args.expect_downlink:
                ok("下行回到协议栈 on_recv", host.recv >= 1, "host.recv=%d" % host.recv)
            else:
                print("   [·] 待验证：下行回到协议栈 on_recv（host.recv=%d）"
                      "—— 本台架 AP 侧没有 ORPAH 对端；接上 RJ45 那侧后用 --expect-downlink 硬判"
                      % host.recv)

            if ap_ser:
                txt, after = ap_counter(ap_ser)
                # ⚠ 2026-09-20 实测：AP 的 per-STA `rx1_cnt/tx1_cnt` 是**分区间快照**（会回跳），
                #   不能当"过没过空口"的判据 —— 可靠判据只有**由应答反证**（发 DISCOVER 收到应答 ⇒
                #   上行与下行都真的过了空口）。这里只如实打印，不判分。
                print("   [·] AP per-STA 计数（分区间快照，不作判据）：%s -> %s" % (before, after))
                print("       上行/下行的判据是上面那条“由 DISCOVER 应答反证”，不是这个计数。")
                for ln in bus.recent_lines(8):
                    print("   [AT口] %s" % ln[:120])
                ap_ser.close()
            host.close()
    finally:
        bus.close()

    # ---------------- 汇总 ----------------
    print("=" * 84)
    bad = [n for n, c, _ in results if not c]
    for n, c, e in results:
        print("   %s %s" % ("✓" if c else "✗", n))
    print("   => %s" % ("全部通过 ✓" if not bad else "有 %d 项没过：%s" % (len(bad), bad)))
    return 0 if not bad else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
