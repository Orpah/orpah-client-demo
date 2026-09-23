#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ch347_i2c.py — 用 **CH347/CH347F 当独立 I²C 主机**（B 方案）

为什么要有它：c4-γ-2 的 SE 在台架上一直"地址被 ACK、字节收下、但从不回应"。
我们固件侧已经把能自查的都自查了（字地址 0x00/0x03 两种都试、逐字节 BTF+AF、
两种 CRC 模式、10/50/100 kHz、读方向探针），所以需要一个**完全独立的主机**来判：
到底是「器件不讲 CryptoAuth」还是「我们固件还有毛病」。

API 出处（**逐条标出处**，WCH 官方 CH347 DLL 头 `CH347DLL_EN.H` V1.5）：
  * `CH347I2C_Set(iIndex, iMode)`      —— 速度位 bit1:0：`00`=20k、`01`=100k(缺省)、`10`=400k、`11`=750k
  * `CH347I2C_SetStretch(iIndex, en)`  —— 时钟延展开关
  * `CH347I2C_SetDelaymS(iIndex, ms)`  —— 下一次流操作前硬件延时
  * `CH347I2C_SetDriverMode(iIndex, m)`—— `0`=开漏（缺省）、`1`=推挽
  * `CH347StreamI2C(iIndex, wLen, wBuf, rLen, rBuf)`
      —— **wBuf 的第一个字节就是「器件地址 + 方向位」**（8 位形式，如 0x60 写 = `0xC0`）；
         紧接着的字节是"字地址/寄存器地址"⇒ 读响应 = `wBuf=[addr|0, 字地址]` + `rLen=N`
  * `CH347StreamI2C_RetACK(iIndex, wLen, wBuf, rLen, rBuf, rAckCount)`
      —— **回一个 ACK 计数**（主机端收到的 ACK 个数）⇒ 这是"器件到底 ACK 了没有"的
         **独立判据**，不依赖我们自己固件里的 AF 标志解读。

接线（见 `docs/ch347f-i2c-crosscheck.md`）：
  * CH347F-EVT 的 **P5 是 I²C 排针**（`docs/ch347f-txah-spi-probe.md` 记的：P4=SPI、P5=I2C）；
    P5 上按**丝印**认 `SCL`/`SDA`/`3V3`/`GND`（我们手上没有 CH347DS1 第 13 页的 P5 丝印清单，
    **这一条未核实**）。
  * ★ **两个主机不能同时驱动同一对线**：测的时候把 nano **断电**（或按住 RST），
    由 CH347 的 `3V3` 给上拉和 SE 供电（SE 空闲只有 µA 级，够）。
  * 共地；两只 4.7 k 上拉保持接到 3V3。

用法：
  python tools\\ch347_i2c.py list
  python tools\\ch347_i2c.py scan                      # 扫 0x01~0x7F，写/读两个方向都探
  python tools\\ch347_i2c.py scan --clk 0              # 20 kHz 对照
  python tools\\ch347_i2c.py xfer 64 --waddr 00 --read 8
  python tools\\ch347_i2c.py atecc 64 --op random --waddr 00
  python tools\\ch347_i2c.py atecc 64 --op info  --waddr 03
  python tools\\ch347_i2c.py atecc 64 --op random --waddr 00 --clk 0 --crc
