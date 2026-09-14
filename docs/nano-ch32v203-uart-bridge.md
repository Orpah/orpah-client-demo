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
| **nano 5V** | **J2 VCC** | 供电（J2 = 板上方那个 JST XH 4 针口，4 脚为 `GND/A31/A30/VCC`，取它的 VCC） |
| **nano G** | **J2 GND** | **共地（必须接）**（同样可从 J4 pin4 / J10 单针取 GND） |

- **PA2/PA3 = USART2** 的依据：`…\SimulateCDC\User\Main.c:15` 注释
  “Example routine to emulate a simulate USB-CDC Device, **USE USART2(PA2/PA3)**”；
  `…\User\UART\UART.c` 里 PA2 配成复用推挽（TX）、PA3 配置为浮空输入（RX）。
- ⚠ **别拿 USART2 干别的**：同一处注释明确写着 “if you need to modify the debugging serial port,
  please do not use USART2” —— 这个口被 CDC 桥占着；板上调试打印走 **USART1**。
- **J4 引脚定义（用户 2026-09-14 提供的 TX-AH「AH UART」原理图段，已确认）**：

  | J4 引脚 | 1 | 2 | 3 | 4 |
  |---|---|---|---|---|
  | 信号 | **VCC** | **IOA12** | **IOA13** | **GND** |

  ⇒ 本次接线用的就是 **pin2 / pin3**，**GND = J4 pin 4**（VCC = J4 pin 1）。
- **与板上丝印的关系**：J4 那一排在面包板视图里从左到右是 `GND / A13 / A12 / VCC`
  （= pin4 / pin3 / pin2 / pin1）⇒ **该排 pin 编号自右向左数**；
  同一段原理图里 **J5 = 2×4（`2X4-2.54MM`，8 脚）**、**J10 = 单针 GND**（本次不用）。
  J4/J5 的焊盘画法见 `fritzing-parts-langhua` 的 `svg/TX-AH-R900PNR`（2026-09-14 已改正：
  J5 = 上面两排 8 个焊盘、J4 = 下面一排 4 个）——与这张原理图的「4 脚 / 2×4」一致。

## 4. 验证（实测通过）

- SecureCRT 连 **COM32**，键入 **`AT+SSID?`** → **有正确回应**。
- 这说明：「PC ↔ nano（USB CDC）↔ 杜邦线 ↔ TX-AH（AT 控制面）」这条链路是通的，
  **AT 控制面可用**（配置/查询模块走这条路）。

## 5. 上机首测结果（2026-09-14，COM32 实测）

> 跑法：`pyserial` 直接开 COM32（115200 8N1）逐条发命令、把**原样字节**打出来（脚本只查 / 只读 +
> 一条测试帧，**没改任何持久配置**：没动 SSID / MODE / KEYMGMT / SAVE）。
> ⚠ **控制台是独占的**：SecureCRT 还连着时 `pyserial` 打开直接报
> `PermissionError(13, '拒绝访问')` —— 换到 Python 之前**必须先断开那个会话**（不用拔线）。

**模块身份**

| 查询 | 真机回应 |
|---|---|
| `AT+VERSION=?` / `AT+VERSION` | `+VERSION: v2.4.1.5-38247, app:0` + `OK` |
| `AT+MAC_ADDR=?` | `+MAC_ADDR:mac addr=4a-06-59-8d-74-40` + `OK` |
| `AT+SSID?` | `SSID: 测试链路` + `OK` |
| `AT+SYSDBG=LMAC,0` | `OK`（**而且真的把周期刷屏关掉了**） |
| `AT+SYSDBG=WNB,1` | `OK` |
| 裸 `AT` | **无应答**（这块固件不认） |

模块当时的状态（它自己每 ~6–18 s 打一屏 `LMAC STATUS` 遥测，下面是抄下来的）：
`local: 4a:06:59:8d:74:40`、**`mode=2`（AP）**、`freq=908.0 bw=8`、`chn: 908.0 / 916.0 / 924.0`、
`sta_list: no sta`、`vcc: 4.94V`、芯温 42–43℃。★ 这是**现场**（可能是之前配置留下的），不是我们的设定。

