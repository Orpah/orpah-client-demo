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

# 控制帧的两个大分类（`hgic_hdr.type`）
TYPE_CMD = 3
TYPE_EVENT = 4
TYPE_CMD2 = 13
TYPE_EVENT2 = 14

# 命令 id（`sdk/include/lib/lmac/hgic.h` 的 `enum hgic_cmd`；只列常用的）
CMD_NAMES = {
    1: "DEV_OPEN", 2: "DEV_CLOSE", 3: "SET_MAC", 4: "SET_SSID", 5: "SET_BSSID",
    7: "SET_CHANNEL", 13: "SET_KEY", 14: "SCAN", 15: "GET_SCAN_LIST", 17: "DISCONNECT",
    18: "GET_BSSID", 20: "GET_STATUS", 22: "SET_TX_POWER", 23: "GET_TX_POWER",
    29: "SET_TX_MCS", 31: "ACS_ENABLE", 37: "GET_FW_STATE", 40: "GET_CONN_STATE",
    41: "SET_WORK_MODE", 42: "SET_PAIRED_STATIONS", 43: "GET_FW_INFO", 44: "PAIRING",
    45: "GET_TEMPERATURE", 46: "ENTER_SLEEP", 47: "OTA", 48: "GET_SSID", 50: "GET_SIGNAL",
    51: "GET_TX_BITRATE", 53: "GET_STA_LIST", 54: "SAVE_CFG", 56: "SET_ETHER_TYPE",
    57: "GET_STA_COUNT", 58: "SET_HEARTBEAT_INT", 65: "RADIO_ONOFF", 74: "SET_PS_MODE",
    75: "LOAD_DEF", 108: "SET_UART_FIXLEN", 109: "GET_UART_FIXLEN",
    163: "GET_WIFI_STATUS_CODE", 192: "SET_SIGNAL_THRESHOLD", 194: "GET_LINK_QUALITY",
}

# 事件 id（`hgic.h` 的 `enum hgic_event`）
EVENT_NAMES = {
    1: "STATE_CHG", 2: "CH_SWICH", 3: "DISCONNECT_REASON", 4: "ASSOC_STATUS",
    5: "SCANNING", 6: "SCAN_DONE", 7: "TX_BITRATE", 8: "PAIR_START", 9: "PAIR_SUCCESS",
    10: "PAIR_DONE", 11: "CONECT_START", 12: "CONECTED", 13: "DISCONECTED", 14: "SIGNAL",
    15: "DISCONNET_LOG", 16: "REQUEST_PARAM", 17: "TESTMODE_STATE", 18: "FWDBG_INFO",
    19: "CUSTOMER_MGMT", 20: "SLEEP_EXIT", 21: "DHCPC_DONE", 22: "CONNECT_FAIL",
    23: "CUST_DRIVER_DATA", 24: "UNPAIR_STA", 25: "BLENC_DATA", 26: "HWSCAN_RESULT",
    27: "EXCEPTION_INFO", 28: "DSLEEP_WAKEUP", 29: "STA_MIC_ERROR", 30: "ACS_DONE",
    31: "FW_INIT_DONE", 32: "ROAM_CONECTED", 33: "MGMT_FRAME", 34: "UNKNOWN_STA",
    35: "ROAM_FAIL",
}

# `struct hgic_fw_info`（`hgic.h`，__packed，28 B）
FW_INFO_FMT = "<IIHH6s2sII"
FW_INFO_LEN = 28

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
    """命令帧：id ≤ 255 用 CMD(3)，否则 CMD2(13)（照 `HDR_CMDID` 的规则）。

    ⚠ **必须把 4 字节的 union 补满**：`struct hgic_ctrl_hdr` = 8 B 帧头 + 4 B union
    （`cmd_id` / `status` / `event_id` …），`sizeof` = **12**；模组端的
    `data = (uint8 *)(ctrl + 1)` 就是**从偏移 12** 取参数（`uart_bus.c` 的
    `uart_bus_proc_cmd` 正是这么干的）。所以 1 字节 `cmd_id` 后面要补 3 个 0，
    否则带参命令的参数会落到错位置上。
    （实测：不带参的 `send-cmd 43` 用 9 B 形式也能回，但那是巧合——它只读 `cmd_id`。）
    """
    if cmd_id <= 0xFF:
        body = bytes([cmd_id]) + b"\x00" * 3 + bytes(payload)
        return frame_host_to_module(TYPE_NAMES_REV["CMD"], body, cookie=cookie)
    body = struct.pack("<H", cmd_id) + b"\x00" * 2 + bytes(payload)
    return frame_host_to_module(TYPE_NAMES_REV["CMD2"], body, cookie=cookie)


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


