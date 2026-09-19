# orpah-client-demo 路线图（真机三步走）

> 2026-09-14 用户定的路线：**先在软件里把两端都跑成"设备"，再一步一步换掉底层**
> （每步只换一样东西，前一步的判据继续成立）。
> 本仓是**客户端硬件 + 固件**；软件侧（仿真器/业务链/协议口径）在其它四仓，见 `README.md`。

## 〇、两条线、各五步

| 步 | 客户端（本仓） | 路由器（`orpah-openwrt-demo` / 本仓软件侧） |
|---|---|---|
| **a** | 软件里的**客户端设备仿真器** — ✅ **已完成**（在 `orpah-over-halow`：`client_sim.py` + `demo_client_sim.py` + `docs/client_sim.md`） | 软件里的**路由器仿真器** — ✅ 早已有（`orpah-over-halow/router.py` + `demo_l4.py`） |
| **b** | PC（当客户端）经 Type-C 接 **TX-AH 开发板** | PC（当路由器）经 Type-C 接 TX-AH 开发板 |
| **c** | 淘宝现成 **CH32 开发板 + ATECC608** 经 Type-C 接 TX-AH 板 | `orpah-openwrt-demo` 的路由器软件经 Type-C 接 TX-AH 板 |
| **d** | **定制 CH32 + ATECC608 载板**经 Type-C 接 TX-AH 板 | 改造现有 OpenWrt 路由器经 Type-C 接 TX-AH 板 |
| **e** | **定制 CH32 + ATECC608 + TX-AH 一体板** | 定制带 TX-AH 模块的完整路由器板 |

**每步的判据是同一套**（软件版现在就能跑，见 `orpah-over-halow/docs/client_sim.md` §2.3）：

1. 设备能**周期**发出 `REQ-CONNECT` / `REPORT`（设计常态 60 s 一次连接）；
2. **服务端收到并验签通过**已签 `ORPAH-ID-REPORT`（不是"我们自认为签了"）；
3. 下行 `ACCESS-INFO` / `TRACKING-STATUS` **真到达设备**；
4. 上游两侧**零丢弃**（守规矩的设备不该被限频误伤）。

## 一、本仓在各步要做什么

| 步 | 本仓交付物 |
|---|---|
| a | 无（软件侧已完成）；本仓只**记下判据与接口**：host 数据口帧格式 = `orpah-over-halow/host_bus.py`（SPI MACBUS `DATA_TX`/`DATA_RX` 语义）、协议 = `Protocol/docs/OrpahIDProtocol.md` |
| b | **PC 侧 UART/AT 数据面** —— ✅ 已实现（在 `orpah-over-halow`：`host_serial.SerialAtBus` =
  `AT+TXDATA` 上行 + `FRAME:RX` 下行；纯 PC 排练 `demo_client_uart.py`，判据双向都断言过）。
  ⚠ **真机未验证**：上机首测要确认三件事（命令写法 / 下行格式 / 数据模式粘性）。 |
| c | 第一版**固件**：CH32V203 裸机主循环 + host 数据口（SPI 从机/主机关系待定）+ §8.2 选级 + 无 RTC（`ts=0`）+ `cap.rtc` 声明；ATECC608B 先用**软件 P-256** 顶（SE 驱动在 d） |
| d | 换上**定制载板** + 真实 **ATECC608B 驱动**（Slot 0 私钥不可导出 / Slot 5 HMAC）+ 产线烧录流程（协议 §6.2） |
| e | 一体板：+ 低功耗/取能（能量轴实测标定，SPEC F-11）、结构/天线、Gerber/PNP 交付 |

## 二、b 步：**PC ↔ TX-AH 板走 UART**（用户 2026-09-14 定）

线上协议（依据：`T-Halow-RJ45/docs/AT_cmd.md` §`AT+TXDATA`、`halow-demo` 的模拟器固件与
`tools/ui/server.py`、`T-Halow-RJ45/tools/thalow_config.py`）：

| 方向 | 做什么 |
|---|---|
| 上行（DATA_TX） | `AT+TXDATA=<len>` → 等 `OK` → 写**裸以太网帧**（含 14B 以太头；`len` 含它） |
| 下行（DATA_RX） | `FRAME:RX <hex>` 行（先 `AT+SYSDBG=WNB,1` 打开帧打印） |

