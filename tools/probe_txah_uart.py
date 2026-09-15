#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
probe_txah_uart.py — 用 PC 接模组**数据口**（mac_bus over UART）做收发联调/首测。

前提（见 docs/txah-uart-macbus.md）：
  * 模组已烧 **MACBUS_UART** 版固件（`halow-demo/TXW8301/tools/fmac_macbus_switch.py uart`
    + 重编 + `at+fwupg` 烧录）——2026-09-16 已编出、**未烧**。
  * 数据口 = 模组 **UART0（IOA10/A11）115200 8N1**；AT/打印口在 **UART1（IOA12/A13）**。
    开发板跳线要选到 A10/A11（通信）那一档；**必须共地**。
  * PC 侧接法二选一：
      - CH347F-EVT 的某一路 UART（`--ch347-uart 0|1`，走 WCH DLL，不需要 pyserial）
      - 任意 USB-UART 适配器（`--port COMx`，需要 pyserial）

帧格式 = `tools/txah_hgic.py`（8 字节 HGIC 头，magic 0x1A2B/0x2B1A，第 5/6 字节 = 整帧长）。

用法：
  python tools\probe_txah_uart.py selftest                 # 离线自测（不碰硬件）
  python tools\probe_txah_uart.py --port COM32 listen --secs 10
  python tools\probe_txah_uart.py --ch347-uart 1 listen --secs 10
  python tools\probe_txah_uart.py --port COM32 send-eth 00005e000153...   # 完整以太帧
  python tools\probe_txah_uart.py --port COM32 send-cmd 109              # GET_UART_FIXLEN
  python tools\probe_txah_uart.py --port COM32 raw 2B 1A 03 00 08 00 00 00

判据（怎么算"通了"）：
  * `listen` 能认出**合法帧**（magic/length 自洽）→ 数据口在往外吐东西；
  * 若一个字节都没读到 → 先查固件烧的哪版、跳线/共地、波特率；
  * 若有字节但一直认不出帧 → 把原始字节贴出来（`--dump-raw`），再看是不是 magic/定长模式不同。