"""
import argparse
import ctypes
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ch347_spi as cs          # noqa: E402  复用同一把 DLL 的加载/错误类型/设备信息结构

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "proto"))
try:
    import run_cross_test as rct        # noqa: E402  CRC-16/BUYPASS 的**单一源**（Python 参考）
    HAVE_RCT = True
except Exception:                       # noqa: BLE001  没有参考实现也不该让工具不能跑
    HAVE_RCT = False

I2C_CLK = {0: "20 kHz", 1: "100 kHz", 2: "400 kHz", 3: "750 kHz"}

# ATECC/SHA 家族的字地址（出处见 docs/atecc608b-se.md §3b）
WADDR_CMD = 0x03        # 命令（写）
WADDR_RESP = 0x00       # 响应（读）


class Ch347I2c(cs.Ch347):
    """在 `ch347_spi.Ch347` 上加 I²C 部分（同一把 DLL、同一个 `iIndex`）。"""

    def _bind(self):
        super()._bind()
        d = self.dll
        d.CH347I2C_Set.restype = ctypes.c_int
        d.CH347I2C_Set.argtypes = [ctypes.c_ulong, ctypes.c_ulong]
        d.CH347StreamI2C.restype = ctypes.c_int
        d.CH347StreamI2C.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                     ctypes.c_ulong, ctypes.c_void_p]
        d.CH347StreamI2C_RetACK.restype = ctypes.c_int
        d.CH347StreamI2C_RetACK.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                            ctypes.c_ulong, ctypes.c_void_p,
                                            ctypes.POINTER(ctypes.c_ulong)]
        # 下面几个不是所有 DLL 版本都导出 ⇒ 有就绑，没有就跳过（别因此整个工具不能用）
        for name, argtypes in (("CH347I2C_SetStretch", [ctypes.c_ulong, ctypes.c_int]),
                               ("CH347I2C_SetDelaymS", [ctypes.c_ulong, ctypes.c_ulong]),
                               ("CH347I2C_SetDriverMode",
                                [ctypes.c_ulong, ctypes.c_ubyte])):
            fn = getattr(d, name, None)
            if fn is not None:
                fn.restype = ctypes.c_int
                fn.argtypes = argtypes

    # ------------------------------------------------------------------ I2C
    def i2c_set(self, mode=1, stretch=None, delay_ms=0):
        """`mode` 的低 2 位 = 速度（0=20k / 1=100k / 2=400k / 3=750k）。"""
        if not self.dll.CH347I2C_Set(self.index, mode):
            return False
        if stretch is not None and hasattr(self.dll, "CH347I2C_SetStretch"):
            self.dll.CH347I2C_SetStretch(self.index, 1 if stretch else 0)
        if delay_ms and hasattr(self.dll, "CH347I2C_SetDelaymS"):
            self.dll.CH347I2C_SetDelaymS(self.index, delay_ms)
        return True

    def i2c_usable(self, mode=1):
        """能不能真当 I²C 主机用。

        ⚠ 沿用 `ch347_spi.spi_usable()` 的教训（2026-09-16 实测）：**没插任何 WCH 设备时
        `CH347OpenDevice(0..3)` 照样返回句柄**、`CH347GetChipType` 给 0(CH341)，
        所以"设备在不在"只能靠**一次真的初始化调用**判断，不能看 open 的返回值。
        """
        return bool(self.i2c_set(mode))

    def xfer(self, write, read_len=0, ret_ack=False):
        """一次 I²C 传输。`write` 的**第一个字节**必须是「地址+方向位」（8 位形式）。

        返回 `(read_bytes, ack_count)`；`ack_count` 只在 `ret_ack=True` 时有意义
        （`CH347StreamI2C_RetACK` 的 ACK 计数），否则为 `None`。
        """
        wbuf = (ctypes.c_ubyte * max(1, len(write)))(*write)
        rbuf = (ctypes.c_ubyte * max(1, read_len))()
        if ret_ack:
            ack = ctypes.c_ulong(0xFFFFFFFF)
            ok = self.dll.CH347StreamI2C_RetACK(self.index, len(write), wbuf,
                                                read_len, rbuf, ctypes.byref(ack))
            if not ok:
                raise cs.Ch347Error("CH347StreamI2C_RetACK 失败（USB/DLL 层）")
            return bytes(rbuf[:read_len]), int(ack.value)
        ok = self.dll.CH347StreamI2C(self.index, len(write), wbuf, read_len, rbuf)
        if not ok:
            raise cs.Ch347Error("CH347StreamI2C 失败（USB/DLL 层）")
        return bytes(rbuf[:read_len]), None

    def probe(self, addr7, read_dir=False):
        """只发地址字节，看器件 ACK 了没有（用 `RetACK` 的 ACK 计数，**独立于我们固件**）。

        返回 `True` = 收到 ACK；`False` = 没收着 ACK（地址没人应答）。
        """
        byte = ((addr7 << 1) | (1 if read_dir else 0)) & 0xFF
        _rd, ack = self.xfer([byte], 0, ret_ack=True)
        return ack >= 1

    def write_read(self, addr7, payload, read_len, waddr=None):
        """CryptoAuth 风格的一次事务：`[addr|0][字地址][payload]` 写完，再读 `read_len`。

        注意：**一条 USB 调用里同时给 write/read 长度**时，DLL 会拼成
        `START + addr(W) + payload + RESTART + addr(R) + read + STOP`
        ⇒ 不适合"先写命令、等器件算完、再从**另一个字地址**读响应"那种两段式。
        所以 `atecc` 子命令用的是**两次独立调用**（先写、再 `[addr|0][字地址]` + 读）。
        """
        w = [(addr7 << 1) & 0xFF]
        if waddr is not None:
            w.append(waddr & 0xFF)
        w.extend(payload)
        return self.xfer(w, read_len)[0]

    def read_from(self, addr7, waddr, read_len):
        """两段式的第二段：`[addr|0][字地址]` + RESTART + `[addr|1]` + 读 `read_len`。"""
        return self.xfer([(addr7 << 1) & 0xFF, waddr & 0xFF], read_len)[0]

    def write_only(self, addr7, payload, waddr=None, ret_ack=False):
        """只写（含可选字地址）。`ret_ack=True` 时回 ACK 计数 ⇒ 能看出**第几个字节**被 NACK。"""
        w = [(addr7 << 1) & 0xFF]
        if waddr is not None:
            w.append(waddr & 0xFF)
        w.extend(payload)
        _rd, ack = self.xfer(w, 0, ret_ack=ret_ack)
        return ack


# ---------------------------------------------------------------------- 报文
def atecc_pkt(op, p1=0x00, p2=0x0000, with_crc=False, extra=b""):
    """组一条 ATECC 风格命令包（形状照 `proto/atecc_msg.c`，**同一套规则**）。"""
    body = bytearray([op & 0xFF, p1 & 0xFF, (p2 >> 8) & 0xFF, p2 & 0xFF]) + bytearray(extra)
    pkt = bytearray([len(body) + 1 + (2 if with_crc else 0)]) + body
    if with_crc:
        if not HAVE_RCT:
            raise cs.Ch347Error("要带 CRC 但没加载到 Python CRC 参考（proto/run_cross_test.py）")
        c = rct.py_crc16(bytes(pkt))
        pkt += bytes([c & 0xFF, (c >> 8) & 0xFF])
    return bytes(pkt)


def crc_ok(buf):
    """按 CryptoAuth 的算法**真验**一遍末尾 CRC（只用于打印判语，不替器件判对错）。"""
    if not HAVE_RCT or len(buf) < 3:
        return None
    c = rct.py_crc16(bytes(buf[:-2]))
    return buf[-2] == (c & 0xFF) and buf[-1] == ((c >> 8) & 0xFF)


def hexs(b):
    return " ".join("%02X" % x for x in b) if b else "-"


# ---------------------------------------------------------------------- CLI
def dev_list(args):
    print("CH347 设备枚举（iIndex 0..3；I²C 与 SPI 共用同一个 iIndex 空间）:")
    found = 0
    for i in range(4):
        try:
            with Ch347I2c(index=i) as dev:
                ct = dev.chip_type()
                name = cs.CHIP_NAMES.get(ct, "未知(%d)" % ct)
                usable = dev.i2c_usable(args.clk)
                print("  iIndex=%d：%s，I²C 初始化 %s" % (i, name, "成功" if usable else "失败"))
                if usable:
                    found += 1
        except cs.Ch347Error as exc:
            print("  iIndex=%d：打不开（%s）" % (i, exc))
    print("可用 I²C 主机：%d 个（注意：`CH347OpenDevice` 成功**不代表**设备在，"
          "上面判的是 `CH347I2C_Set` 的返回值）" % found)
    return 0 if found else 2


def cmd_scan(args):
    with Ch347I2c(index=args.index) as dev:
        if not dev.i2c_usable(args.clk):
            print("I²C 初始化失败：设备没插 / 驱动不对 / iIndex 不对")
            return 2
        print("扫描 0x01~0x7F（写方向 + 读方向各一次；ACK 用 CH347StreamI2C_RetACK 的计数判）")
        print("时钟：%s（CH347I2C_Set mode=%d）" % (I2C_CLK.get(args.clk, "?"), args.clk))
        hits = []
        for a in range(0x01, 0x80):
            w = dev.probe(a, read_dir=False)
            r = dev.probe(a, read_dir=True)
            if w or r:
                hits.append((a, w, r))
                print("  0x%02X：写方向 %s / 读方向 %s%s"
                      % (a, "ACK" if w else "NACK", "ACK" if r else "NACK",
                         "   ← 两个方向都 ACK = 真从机" if (w and r) else ""))
        if not hits:
            print("  没有任何地址应答（两个方向都空）")
        else:
            print("共 %d 个地址有应答" % len(hits))
        return 0


def cmd_xfer(args):
    with Ch347I2c(index=args.index) as dev:
        if not dev.i2c_usable(args.clk):
            print("I²C 初始化失败")
            return 2
        payload = bytes.fromhex(args.hex) if args.hex else b""
        waddr = int(args.waddr, 16) if args.waddr is not None else None
        w = [(args.addr << 1) & 0xFF] + ([waddr] if waddr is not None else []) + list(payload)
        print("写 %d 字节：%s" % (len(w), hexs(w)))
        if args.read:
            rd, ack = dev.xfer(w, args.read)
            print("读 %d 字节：%s" % (args.read, hexs(rd)))
        else:
            ack = dev.write_only(args.addr, payload, waddr, ret_ack=True)
            print("ACK 计数 = %s（= 写缓冲里被 ACK 的字节数；少于 1+payload 就是中途被 NACK）"
                  % ("-" if ack is None else ack))
    return 0


def cmd_atecc(args):
    """★ 关键一跳：用**独立主机**把 `Info` / `Random` 跑一遍，看它到底回不回。"""
    with Ch347I2c(index=args.index) as dev:
        if not dev.i2c_usable(args.clk):
            print("I²C 初始化失败")
            return 2
        waddr = int(args.waddr, 16)
        print("CH347 独立主机：addr=0x%02X，命令字地址=0x%02X，响应字地址=0x%02X，时钟=%s，CRC=%s"
              % (args.addr, waddr, int(args.resp_waddr, 16),
                 I2C_CLK.get(args.clk, "?"), "带" if args.crc else "不带"))

        if args.op == "random":
            pkt = atecc_pkt(0x1B, 0x00, 0x0000, args.crc)
            n = 38 if args.crc else 36
        else:                                   # info / DevRev
            pkt = atecc_pkt(0x30, 0x00, 0x0000, args.crc)
            n = 10 if args.crc else 8
        print("命令包（%d 字节）：%s" % (len(pkt), hexs(pkt)))

        ack = dev.write_only(args.addr, pkt, waddr, ret_ack=True)
        print("① 写命令：ACK 计数 = %s（期望 = %d = 地址 + 字地址 + 包）"
              % ("-" if ack is None else ack, 2 + len(pkt)))

        rd = dev.read_from(args.addr, int(args.resp_waddr, 16), n)
        print("② 读响应（%d 字节）：%s" % (n, hexs(rd)))
        if rd:
            cnt = int.from_bytes(rd[:4], "big")
            print("   count 字段 = %d（大端）；本长度 %d" % (cnt, n))
            ck = crc_ok(rd)
            if ck is not None:
                print("   CRC：%s" % ("对" if ck else "**不对或器件不挂 CRC**"))
            if rd[:4] == b"\xff\xff\xff\xff":
                print("   全 0xFF ⇒ **从机一个字节都没驱动**（器件没准备响应/不认这条命令）")
            elif cnt in (8, 10, 36, 38):
                print("   ★ count 合法 ⇒ **器件真的回了响应** —— 那就说明我们固件侧还有问题！")
    return 0


def cmd_selftest(args):
    """离线自检：不碰硬件，只验报文组包与（能拿到时的）CRC 参考。"""
    bad = 0
    for crc in (False, True):
        p = atecc_pkt(0x1B, 0x00, 0x0000, crc)
        want_len = 7 if crc else 5
        ok = len(p) == want_len and p[1] == 0x1B
        if crc:
            ok = ok and crc_ok(p) is True
        bad += 0 if ok else 1
        print("  %s Random 包 crc=%s：%s" % ("ok  " if ok else "BAD ", crc, hexs(p)))
    p = atecc_pkt(0x30, 0x00, 0x0000, False)
    ok = len(p) == 5 and p[1] == 0x30
    bad += 0 if ok else 1
    print("  %s Info  包：%s" % ("ok  " if ok else "BAD ", hexs(p)))
    print("CRC 参考（proto/run_cross_test.py）%s" % ("已加载" if HAVE_RCT else "**没加载到**"))
    return 2 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="CH347/CH347F 当独立 I²C 主机（B 方案）")
    ap.add_argument("--index", type=int, default=0, help="设备序号（缺省 0）")
    ap.add_argument("--clk", type=int, default=1, choices=[0, 1, 2, 3],
                    help="0=20kHz 1=100kHz(缺省) 2=400kHz 3=750kHz")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="枚举 CH347 设备并试一次 I²C 初始化")
    sub.add_parser("selftest", help="离线自检（不碰硬件）")

    sub.add_parser("scan", help="扫 0x01~0x7F（写/读两个方向）")

    p = sub.add_parser("xfer", help="任意一次传输（写缓冲首字节=地址+方向位）")
    p.add_argument("addr", type=lambda s: int(s, 16), help="7 位地址（hex）")
    p.add_argument("hex", nargs="?", default="", help="要写的 payload（hex 串，可空）")
    p.add_argument("--waddr", default=None, help="先写一个字地址（hex，如 00 / 03）")
    p.add_argument("--read", type=int, default=0, help="读完 N 字节")

    p = sub.add_parser("atecc", help="用独立主机跑 Info / Random（关键判据）")
    p.add_argument("addr", type=lambda s: int(s, 16), help="7 位地址（hex，如 64）")
    p.add_argument("--op", default="random", choices=["random", "info"])
    p.add_argument("--waddr", default="%02x" % WADDR_CMD, help="命令字地址（hex，缺省 03）")
    p.add_argument("--resp-waddr", default="%02x" % WADDR_RESP, help="响应字地址（hex，缺省 00）")
    p.add_argument("--crc", action="store_true", help="命令包挂 CRC-16/BUYPASS")

    args = ap.parse_args()
    fn = {"list": dev_list, "selftest": cmd_selftest, "scan": cmd_scan,
          "xfer": cmd_xfer, "atecc": cmd_atecc}[args.cmd]
    try:
        return fn(args)
    except cs.Ch347Error as exc:
        print("错误：%s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
