#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
ch347_spi.py — 从 PC 访问 WCH **CH347 / CH347F / CH339W**（USB→SPI/I2C/JTAG/UART 桥）。

用途（`orpah-client-demo` 的 b 步真机夹具）：
  给「PC → CH347F-EVT → TX-AH 模组」这条通路提供 PC 侧最底层的收发能力
  （SPI 主机 + UART），供 `probe_txah_spi.py` 等联调脚本调用。

依赖（都是**本机已有**，不是仓库内文件）：
  * 驱动 + DLL：WCH CH347 驱动包里的 `CH347DLLA64.DLL`（本机在 `C:\Windows\System32\`，
    32 位版本是 `C:\Windows\SysWOW64\CH347DLL.DLL`）。设备管理器中设备应由 WCH 驱动接管；
    若曾用 Zadig 换成 libusb（2026-03 那次给模拟器做 SPI 时换过），本模块会 open 失败 ——
    想用 DLL 就得把驱动换回 WCH 的，想用 libusb 则另写实现（本模块不做）。
  * 头文件依据：`CH347EVT\EVT\TOOLS\CH347Demo\ExternalLib\CH347DLL_EN.H`（V1.5）——
    本文件里的结构体/函数签名逐条照着它写，不猜。

已知边界（如实）：
  * **本机实测（2026-09-16）**：CH347F-EVT 插上后 `index 0` 是 CH347F（`CH347GetChipType=2`，
    驱动 2.05 / 设备 2.00），SPI 可用；`auto_deassert=1` 时传输**最后一个字节容易读成 0x00**
    → 要读从机响应请用 `auto_deassert=0` + 手动 `cs()`（`selftest` / `probe_txah_spi.py` 都这么做）。
  * **没插任何 WCH 设备时** `CH347OpenDevice(0..3)` 也可能返回句柄（芯片类型读成 0=CH341）
    → 判"设备在不在"要用 `spi_usable()`（= `CH347SPI_Init` 成功），别只看 open 的返回值。
  * `CH347GetDeviceInfor` 的 `mDeviceInforS` 里 `DevicePath[MAX_PATH]` 依赖 `windows.h` 的
    `MAX_PATH`（本机取 260），**没确认过**，所以本模块不解析它；设备身份走
    `CH347GetChipType` / `CH347GetVersion` / `CH347SPI_GetCfg`（这些是确定的）。
  * `CH347GetVersion` 按头文件里 4 个单字节 BCD 读（形参名 `iDriverVer`/`iDLLVer`/`ibcdDevice`/
    `iChipType`）—— 实测打出来的数看着合理（2.05/2.02/2.00/CH347F），但**是推断的读法**。
  * CH347F 的 SPI 是**主机侧**（master only）：不能当 SPI 从机。
  * SPI 时钟档位见 `SPI_SPEEDS_HZ`（218.75 kHz ~ 60 MHz）；给了非精确值 DLL 会就近取档
    （头文件明说），本模块只**提示**不报错。

命令行：
  python tools\ch347_spi.py list              # 枚举 CH347 设备（0..3）
  python tools\ch347_spi.py selftest          # 自环：MOSI↔MISO 短接时应读回原样
  python tools\ch347_spi.py xfer DEADBEEF     # 原样发十六进制串并打印回读
  python tools\ch347_spi.py cfg               # 读回 SPI 控制器配置
"""
import argparse
import ctypes
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DLL_PATHS = [
    r"C:\Windows\System32\CH347DLLA64.DLL",
    r"C:\Windows\SysWOW64\CH347DLL.DLL",
]

# CH347GetChipType 返回值
CHIP_TYPE_CH341 = 0
CHIP_TYPE_CH347 = 1
CHIP_TYPE_CH347F = 2
CHIP_TYPE_CH339W = 3
CHIP_NAMES = {0: "CH341", 1: "CH347/CH347T", 2: "CH347F", 3: "CH339W"}

# 头文件 §SetFrequency 列出的档位（没有对应档就取最近的）
SPI_SPEEDS_HZ = [
    60_000_000, 48_000_000, 36_000_000, 30_000_000, 28_000_000, 24_000_000, 18_000_000,
    15_000_000, 14_000_000, 12_000_000, 9_000_000, 7_500_000, 7_000_000, 6_000_000,
    4_500_000, 3_750_000, 3_500_000, 3_000_000, 2_250_000, 1_875_000, 1_750_000,
    1_500_000, 1_125_000, 937_500, 875_000, 750_000, 562_500, 468_750, 437_500,
    375_000, 281_250, 218_750,
]


class Ch347Error(RuntimeError):
    pass


def nearest_hz(hz):
    """CH347 没有对应档位时会自己取最近的（头文件 §SetFrequency），这里只是**告知**用。"""
    return min(SPI_SPEEDS_HZ, key=lambda x: abs(x - hz))


class M_SPI_CFG(ctypes.Structure):
    """对应头文件 `struct _SPI_CONFIG`（`#pragma pack(1)`，共 20 字节）。"""
    _pack_ = 1
    _fields_ = [
        ("iMode", ctypes.c_ubyte),                  # 0-3 = SPI Mode0/1/2/3
        ("iClock", ctypes.c_ubyte),                 # 0=60M,1=30M,2=15M,3=7.5M,4=3.75M,5=1.875M,6=937.5K,7=468.75K
        ("iByteOrder", ctypes.c_ubyte),             # 0=LSB first, 1=MSB first
        ("iSpiWriteReadInterval", ctypes.c_ushort),  # 读写间隔，单位 uS
        ("iSpiOutDefaultData", ctypes.c_ubyte),     # 读数据时 MOSI 的默认输出字节
        ("iChipSelect", ctypes.c_ulong),            # bit7=0 忽略片选；bit7=1 有效，bit1:0 = 00/01 → CS1/CS2
        ("CS1Polarity", ctypes.c_ubyte),            # bit0: 0=低有效, 1=高有效
        ("CS2Polarity", ctypes.c_ubyte),
        ("iIsAutoDeativeCS", ctypes.c_ushort),      # 操作完是否自动撤销片选
        ("iActiveDelay", ctypes.c_ushort),          # 片选后读写延时，uS
        ("iDelayDeactive", ctypes.c_ulong),         # 撤片选后延时，uS
    ]


