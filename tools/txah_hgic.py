#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
txah_hgic.py — 泰芯 AH 模组（TXW8301，FMAC 固件）**主机口 mac_bus** 的 HGIC 帧编解码。

纯逻辑、无硬件依赖（可离线自测）。真机收发见 `probe_txah_uart.py`。

单一源（都是**模组 SDK 源码**，不是猜的）：
  * `sdk/include/lib/lmac/hgic.h`
      - `HGIC_HDR_TX_MAGIC 0x1A2B`（主机→模组）、`HGIC_HDR_RX_MAGIC 0x2B1A`（模组→主机）
      - `enum hgic_hdr_type { ACK=1, FRM=2, CMD=3, EVENT=4, ..., FRM2=9, CMD2=13, EVENT2=14, ... }`
      - `struct hgic_hdr { u16 magic; u8 type; u8 ifidx:4|flags:4; u16 length; u16 cookie; }` (`__packed`，**8 字节**)
      - `struct hgic_frm_hdr { hgic_hdr hdr; union{rx_info|tx_info|u8 rev[24]}; }` → 数据帧头 = **8 + 24 = 32 B**
      - `struct hgic_frm_hdr2 { hgic_hdr hdr; }` → "2" 系列是**精简**版（只有 8 B 头）
      - `HDR_CMDID(ctl)`：`type==CMD2` 时 id 在 `cmd2.cmd_id`（id>255 走 CMD2/EVENT2）
  * `sdk/lib/bus/macbus/uart_bus.c`（**UART 上的定帧规则**）
      - 收满 2 字节比对 magic（小端：主机发出去的第一、二字节是 `2B 1A`）
      - **第 5、6 字节 = 帧长**（16bit 小端，**整帧长度**，含 8 字节头）
      - 收够 `frm_len` 即一帧；另有 `fixlen` 定长模式（`HGIC_CMD_SET_UART_FIXLEN`）
  * `project/project_config.h`：切 UART 时 `WIFIMGR_FRM_TYPE = WIFIMGR_FRM_TYPE_RAW`
      → 「裸数据格式，**由固件完成以太网帧格式封装**」（《TXSDK 主控交互指南》§2）

⚠ 尚不确定、要靠真机试的：**RAW 模式下载荷到底要不要带以太头 / 要不要那 24 字节 info**
  —— 所以本模块两种都支持（`data_frame(..., with_frm_info=...)`），工具会把实际发的原样打印出来。

⚠ 本仓模拟器 / `host_bus.py` 里那套 `AA 55 …` 是**模拟版**宿主协议（见
  `halow-demo/simulator/docs/spi_protocol.md` 的"与真实 MACBUS 的差异"），**别和这里混**。

用法：
  python tools\txah_hgic.py selftest            # 离线自测（编→解→分片喂→重同步）
  python tools\txah_hgic.py hex 2B 1A 03 00 08 00 00 00   # 解析一段十六进制
