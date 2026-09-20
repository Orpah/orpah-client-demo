#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
eth_frames.py — 台架上用的**造帧/解帧**小工具（单一源）。

谁用：
  * `tools/hgic_loop_test.py`（受控闭环：ARP / DHCP DISCOVER / 0x88b5）
  * `tools/demo_client_hgic.py`（b 步真链路联调：用 DHCP DISCOVER **主动触发**一条下行）

为什么放一处：这些帧是**判据**的一部分（例如"发 DISCOVER → 对端必然回一条 DHCP 应答"），
两处各写一份迟早会漂。造帧规则本身很朴素（RFC 826 / RFC 2131），不需要外部依赖。
"""
import struct

__all__ = ["eth", "ip_cksum", "arp_request", "dhcp_discover", "eth_summary"]


def eth(dst, src, ethertype, payload):
    """拼一条以太帧（**完整**含 14B 头 —— 模组是二层桥，靠目的 MAC 决定发到哪）。"""
    return bytes(dst) + bytes(src) + struct.pack(">H", ethertype) + bytes(payload)


def ip_cksum(b):
    """IPv4 头校验和（标准 one's complement）。"""
    b = bytes(b)
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def arp_request(who_has, tell, src_mac):
    """ARP 请求（广播）：who-has `who_has` tell `tell`。"""
    return eth(b"\xff" * 6, src_mac, 0x0806,
               struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1) + bytes(src_mac) + bytes(tell) +
               b"\0" * 6 + bytes(who_has))


def dhcp_discover(src_mac, xid=0x12345678):
    """最小可用的 DHCP DISCOVER（广播）—— 用它**主动**让 DHCP 服务回一条下行帧。

    上行方向（客户端→AP）发出去，AP 侧协议栈会应答（实测 ~30ms），
    于是"下行真的能到"这件事就有了**因果**证据，不再靠等偶发流量。
    """
    bootp = (b"\x01\x01\x06\x00" + struct.pack(">I", xid) + b"\x00\x00\x80\x00" +
             b"\0" * 16 + bytes(src_mac) + b"\0" * 10 + b"\0" * 64 + b"\0" * 128)
    opts = b"\x63\x82\x53\x63" + b"\x35\x01\x01" + b"\xff"
    udp = struct.pack(">HHHH", 68, 67, 8 + len(bootp + opts), 0) + bootp + opts
    ip = (b"\x45\x00" + struct.pack(">H", 20 + len(udp)) + b"\x00\x01\x00\x00\x40\x11\x00\x00" +
          b"\x00\x00\x00\x00" + b"\xff\xff\xff\xff")
    ip = ip[:10] + struct.pack(">H", ip_cksum(ip)) + ip[12:]
    return eth(b"\xff" * 6, src_mac, 0x0800, ip + udp)


def eth_summary(p):
    """把一条以太帧讲成人话（ARP / IPv4 / UDP-DHCP / IPv6 / ORPAH 0x88b5 认得出来）。"""
    p = bytes(p)
    if len(p) < 14:
        return "（%d 字节，不像完整以太帧）" % len(p)
    dst, src, et = p[:6].hex(":"), p[6:12].hex(":"), struct.unpack(">H", p[12:14])[0]
    ex = ""
    if et == 0x0806 and len(p) >= 42:
        ex = " ARP op=%d %s->%s" % (struct.unpack(">H", p[20:22])[0],
                                    ".".join(map(str, p[28:32])), ".".join(map(str, p[38:42])))
    elif et == 0x0800 and len(p) >= 34:
        ex = " IPv4 %s->%s" % (".".join(map(str, p[26:30])), ".".join(map(str, p[30:34])))
        if p[23] == 17 and len(p) >= 38:
            sp, dp = struct.unpack(">HH", p[34:38])
            ex += " UDP %d->%d" % (sp, dp) + ("  ★DHCP!" if 67 in (sp, dp) else "")
    elif et == 0x86DD:
        ex = " IPv6"
    elif et == 0x88B5:
        ex = " ★ORPAH(0x88b5) %r" % p[14:60]
    return "dst=%s src=%s et=0x%04x%s [%dB]" % (dst, src, et, ex, len(p))


def is_dhcp(frame):
    """这条下行帧是不是 DHCP（UDP 67/68）应答？"""
    p = bytes(frame)
    return (len(p) >= 38 and p[12:14] == b"\x08\x00" and p[23] == 17
            and 67 in struct.unpack(">HH", p[34:38]))