class Ch347:
    """CH347 设备句柄。SPI/UART 都挂在同一个 `iIndex`（0..3）上。"""

    def __init__(self, index=0, dll_path=None):
        self.index = index
        self._opened = False
        self._uart_opened = False
        path = dll_path or self._find_dll()
        try:
            self.dll = ctypes.WinDLL(path)
        except OSError as exc:  # 缺 DLL / 位数不对
            raise Ch347Error("加载 %s 失败：%s（先装 WCH CH347 驱动包）" % (path, exc))
        self.dll_path = path
        self._bind()

    @staticmethod
    def _find_dll():
        for p in DLL_PATHS:
            if os.path.exists(p):
                return p
        raise Ch347Error("找不到 CH347DLLA64.DLL / CH347DLL.DLL（本机未装 WCH CH347 驱动包）")

    def _bind(self):
        d = self.dll
        d.CH347OpenDevice.restype = ctypes.c_void_p
        d.CH347OpenDevice.argtypes = [ctypes.c_ulong]
        d.CH347CloseDevice.restype = ctypes.c_int
        d.CH347CloseDevice.argtypes = [ctypes.c_ulong]
        d.CH347GetChipType.restype = ctypes.c_ubyte
        d.CH347GetChipType.argtypes = [ctypes.c_ulong]
        d.CH347GetVersion.restype = ctypes.c_int
        d.CH347GetVersion.argtypes = [ctypes.c_ulong] + [ctypes.POINTER(ctypes.c_ubyte)] * 4
        d.CH347SetTimeout.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
        # ---- SPI ----
        d.CH347SPI_Init.restype = ctypes.c_int
        d.CH347SPI_Init.argtypes = [ctypes.c_ulong, ctypes.POINTER(M_SPI_CFG)]
        d.CH347SPI_GetCfg.restype = ctypes.c_int
        d.CH347SPI_GetCfg.argtypes = [ctypes.c_ulong, ctypes.POINTER(M_SPI_CFG)]
        d.CH347SPI_SetFrequency.restype = ctypes.c_int
        d.CH347SPI_SetFrequency.argtypes = [ctypes.c_ulong, ctypes.c_ulong]
        d.CH347SPI_SetDataBits.restype = ctypes.c_int
        d.CH347SPI_SetDataBits.argtypes = [ctypes.c_ulong, ctypes.c_ubyte]
        d.CH347SPI_ChangeCS.restype = ctypes.c_int
        d.CH347SPI_ChangeCS.argtypes = [ctypes.c_ulong, ctypes.c_ubyte]
        d.CH347SPI_WriteRead.restype = ctypes.c_int
        d.CH347SPI_WriteRead.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                         ctypes.c_void_p]
        d.CH347SPI_Write.restype = ctypes.c_int
        d.CH347SPI_Write.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                     ctypes.c_ulong, ctypes.c_void_p]
        # ---- UART（留给后续 UART 路线用；本轮不测）----
        d.CH347Uart_Open.restype = ctypes.c_void_p
        d.CH347Uart_Open.argtypes = [ctypes.c_ulong]
        d.CH347Uart_Close.argtypes = [ctypes.c_ulong]
        d.CH347Uart_Init.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ubyte,
                                     ctypes.c_ubyte, ctypes.c_ubyte, ctypes.c_ubyte]
        d.CH347Uart_SetTimeout.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
        d.CH347Uart_Write.argtypes = [ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.POINTER(ctypes.c_ulong)]
        d.CH347Uart_Read.argtypes = [ctypes.c_ulong, ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_ulong)]
        d.CH347Uart_QueryBufUpload.argtypes = [ctypes.c_ulong,
                                               ctypes.POINTER(ctypes.c_longlong)]

    # ------------------------------------------------------------------ 设备
    def open(self, write_timeout=1000, read_timeout=1000):
        h = self.dll.CH347OpenDevice(self.index)
        if not h:
            raise Ch347Error(
                "CH347OpenDevice(%d) 失败：设备没插 / 驱动不是 WCH 的（被 Zadig 换成 libusb "
                "的话 DLL 用不了）/ 该序号无设备" % self.index)
        self._opened = True
        self.dll.CH347SetTimeout(self.index, write_timeout, read_timeout)
        return self

    def close(self):
        if self._uart_opened:
            self.dll.CH347Uart_Close(self.index)
            self._uart_opened = False
        if self._opened:
            self.dll.CH347CloseDevice(self.index)
            self._opened = False

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()

    def chip_type(self):
        return int(self.dll.CH347GetChipType(self.index))

    def spi_usable(self):
        """能不能真当 SPI 主机用：`CH347SPI_Init` 成功才算。

        ⚠ 实测（2026-09-16，本机**没插任何 WCH 设备**时）：`CH347OpenDevice(0..3)`
        **照样返回句柄**、`CH347GetChipType` 返回 0(CH341)，只有 `CH347SPI_Init` 会失败。
        所以**不能用 open 的返回值判断"设备在不在"**，要用这里。
        """
        try:
            self.spi_init(mode=0, hz=375_000, cs=1, auto_deassert=1)
            return True
        except Ch347Error:
            return False

    def version(self):
        """驱动/DLL/设备版本 + 芯片类型。

        头文件里是 4 个 `PUCHAR`（形参名 `iDriverVer` / `iDLLVer` / `ibcdDevice` / `iChipType`，
        都是单数）→ 按**单字节 BCD** 读，最后一个是 `CHIP_TYPE_*`。
        ⚠ 按命名推断，**没有上机验证过**；若打印出明显离谱的数字，改这里。
        """
        v = [ctypes.c_ubyte(0) for _ in range(4)]
        if not self.dll.CH347GetVersion(self.index, *(ctypes.byref(x) for x in v)):
            raise Ch347Error("CH347GetVersion 失败")
        ver = [int(x.value) for x in v]
        bcd = lambda b: "%d.%02d" % (b >> 4, b & 0x0F)      # noqa: E731
        return {"驱动版本": bcd(ver[0]), "DLL版本": bcd(ver[1]), "设备版本": bcd(ver[2]),
                "芯片类型": CHIP_NAMES.get(ver[3], "未知(%d)" % ver[3])}

    # -------------------------------------------------------------------- SPI
    def spi_init(self, mode=0, hz=1_000_000, msb_first=True, cs=1, cs_low=True,
                 auto_deassert=1, default_out=0xFF, interval_us=0,
                 active_delay_us=0, deactive_delay_us=0):
        """按 Hz 设时钟 → 配结构体 → CH347SPI_Init（顺序照头文件：SetFrequency 后要 Init）。

        `hz` 不必是 `SPI_SPEEDS_HZ` 里的精确值 —— DLL 会就近取档（头文件明说）。
        """
        if mode not in (0, 1, 2, 3):
            raise Ch347Error("SPI mode 只能 0..3")
        if not self.dll.CH347SPI_SetFrequency(self.index, hz):
            raise Ch347Error("CH347SPI_SetFrequency(%d) 失败" % hz)
        cfg = M_SPI_CFG()
        cfg.iMode = mode
        cfg.iClock = 0
        cfg.iByteOrder = 1 if msb_first else 0
        cfg.iSpiWriteReadInterval = interval_us
        cfg.iSpiOutDefaultData = default_out
        cfg.iChipSelect = (0x80 | (0x01 if cs == 2 else 0x00)) if cs in (1, 2) else 0x00
        cfg.CS1Polarity = 0 if cs_low else 1
        cfg.CS2Polarity = 0 if cs_low else 1
        cfg.iIsAutoDeativeCS = auto_deassert
        cfg.iActiveDelay = active_delay_us
        cfg.iDelayDeactive = deactive_delay_us
        if not self.dll.CH347SPI_Init(self.index, ctypes.byref(cfg)):
            raise Ch347Error("CH347SPI_Init 失败（mode=%d cs=%s）" % (mode, cs))
        return cfg

    def spi_cfg(self):
        cfg = M_SPI_CFG()
        if not self.dll.CH347SPI_GetCfg(self.index, ctypes.byref(cfg)):
            raise Ch347Error("CH347SPI_GetCfg 失败")
        return cfg

    def cs(self, assert_low=True):
        """手动拉片选：True=选中（拉低），False=释放。"""
        if not self.dll.CH347SPI_ChangeCS(self.index, 1 if assert_low else 0):
            raise Ch347Error("CH347SPI_ChangeCS(%s) 失败" % assert_low)

    def xfer(self, data, cs=1):
        """一次事务收发；cs=None → 不动片选（用于 CS 保持高/低时打时钟）。

        cs=1/2 → 用 CS1/CS2（bit7 置 1，交给硬件按 Init 的配置处理）；
        cs=None → bit7=0，忽略片选控制，只在 CS 当前电平下发时钟。
        """
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        csel = (0x80 | (0x01 if cs == 2 else 0x00)) if cs in (1, 2) else 0x00
        if not self.dll.CH347SPI_WriteRead(self.index, csel, len(data), buf):
            raise Ch347Error("CH347SPI_WriteRead(len=%d, cs=%s) 失败" % (len(data), cs))
        return bytes(buf.raw[:len(data)])

    def clock_idle(self, nbytes=1, fill=0xFF):
        """CS 电平不动，只打 nbytes 个字节的时钟（SD-SPI 上电/收尾要用）。"""
        return self.xfer(bytes([fill]) * nbytes, cs=None)

    # ------------------------------------------------------------------- UART
    def uart_open(self, baud=115200, databits=8, parity=0, stopbits=0, byte_timeout=0,
                  write_timeout=1000, read_timeout=1000):
        if not self.dll.CH347Uart_Open(self.index):
            raise Ch347Error("CH347Uart_Open(%d) 失败" % self.index)
        self._uart_opened = True
        if not self.dll.CH347Uart_Init(self.index, baud, databits, parity, stopbits,
                                       byte_timeout):
            raise Ch347Error("CH347Uart_Init(baud=%d) 失败" % baud)
        self.dll.CH347Uart_SetTimeout(self.index, write_timeout, read_timeout)
        return self

    def uart_write(self, data):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        n = ctypes.c_ulong(len(data))
        if not self.dll.CH347Uart_Write(self.index, buf, ctypes.byref(n)):
            raise Ch347Error("CH347Uart_Write 失败")
        return n.value

    def uart_read(self, want=256):
        buf = ctypes.create_string_buffer(want)
        n = ctypes.c_ulong(want)
        if not self.dll.CH347Uart_Read(self.index, buf, ctypes.byref(n)):
            return b""
        return bytes(buf.raw[:n.value])

    def uart_pending(self):
        n = ctypes.c_longlong(0)
        if not self.dll.CH347Uart_QueryBufUpload(self.index, ctypes.byref(n)):
            return -1
        return n.value


