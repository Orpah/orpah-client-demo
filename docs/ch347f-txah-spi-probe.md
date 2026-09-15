# PC → CH347F-EVT → TX-AH：SPI 电气探测（b 步候选通路 ①）

> 2026-09-16 起。目的：**先证明这条电气通路存不存在**，再谈上层协议。
> 上位机工具：`tools/ch347_spi.py`（CH347/CH347F 的 PC 侧访问）、
> `tools/probe_txah_spi.py`（探测脚本）。

## 一、为什么先做"电气探测"，而不是直接发数据

把模组 SDK 源码（`f:\git\halow-demo\TXW8301\FMAC_SDK` → junction 到
`F:\git\tianlu\TXW8301\TX_AH_SDK_2.4_...\TXW8301_FMAC-v2.4.1.5-39777`）翻过之后，
**主机口这件事只有三种实现**，没有"SPI 主机口"这个实现：

| 事实 | 出处 |
|---|---|
| `mac_bus` 只实现了 **SDIO / UART / USB**（源码只有 `sdio_bus.c`、`uart_bus.c`、`usb_bus.c`） | `sdk/lib/bus/macbus/` |
| `mac_bus_spi_attach` / `mac_bus_sdspi_attach` 只有**声明**，SDK 里没有实现 | `sdk/include/lib/bus/macbus/mac_bus.h:52-53` |
| 默认固件就是 SDIO | `project/project_config.h:3` → `#define MACBUS_SDIO`（USB/UART 被注释） |
| 「**SPI 接口和 SDIO 接口是同一固件**」 | `TX_AH_SDK_2.4/changelog.txt`（说明段） |
| 现存所有固件 bin 都是 SDIO 版（含 `mac_bus_sdio_attach`，都没有 `uart bus fixlen` 串） | 扫 `FMAC_SDK/project/*.bin`、`out/FMAC/*` |
| 「目前 2.x 版本**不支持**网桥+串口透传」 | 同上 changelog（`wnb-uartp2p` 是 1.6 时代的东西） |
| 主机口线上协议 = 泰芯 **HGIC** 帧：`magic(0x1A2B 主机→模组 / 0x2B1A 模组→主机) + type + ifidx/flags + length + cookie`（8 B 头）+ 载荷；UART 用 magic+帧长定帧 | `sdk/include/lib/lmac/hgic.h`、`uart_bus.c` |

⇒ 结论：**"SPI 主机口"走的是 SDIO 控制器的 SPI 模式**，线上是 **SD/SDIO-over-SPI** 协议，
需要**厂商主控驱动**（我们手上没有主机侧实现）。
所以本轮判据不是"我们的 AA55 帧通不通"，而是 **SD-SPI 的 CMD0 / CMD5 / CMD8 有没有合法 R1**。

## 二、硬件与接线（模组开发板 V1.6）

模组的 SPI 与 SDIO **共用管脚**（模组规格书接口表 / 《泰芯AH模组开发板使用说明》2 节）：

| 模组侧丝印 | 模组脚 | CH347F-EVT | 备注 |
|---|---|---|---|
| `SD_CLK` / `SPI_CLK1` | IOA6 | SPI **SCK** | 原厂说明：需把 **R18/R20/R21/R23/R3/R25 焊 0R**（SPI 飞线） |
| `SD_CMD` / `SPI_MOSI1` | IOA7 | SPI **MOSI** | 同上 |
| `SD_D0` / `SPI_MISO1` | IOA8 | SPI **MISO** | 同上 |
| `SD_D1` / `SPI_INTIO1` | IOA9 | （本轮不接；可接 CH347 GPIO 观测中断） | |
| `SD_D3` / `CS1` | IOA11 | SPI **CS1** | 同上 |
| GND | GND | GND | **必须共地** |