- **PC 侧实现已完成**：`orpah-over-halow/host_serial.py`（`SerialAtBus`）+ `client_sim.py --transport serial`。
- **纯 PC 排练已完成**：`orpah-over-halow/demo_client_uart.py` —— 拿**模拟器的 AT 控制台**跑同一套
  `AT+TXDATA`/`FRAME:RX`，双向验收（上行过空口 / 下行收到 / 连发不串）。
- **本仓在 b 步的交付物 = 无代码**（固件从 c 步开始）；本仓只承接判据与接线/供电说明。
- ⚠ **与真机的差异（上机首测必做，未做）**：① `AT+TXDATA=<len>` 的写法（等号？要不要带
  `txbw,mcs,priority`？）；② 下行是否真是 `FRAME:RX <hex>` —— **仓里两份记录不一致**
  （`halow-demo` 真机实测说 TX-AH fmac 的 AT 无用户数据命令）→ 用 `--dump-lines` 看真板原样输出；
  ③ 数据模式的粘性与恢复（`resync()` 照抄 `thalow_config.py`）。

### b 步真机夹具已就绪（用户 2026-09-14 实测，细节见 `docs/nano-ch32v203-uart-bridge.md`）

- **PC 这头的“串口”是拿 nanoCH32V203 板当 USB-UART 桥拨出来的**：板子原生 USB、无 CH340，
  出不了 COM 口 → 用 MounRiver 编 WCH 官方 `SimulateCDC` 例程、WCHISPTool V3.3 刷进去
  （BOOT+RST 进刷机态），按 RST 后设备管理器出现 **`USB串口设备(COM32)`**（115200 8N1）。
- **接线**：nano `A2`(USART2_TX) → TX-AH `IOA13`(J4 pin3)；nano `A3`(USART2_RX) → TX-AH `IOA12`(J4 pin2)；
  nano `5V` → TX-AH `J2 VCC`；nano `G` → TX-AH `J2 GND`（共地）。
- **已验过**：SecureCRT 连 COM32、键 `AT+SSID?` **有正确回应** ⇒ **AT 控制面通了**。

### ★ 上机首测结论（2026-09-14 实测）：UART 数据面走不通，**通路待重定**

模块身份：`AT+VERSION=?` → `v2.4.1.5-38247`（第 4 位 = `5`）、MAC `4a-06-59-8d-74-40`、
SSID `测试链路`、**当时是 AP 模式**（`mode=2`、908.0MHz/bw8、无 sta）。

- **控制面可用** ✓：`AT+SSID?` / `AT+VERSION=?` / `AT+MAC_ADDR=?` 都答；`AT+SYSDBG=LMAC,0`
  还真能把周期 `LMAC STATUS` 刷屏关掉；`resync()`（1700 字节填充）在真机上**有效**。
- **✘ 数据面：`AT+TXDATA` 四种写法全部静默无应答**（`AT+TXDATA=29` / `=29,8,0,0` / `?` / `=?`）
  —— 不是 `ERROR`，是一句话都不回；结合版本号第 4 位 = `5`，与 `halow-demo/simulator/AGENTS.md`
  的 **2026-09-07 真机结论完全一致**：这块是 **fmac 固件，AT 只有控制面、没有 AT 级发数据命令**
  （那份记录：payload 需走**主机 SDIO/SPI(MACBUS)** 或换“网络版固件”）。
  ⇒ `AT+TXDATA`/`FRAME:RX` 这套（`host_serial.py`）**在真机上无效**（离线排练仍有效，留着当 API 对照）。
- **⇒ 候选通路（要用户拍板）**：① **模块主机接口 SPI/SDIO（MACBUS）** + PC 侧 USB→SPI 桥
  （CH341A/CH347A）—— 与最终产品形态一致（CH32V203 侧本就要走这个），且帧语义
  （`AA 55 ...`）当初就是按 MACBUS 设计的；② 换**网络版固件**（需厂商固件/授权）；
  ③ 改用**数据面走网口**的那块板（但 `halow-demo` 2026-09-07 实测：TH-RJ45 ↔ TX-AH
  **跨固件可发现不可关联**，选它得先确认能配对）。
  ⚠ 2026-09-16 复核后：① 的 **SPI 变体**按上表**不可行**（模组侧无 SPI 主机口实现），
  同一条路的 **UART 变体**（CH347F 的两路 UART 也够用）才可行。