# --------------------------------------------------------------------------- CLI
def _open(index):
    dev = Ch347(index)
    dev.open()
    return dev


def cmd_list(args):
    found = 0
    for i in range(args.count):
        try:
            dev = _open(i)
        except Ch347Error as exc:
            print("index %d: %s" % (i, exc))
            continue
        try:
            t = dev.chip_type()
            usable = dev.spi_usable()
            print("index %d: CH347GetChipType=%d(%s) %s  SPI %s"
                  % (i, t, CHIP_NAMES.get(t, "未知"), dev.version(),
                     "可用 ✓" if usable else "不可用 ✗（不是 CH347F，或驱动没绑到 WCH）"))
            if usable:
                found += 1
        finally:
            dev.close()
    print("-" * 70)
    print("可用（能当 SPI 主机）的设备：%d 个；用 DLL：%s" % (found, Ch347._find_dll()))
    if not found:
        print("[!] 没找到可用的 CH347F。两个常见原因：")
        print("    ① 板子没插（注意：**没设备时 CH347OpenDevice 也可能返回句柄**，别只看它）")
        print("    ② 驱动不是 WCH 的 —— 2026-03 曾用 Zadig 给 CH347F 装过 libusb-win32，")
        print("       那种状态下这个 DLL 用不了；要先把驱动换回 WCH 的（设备管理器更新驱动）")
    return 0 if found else 1


