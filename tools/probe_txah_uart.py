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
  python tools\probe_txah_uart.py --ch347-uart 0 probe      # ★ 数据口活性探测（先跑这个）
  python tools\probe_txah_uart.py --ch347-uart 0 loopback   # 自环（TXD0↔RXD0 短接），验桥和线
  python tools\probe_txah_uart.py --port COM32 listen --secs 10
  python tools\probe_txah_uart.py --ch347-uart 0 listen --secs 10
  python tools\probe_txah_uart.py --port COM32 send-eth 00005e000153...   # 完整以太帧
  python tools\probe_txah_uart.py --port COM32 send-cmd 109              # GET_UART_FIXLEN
  python tools\probe_txah_uart.py --port COM32 raw 2B 1A 03 00 08 00 00 00

判据（怎么算"通了"）：
  ⚠ **`listen` 读到 0 字节 ≠ 不通**：数据口不是打印口，模组没东西要发时它是安静的。
    所以先跑 `probe`（它会**主动**发一问一答的命令帧），有回帧才算活。
  * `probe` 收到合法回帧 → 数据口活的；
  * `loopback` 不通 → 先解决桥/线，别怀疑模组；
  * 都没有 → 按 `probe` 结尾给的顺序查（**第一步：AT 口复位后有没有 `uart bus fixlen=…`**）。