- 细节与证据（各命令的原样回应）见 `docs/nano-ch32v203-uart-bridge.md` §5。

### ★ 2026-09-16 复核（翻模组 SDK 源码后，三条候选通路的可行性都定了）

依据（模组侧源码 = `F:\git\tianlu\TXW8301\TX_AH_SDK_2.4_...\TXW8301_FMAC-v2.4.1.5-39777`，
经 junction 挂在 `halow-demo\TXW8301\FMAC_SDK`）：

| 主机口 | 模组侧实现 | PC 侧可行性（无厂商主控驱动时） |
|---|---|---|
| **SDIO**（默认固件） | ✅ `sdk/lib/bus/macbus/sdio_bus.c`；`project_config.h` 默认 `#define MACBUS_SDIO` | ✗ PC 一般没有 SDIO 主控；要厂商 Linux/RTOS 驱动 |
| **UART** | ✅ `uart_bus.c`（要改宏重编；UART0 = IOA10/IOA11，115200，RAW=裸以太帧） | ★ **可自实现**：协议有源码（HGIC 8 B 头 `magic/type/ifidx/flags/length/cookie`） |
| **USB** | ✅ `usb_bus.c`（要重编） | ✗ 走 `usb_device_wifi_*`，厂商私有 USB WiFi 类，要厂商驱动 |
| **SPI** | ❌ **没有实现**（`mac_bus.h` 只有声明）；changelog 说「SPI 接口和 SDIO 接口是同一固件」→ 实为 **SDIO 控制器的 SPI 模式**（SD/SDIO-over-SPI 协议） | ✗ 要厂商主控驱动 |

- 另：`uartp2p`（串口透传）在 **2.x 不支持**（changelog 明写）；`wnb-uartp2p` 是 1.6 时代的东西。
- 开发板 UART 跳线：**A10/A11 = 主机通信口**、**A12/A13 = 打印/AT 口** —— 2026-09-14 首测接的是
  A12/A13（打印口），所以只通 AT、没有数据命令。
- 现有固件 bin（`FMAC_SDK/project/*.bin`、`out/FMAC/*`）**全是 SDIO 版**，没有 UART macbus 版。
- 因此 2026-09-16 用户选：**先做 SPI 电气探测**（判据 = SD-SPI 的 CMD0/CMD5 有没有合法 R1），
  工具与接线见 `docs/ch347f-txah-spi-probe.md`；**UART 路线**（改 `project_config.h` 开
  `MACBUS_UART` + 重编 + `at+fwupg` 烧录 + PC 侧 HGIC 驱动）。

### ✓ 2026-09-16 进展：UART 路线「控制面 + 回程」已通（真机实测）

- **固件**：`MACBUS_UART` + `WIFIMGR_FRM_TYPE_HGIC` 已编、已烧、已由启动打印确认
  （`[mbus cfg] frm_type=1 (0=ETHER 1=HGIC 2=RAW) bus=4`）；宏切换脚本
  `halow-demo/TXW8301/tools/fmac_macbus_switch.py`；另有临时 RX/TX 调试打印
  （`docs/txah-uart-macbus.md` §5.2，含回退）。
- **PC 侧 HGIC 驱动已写**：`tools/txah_hgic.py`（编解码 + `ctrl_info`/`fw_info_decode`，
  含真机黄金样本自测）+ `tools/probe_txah_uart.py`（listen/probe/send-cmd/send-eth/xfer/loopback）。
- **实测通了**：`send-cmd 43`(GET_FW_INFO) 回 40 B，解出 app=2.4.1.5 / svn=39777 / mac /
  smt_dat，全部与启动日志逐项对上；模组每 ~5 s 上报事件 7 = `TX_BITRATE`。
  ⚠ 关键教训：**`WIFIMGR_FRM_TYPE_RAW`（厂商注释里的“串口默认”）是纯数据管，
  主机命令不会被分发** —— 换成 `HGIC` 才通（详见 `docs/txah-uart-macbus.md` §5.1–§5.4）。
- **12 B 控制头已上机复验 ✓**（2026-09-16）：`cmd_frame()` 补满 4 B union 后
  （`2b 1a 03 00 0c 00 01 00 2b 00 00 00`）`send-cmd 43` 回包与之前逐字节相同
  —— 这是当时 PC 侧唯一未验证的改动，现结。