- 供电：模组 VCC **3.1~3.3 V（别接 5 V）**；开发板自带供电时按说明选 `VCC`/`SVCC` 跳帽。
- ⚠ 该组脚与 **TF 卡座**共用 —— 探测前先确认没有别的东西在驱动这几根线。
- ⚠ 本脚本只发探测字节（CMD0/CMD5/CMD8/CMD58/CMD55 都是只读或"回 idle"的标准命令），
  **不写模组、不动 flash、不烧固件**。

## 三、怎么跑

```powershell
cd F:\git\orpah-client-demo
# 0) 先看桥在不在、能不能当 SPI 主机
python tools\ch347_spi.py list                 # index 0 应为 CH347F + "SPI 可用 ✓"
# 1) 桥自检：MOSI↔MISO 短接
python tools\ch347_spi.py selftest             # 应 "自环通过 ✓"
# 2) MISO 输入通道自检：把 CH347F 的 MISO 短到 GND，再短到 3V3，各跑一次
python tools\probe_txah_spi.py misocheck gnd   # 应读到 全 0x00 ← **这一档才是判据**
python tools\probe_txah_spi.py misocheck vcc   # 应读到 全 0xFF（悬空也会通过，只排除极端情况）
# 3) 接上模组后：跑全套（MISO 观测 + 模式×时钟矩阵 + SD/SDIO-SPI 命令）
python tools\probe_txah_spi.py
```

## 四、2026-09-16 实测记录（PC 侧）

- **CH347F-EVT 在机上、驱动是 WCH 的**：`CH347GetChipType=2`（CH347F），驱动 2.05 / DLL 2.02 /
  设备 2.00，`CH347SPI_Init` 成功（`python tools\ch347_spi.py list` → `SPI 可用 ✓`）。
- 顺手记两条**DLL 的行为**（踩过，写进 `tools/ch347_spi.py` 注释）：
  1. **没插任何 WCH 设备时** `CH347OpenDevice(0..3)` 也可能返回句柄（芯片类型读成 0=CH341）
     → 判"设备在不在"必须用 `CH347SPI_Init` 是否成功。
  2. `auto_deassert=1` 时**最后一个字节容易读成 0x00** → 读响应一律 `auto_deassert=0` + 手动 CS。
- 模组侧（**当时还没接线/没共地**）：`bus` / `sweep` / `sdspi` 全部读回**全 0xFF**，
  SD-SPI 的 CMD0/CMD5/CMD8 **都没有合法 R1**。
  ⚠ **但这不能算"模组没答"** —— `misocheck gnd` 也是全 0xFF（MISO 悬空即读 0xFF），
  说明当时连"这条输入线活不活"都没证明。**先接线 + 先把 misocheck 跑过，再谈探测结论。**

## 五、判据与下一步

- **判据（电气层）**：`misocheck` 两个方向都对 → 桥这一侧可信；
  然后看 `sdspi`：**CMD0 → R1=0x01** 或 **CMD5 → 合法 R1** ⇒ 模组的 SDIO 控制器在 SPI 模式下有响应。
- 有响应 ⇒ 下一步是 **SDIO 功能协议**（CMD52/CMD53 读 CCCR/FBR），这需要**主机侧寄存器用法**，
  我们手上没有 ⇒ **要找原厂要《主控驱动开发指南》/ Linux 驱动源码**。
- 没响应（且电气/输入通道都验过）⇒ 依次排查：模组供电与共地 → 0R 电阻是否焊上 →
  该组脚是否被 TF 卡座占用 → 换 CS2 / 换 3.75 MHz 再试 → 再问原厂"SPI 模式要不要固件/跳线配合"。

## 六、未做（如实）

- 没接线、没共地、没接模组：**SPI 通路是否可用，本次没有结论**。
- `CH347SPI_SetDataBits`（CH347F 的 16 bit 模式）、`CH347GPIO_*`（观测 INTIO）都没用过。
- **UART 路线**（`MACBUS_UART`）的固件改动与 PC 侧 HGIC 驱动：本轮未做，见 `ROADMAP.md` §二。