⚠ 本工具**不做**协议语义（不改模组状态、不写 flash）；要改配置请走 AT 口。
"""
import argparse
import re
import subprocess
import sys
import time

from txah_hgic import (CMD_GET_UART_FIXLEN, MAX_FRAME, StreamParser, cmd_frame, data_frame,
                       describe, selftest as hgic_selftest)

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BAUD = 115200


# --------------------------------------------------------------------- 传输
class SerialTransport:
    """普通 USB-UART / CH347F 的 VCP COM 口（pyserial）。

    ⚠ CH347F 在本机是 **VCP 模式**，它的两路 UART 就是普通串口
    （`USB-HiSpeed-SERIAL-A/B CH347F`，`MI_00`=UART0、`MI_02`=UART1）——
    走这里通常比走 WCH DLL 更省事。
    """

    def __init__(self, port, baud=BAUD):
        try:
            import serial                                # 延迟导入，没装也能跑 selftest
        except ImportError as exc:
            raise SystemExit(
                "没装 pyserial（当前解释器：%s）。两条路任选：\n"
                "  ① 装：%s -m pip install pyserial\n"
                "  ② 换有 pyserial 的解释器跑，例如 C:\\Python313\\python.exe tools\\probe_txah_uart.py ...\n"
                "（原始错误：%s）" % (sys.executable, sys.executable, exc))
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

    def __init__(self, index=0, uart=0, baud=BAUD, byte_timeout=None):
        from ch347_spi import Ch347
        self.dev = Ch347(index)
        kw = {} if byte_timeout is None else {"byte_timeout": byte_timeout}
        self.dev.uart_open(baud=baud, uart=uart, **kw)   # 不 open() 设备，见上面注释
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


def find_ch347_com(uart=0):
    """找 CH347F **VCP 模式**下的 UART COM 口（按 USB 接口号：**0 = MI_00 = UART0、1 = MI_02 = UART1**）。

    实测（2026-09-16，本机）：这条路能通（loopback 原样读回），而 WCH DLL 的
    `CH347Uart_*` 在 VCP 模式下**写得出、读不回**（`--ch347-uart 0 loopback` 失败）——
    所以 VCP 模式下就**用 COM 口**。
    """
    mi = {0: "MI_00", 1: "MI_02"}.get(uart)
    if mi is None:
        return None
    ps = ("Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like "
          "'USB\\VID_1A86&PID_55DE&%s*' } | Select-Object -ExpandProperty Name" % mi)
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=20)
    except Exception:                                     # noqa: BLE001
        return None
    m = re.search(r"\((COM\d+)\)", out.stdout or "")
    return m.group(1) if m else None


def open_transport(args):
    port = getattr(args, "port", None)
    ch347_com = getattr(args, "ch347_com", None)
    ch347_uart = getattr(args, "ch347_uart", None)
    baud = getattr(args, "baud", BAUD)
    index = getattr(args, "index", 0)
    if ch347_com is not None:
        found = find_ch347_com(ch347_com)
        if not found:
            raise SystemExit("没找到 CH347F 的 UART%d COM 口（板子插着吗？驱动是 WCH 的 VCP 吗？）"
                             "—— 也可以自己看设备管理器后手动给 --port COMxx" % ch347_com)
        print("[i] CH347F UART%d（MI_%02d）= %s" % (ch347_com, 0 if ch347_com == 0 else 2, found))
        return SerialTransport(found, baud)
    if ch347_uart is not None:
        bt = getattr(args, "byte_timeout", None)
        return Ch347UartTransport(index=index, uart=ch347_uart, baud=baud, byte_timeout=bt)
    if port:
        return SerialTransport(port, baud)
    raise SystemExit("三选一：`--ch347-com 0|1`（推荐，自动找 CH347F 的 VCP COM 口）、"
                     "`--port COMx`（任意串口）、`--ch347-uart 0|1`（走 WCH DLL，"
                     "本机 VCP 模式下实测不通）")


# ------------------------------------------------------------------- 子命令
def do_listen(t, args):
    parser = StreamParser(expect_from_module=True if args.only_rx else None)
    t.flush()
    print("监听 %s（%s，%.1f 秒）…" % (t.name(), "只看模组→主机" if args.only_rx else "两个方向都认",
                                   args.secs))
    end = time.time() + args.secs
    nframe, nbytes = 0, 0
    pend = None                 # [连续重复次数, hdr, payload]
    tally = {}

    def flush():
        """把「连续同一条」合并成一行再打 —— 模组会周期性刷同一种事件（实测 TX_BITRATE）。"""
        if not pend:
            return
        cnt, hdr, payload = pend
        print("#%-3d %s" % (nframe - cnt + 1, describe(hdr, payload, full_payload=args.full)))
        if cnt > 1:
            print("     ↑ 同一条连续重复 %d 次（合并显示）" % cnt)
        info = ctrl_info(hdr, payload)
        key = ((hdr["type_name"], info["id"], info["name"]) if info
               else (hdr["type_name"], None, None))
        tally[key] = tally.get(key, 0) + cnt
        pend.clear()

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
            if pend and pend[1]["type"] == hdr["type"] and pend[2] == payload:
                pend[0] += 1
                continue
            flush()
            pend = [1, hdr, payload]
    flush()
    print("-" * 70)
    print("读到 %d 字节 / 认出 %d 帧；杂字节 %d，长度非法跳过的 %d"
          % (nbytes, nframe, parser.garbage, parser.bad_length))
    if tally:
        print("按类型统计：")
        for (tn, cid, nm), cnt in sorted(tally.items(), key=lambda kv: -kv[1]):
            print("   %-8s id=%-4s %-16s x%d" % (tn, cid if cid is not None else "-", nm or "-", cnt))
    if nbytes == 0:
        print("=> 一个字节都没有：先确认①烧的是 MACBUS_UART 版固件 ②跳线在 A10/A11（通信）"
              "③共地 ④波特率 115200")
    elif nframe == 0:
        print("=> 有字节但没认出帧：把上面的原始字节（`--dump-raw`）贴出来看 magic/长度；"
              "也可能是定长（fixlen）模式")
    else:
        print("=> 认出了合法 HGIC 帧 ✓（数据帧载荷约定：`send-eth` 的 `--frame-type`/"
              "`--no-ethernet` 两种都试）")
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
    time.sleep(getattr(args, "wait", 1.0))
    back = t.read()
    if back:
        print("回读 %s" % back.hex(" "))
        p = StreamParser()
        for hdr, pl in p.feed(back):
            print("  %s" % describe(hdr, pl))
    else:
        print("（%d 秒内没有回读；模组侧若有配对的对端，数据应经空口发出去了）" % getattr(args, "wait", 1.0))
    return 0


def do_send_cmd(t, args):
    payload = bytes.fromhex("".join(args.hex)) if args.hex else b""
    f = cmd_frame(args.cmd_id, payload, cookie=args.cookie)
    print("发命令帧（id=%d）：%s" % (args.cmd_id, f.hex(" ")))
    t.write(f)
    time.sleep(getattr(args, "wait", 1.0))
    back = t.read()
    if not back:
        print("（%d 秒内没有应答）" % getattr(args, "wait", 1.0))
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
    time.sleep(getattr(args, "wait", 1.0))
    back = t.read()
    print("回读 %s" % (back.hex(" ") if back else "（无）"))
    return 0


def _read_for(t, secs):
    """在 secs 秒内尽量多读（返回累计字节）。"""
    buf = b""
    end = time.time() + secs
    while time.time() < end:
        chunk = t.read()
        if chunk:
            buf += chunk
            end = time.time() + min(0.2, secs)      # 读到东西就再等一小会儿
        else:
            time.sleep(0.005)
    return buf


def do_loopback(t, args):
    """UART **自环**：把本路 TXD 与 RXD 短接，读回应等于发出去的。

    用来把"桥/线的问题"和"模组的问题"分开 —— 自环不通就别怀疑模组。
    """
    pat = bytes([0x55, 0xAA, 0x00, 0xFF, 0x2B, 0x1A])
    print("自环（要求把本路 **TXD 与 RXD 短接**；CH347F P2 上就是把 `TXD0` 和 `RXD0` 用一根线连起来）")
    print("发 %s" % pat.hex(" "))
    t.flush()
    t.write(pat)
    back = _read_for(t, getattr(args, "wait", 1.0))
    print("读 %s" % (back.hex(" ") if back else "（无）"))
    if pat in back:
        print("=> 自环通过 ✓（桥 + 这根线没问题；那么'模组不答'就真是模组那侧的事）")
        return 0
    print("=> 自环不通 ✗：要么 TXD/RXD 没短接，要么桥/线/驱动有问题（先解决这个）")
    return 2


def do_probe(t, args):
    """数据口活性探测：① 被动听一会儿 ② 主动发一问一答的命令帧 ③ 报结论。"""
    print("① 被动监听 %.1f 秒（⚠ 模组平时可能是安静的：没有帧**不等于**不通）…" % args.secs)
    got = _read_for(t, args.secs)
    if got:
        print("   收到 %d 字节：%s" % (len(got), got[:64].hex(" ")))
    else:
        print("   一个字节都没有（正常也可能是这样）")
    p = StreamParser()
    frames = p.feed(got)
    for hdr, payload in frames:
        print("   帧：%s" % describe(hdr, payload))
    print("   杂字节 %d，长度非法 %d" % (p.garbage, p.bad_length))

    print("② 主动探测：发 `GET_UART_FIXLEN`(id=%d) 命令帧 —— 只读，不改模组配置。" % CMD_GET_UART_FIXLEN)
    print("   （id 108 是 SET_UART_FIXLEN，会写 flash，**别拿它当探针**）")
    f = cmd_frame(CMD_GET_UART_FIXLEN, cookie=0x1234)
    print("   发 %s" % f.hex(" "))
    t.flush()
    t.write(f)
    back = _read_for(t, getattr(args, "wait", 1.0) + 0.5)
    print("   读 %s" % (back.hex(" ") if back else "（无）"))
    p2 = StreamParser()
    got2 = p2.feed(back)
    for hdr, payload in got2:
        print("   帧：%s" % describe(hdr, payload, full_payload=True))

    print("-" * 70)
    if got2:
        print("=> **数据口是活的** ✓：模组回了合法 HGIC 帧（应答载荷见上）")
        return 0
    if back:
        print("=> 有回字节但认不出帧：把原始字节贴出来（magic/长度/定长模式都可能不一样）")
        return 2
    print("=> 命令帧没有任何回应。按这个顺序查（每一层都验过再往下）：")
    print("   1) **打印口**（AT 那根线）复位后有没有 `uart bus fixlen=...` 这一行 ——")
    print("      这行由 MACBUS_UART 固件的 `mac_bus_uart_attach` 打印；**没有就是没烧进去/没跑起来**")
    print("   2) 用 `loopback` 子命令做 CH347F 自环（TXD0↔RXD0 短接）—— 桥和线是否可信")
    print("   3) 共地、模组供电（别用 CH347F 的 3V3）、开发板 UART 跳线档位、A10/A11 是否真引出来")
    print("   4) 波特率 115200 8N1（`--baud`）")
    return 2


def main():
    # ⚠ 这些选项既在顶层也在各子命令上（`parents=[common]`），所以默认值必须用 SUPPRESS：
    # 否则子命令会用它自己的默认值把顶层解析到的值覆盖掉（`--ch347-uart 0 loopback` 会变成 None）。
    common = argparse.ArgumentParser(add_help=False)
    S = argparse.SUPPRESS
    common.add_argument("--port", default=S, help="串口名，如 COM23")
    common.add_argument("--ch347-com", type=int, choices=(0, 1), default=S,
                        help="★ 推荐：自动找 CH347F 的 VCP COM 口（0=UART0、1=UART1）")
    common.add_argument("--ch347-uart", type=int, choices=(0, 1), default=S,
                        help="走 WCH DLL 的 UART（本机 VCP 模式下实测不通，留作对照）")
    common.add_argument("--index", type=int, default=S, help="CH347F 设备序号（默认 0）")
    common.add_argument("--baud", type=int, default=S)
    common.add_argument("--wait", type=float, default=S, help="发完等回读的秒数（默认 1）")
    common.add_argument("--byte-timeout", type=int, default=S,
                        help="只给 --ch347-uart 用：CH347Uart_Init 的 ByteTimeout（单位 100us）。"
                             "默认 0；若 DLL 那条路写得出读不回，可试 1/2/5")

    ap = argparse.ArgumentParser(description="TX-AH 模组数据口（mac_bus over UART）联调",
                                 parents=[common])
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("selftest", help="离线自测（不碰硬件）",
                   parents=[common]).set_defaults(func=None)

    p = sub.add_parser("listen", help="只读并解析", parents=[common])
    p.add_argument("--secs", type=float, default=10.0)
    p.add_argument("--full", action="store_true", help="完整打印载荷")
    p.add_argument("--dump-raw", action="store_true", help="把原始字节也打出来")
    p.add_argument("--only-rx", action="store_true", help="只认模组→主机的 magic")
    p.set_defaults(func=do_listen)

    p = sub.add_parser("probe", help="活性探测：被动听 + 主动发一问一答的命令帧", parents=[common])
    p.add_argument("--secs", type=float, default=3.0, help="被动听多久（默认 3 秒）")
    p.add_argument("--dump-raw", action="store_true")
    p.set_defaults(func=do_probe)

    p = sub.add_parser("loopback", help="UART 自环（TXD↔RXD 短接），先把桥和线验掉", parents=[common])
    p.set_defaults(func=do_loopback)

    p = sub.add_parser("send-eth", help="发数据帧（载荷给十六进制）", parents=[common])
    p.add_argument("hex", nargs="+")
    p.add_argument("--frame-type", choices=("frm2", "frm"), default="frm2")
    p.add_argument("--with-frm-info", action="store_true", help="FRM 时是否补 24B info")
    p.add_argument("--no-ethernet", dest="ethernet", action="store_false",
                   help="载荷是裸数据（不带以太头）")
    p.add_argument("--cookie", type=lambda s: int(s, 0), default=1)
    p.set_defaults(func=do_send_eth)

    p = sub.add_parser("send-cmd", help="发命令帧", parents=[common])
    p.add_argument("cmd_id", type=int, help="如 %d = GET_UART_FIXLEN" % CMD_GET_UART_FIXLEN)
    p.add_argument("hex", nargs="*")
    p.add_argument("--cookie", type=lambda s: int(s, 0), default=1)
    p.set_defaults(func=do_send_cmd)

    p = sub.add_parser("raw", help="原样发字节（调试）", parents=[common])
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