- **✓ 2026-09-20：数据面端到端也跑通了（闭环）** —— 台架没用第二块 TX-AH，而是
  **客户端 TX-AH EVB(FMAC, STA) ↔ T-Halow-RJ45(WNB, AP)**（V2.4 ↔ V2.4，`halowlink`/9080/bw8/open）：
  * PC 经 **CH347F UART0** 发 3 条以太帧 → 客户端 AT 口逐条 `[mbus rx] 50/294/41 byte(s)`（= 8B HGIC 头 + 载荷）；
  * **上行过空口**：AP per-STA `rx1_cnt 11 → 12`；
  * **下行到达**：数据口收到 `FRM2`，内含 `src=d6:a2:2a:82:67:c0`（AP 的 MAC）的 **DHCP 应答**（UDP 67→68）——
    这条正好补上 `docs/txah-uart-macbus.md` §7.5「下行不通」的缺口；
  * 一键复现 = `python tools\hgic_loop_test.py`（退出码 0 = 成立）；
  * ⚠ **仍未验证**：PC 从 **AP 侧主机口**（TH-RJ45 的 **RJ45**）注入的帧能不能下行 —— 本机没有有线网卡，没接。
    细节与五条硬规矩（改角色不能复位、`AT+RSSI` 恒 0、判下行别用 `tx1`、AT 无数据面命令、CH347F VCP 抽风）
    见 `docs/txah-uart-macbus.md` §八。
- 另一条（**未走**，留档）：用户 2026-09-16 曾选**第二块 TX-AH 当对端**
  （模块 A = STA ↔ CH347F `P2`；模块 B = AP ↔ CH347F `P3`；两块都刷同一版固件；判据 `xfer`）——
  那条路上**只有上行通、下行不通**（§7.4 表）。

## 三、依赖（装机清单）

- **工具链**：MounRiver Studio（自带 `riscv-none-elf-gcc`）—— `-DWCH_INTERRUPT_FAST` 必须用它；
  或用 xPack 版但要**改中断模型**（参考固件已注明会跑飞）。**已装 MounRiver Studio V2.5.0**（用户 2026-09-14）。
- **烧录**：① **WCHISPTool V3.3**（芯片内置 bootloader，BOOT+RST 进刷机态，**不需要 COM 口**）
  —— b 步夹具就是用它刷的；② WCH-Link（SWD）+ OpenOCD `interface/wch-link.cfg` + `target/ch32v20x.cfg`，
  或 MounRiver 下载按钮。**烧录一律由用户执行**。
- **WCH EVT 包**：`CH32V20xEVT`（<https://file.wch.cn/download/file?id=385>）—— b 步的
  `EVT/EXAM/USB/USBD/SimulateCDC` 就是从中编译出来的；本机在 `D:\Downloads\CH32V20xEVT`。
- **参考固件**：`halow-demo/simulator/firmware/`（CH32V203，裸机、无 RTOS、`-nostdlib`）。
- **协议**：`Protocol/docs/OrpahIDProtocol.md`（SN/校验/签名/密钥/降级/限频）、
  `Protocol/docs/orpah-over-halow/SPEC.md`（报文/走失表/覆盖/能量/设计常态 60 s）。

## 四、未做（如实）

- 本仓目前**只有规则、骨架与本文档**（`AGENTS.md` / `README.md` / `ROADMAP.md` / `.gitignore`
  / `docs/`），**没有任何代码、自家固件未上机**。
- b 步：**物理通路已重定为 UART macbus（CH347F 两路 UART + HGIC）**；**控制面 + 回程**（2026-09-16）与
  **数据面端到端（上行过空口 + 下行到达主机口）**（2026-09-20）**都已真机跑通** ——
  一键判据 `python tools\hgic_loop_test.py`，详见 `docs/txah-uart-macbus.md` §7.4/§八。
  仅剩：**从 AP 侧主机口（RJ45）注入的下行**没验（本机没有线网卡）。
- c 步起的都未做：工具链虽已装但**本仓固件一行未写**；CH32 板 / ATECC608 未接线；
  ATECC608B 驱动、产线烧录、低功耗与取能标定全未做。
- 客户端侧的 `seen_routers`（设备看到哪些路由器）在仿真器里仍是**演示写死值**，
  真机要等空口侧给出可用读数（且 `xport` 不在签名内，见 SPEC F-12）。
