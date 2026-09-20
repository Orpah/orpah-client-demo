#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
l2bus.py — 有线侧（Router/Server 那一半）的**裸以太帧**传输，走 Npcap + scapy。

b 步真链路的两半（见 `tools/demo_client_hgic.py` 的注释）：
    客户端 TX-AH(STA) ──CH347F/UART0+HGIC── PC        ← `hgic_bus.py`（客户端侧）
    客户端 ──空口── TH-RJ45(AP，L2 透明桥) ──RJ45── 有线路 ── PC   ← **本文件**（Router/Server 侧）

于是 PC 可以**同时扮 Router + Server**：从这块"有线网卡"上收客户端发来的 `ethertype 0x88B5`
裸帧，按 ORPAH 的 Router/Server 逻辑处理，再把下行帧从同一个口送回去（原路经空口回到客户端）。

为什么是 Npcap + scapy：Windows 上要收发**裸以太帧**（不是 IP/UDP）只有这条路；本机已装
Npcap 1.71 + Wireshark（服务在跑）。⚠ 需要 `pip install scapy`；Npcap 若勾了"仅管理员可用"
则本工具也要以管理员身份运行（默认不勾 ⇒ 普通用户可用）。

用法：
    python tools\l2bus.py list                      # 列出可用网卡（挑有线路那张）
    python tools\l2bus.py probe --secs 15          # 打开 + 打印本机 MAC + 看有没有 0x88B5 帧
    python tools\l2bus.py probe --iface "以太网 2" --secs 15

作为库用（与 `hgic_bus.py` 同 API，可直接喂给上游的 Router/Server 侧代码）：
    bus = L2Bus(iface="以太网 2");  bus.connect()
    bus.send_frame(eth_frame);      f = bus.recv_frame(timeout=2.0)
    bus.close();                    bus.local_mac()   # ★ 客户端要把上行帧发给这个 MAC