def ctrl_info(hdr, payload):
    """解析控制帧载荷，返回 dict（`kind`/`id`/`name`/`status`/`data`）或 None。

    帧式来自 **2026-09-16 真机实测**（`probe_txah_uart.py send-cmd`）：

    * 请求（主机→模组）：`cmd_id(1) [+ 参数]`
    * 应答（模组→主机）：`cmd_id(1) | status(1) | len(2, LE) | data(len)`
      —— 实测 `send-cmd 43` 回 40 B = 8(hdr) + 1 + 1 + 2 + **28**，
      而模组日志同时打了 `resp cmd, ret:28`；这 28 B 正好是 `struct hgic_fw_info`。
    * 事件（模组→主机）：`event_id(1) | data`
    * CMD2/EVENT2 时 id 是 u16（同 `HDR_CMDID()` 的规则）

    ⚠ 也有"只回一个 cmd_id"的短应答（实测 `send-cmd 1` / `send-cmd 20` 就是），
    那种情况 `status` 为 None —— 不当成错误，也不编造 status。
    """
    if not payload:
        return None
    if hdr["type"] in (TYPE_CMD2, TYPE_EVENT2):
        if len(payload) < 2:
            return None
        cid, body = struct.unpack_from("<H", payload, 0)[0], payload[2:]
    else:
        cid, body = payload[0], payload[1:]

    is_ctrl = hdr["type"] in (TYPE_CMD, TYPE_CMD2)
    if is_ctrl and hdr["from_module"]:
        if len(body) >= 3:
            status = body[0]
            ln = struct.unpack_from("<H", body, 1)[0]
            if 3 + ln == len(body):
                return {"kind": "resp", "id": cid, "status": status, "data": body[3:],
                        "name": CMD_NAMES.get(cid)}
        # 短应答：只回了 cmd_id，没有 status/len/data（实测 send-cmd 1 / 20 就是这样）
        return {"kind": "resp", "id": cid, "status": None, "data": b"",
                "name": CMD_NAMES.get(cid)}
    # 请求、事件：hdr(8) + union(4) 之后才是数据
    return {"kind": "req" if not hdr["from_module"] else "resp", "id": cid,
            "status": None, "data": body[3:] if len(body) >= 3 else b"",
            "name": (CMD_NAMES.get(cid) if is_ctrl else EVENT_NAMES.get(cid))}


def fw_info_decode(data):
    """`struct hgic_fw_info` → 可读 dict；长度不够回 None（不猜）。"""
    if len(data) < FW_INFO_LEN:
        return None
    ver, svn, chip_id, cpuid, mac, _resv, app_ver, smt = struct.unpack(FW_INFO_FMT, data[:FW_INFO_LEN])
    return {
        # 实测字节序：`05 01 04 02` → "2.4.1.5"
        "app": "%d.%d.%d.%d" % (data[3], data[2], data[1], data[0]),
        "version_raw": ver,
        "svn": svn,
        "chip_id": chip_id,
        "cpuid": cpuid,
        "mac": ":".join("%02x" % b for b in mac),
        "app_version": app_ver,
        "smt_dat": smt,
    }


def _u32_words(data, maxn=4):
    return " ".join("0x%08x" % struct.unpack_from("<I", data, i)[0]
                    for i in range(0, min(len(data), 4 * maxn), 4) if i + 4 <= len(data))