def cmd_selftest(args):
    """自环：把 MOSI 与 MISO 短接（或用跳线帽），读回应等于发出去的。

    ⚠ 实测（2026-09-16）：用 `auto_deassert=1` 时**最后一个字节容易读成 0x00**
    （DLL 在传输末尾撤销片选的时序导致）→ 自环/读响应一律用 `auto_deassert=0` + 手动 CS。
    """
    dev = _open(args.index)
    try:
        print("设备：%s  %s" % (CHIP_NAMES.get(dev.chip_type(), "?"), dev.version()))
        dev.spi_init(mode=args.mode, hz=args.hz, cs=1, auto_deassert=0)
        pat = bytes([0x5A, 0xA5, 0x00, 0xFF, 0x01, 0x80])
        print("自环（要求 MOSI↔MISO 短接）：发 %s" % pat.hex(" "))
        dev.cs(assert_low=True)
        got = dev.xfer(pat, cs=None)
        dev.cs(assert_low=False)
        print("                       读 %s" % got.hex(" "))
        ok = got == pat
        print("=> %s" % ("自环通过 ✓" if ok else "自环失败 ✗（MOSI/MISO 没短接，或桥/驱动/接线有问题）"))
        return 0 if ok else 2
    finally:
        dev.close()


def cmd_cfg(args):
    dev = _open(args.index)
    try:
        dev.spi_init(mode=args.mode, hz=args.hz)
        c = dev.spi_cfg()
        print("芯片 %s" % CHIP_NAMES.get(dev.chip_type(), "?"))
        print("SPI 配置：mode=%d byteOrder=%d chipSelect=0x%02X CS1pol=%d CS2pol=%d "
              "autoDeassert=%d activeDelay=%dus deactiveDelay=%dus defaultOut=0x%02X"
              % (c.iMode, c.iByteOrder, c.iChipSelect, c.CS1Polarity, c.CS2Polarity,
                 c.iIsAutoDeativeCS, c.iActiveDelay, c.iDelayDeactive, c.iSpiOutDefaultData))
    finally:
        dev.close()


