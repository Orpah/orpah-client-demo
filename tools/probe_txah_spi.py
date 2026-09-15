#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
probe_txah_spi.py — **电气探测**：从 PC（CH347F-EVT）用 SPI 打 TX-AH 模组的 SPI 脚，看它答不答。

为什么先做这一步（2026-09-16 用户定）：
  模组固件（FMAC v2.4.1.5）的 `mac_bus` 只实现了 **SDIO / UART / USB** 主机口，SDK 里
  **没有 SPI 主机口的实现**（`sdk/lib/bus/macbus/` 只有 sdio_bus.c / uart_bus.c / usb_bus.c，
  `mac_bus.h` 里的 `mac_bus_spi_attach` 只有声明）。而厂商 changelog 写的是
  「SPI接口和SDIO接口是同一固件」→ SPI 走的是 **SDIO 控制器的 SPI 模式**，
  线上协议是 **SD/SDIO-over-SPI**（不是我们模拟器那套 AA55 帧）。
  所以本脚本的判据是**SD 协议**：低速率下打 CMD0 / CMD8 / CMD5，看有没有合法 R1/R4/R7。
  答了就说明「这条电气+固件通路存在」，再谈上层；不答就如实记「没观察到应答」。

接线（泰芯 AH 模组开发板 V1.6；模组 SPI = SDIO 管脚复用，见《泰芯AH模组开发板使用说明》2 节）：
  | 模组侧丝印 | 模组脚 | CH347F-EVT | 备注 |
  |---|---|---|---|
  | SD_CLK / SPI_CLK1 | IOA6  | SPI **SCK**  | 开发板需把 R18/R20/R21/R23/R3/R25 焊 0R（原厂说明） |
  | SD_CMD / SPI_MOSI1| IOA7  | SPI **MOSI** | 同上 |
  | SD_D0  / SPI_MISO1| IOA8  | SPI **MISO** | 同上 |
  | SD_D1  / SPI_INTIO1| IOA9 | （可接 CH347 GPIO 观测中断，本轮不接） |
  | SD_D3  / CS1      | IOA11 | SPI **CS1**  | 同上 |
  | GND                | GND   | GND          | 必须共地 |
  供电：模组 VCC 3.1~3.3V（**别接 5V**）；开发板自带供电时按板子说明选 VCC/SVCC 跳帽。

安全：本脚本只发探测字节，**不写模组**（CMD0/CMD5/CMD8/CMD58/CMD3/CMD52 读都是只读/复位到 idle 的
标准命令，不会改 flash，也不会烧固件）。⚠ 但 SPI 与 SDIO 复用同一组脚 —— 若模组此刻正被
TF 卡座/别的线驱动，请先断开那一路。

用法：
  python tools\probe_txah_spi.py                     # 跑 bus + sweep + sdspi（推荐）
  python tools\probe_txah_spi.py misocheck gnd       # MISO 输入通道自检（先把 MISO 接到 GND）
  python tools\probe_txah_spi.py bus                 # 只看 MISO 有没有被从机驱动
  python tools\probe_txah_spi.py sweep               # 模式 0..3 × 时钟档 矩阵
  python tools\probe_txah_spi.py sdspi --hz 375000   # SD/SDIO-SPI 命令序列（默认 375 kHz 起）
  python tools\probe_txah_spi.py raw 40 00 00 00 00 95   # 原样发一串字节
