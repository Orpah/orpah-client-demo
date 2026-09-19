#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
hgic_loop_test.py — b 步真机台架：**受控闭环**（上行过空口 + 下行到达客户端主机口）。

台架（2026-09-20 实测用的那套）：
    客户端 TX-AH EVB（FMAC，**STA**）  ←空口→  T-Halow-RJ45（WNB，**AP**）
    PC ──CH347F UART0(P2)── 客户端 UART0(A10/A11)      数据口（HGIC/mac_bus）
    PC ──USB-UART── 客户端 UART1(A12/A13)              AT/打印口（逐帧 `[mbus rx]/[mbus tx]` 日志）
    PC ──USB-UART── TH-RJ45 的 USB-C                    AP 的 AT 口（LMAC/`STA1:`/`rx1:`/`tx1:`）
    TH-RJ45 的 RJ45 **空着**（下行由 AP 侧协议栈自己产生：本脚本用 DHCP DISCOVER 触发它）

它做什么（每步的判据都写进输出）：
  1. 读 AP 的 per-STA 基线（`STA1:` / `rx1:` / `tx1:`）；
  2. 从数据口发 3 条以太帧（ARP / DHCP DISCOVER / 0x88b5），**每条都要在客户端 AT 口看到
     `[mbus rx] <8+len> byte(s)` 才算发出去**，否则重发 —— CH347F 的 VCP 会周期性抽风
     （I/O 报 PermissionError(13)），靠模组自己的日志兜住"到底发出去没有"；
  3. 20 s 内只听数据口，认 `FRM2`（帧里就是完整以太帧）；
  4. 回看 AT 口的 `[mbus tx]`（模组交给主机的帧 = 下行旁证）；
  5. 再读 AP 的 per-STA 计数，打印结论。

判据（怎么算"通了"）：
  * **上行过空口** = AP per-STA `rx1 cnt/data` 增长；
    ⚠ `tx1` 对广播不动（广播记在 `mcast`），**不要拿 `tx1` 判下行**；
  * **下行到达** = 数据口收到 `FRM2`（内含以太帧）；
  * 退出码：0 = 两个都成立；2 = 有一步不成立；1 = 串口打不开。

⚠ 端口号会变（`USB-HiSpeed-SERIAL-A/B CH347F` = MI_00/MI_02；板载 CH340E 与 TH-RJ45 的 CH340
  各自独立）——用 `--ap-port/--at-port/--data-port` 传。也**别让别的串口工具占着**（WindTerm 之类）
  否则 I/O 会报 PermissionError(13)。

⚠ 台架前提（缺一样都测不出来）：
  * 客户端模组烧的是 **MACBUS_UART 版固件**（`halow-demo/TXW8301/tools/fmac_macbus_switch.py uart`
    + 重编 + `at+fwupg`）；没烧的话 `probe_txah_uart.py probe` 就不会有应答；
  * 客户端 = STA、TH-RJ45 = AP，**改角色后不要复位**（`WIFIMODE` 不跨 RST 保存）；
  * 两者 SSID / CHAN_LIST / BSS_BW 一致（本台架：`halowlink` / `9080` / `8` / OPEN）。