def cmd_xfer(args):
    data = bytes.fromhex(args.hex.replace(" ", "").replace(",", ""))
    dev = _open(args.index)
    try:
        dev.spi_init(mode=args.mode, hz=args.hz, cs=args.cs, auto_deassert=1)
        print("发 %s" % data.hex(" "))
        got = dev.xfer(data, cs=args.cs)
        print("收 %s" % got.hex(" "))
    finally:
        dev.close()


def main():
    ap = argparse.ArgumentParser(description="CH347/CH347F PC 侧访问（SPI + UART）")
    ap.add_argument("--index", type=int, default=0, help="设备序号，默认 0")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="枚举设备")
    p.add_argument("--count", type=int, default=4)
    p.set_defaults(func=cmd_list)
    for name, func, helptext in (("selftest", cmd_selftest, "自环测试（MOSI↔MISO 短接）"),
                                 ("cfg", cmd_cfg, "读回 SPI 配置"),
                                 ("xfer", cmd_xfer, "发一串十六进制并打印回读")):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("mode", nargs="?", type=int, default=0, help="SPI 模式 0..3")
        p.add_argument("hz", nargs="?", type=int, default=1_000_000, help="SPI 时钟 Hz")
        p.add_argument("--cs", type=int, default=1, choices=(1, 2))
        if name == "xfer":
            p.add_argument("hex")
        p.set_defaults(func=func)
    args = ap.parse_args()
    try:
        hz = getattr(args, "hz", None)
        if hz is not None and hz not in SPI_SPEEDS_HZ:
            print("[i] 时钟 %d Hz 不是 CH347 的精确档位，DLL 会就近取 %d Hz"
                  % (hz, nearest_hz(hz)))
        return args.func(args)
    except Ch347Error as exc:
        print("[!] %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
