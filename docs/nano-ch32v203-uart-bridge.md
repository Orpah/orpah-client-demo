# nanoCH32V203 当 USB-UART 桥（b 步真机测试夹具，COM32）

> **2026-09-14 用户实测跑通并记录。** 用途：让 PC 通过 UART 接上 **TX-AH 模块的 AT 控制口**，
> 为 b 步（PC 当客户端 ↔ TX-AH 开发板）做第一轮真机连通。
> 实测环境：**Windows 11** + **WCHISPTool V3.3** + **MounRiver Studio V2.5.0**。
>
> 结论一句话：**nano 板刷上 WCH 官方的 `SimulateCDC` 固件后变成一个 USB 转串口桥，
> 设备管理器出现 `USB串口设备(COM32)`；SecureCRT 连 COM32 键 `AT+SSID?` 有正确回应
> ⇒ 这条「PC → USB CDC → 硬线 → TX-AH AT 口」已经通了。** 数据面（`AT+TXDATA`）仍未验证。

## 1. 为什么要在 nano 板上刷这个固件

- nanoCH32V203 的主控是 **CH32V203C8T6**，用的是**芯片内置的原生 USB**；板上
  **没有 CH340 之类的 USB-UART 桥芯片**（`nanoCH32V203.pdf` 第 2 页 BOM 里 USB 只有
  `Type_C_16P` ×2，没有任何串口桥 IC；板子自己的 `README_cn.md` 也只说“板载双 TYPE-C
  USB 接口”“可通过 USB 口下载烧录”，原文是 USB1 支持 Device、USB2 支持 Host/Device），
  出厂固件也不是 CDC 设备 → **Win11 下看不到任何 COM 口**（这跟“驱动没装”不是一回事）。
- 烧得进去是另一条路：**WCHISPTool 走芯片内置 bootloader**（BOOT+RST 进刷机态），
  不需要 COM 口就能刷。
- 于是刷 WCH 官方例程 **`SimulateCDC`**：它把 CH32V203 变成
  「USB CDC ↔ **USART2(PA2/PA3)**」的**双向透传桥** → 出现一个普通 COM 口（本次 = **COM32**），
  我们就可以拿它当 USB-TTL 用。

## 2. 刷机步骤（实测）

| # | 做什么 | 备注 |
|---|---|---|
| 1 | 下载 `CH32V20xEVT.ZIP`：<https://file.wch.cn/download/file?id=385> | WCH 官网下载页的直链 |
| 2 | 解压 | 本机解到 `D:\Downloads\CH32V20xEVT`（VS Code 工作区里已加入该目录） |
| 3 | 下载并安装 **MounRiver Studio V2.5.0**（`MounRiver_Studio_Setup_V2.5.0.zip`） | 装完自带 `riscv-none-elf-gcc` |
| 4 | 用 MounRiver 编译 `EVT\EXAM\USB\USBD\SimulateCDC` | 产物 = `…\SimulateCDC\obj\SimulateCDC.hex` |
| 5 | 板子进刷机态：**持续按住 BOOT → 按一下 RST 并松开 → 最后松开 BOOT**；WCHISPTool 里选 **CH32Vx / CH32V203 / 下载方式 USB**，选上面的 `SimulateCDC.hex` 刷入 | 刷机不需 COM 口；按键顺序照板子手册（`README_cn.md`） |
| 6 | 按 **RST** 重启 → 设备管理器出现 **USB串口设备(COM32)** | 之后用 SecureCRT 等串口工具连 COM32 |

**串口参数 = 115200 8N1。** 依据：
- CH32V 侧：`EVT\EXAM\USB\USBD\SimulateCDC\User\UART\UART.h` 的
  `#define DEF_UARTx_BAUDRATE 115200`（8 数据位/无校验/1 停止位为默认分支）；
- TX-AH 侧 AT 口：`T-Halow-RJ45/tools/thalow_config.py` 的 `BAUD = 115200`（AT 用新行模式）。

