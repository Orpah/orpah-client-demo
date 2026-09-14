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

## 三、依赖（装机清单，未做）

- **工具链**：MounRiver Studio（自带 `riscv-none-elf-gcc`）—— `-DWCH_INTERRUPT_FAST` 必须用它；
  或用 xPack 版但要**改中断模型**（参考固件已注明会跑飞）。
- **烧录**：WCH-Link（SWD）+ OpenOCD `interface/wch-link.cfg` + `target/ch32v20x.cfg`；
  或 MounRiver 下载按钮。**烧录由用户执行**。
- **参考固件**：`halow-demo/simulator/firmware/`（CH32V203，裸机、无 RTOS、`-nostdlib`）。
- **协议**：`Protocol/docs/OrpahIDProtocol.md`（SN/校验/签名/密钥/降级/限频）、
  `Protocol/docs/orpah-over-halow/SPEC.md`（报文/走失表/覆盖/能量/设计常态 60 s）。

## 四、未做（如实）

- 本仓目前**只有规则与骨架**（`AGENTS.md` / `README.md` / `.gitignore` / `LICENSE` / 本文件），
  **没有任何代码、没有硬件实测、没有上机验证**。
- b 步物理通路未定；工具链未装；CH32 板 / ATECC608 / TX-AH 板未接线；
  ATECC608B 驱动、产线烧录、低功耗与取能标定全部未做。
- 客户端侧的 `seen_routers`（设备看到哪些路由器）在仿真器里仍是**演示写死值**，
  真机要等空口侧给出可用读数（且 `xport` 不在签名内，见 SPEC F-12）。