def describe(hdr, payload, full_payload=False):
    head = ("%-16s type=%-8s len=%-5d ifidx=%d flags=%d cookie=0x%04X" %
            (hdr["magic_name"], hdr["type_name"], hdr["length"], hdr["ifidx"],
             hdr["flags"], hdr["cookie"]))
    lines = [head]
    if payload:
        hexs = payload.hex(" ")
        if not full_payload and len(hexs) > 96:
            hexs = hexs[:96] + " …(%d B)" % len(payload)
        lines.append("     载荷 " + hexs)

    info = ctrl_info(hdr, payload)
    if info:
        is_ctrl = hdr["type"] in (TYPE_CMD, TYPE_CMD2)
        line = "     %s id=%d" % ("命令" if is_ctrl else "事件", info["id"])
        line += "(%s)" % (info["name"] or "未知")
        if info["kind"] == "req":
            line += "  请求"
        else:
            line += "  应答" if is_ctrl else "  上报"
            if info["status"] is None:
                if is_ctrl:
                    line += "（只回了 id，无 status/len 字段）"
            else:
                line += " status=%d data=%d B" % (info["status"], len(info["data"]))
        lines.append(line)

        # 已知结构的载荷（只有一份解码，别在调用方另写）
        if info["id"] == 43 and is_ctrl and info["data"]:
            fi = fw_info_decode(info["data"])
            if fi:
                lines.append("       FW: app=%s svn=%d chip_id=0x%04X mac=%s smt_dat=%d"
                             % (fi["app"], fi["svn"], fi["chip_id"], fi["mac"], fi["smt_dat"]))
        elif not is_ctrl and info["data"]:
            w = _u32_words(info["data"])
            if w:
                lines.append("       data u32: " + w)
    return "\n".join(lines)


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
    chk("控制头 12 B（参数从偏移 12 开始）", len(c) == 12 + 2 and c[12:] == b"\x01\x40",
        "len=%d" % len(c))
    c2 = cmd_frame(65000, b"\x01")
    chk("id>255 → CMD2 + u16 id", parse_header(c2)["type_name"] == "CMD2"
        and struct.unpack_from("<H", c2, 8)[0] == 65000)
    chk("CMD2 参数也从偏移 12 开始", c2[12:] == b"\x01", c2.hex(" "))

    mod = build(MAGIC_MODULE_TO_HOST, 4, b"evt", cookie=7)
    both = StreamParser()
    chk("不限定方向时两个 magic 都认", len(both.feed(f + mod)) == 2)

    # --- 2026-09-16 真机黄金样本（`probe_txah_uart.py --ch347-com 0 send-cmd 43` 抓到）---
    REQ_43 = bytes.fromhex("2b1a0300090001002b")
    RESP_43 = bytes.fromhex("1a2b0300280001002b001c0005010402619b000002400100"
                            "4a06598d7440598d00000000ad6f6407")
    EVENT_7 = bytes.fromhex("1a2b040110000000070000008b000000")
    ECHO_1 = bytes.fromhex("1a2b03000900010001")

    hr = parse_header(RESP_43)
    chk("黄金样本：应答 40 B / length=40", len(RESP_43) == 40 and hr["length"] == 40)
    chk("黄金样本：来自模组、type=CMD", hr["from_module"] and hr["type_name"] == "CMD")
    i = ctrl_info(hr, RESP_43[8:])
    chk("黄金样本：cmd_id=43 / status=0 / data=28 B",
        i["id"] == 43 and i["status"] == 0 and len(i["data"]) == 28,
        "id=%s status=%s data=%d" % (i["id"], i["status"], len(i["data"])))
    fi = fw_info_decode(i["data"])
    chk("黄金样本：fw_info version = 2.4.1.5", bool(fi) and fi["app"] == "2.4.1.5",
        fi["app"] if fi else "None")
    chk("黄金样本：svn = 39777", bool(fi) and fi["svn"] == 39777)
    chk("黄金样本：mac = 4a:06:59:8d:74:40", bool(fi) and fi["mac"] == "4a:06:59:8d:74:40",
        fi["mac"] if fi else "None")
    chk("黄金样本：smt_dat = 124022701", bool(fi) and fi["smt_dat"] == 124022701)
    ev = ctrl_info(parse_header(EVENT_7), EVENT_7[8:])
    chk("黄金样本：事件 7 = TX_BITRATE，值 139",
        ev["id"] == 7 and ev["name"] == "TX_BITRATE"
        and struct.unpack_from("<I", ev["data"], 0)[0] == 139)
    e1 = ctrl_info(parse_header(ECHO_1), ECHO_1[8:])
    chk("黄金样本：短应答只有 cmd_id（status=None，不编造）",
        e1["id"] == 1 and e1["status"] is None)
    rq = ctrl_info(parse_header(REQ_43), REQ_43[8:])
    chk("黄金样本：请求 kind=req / id=43", rq["kind"] == "req" and rq["id"] == 43)

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