"""
import argparse
import os
import queue
import sys
import threading
import time
import warnings

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
# scapy 导入时会顺着 cryptography 打一条 FFDH 弃用警告 —— 与本事无关，压掉（PowerShell 还会把它
# 当 NativeCommandError 红一片，看着像报错）。
warnings.filterwarnings("ignore", message=r".*Diffie-Hellman.*")
warnings.filterwarnings("ignore", module=r"scapy.*")

ORPAH_ETHERTYPE = 0x88B5

try:
    from scapy.all import AsyncSniffer, conf, get_if_hwaddr, get_if_list  # noqa: E402
    from scapy.config import conf as scapy_conf                          # noqa: E402
except ImportError as _exc:                                              # pragma: no cover
    AsyncSniffer = conf = get_if_hwaddr = get_if_list = None
    scapy_conf = None
    _SCAPY_ERR = _exc
else:
    _SCAPY_ERR = None


def _need_scapy():
    if _SCAPY_ERR is not None:
        raise RuntimeError("没装 scapy（裸以太帧收发要用它）：pip install scapy\n原始错误：%r" % _SCAPY_ERR)
    if not os.path.exists(r"C:\Windows\System32\Npcap"):
        print("[l2bus] ⚠ 没看到 Npcap（C:\\Windows\\System32\\Npcap）—— scapy 需要它才能收发裸帧")


def list_ifaces():
    """返回 [(iface, mac)]，scapy 视角下的可用网卡。"""
    _need_scapy()
    out = []
    for i in get_if_list():
        try:
            out.append((i, get_if_hwaddr(i)))
        except Exception:                              # noqa: BLE001
            out.append((i, "<无 MAC>"))
    return out


def _pick_iface(iface):
    """`--iface` 可以给 scapy 名、给 Windows 名（如 'WLAN 4'）、或给 MAC。"""
    _need_scapy()
    cand = list_ifaces()
    if iface:
        for name, mac in cand:
            if iface == name or iface.lower() == mac.lower():
                return name
        # 允许给 Windows 里的显示名（scapy 的 NPF 名字里带 GUID，对不上就用 MAC 匹配）
        for name, mac in cand:
            if iface.lower() in name.lower():
                return name
        raise SystemExit("找不到网卡 %r；用 `list` 看可选项" % iface)
    # 没给就挑"像有线口"的：排除蓝牙/回环/虚拟
    for name, mac in cand:
        low = name.lower()
        if "loopback" in low or mac == "00:00:00:00:00:00":
            continue
        if mac.lower() in ("f4:4e:fc:89:79:6f",):      # 蓝牙 PAN（本机实测）
            continue
        return name
    raise SystemExit("挑不出网卡，请用 --iface 指定（先跑 list）")


class L2Bus:
    """有线侧的裸以太帧传输。API 与 `hgic_bus.HgicBus` / 上游 `SerialAtBus` 一致。"""

    def __init__(self, iface=None, ethertype=ORPAH_ETHERTYPE, log=None,
                 promisc=True, store_outgoing=True):
        _need_scapy()
        self.iface = iface
        self.ethertype = ethertype
        self.log = log
        self.promisc = promisc
        self.store_outgoing = store_outgoing
        self.mac = None
        self.sock = None
        self.sniffer = None
        self.rx_frames = queue.Queue()
        self.tx_frames = 0
        self.tx_fail = 0
        self.rx_frames_n = 0
        self.bad = 0

    # ---------------- 连接 ----------------
    def connect(self, retries=3, interval=1.0):
        if self.sock is not None:
            return True                                  # 幂等
        self.iface = _pick_iface(self.iface)
        try:
            self.mac = get_if_hwaddr(self.iface)
        except Exception:                                # noqa: BLE001
            self.mac = None
        try:
            self.sock = scapy_conf.L2socket(iface=self.iface)
        except Exception as e:                           # noqa: BLE001
            self._say("打开发送 socket 失败：%r" % (e,))
            return False
        flt = "ether proto 0x%04x" % self.ethertype
        self.sniffer = AsyncSniffer(iface=self.iface, filter=flt, store=False,
                                    prn=self._on_pkt, promisc=True)
        self.sniffer.start()
        time.sleep(0.5)                                  # 让 Npcap 把过滤器挂上
        self._say("已就绪：iface=%s mac=%s filter=%r" % (self.iface, self.mac, flt))
        return True

    def close(self):
        if self.sniffer is not None:
            try:
                self.sniffer.stop()
            except Exception:                            # noqa: BLE001
                pass
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:                            # noqa: BLE001
                pass
        self.sock = None

    def _say(self, *a):
        if self.log:
            self.log("[l2bus]", *a)
        else:
            print("[l2bus]", *a, flush=True)

    def _on_pkt(self, pkt):
        try:
            raw = bytes(pkt)
        except Exception:                                # noqa: BLE001
            self.bad += 1
            return
        if len(raw) < 14:
            self.bad += 1
            return
        self.rx_frames.put(raw)
        self.rx_frames_n += 1

    # ---------------- 收发 ----------------
    def send_frame(self, eth_frame):
        eth = bytes(eth_frame)
        if self.sock is None:
            self.tx_fail += 1
            return False
        if len(eth) < 14:
            self._say("帧太短（%dB < 14B 以太头）→ 不发" % len(eth))
            self.tx_fail += 1
            return False
        try:
            self.sock.send(eth)
        except Exception as e:                           # noqa: BLE001
            self._say("发送失败：%r（Npcap 需要管理员权限？）" % (e,))
            self.tx_fail += 1
            return False
        self.tx_frames += 1
        return True

    def recv_frame(self, timeout=2.0):
        try:
            return self.rx_frames.get(timeout=timeout)
        except queue.Empty:
            return None

    # ---------------- 观测 ----------------
    def local_mac(self):
        return self.mac

    def stats(self):
        return {"iface": self.iface, "mac": self.mac, "tx_frames": self.tx_frames,
                "tx_fail": self.tx_fail, "rx_frames": self.rx_frames_n,
                "queued": self.rx_frames.qsize(), "bad": self.bad}


def eth_summary(p):
    import struct
    p = bytes(p)
    if len(p) < 14:
        return "（%d 字节）" % len(p)
    dst, src, et = p[:6].hex(":"), p[6:12].hex(":"), struct.unpack(">H", p[12:14])[0]
    ex = " ★ORPAH" if et == ORPAH_ETHERTYPE else ""
    if et == 0x88B5:
        ex += " %r" % (p[14:60],)
    return "dst=%s src=%s et=0x%04x%s [%dB]" % (dst, src, et, ex, len(p))


def main():
    ap = argparse.ArgumentParser(description="有线侧裸以太帧传输（Npcap + scapy）")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="列出可用网卡")
    p = sub.add_parser("probe", help="打开网卡、打印本机 MAC、看有没有 0x88B5 帧")
    p.add_argument("--iface", default=None, help="网卡（名字或 MAC；不给就自动挑）")
    p.add_argument("--secs", type=float, default=15.0)
    args = ap.parse_args()

    if args.cmd == "list":
        for name, mac in list_ifaces():
            print("%-46s %s" % (name, mac))
        return 0

    bus = L2Bus(iface=args.iface)
    if not bus.connect():
        return 1
    print("本机 MAC（客户端上行要发给它）：%s" % bus.local_mac())
    print("等 %.0fs 看 0x88B5 帧（现在没插线/对端没发就是空，正常）…" % args.secs)
    t0, n = time.time(), 0
    while time.time() - t0 < args.secs:
        f = bus.recv_frame(timeout=0.5)
        if f:
            n += 1
            print("   [收] %s" % eth_summary(f))
    print("共收到 %d 条；stats=%s" % (n, bus.stats()))
    bus.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