⚠ 本工具**不做**协议语义（不改模组状态、不写 flash）；要改配置请走 AT 口。
"""
import argparse
import sys
import time

from txah_hgic import (CMD_GET_UART_FIXLEN, MAX_FRAME, StreamParser, cmd_frame, data_frame,
                       describe, selftest as hgic_selftest)

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BAUD = 115200


# --------------------------------------------------------------------- 传输
class SerialTransport:
    """普通 USB-UART（pyserial）。"""

    def __init__(self, port, baud=BAUD):
        import serial                                    # 延迟导入，没装也能跑 selftest
        self.ser = serial.Serial(port, baud, timeout=0.05)

    def read(self, maxn=4096):
        n = self.ser.in_waiting
        return self.ser.read(min(n, maxn)) if n else b""

    def write(self, data):
        return self.ser.write(bytes(data))

    def flush(self):
        time.sleep(0.1)
        self.read(MAX_FRAME)

    def close(self):
        self.ser.close()

    def name(self):
        return "串口 %s @%d" % (self.ser.port, self.ser.baudrate)


class Ch347UartTransport:
    """CH347F-EVT 的 UART（走 WCH DLL，见 tools/ch347_spi.py）。

    ⚠ 两点实测（2026-09-16）：
      * UART 是**独立索引空间**：CH347F 上 **0 = UART0、1 = UART1**
        （`CH347Uart_GetDeviceInfor().FuncDescStr` = `CH347F.M0:USB2.0 To VCP UART0/1`）。
      * **故意不调 `CH347OpenDevice()`**：它会占住"设备索引 0"（DLL 里 SPI 功能就在 0），
        之后 UART0 的 `CH347Uart_Init` 会**失败**（实测：先 `CH347OpenDevice(0)` 再开 UART0
        → Init=0；只开 UART0 → Init=1）。UART API 自己会开设备。
        ⇒ UART0 + UART1 同时用没问题（我们的用法：数据口 + AT 口）；想同时用 **SPI** 时，
        只能配 UART1（SPI + UART1 可共存，SPI + UART0 冲突）。
    """

    def __init__(self, index=0, uart=0, baud=BAUD):
        from ch347_spi import Ch347
        self.dev = Ch347(index)
        self.dev.uart_open(baud=baud, uart=uart)     # 不 open() 设备，见上面注释
        self.uart = uart
        self.baud = baud

    def read(self, maxn=MAX_FRAME):
        n = self.dev.uart_pending()
        if n <= 0:
            return b""
        return self.dev.uart_read(min(n, maxn))

    def write(self, data):
        return self.dev.uart_write(data)

    def flush(self):
        time.sleep(0.1)
        self.read()

    def close(self):
        self.dev.close()

    def name(self):
        return "CH347F UART%d @%d" % (self.uart, self.baud)


def open_transport(args):
    if args.ch347_uart is not None:
        return Ch347UartTransport(index=args.index, uart=args.ch347_uart, baud=args.baud)
    if args.port:
        return SerialTransport(args.port, args.baud)
    raise SystemExit("要么给 --port COMx（普通 USB-UART），要么给 --ch347-uart 0|1（CH347F-EVT）")


# ------------------------------------------------------------------- 子命令
def do_listen(t, args):
    parser = StreamParser()
    t.flush()
    print("监听 %s（%s，%.1f 秒）…" % (t.name(), "只看模组→主机" if args.only_rx else "两个方向都认",
                                   args.secs))
    end = time.time() + args.secs
    nframe, nbytes = 0, 0
    while time.time() < end:
        chunk = t.read()
        if not chunk:
            time.sleep(0.005)
            continue
        nbytes += len(chunk)
        if args.dump_raw:
            print("  原始 %s" % chunk.hex(" "))
        for hdr, payload in parser.feed(chunk):
            nframe += 1
            print("#%-3d %s" % (nframe, describe(hdr, payload, full_payload=args.full)))
    print("-" * 70)
    print("读到 %d 字节 / 认出 %d 帧；杂字节 %d，长度非法跳过的 %d"
          % (nbytes, nframe, parser.garbage, parser.bad_length))
    if nbytes == 0:
        print("=> 一个字节都没有：先确认①烧的是 MACBUS_UART 版固件 ②跳线在 A10/A11（通信）"
              "③共地 ④波特率 115200")
    elif nframe == 0:
        print("=> 有字节但没认出帧：把上面的原始字节（`--dump-raw`）贴出来看 magic/长度；"
              "也可能是定长（fixlen）模式")
    else:
        print("=> 认出了合法 HGIC 帧 ✓（下一步：数据帧载荷是裸数据还是带以太头，"
              "用 send-eth/send-cmd 各试一次）")
    return 0 if nframe else 2


def do_send_eth(t, args):
    payload = bytes.fromhex("".join(args.hex))
    if args.ethernet and len(payload) < 14:
        print("[!] 说是完整以太帧但只有 %d 字节（< 14 的以太头）" % len(payload))
    f = data_frame(payload, cookie=args.cookie, with_frm_info=args.with_frm_info,
                   lean=(args.frame_type == "frm2"))
    print("发 %d 字节（类型 %s%s）：%s"
          % (len(f), "FRM2 精简头" if args.frame_type == "frm2" else "FRM + 24B info",
             "" if args.ethernet else "，载荷按裸数据处理", f.hex(" ")))
    t.write(f)
    time.sleep(args.wait)
    back = t.read()
    if back:
        print("回读 %s" % back.hex(" "))
        p = StreamParser()
        for hdr, pl in p.feed(back):
            print("  %s" % describe(hdr, pl))
    else:
        print("（%d 秒内没有回读；模组侧若有配对的对端，数据应经空口发出去了）" % args.wait)
    return 0


def do_send_cmd(t, args):
    payload = bytes.fromhex("".join(args.hex)) if args.hex else b""
    f = cmd_frame(args.cmd_id, payload, cookie=args.cookie)
    print("发命令帧（id=%d）：%s" % (args.cmd_id, f.hex(" ")))
    t.write(f)
    time.sleep(args.wait)
    back = t.read()
    if not back:
        print("（%d 秒内没有应答）" % args.wait)
        return 2
    print("回读 %s" % back.hex(" "))
    p = StreamParser()
    for hdr, pl in p.feed(back):
        print("  %s" % describe(hdr, pl, full_payload=True))
    return 0


def do_raw(t, args):
    data = bytes.fromhex("".join(args.hex))
    print("原样发 %d 字节：%s" % (len(data), data.hex(" ")))
    t.write(data)
    time.sleep(args.wait)
    back = t.read()
    print("回读 %s" % (back.hex(" ") if back else "（无）"))
    return 0


def main():
    ap = argparse.ArgumentParser(description="TX-AH 模组数据口（mac_bus over UART）联调")
    ap.add_argument("--port", help="串口名，如 COM32")
    ap.add_argument("--ch347-uart", type=int, choices=(0, 1), help="用 CH347F-EVT 的 UART0/UART1")
    ap.add_argument("--index", type=int, default=0, help="CH347F 设备序号（默认 0）")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--wait", type=float, default=1.0, help="发完等回读的秒数")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("selftest", help="离线自测（不碰硬件）")
    p.set_defaults(func=None)

    p = sub.add_parser("listen", help="只读并解析")
    p.add_argument("--secs", type=float, default=10.0)
    p.add_argument("--full", action="store_true", help="完整打印载荷")
    p.add_argument("--dump-raw", action="store_true", help="把原始字节也打出来")
    p.add_argument("--only-rx", action="store_true", help="只认模组→主机的 magic")
    p.set_defaults(func=do_listen)

    p = sub.add_parser("send-eth", help="发数据帧（载荷给十六进制）")
    p.add_argument("hex", nargs="+")
    p.add_argument("--frame-type", choices=("frm2", "frm"), default="frm2")
    p.add_argument("--with-frm-info", action="store_true", help="FRM 时是否补 24B info")
    p.add_argument("--no-ethernet", dest="ethernet", action="store_false",
                   help="载荷是裸数据（不带以太头）")
    p.add_argument("--cookie", type=lambda s: int(s, 0), default=1)
    p.set_defaults(func=do_send_eth)

    p = sub.add_parser("send-cmd", help="发命令帧")
    p.add_argument("cmd_id", type=int, help="如 %d = GET_UART_FIXLEN" % CMD_GET_UART_FIXLEN)
    p.add_argument("hex", nargs="*")
    p.add_argument("--cookie", type=lambda s: int(s, 0), default=1)
    p.set_defaults(func=do_send_cmd)

    p = sub.add_parser("raw", help="原样发字节（调试）")
    p.add_argument("hex", nargs="+")
    p.set_defaults(func=do_raw)

    args = ap.parse_args()
    if args.func is None:
        return hgic_selftest()
    t = open_transport(args)
    try:
        return args.func(t, args)
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())