"""
import argparse
import re
import struct
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

import os                                                       # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial                                                    # noqa: E402
from txah_hgic import CookieCounter, StreamParser, data_frame, describe   # noqa: E402

HOST_MAC = bytes.fromhex("94186537d5b1")     # 客户端 host 侧 MAC（AT 口日志里能看到）
LOG_DEFAULT = "hgic_loop_test.log"


class RetrySerial:
    """pyserial 的耐打包装：I/O 报错（PermissionError(13)/ClearCommError）就重开继续。"""

    def __init__(self, port, baud=115200):
        self.port, self.baud, self.ser, self.reopens = port, baud, None, 0
        self._open()

    def _open(self):
        try:
            if self.ser:
                self.ser.close()
        except Exception:                                        # noqa: BLE001
            pass
        self.ser = serial.Serial(self.port, self.baud, timeout=0.05)

    def _retry(self, fn, what, tries=10):
        last = None
        for _ in range(tries):
            try:
                return fn()
            except Exception as e:                               # noqa: BLE001
                last = e
                self.reopens += 1
                time.sleep(0.5)
                try:
                    self._open()
                except Exception:                                # noqa: BLE001
                    time.sleep(0.8)
        print("   [!] %s 反复失败：%r" % (what, last))
        return b""

    def read(self):
        def go():
            n = self.ser.in_waiting
            return self.ser.read(min(n, 4096)) if n else b""
        return self._retry(go, "%s 读" % self.port)

    def write(self, data):
        return self._retry(lambda: self.ser.write(bytes(data)), "%s 写" % self.port)

    def drain(self, secs=0.3):
        t0 = time.time()
        while time.time() - t0 < secs:
            self.read()

    def close(self):
        try:
            self.ser.close()
        except Exception:                                        # noqa: BLE001
            pass


def read_txt(s, secs):
    t0, buf = time.time(), ""
    while time.time() - t0 < secs:
        c = s.read()
        if c:
            buf += c.decode("utf-8", "replace")
        else:
            time.sleep(0.02)
    return buf


def ap_cmd(s, c, secs=5.0):
    try:
        s.ser.reset_input_buffer()
    except Exception:                                            # noqa: BLE001
        pass
    s.write((c + "\r\n").encode("ascii"))
    return read_txt(s, secs)


def ap_counts(txt):
    d = {}
    for k, pat in (("rx1_cnt", r"rx1:.*?cnt=(\d+)"), ("rx1_data", r"rx1:[^\n]*?data=(\d+)KB"),
                   ("tx1_cnt", r"tx1:.*?cnt=(\d+)"), ("tx1_data", r"tx1:[^\n]*?data=(\d+)KB"),
                   ("rx_cnt", r"rx :.*?cnt=(\d+)"), ("tx_cnt", r"tx :.*?cnt=(\d+)")):
        v = [int(x) for x in re.findall(pat, txt, re.S)]
        if v:
            d[k] = max(v)
    return d


def eth(dst, src, et, payload):
    return dst + src + struct.pack(">H", et) + payload


def ip_cksum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def arp_request(who, tell):
    return eth(b"\xff" * 6, HOST_MAC, 0x0806,
               struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1) + HOST_MAC + tell + b"\0" * 6 + who)


def dhcp_discover():
    """一条最小可用的 DHCP DISCOVER（广播）——用它触发 AP 侧协议栈回一条下行帧。"""
    bootp = (b"\x01\x01\x06\x00" + struct.pack(">I", 0x12345678) + b"\x00\x00\x80\x00" +
             b"\0" * 16 + HOST_MAC + b"\0" * 10 + b"\0" * 64 + b"\0" * 128)
    opts = b"\x63\x82\x53\x63" + b"\x35\x01\x01" + b"\xff"
    udp = struct.pack(">HHHH", 68, 67, 8 + len(bootp + opts), 0) + bootp + opts
    ip = (b"\x45\x00" + struct.pack(">H", 20 + len(udp)) + b"\x00\x01\x00\x00\x40\x11\x00\x00" +
          b"\x00\x00\x00\x00" + b"\xff\xff\xff\xff")
    ip = ip[:10] + struct.pack(">H", ip_cksum(ip)) + ip[12:]
    return eth(b"\xff" * 6, HOST_MAC, 0x0800, ip + udp)


def eth_summary(p):
    """把一条以太帧讲成人话（ARP/IPv4/UDP/DHCP/0x88b5 认出来）。"""
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
        ex = " ★ORPAH(0x88b5) %r" % p[14:34]
    return "dst=%s src=%s et=0x%04x%s [%dB]" % (dst, src, et, ex, len(p))


def main():
    ap = argparse.ArgumentParser(description="b 步受控闭环台架（HGIC 数据口 + 空口 + AP 计数）")
    ap.add_argument("--ap-port", default="COM8", help="TH-RJ45 的 AT 口（USB-C，CH340）")
    ap.add_argument("--at-port", default="COM6", help="客户端 AT/打印口（板载 CH340E）")
    ap.add_argument("--data-port", default="COM23", help="客户端数据口（CH347F UART0 = A 路）")
    ap.add_argument("--listen-secs", type=float, default=20.0, help="发完之后听数据口多久")
    ap.add_argument("--log", default=LOG_DEFAULT, help="把本次输出存一份（默认 %s）" % LOG_DEFAULT)
    args = ap.parse_args()

    lines = []

    def say(s=""):
        print(s)
        lines.append(s)

    ap_s, at_s, t_s = RetrySerial(args.ap_port), RetrySerial(args.at_port), RetrySerial(args.data_port)

    say("=" * 84)
    say("1) 基线：AP 的 per-STA 计数 + 客户端 AT 口 mbus 日志")
    txt = ap_cmd(ap_s, "AT+VERSION=?")
    base = ap_counts(txt)
    for ln in txt.splitlines():
        if any(k in ln for k in ("mode=", "STA1", "tx1:", "rx1:", "agc=")):
            say("   | %s" % ln.strip()[:130])
    say("   基线：%s" % base)
    at_s.drain(1.5)

    FRAMES = [("ARP who-has 192.168.1.1（42B）",
               arp_request(struct.pack(">BBBB", 192, 168, 1, 1),
                           struct.pack(">BBBB", 192, 168, 1, 225)), 50),
              ("DHCP DISCOVER（286B）", dhcp_discover(), 294),
              ("ORPAH 0x88b5（33B）", eth(b"\xff" * 6, HOST_MAC, 0x88B5, b"ORPAH-DOWNLINK-TEST"), 41)]

    say("=" * 84)
    say("2) 逐条发；每条都要在 AT 口看到 `[mbus rx] <8+len> byte(s)` 才算发出去（否则重发）")
    ck = CookieCounter(1)
    sent_ok = 0
    for name, f, want in FRAMES:
        ok = False
        for tries in range(1, 7):
            at_s.drain(0.3)
            t_s.write(data_frame(f, cookie=ck.next(), lean=True))
            time.sleep(0.6)
            if re.search(r"\[mbus rx\]\s+%d byte" % want, read_txt(at_s, 1.2)):
                ok = True
                sent_ok += 1
                say("   ✓ %-26s 第 %d 次确认（模组收到 %d 字节）" % (name, tries, want))
                break
            say("     · %s 第 %d 次没确认，重发" % (name, tries))
        if not ok:
            say("   ✗ %s：6 次都没确认（端口抽风 / 载荷被拒 / 数据口没接对）" % name)

    say("=" * 84)
    say("3) 之后 %.0fs 只听数据口（FRM2 里就是完整以太帧 = 下行）" % args.listen_secs)
    down, p = [], StreamParser(expect_from_module=True)
    t0 = time.time()
    while time.time() - t0 < args.listen_secs:
        for hdr, pl in p.feed(t_s.read()):
            if hdr["type_name"] == "FRM2":
                down.append(pl)
                say("   [数据口] ★下行帧：%s" % eth_summary(pl))
            elif hdr["type_name"] != "EVENT":
                say("   [数据口] %s" % describe(hdr, pl))
        time.sleep(0.02)

    say("=" * 84)
    say("4) AT 口回看（`[mbus tx]` = 模组交给主机的帧 = 下行旁证；也看有没有报错）")
    log = read_txt(at_s, 2.0)
    for l in [x.strip() for x in log.splitlines() if "[mbus tx]" in x][-6:]:
        say("   [AT口] %s" % l[:120])
    errs = [x.strip() for x in log.splitlines() if re.search(r"cookie err|drop|fail|ERROR", x)]
    say("   AT 口报错行：%s" % (errs[-3:] if errs else "（无）"))

    say("=" * 84)
    say("5) AP per-STA 计数对比（⚠ 这些是分区间快照，可能不变 —— 只看它、会误判）")
    txt2 = ap_cmd(ap_s, "AT+VERSION=?")
    now = ap_counts(txt2)
    for k in sorted(set(base) | set(now)):
        b, n = base.get(k), now.get(k)
        moved = (b is not None and n is not None and n != b and k.startswith(("rx1", "tx1")))
        note = "  <== 动了" if moved else ("   （板子自己的 rx/tx，不参与判定）" if k in ("rx_cnt", "tx_cnt") else "")
        say("   %-9s %s -> %s%s" % (k, b, n, note))
    for ln in txt2.splitlines():
        if any(k in ln for k in ("STA1", "tx1:", "rx1:", "agc=")):
            say("   | %s" % ln.strip()[:130])

    up_ctr = (now.get("rx1_cnt") or 0) > (base.get("rx1_cnt") or 0) or \
             (now.get("rx1_data") or 0) > (base.get("rx1_data") or 0)
    dhcp_down = [d for d in down if len(d) >= 38 and d[12:14] == b"\x08\x00" and d[23] == 17
                 and 67 in struct.unpack(">HH", d[34:38])]
    # 上行判定：per-STA 计数增长（可能因快照没抓到）**或** AP 对我方 DISCOVER 的应答
    # —— 后者更硬：应答只可能是因为我们的帧已经过空口到了 AP。
    up = up_ctr or bool(dhcp_down)
    say("=" * 84)
    say("结论：")
    say("   我方帧被模组收下（AT 口确认）    ：%d/3" % sent_ok)
    say("   上行过空口                      ：%s" % (
        "★是（AP per-STA rx1 计数增长）" if up_ctr else
        ("★是（由 DHCP 应答反推：AP 是收到我方 DISCOVER 才应答的）" if dhcp_down else "没看出来")))
    say("   下行到达（数据口收到 FRM2）      ：%d 条（其中 DHCP 应答 %d 条）" % (len(down), len(dhcp_down)))
    say("   串口重开：AP %d / AT %d / 数据口 %d" % (ap_s.reopens, at_s.reopens, t_s.reopens))
    ok = (sent_ok == 3) and up and bool(down)
    say("   => %s" % ("✓ 闭环成立（上行 + 下行都实测到）" if ok else "✗ 有一步没成立，见上面各行"))

    if args.log:
        with open(args.log, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        say("（本次输出已存 %s）" % args.log)

    ap_s.close()
    at_s.close()
    t_s.close()
    return 0 if ok else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except serial.SerialException as e:
        print("[!] 串口打不开：%r" % (e,))
        print("    —— 端口被别的程序占着（WindTerm/SecureCRT/串口监视器）或板子没插；"
              "用 --ap-port/--at-port/--data-port 指定正确端口。")
        sys.exit(1)