**★ 数据面：`AT+TXDATA` 在真机上不被支持（实测）**

四种写法**全部一句话都不回**（不是 `ERROR`，是静默丢弃）：

```
AT+TXDATA=29          → 0 字节
AT+TXDATA=29,8,0,0    → 0 字节
AT+TXDATA?            → 0 字节
AT+TXDATA=?           → 0 字节
```

结合版本号**第 4 位 = `5`** ⇒ 与 `halow-demo/simulator/AGENTS.md` 的 2026-09-07 真机结论
**完全一致**：**这块 TX-AH 是 fmac 固件，AT 只有控制面、没有 AT 级发数据命令**
（那份记录的原话：payload 需走**主机 SDIO/SPI(MACBUS)** 或换"网络版固件"）。

⇒ **所以 b 步原先定的"UART 数据面"在这块模块上走不通**：
`AT+TXDATA` / `FRAME:RX` 这套（`orpah-over-halow/host_serial.py`）在真机上无效 ——
离线排练（对模拟器的 AT 控制台）仍然有效，可以留着当"同一套 API 的另一条线"，但**不能当真实通路**。
**通路要重新定**（候选：① 模块主机接口 SPI/SDIO MACBUS + PC 侧 USB→SPI 桥；② 换固件；
③ 改用数据面走网口的那块板）——**这一条要用户拍板，见 `ROADMAP.md` §二。**

**其它两条有用的实测**

- **`resync()` 在真机上有效** ✓：喂 1700 字节 `0x55` 填充 + 再问 `AT+SSID?` → 正常应答
  （就算控制台被数据模式/噪声卡住也救得回来）。
- **LMAC 刷屏对我们的代码是个要求**：模块默认每 ~6–18 s 打一屏遥测，会把行解析器 / `--dump-lines`
  淹掉（实测第一轮 `AT` 的应答就是这么被淹的）→ 我们的 `SerialAtBus` **开机应先发
  `AT+SYSDBG=LMAC,0`**（《`T-Halow-RJ45/tools/thalow_config.py`》的 `quiet()` 同样做法）。
  ★ **尚未改代码**（通路定了再一起改，免得白改）。

## 6. 没做 / 注意（如实）

- **数据面：已实测为"这块固件不支持"**（见 §5）—— 不是"还没试"。b 步的通路**待重新决策**。
- **COM32 是 Windows 给这个 USB CDC 实例分配的号**：换 USB 口、换机器、重装固件都可能变，
  以设备管理器为准（别把 32 写进脚本常量）。
- **SimulateCDC 只是测试夹具**（把 nano 当 USB-UART 桥），**不是**我们的客户端固件；
  本仓自己的固件从 c 步开始，**尚未编写、未上机**。
- **nano 板 ≠ 我们的目标硬件**：目标是 CH32V203 + ATECC608B 的载板；这里只用 nano 的
  USB CDC + USART2 通路，与 SE、与低功耗取能都无关。

## 7. 资料位置（本机）

| 内容 | 路径 |
|---|---|
| nano 板资料（原理图/BOM、手册） | `F:\BaiduNetdiskDownload\nanoCH32V203\nanoCH32V203`（`hardware\nanoCH32V203.pdf`） |
| CH32V20xEVT（含 SimulateCDC 工程） | `D:\Downloads\CH32V20xEVT`（下载链接见 §2 第 1 步） |
| PC 侧 UART/AT 实现 + 排练脚本 | `orpah-over-halow/host_serial.py`、`demo_client_uart.py`、`client_sim.py --transport serial` |
| TX-AH 的 AT 命令文档 / 配置工具 | `T-Halow-RJ45/docs/AT_cmd.md`、`T-Halow-RJ45/tools/thalow_config.py` |
| 本仓 b 步判据与接线要求 | `ROADMAP.md` §二 |