"""
import argparse
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

MAGIC_HOST_TO_MODULE = 0x1A2B          # HGIC_HDR_TX_MAGIC
MAGIC_MODULE_TO_HOST = 0x2B1A          # HGIC_HDR_RX_MAGIC

TYPE_NAMES = {
    1: "ACK", 2: "FRM", 3: "CMD", 4: "EVENT", 5: "FIRMWARE", 6: "NLMSG", 7: "BOOTDL",
    8: "TEST", 9: "FRM2", 10: "TEST2", 11: "SOFTFC", 12: "OTA", 13: "CMD2", 14: "EVENT2",
    15: "BOOTDL_DATA", 16: "IFBR", 17: "BEACON", 18: "AGGFRM", 19: "BLUETOOTH", 20: "TESTBOX",
}
TYPE_NAMES_REV = {v: k for k, v in TYPE_NAMES.items()}

HDR_LEN = 8            # struct hgic_hdr（packed）
FRM_INFO_LEN = 24      # struct hgic_frm_hdr 里 union 的大小
MAX_FRAME = 4096       # uart_bus.c: UART_BUS_RX_BUF_SIZE

# 常用命令 id（hgic.h，只列与本项目有关的；完整表见源码）
CMD_SET_UART_FIXLEN = 108
CMD_GET_UART_FIXLEN = 109


def build(magic, type_, payload=b"", cookie=0, ifidx=0, flags=0, length=None):
    """组一帧。`length` 默认 = 8 + len(payload)（UART 上的规则就是整帧长度）。"""
    body = bytes(payload)
    total = HDR_LEN + len(body) if length is None else length
    if not (HDR_LEN <= total <= MAX_FRAME):
        raise ValueError("帧长 %d 超出 [%d, %d]" % (total, HDR_LEN, MAX_FRAME))
    ifidx_flags = ((ifidx & 0x0F) << 4) | (flags & 0x0F)
    return struct.pack("<HBBHH", magic, type_ & 0xFF, ifidx_flags, total, cookie & 0xFFFF) + body


def frame_host_to_module(type_, payload=b"", **kw):
    return build(MAGIC_HOST_TO_MODULE, type_, payload, **kw)


def data_frame(payload, cookie=0, with_frm_info=False, lean=True):
    """数据帧：`lean=True` → `FRM2`（8B 头，载荷紧跟）；否则 `FRM`（8B 头 + 24B info + 载荷）。

    ⚠ 到底用哪种、载荷要不要带以太头，**要靠真机试**（见模块文档串首的说明）。
    """
    if lean:
        return frame_host_to_module(TYPE_NAMES_REV["FRM2"], payload, cookie=cookie)
    pre = b"\x00" * (FRM_INFO_LEN if with_frm_info else 0)
    return frame_host_to_module(TYPE_NAMES_REV["FRM"], pre + payload, cookie=cookie)


def cmd_frame(cmd_id, payload=b"", cookie=0):
    """命令帧：id ≤ 255 用 CMD(3)，否则 CMD2(13)（照 `HDR_CMDID` 的规则）。"""
    if cmd_id <= 0xFF:
        return frame_host_to_module(TYPE_NAMES_REV["CMD"], bytes([cmd_id]) + bytes(payload),
                                    cookie=cookie)
    return frame_host_to_module(TYPE_NAMES_REV["CMD2"],
                                struct.pack("<H", cmd_id) + bytes(payload), cookie=cookie)


def parse_header(buf):
    """解析前 8 字节；返回 dict（magic/type/ifidx/flags/length/cookie/from_module）。"""
    if len(buf) < HDR_LEN:
        return None
    magic, type_, ifidx_flags, length, cookie = struct.unpack_from("<HBBHH", buf, 0)
    return {
        "magic": magic,
        "magic_name": {MAGIC_HOST_TO_MODULE: "主机→模组(TX)",
                       MAGIC_MODULE_TO_HOST: "模组→主机(RX)"}.get(magic, "未知 magic"),
        "from_module": magic == MAGIC_MODULE_TO_HOST,
        "type": type_,
        "type_name": TYPE_NAMES.get(type_, "未知(%d)" % type_),
        "ifidx": (ifidx_flags >> 4) & 0x0F,
        "flags": ifidx_flags & 0x0F,
        "length": length,
        "cookie": cookie,
    }


class StreamParser:
    """按 uart_bus.c 的规则从字节流里切帧（含**重新同步**：字节丢失/杂音后能再对上）。"""

    def __init__(self, expect_from_module=None, max_frame=MAX_FRAME):
        self.buf = bytearray()
        self.expect_from_module = expect_from_module
        self.max_frame = max_frame
        self.garbage = 0            # 丢掉的杂字节数（可见，不静默）
        self.bad_length = 0         # 长度字段不合法而跳过的帧数

    def feed(self, data):
        """喂入新字节，返回本次识别出的帧列表 [(header_dict, payload_bytes), ...]。"""
        self.buf += bytes(data)
        out = []
        while True:
            pos = self._find_magic()
            if pos < 0:
                # 没找到 magic：留住最后 1 字节（可能是 magic 的前半）
                if len(self.buf) > 1:
                    self.garbage += len(self.buf) - 1
                    del self.buf[:-1]
                break
            if pos > 0:
                self.garbage += pos
                del self.buf[:pos]
            if len(self.buf) < HDR_LEN:
                break
            hdr = parse_header(self.buf)
            if not (HDR_LEN <= hdr["length"] <= self.max_frame):
                self.bad_length += 1
                del self.buf[:1]        # 这个 magic 是假的，跳过 1 字节继续找
                continue
            if len(self.buf) < hdr["length"]:
                break                   # 还没收齐
            out.append((hdr, bytes(self.buf[HDR_LEN:hdr["length"]])))
            del self.buf[:hdr["length"]]
        return out

    def _find_magic(self):
        magics = (MAGIC_MODULE_TO_HOST, MAGIC_HOST_TO_MODULE) if self.expect_from_module is None \
            else ((MAGIC_MODULE_TO_HOST,) if self.expect_from_module else (MAGIC_HOST_TO_MODULE,))
        raw = bytes(self.buf)
        best = -1
        for mg in magics:
            i = raw.find(struct.pack("<H", mg))
            if i >= 0 and (best < 0 or i < best):
                best = i
        return best


def describe(hdr, payload, full_payload=False):
    head = ("%-16s type=%-8s len=%-5d ifidx=%d flags=%d cookie=0x%04X" %
            (hdr["magic_name"], hdr["type_name"], hdr["length"], hdr["ifidx"],
             hdr["flags"], hdr["cookie"]))
    hexs = payload.hex(" ")
    if not full_payload and len(hexs) > 96:
        hexs = hexs[:96] + " …(%d B)" % len(payload)
    return head + ("\n     载荷 " + hexs if payload else "  (无载荷)")


def selftest():
    ok = True

    def chk(name, cond, extra=""):
        nonlocal ok
        print("  %-46s %s %s" % (name, "OK" if cond else "FAIL", extra))
        ok = ok and bool(cond)

    f = data_frame(bytes(range(14)) + b"hello", cookie=0x1234)
    chk("FRM2 帧长 = 8+19", len(f) == 8 + 19, "len=%d" % len(f))
    h = parse_header(f)
    chk("magic = 0x1A2B（主机→模组）", h["magic"] == MAGIC_HOST_TO_MODULE)
    chk("type = FRM2", h["type_name"] == "FRM2")
    chk("length 字段 = 整帧长", h["length"] == len(f))
    chk("cookie 往返", h["cookie"] == 0x1234)
    chk("载荷往返", f[8:] == bytes(range(14)) + b"hello")

    p = StreamParser(expect_from_module=False)
    frames = p.feed(f)
    chk("整帧喂入 → 1 帧", len(frames) == 1 and frames[0][1] == f[8:])

    p2 = StreamParser(expect_from_module=False)
    got = []
    for i in range(0, len(f), 3):
        got += p2.feed(f[i:i + 3])
    chk("分片喂入（每 3 字节）→ 仍 1 帧", len(got) == 1 and got[0][1] == f[8:])
    chk("分片喂入不产生杂字节", p2.garbage == 0, "garbage=%d" % p2.garbage)

    p3 = StreamParser(expect_from_module=False)
    got3 = []
    for b in b"\x00\xff\xaa" + f:               # 前面塞垃圾字节
        got3 += p3.feed(bytes([b]))
    chk("垃圾前缀 → 重新同步到 1 帧", len(got3) == 1 and got3[0][1] == f[8:])
    chk("垃圾字节被计数（不静默）", p3.garbage == 3, "garbage=%d" % p3.garbage)

    c = cmd_frame(CMD_SET_UART_FIXLEN, b"\x01\x40")
    hc = parse_header(c)
    chk("命令帧 type=CMD id=108", hc["type_name"] == "CMD" and c[8] == 108)
    c2 = cmd_frame(65000, b"\x01")
    chk("id>255 → CMD2 + u16 id", parse_header(c2)["type_name"] == "CMD2"
        and struct.unpack_from("<H", c2, 8)[0] == 65000)

    mod = build(MAGIC_MODULE_TO_HOST, 4, b"evt", cookie=7)
    both = StreamParser()
    chk("不限定方向时两个 magic 都认", len(both.feed(f + mod)) == 2)

    print("=> 自测 %s" % ("全部通过 ✓" if ok else "有失败 ✗"))
    return 0 if ok else 1


def cmd_hex(args):
    data = bytes.fromhex("".join(args.hex))
    p = StreamParser()
    frames = p.feed(data)
    if p.garbage:
        print("（前面/中间丢弃了 %d 个非帧字节）" % p.garbage)
    if not frames:
        print("没识别出 HGIC 帧（是否 magic/长度不对？原样：%s）" % data.hex(" "))
        return 1
    for hdr, payload in frames:
        print(describe(hdr, payload, full_payload=args.full))
    return 0


def main():
    ap = argparse.ArgumentParser(description="泰芯 AH 模组 HGIC 帧编解码（离线）")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest", help="离线自测").set_defaults(func=lambda a: selftest())
    p = sub.add_parser("hex", help="解析一段十六进制")
    p.add_argument("hex", nargs="+")
    p.add_argument("--full", action="store_true", help="完整打印载荷")
    p.set_defaults(func=cmd_hex)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