另：`python tools\ch347_spi.py selftest`（MOSI↔MISO 短接）先证明**桥本身**没问题，
再怀疑模组 —— 顺序别倒。`misocheck` 是同一件事的另一半：证明 **MISO 输入通道**活着
（把 MISO 短到 GND 应读 0x00，短到 3V3 应读 0xFF）—— 否则"读回全 0xFF"可能只是
CH347F 这条输入线根本没接上，而不是模组没答。
"""
import argparse
import sys

from ch347_spi import CHIP_NAMES, Ch347, Ch347Error, SPI_SPEEDS_HZ, nearest_hz

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# SD-SPI 命令（CMD = 0x40 | index），CRC 用 CRC7 现算（poly 0x09），不抄传说值
CMD_GO_IDLE_STATE = 0
CMD_IO_SEND_OP_COND = 5      # SDIO 专用：返回 R4（含 I/O 功能数）→ 是不是 SDIO 卡就看这条
CMD_SEND_IF_COND = 8
CMD_READ_OCR = 58
CMD_APP_CMD = 55
ACMD_SD_SEND_OP_COND = 41
# 还没用上、留给后续「读 CCCR」的常量（SDIO 简化规范）：
CMD_IO_RW_DIRECT = 52
SDIO_CCCR_REVISION = 0x00
SDIO_CARD_CAPABILITY = 0x08

SWEEP_MODES = (0, 1, 2, 3)
SWEEP_HZ = (218_750, 375_000, 1_875_000, 3_750_000)

FILL = 0xFF


def crc7(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x09) & 0x7F if crc & 0x40 else (crc << 1) & 0x7F
    return crc


def sd_cmd(index, arg=0, extra=0):
    """返回 (要发的字节, 期望的响应体字节数 extra：R1=0 / R3,R7=4 / R4=4)。"""
    body = bytes([0x40 | (index & 0x3F),
                  (arg >> 24) & 0xFF, (arg >> 16) & 0xFF, (arg >> 8) & 0xFF, arg & 0xFF])
    return body + bytes([(crc7(body) << 1) | 0x01]), extra


def short_hex(data):
    if not data:
        return "(空)"
    if len(set(data)) == 1:
        return "全 0x%02X ×%d" % (data[0], len(data))
    runs = []
    cur, n = data[0], 1
    for b in data[1:]:
        if b == cur:
            n += 1
        else:
            runs.append("0x%02X×%d" % (cur, n))
            cur, n = b, 1
    runs.append("0x%02X×%d" % (cur, n))
    return " ".join(runs) if len(runs) <= 6 else data.hex(" ")


class Probe:
    def __init__(self, args):
        self.args = args
        self.dev = Ch347(args.index)
        self.dev.open()
        if not self.dev.spi_usable():
            raise Ch347Error(
                "index %d 上的设备（CH347GetChipType=%d）**不能当 SPI 主机用** —— 大概率不是 "
                "CH347F（比如是 CH340/CH341 串口适配器），或者驱动不是 WCH 的。"
                "先跑 tools\\ch347_spi.py list 看清楚。" % (args.index, self.dev.chip_type()))
        self.observations = []          # (标题, 结论) —— 汇总用

    def close(self):
        self.dev.close()

    def note(self, title, verdict):
        self.observations.append((title, verdict))
        print("   => %s" % verdict)

    # ------------------------------------------------------------- bus 观测
    def _miso_sample(self, mode, hz, fill, cs_assert, nbytes=16, cs=1):
        self.dev.spi_init(mode=mode, hz=hz, cs=cs, auto_deassert=0)
        self.dev.cs(assert_low=cs_assert)
        got = self.dev.xfer(bytes([fill]) * nbytes, cs=None)
        self.dev.cs(assert_low=False)
        return got

    def cmd_bus(self):
        print("\n[1] MISO 观测（看有没有从机在驱动这条线）")
        print("    判据：CS 拉高时从机应放开 MISO（读回恒定值）；CS 拉低后若读到"
              "非恒定/非全同模式，才说明有器件在驱动 MISO。")
        for label, cs_assert in (("CS 高(释放)", False), ("CS 低(选中)", True)):
            for fill in (0xFF, 0x00):
                got = self._miso_sample(self.args.mode, self.args.hz, fill, cs_assert,
                                        cs=self.args.cs)
                print("    %-10s 填充 0x%02X → MISO %s" % (label, fill, short_hex(got)))
        got_lo = self._miso_sample(self.args.mode, self.args.hz, FILL, True, cs=self.args.cs)
        got_hi = self._miso_sample(self.args.mode, self.args.hz, FILL, False, cs=self.args.cs)
        if got_lo == got_hi:
            self.note("MISO 观测", "CS 高/低读回相同（%s）→ **没看出有从机在驱动 MISO**" %
                      short_hex(got_lo))
            return False
        self.note("MISO 观测", "CS 低时读回与 CS 高不同 → 有器件在驱动 MISO（值得继续）")
        return True

    # ----------------------------------------------------------- sweep 矩阵
    def cmd_sweep(self):
        print("\n[2] 模式 × 时钟 矩阵（CS 选中，发 16 个 0xFF，看 MISO）")
        print("    %-6s %-10s %s" % ("mode", "时钟", "MISO 读回"))
        table = {}
        for mode in SWEEP_MODES:
            for hz in SWEEP_HZ:
                try:
                    got = self._miso_sample(mode, hz, FILL, True, cs=self.args.cs)
                except Ch347Error as exc:
                    print("    %-6d %-10d 失败：%s" % (mode, hz, exc))
                    continue
                table[(mode, hz)] = got
                print("    %-6d %-10d %s" % (mode, hz, short_hex(got)))
        uniq = {v for v in table.values()}
        if not uniq:
            self.note("模式×时钟扫描", "所有组合都失败，没拿到数据")
            return False
        if len(uniq) == 1:
            self.note("模式×时钟扫描", "所有组合读回一致（%s）→ 没有时钟/模式相关的应答" %
                      short_hex(next(iter(uniq))))
            return False
        self.note("模式×时钟扫描", "不同组合读回**不一致** → 值得逐个模式试 SD-SPI 命令")
        return True

    # ------------------------------------------------------- SD/SDIO over SPI
    def _cmd(self, index, arg=0, extra=0, tries=8):
        """发一条 SD-SPI 命令，返回 (R1, extra_bytes, 原始回读)。

        SD-SPI 时序：CS 拉低 → 发 6 字节命令 → 继续发 0xFF 读到 R1（最高位为 0）→
        需要时再读 extra 个字节 → CS 拉高 + 补 8 个时钟。
        """
        body, _ = sd_cmd(index, arg, extra)
        self.dev.cs(assert_low=True)
        raw = self.dev.xfer(body, cs=None)
        r1 = None
        for _ in range(tries):
            b = self.dev.xfer(b"\xFF", cs=None)
            raw += b
            if b[0] & 0x80 == 0:           # R1 最高位为 0
                r1 = b[0]
                break
        extra_bytes = b""
        if r1 is not None and extra:
            extra_bytes = self.dev.xfer(b"\xFF" * extra, cs=None)
            raw += extra_bytes
        self.dev.cs(assert_low=False)
        self.dev.clock_idle(1)
        return r1, extra_bytes, raw

    def cmd_sdspi(self):
        hz = self.args.hz
        print("\n[3] SD/SDIO over SPI 命令序列（时钟 %d Hz，%s；CS%d）"
              % (hz, "模式 %d" % self.args.mode, self.args.cs))
        try:
            self.dev.spi_init(mode=self.args.mode, hz=hz, cs=self.args.cs, auto_deassert=0)
        except Ch347Error as exc:
            print("    初始化失败：%s" % exc)
            return None
        # 上电序列：CS 高，打 ≥74 个时钟
        self.dev.cs(assert_low=False)
        start_hi = self.dev.clock_idle(10)
        print("    上电时钟(CS 高, 10B)：MISO %s" % short_hex(start_hi))

        results = {}
        for name, idx, arg, extra in (
                ("CMD0  GO_IDLE_STATE", CMD_GO_IDLE_STATE, 0, 0),
                ("CMD8  SEND_IF_COND", CMD_SEND_IF_COND, 0x000001AA, 4),
                ("CMD5  IO_SEND_OP_COND(SDIO)", CMD_IO_SEND_OP_COND, 0, 4),
                ("CMD58 READ_OCR", CMD_READ_OCR, 0, 4),
        ):
            r1, extra_bytes, raw = self._cmd(idx, arg, extra)
            results[name] = r1
            tag = "无响应（读回全 1）" if r1 is None else "R1=0x%02X" % r1
            detail = ("  +%s" % extra_bytes.hex(" ")) if extra_bytes else ""
            print("    %-28s → %s%s" % (name, tag, detail))
            print("        原始 %s" % short_hex(raw))

        # APP 命令：CMD55 + ACMD41
        r1, _, raw = self._cmd(CMD_APP_CMD, 0)
        print("    %-28s → %s" % ("CMD55 APP_CMD", "无响应" if r1 is None else "R1=0x%02X" % r1))
        if r1 is not None and not (r1 & 0x04):      # 不是 illegal command
            r1b, eb, raw = self._cmd(ACMD_SD_SEND_OP_COND, 0x40000000, 4)
            print("    %-28s → %s" % ("ACMD41 SD_SEND_OP_COND",
                                      "无响应" if r1b is None else "R1=0x%02X %s" %
                                      (r1b, eb.hex(" "))))

        r1_cmd5 = results.get("CMD5  IO_SEND_OP_COND(SDIO)")
        r1_cmd0 = results.get("CMD0  GO_IDLE_STATE")
        if r1_cmd5 is not None:
            self.note("SDIO 判定", "CMD5 有合法 R1（0x%02X）→ **模组的 SDIO 控制器在 SPI 模式下"
                      "有响应**（这是 SDIO 卡才有的命令）" % r1_cmd5)
            return "sdio"
        if r1_cmd0 is not None and r1_cmd0 == 0x01:
            self.note("SD 判定", "CMD0 → R1=0x01（idle）→ 像**标准 SD/SDIO 卡**的 SPI 模式，"
                      "但 CMD5 没给出 SDIO 标识；继续要看 CMD8/CMD58 的 OCR")
            return "sd"
        self.note("SD/SDIO 判定", "CMD0/CMD5/CMD8 都没有合法 R1 → **在这个时钟/模式下"
                  "没看到 SD 协议应答**")
        return None

    def cmd_misocheck(self):
        """MISO 输入通道自检：人为把 MISO 拉到固定电平，看 CH347F 读到的对不对。

        ⚠ **决定性的是 `gnd` 那一档**：悬空/未接线时 MISO 也读 0xFF，所以 `vcc` 档天然会
        "通过"，它只用来排除"MISO 被永久拉低"这种极端情况。
        """
        want = self.args.level
        expect = 0x00 if want == "gnd" else 0xFF
        print("\n[M] MISO 输入通道自检（= 先证明**桥这一侧**没问题）")
        print("    做法：把 CH347F 的 MISO 用一根线接到 %s，然后跑本命令。"
              % ("GND（地）" if want == "gnd" else "3.3V"))
        self.dev.spi_init(mode=self.args.mode, hz=self.args.hz, cs=self.args.cs,
                          auto_deassert=0)
        self.dev.cs(assert_low=True)
        got = self.dev.xfer(b"\xFF" * 16, cs=None)
        self.dev.cs(assert_low=False)
        print("    读回：%s（期望 %s）" % (short_hex(got), "全 0x00" if expect == 0 else "全 0xFF"))
        if got == bytes([expect]) * 16:
            if want == "gnd":
                self.note("MISO 输入通道",
                          "短到 GND 读到 0x00 → **这条输入通道是活的** —— 那样的话"
                          "「读回全 0xFF」才真的说明从机没答")
            else:
                self.note("MISO 输入通道", "短到 3.3V 读到 0xFF（这一档悬空也会通过，"
                          "**判据看 `misocheck gnd`**）")
            return True
        self.note("MISO 输入通道",
                  "**没读到期望值** → 先怀疑接线/共地/这根线，别把 CH347F 读到的 0xFF 当成"
                  "模组的回答")
        return False

    def cmd_raw(self):
        data = bytes(int(x, 16) for x in self.args.bytes_)
        self.dev.spi_init(mode=self.args.mode, hz=self.args.hz, cs=self.args.cs,
                          auto_deassert=0)
        got = self.dev.xfer(data, cs=self.args.cs)
        print("发 %s" % data.hex(" "))
        print("收 %s" % got.hex(" "))
        return None

    def summary(self):
        print("\n" + "=" * 78)
        print("小结（**只陈述这次看到的事实**）")
        for title, verdict in self.observations:
            print("  · %s：%s" % (title, verdict))
        if not self.observations:
            print("  · 没跑任何探测")
        print("下一步（按证据选）：")
        print("  · **先做 MISO 输入通道自检**（`misocheck gnd` / `misocheck vcc`）："
              "把 MISO 人为短到 GND/3V3，读到对应值才说明这条输入线是活的 ——")
        print("    否则「读回全 0xFF」可能只是 MISO 没接上/没共地，而不是模组没答")
        print("  · 若 MISO 从没被驱动（且输入通道自检通过）→ 先查：① 模组供电/共地 "
              "② 0R 电阻焊了没（原厂说明 R3/R18/R20/R21/R23/R25）")
        print("    ③ 模组此刻是否被 TF 卡座/Miniusb 那一路占用 ④ 换 CS2 / 换 3.75M 时钟再试")
        print("  · 若 CMD0/CMD5 有 R1 → 这条电气通路成立，再谈 SDIO 功能协议；"
              "此时也要**找原厂要主控驱动/主控开发指南**（我们手上没有主机侧实现）")


def main():
    ap = argparse.ArgumentParser(description="TX-AH 模组 SPI 电气探测（PC 侧 CH347F）")
    ap.add_argument("cmd", nargs="?", default="all",
                    choices=("all", "bus", "sweep", "sdspi", "raw", "misocheck"))
    ap.add_argument("--index", type=int, default=0, help="CH347F 设备序号（默认 0）")
    ap.add_argument("--mode", type=int, default=0, choices=(0, 1, 2, 3), help="SPI 模式")
    ap.add_argument("--hz", type=int, default=375_000, help="SPI 时钟 Hz（默认 375k，SD 初始化慢速）")
    ap.add_argument("--cs", type=int, default=1, choices=(1, 2), help="用 CH347F 的 CS1 还是 CS2")
    ap.add_argument("--level", default=None, choices=(None, "gnd", "vcc"),
                    help="misocheck：MISO 被人为接到哪一端（也可以直接写 `misocheck vcc`）")
    ap.add_argument("bytes_", nargs="*", help="raw 模式/ misocheck 的位置参数")
    args = ap.parse_args()
    if args.cmd == "misocheck":
        if args.bytes_:
            args.level = args.bytes_[0]
        args.level = args.level or "gnd"
        if args.level not in ("gnd", "vcc"):
            print("[!] misocheck 的档位只能是 gnd / vcc（收到 %r）" % args.level)
            return 2
    if args.hz not in SPI_SPEEDS_HZ:
        print("[i] 时钟 %d Hz 不是精确档位，DLL 会就近取 %d Hz" % (args.hz, nearest_hz(args.hz)))
    try:
        probe = Probe(args)
    except Ch347Error as exc:
        print("[!] %s" % exc)
        print("    先插上 CH347F-EVT 并确认驱动为 WCH 的（`python tools\\ch347_spi.py list`）；"
              "插好前可以先做 `python tools\\ch347_spi.py selftest`（MOSI↔MISO 短接）自己验桥。")
        return 2
    try:
        print("设备：%s  %s" % (CHIP_NAMES.get(probe.dev.chip_type(), "?"), probe.dev.version()))
        if args.cmd == "raw":
            probe.cmd_raw()
        elif args.cmd == "misocheck":
            probe.cmd_misocheck()
        else:
            if args.cmd in ("all", "bus"):
                probe.cmd_bus()
            if args.cmd in ("all", "sweep"):
                probe.cmd_sweep()
            if args.cmd in ("all", "sdspi"):
                probe.cmd_sdspi()
            if args.cmd == "all":
                probe.summary()
    finally:
        probe.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