> 注：`EVT\EXAM\USB\` 下有 `USBFS\DEVICE\SimulateCDC` 与 `USBD\SimulateCDC` 两个版本，
> **这次用的是 `USBD\SimulateCDC`**。

## 3. 接线（PC ↔ TX-AH 模块的 AT 口）

| nanoCH32V203 | TX-AH 开发板 | 说明 |
|---|---|---|
| **nano A2**（= PA2 = **USART2_TX**） | **IOA13 = J4 Pin 3** | 过桥后就是 PC 的 TX → 模块的 RX 侧信号 |
| **nano A3**（= PA3 = **USART2_RX**） | **IOA12 = J4 Pin 2** | 模块的 TX → PC 的 RX |
| **nano 5V** | **J2 VCC** | 供电 |
| **nano G** | **J2 GND** | **共地（必须接）** |

- **PA2/PA3 = USART2** 的依据：`…\SimulateCDC\User\Main.c:15` 注释
  “Example routine to emulate a simulate USB-CDC Device, **USE USART2(PA2/PA3)**”；
  `…\User\UART\UART.c` 里 PA2 配成复用推挽（TX）、PA3 配置为浮空输入（RX）。
- ⚠ **别拿 USART2 干别的**：同一处注释明确写着 “if you need to modify the debugging serial port,
  please do not use USART2” —— 这个口被 CDC 桥占着；板上调试打印走 **USART1**。
- **对照 TX-AH 面包板丝印**：JU 区**下面那一排就是 J4**（面包板视图里从左到右是
  `GND / A13 / A12 / VCC`）→ 由此 pin2 = A12、pin3 = A13，也就是 **J4 那一排的 pin 编号是
  自右向左数**的（★ 这一条是从「上面两条接线」+「丝印」推出来的，下次接线照此；
  以你板上实际丝印为准）。J4 的 4 个焊盘画法见 `fritzing-parts-langhua` 的
  `svg/TX-AH-R900PNR`（2026-09-14 已改正：J5 = 上面两排 8 个焊盘、J4 = 下面一排 4 个）。

## 4. 验证（实测通过）

- SecureCRT 连 **COM32**，键入 **`AT+SSID?`** → **有正确回应**。
- 这说明：「PC ↔ nano（USB CDC）↔ 杜邦线 ↔ TX-AH（AT 控制面）」这条链路是通的，
  **AT 控制面可用**（配置/查询模块走这条路）。

## 5. 没做 / 注意（如实）

- **数据面仍未验证**：b 步真正要用的 `AT+TXDATA=<len>` + 裸以太帧（上行）/ `FRAME:RX <hex>`
  （下行）**还没在真机上试过**（三个待确认项见 `ROADMAP.md` §二；`halow-demo` 的真机实测与
  `T-Halow-RJ45` 手册在这点上是**矛盾**的）。下一步：
  `python client_sim.py --transport serial --serial-port COM32 --dump-lines 30 --cycles 1`
  —— 先看真板原样输出（`--dump-lines`），再决定命令写法/下行格式/数据模式粘性。
- **COM32 是 Windows 给这个 USB CDC 实例分配的号**：换 USB 口、换机器、重装固件都可能变，
  以设备管理器为准（别把 32 写进脚本常量）。
- **SimulateCDC 只是测试夹具**（把 nano 当 USB-UART 桥），**不是**我们的客户端固件；
  本仓自己的固件从 c 步开始，**尚未编写、未上机**。
- **nano 板 ≠ 我们的目标硬件**：目标是 CH32V203 + ATECC608B 的载板；这里只用 nano 的
  USB CDC + USART2 通路，与 SE、与低功耗取能都无关。

## 6. 资料位置（本机）

| 内容 | 路径 |
|---|---|
| nano 板资料（原理图/BOM、手册） | `F:\BaiduNetdiskDownload\nanoCH32V203\nanoCH32V203`（`hardware\nanoCH32V203.pdf`） |
| CH32V20xEVT（含 SimulateCDC 工程） | `D:\Downloads\CH32V20xEVT`（下载链接见 §2 第 1 步） |
| PC 侧 UART/AT 实现 + 排练脚本 | `orpah-over-halow/host_serial.py`、`demo_client_uart.py`、`client_sim.py --transport serial` |
| TX-AH 的 AT 命令文档 / 配置工具 | `T-Halow-RJ45/docs/AT_cmd.md`、`T-Halow-RJ45/tools/thalow_config.py` |
| 本仓 b 步判据与接线要求 | `ROADMAP.md` §二 |
