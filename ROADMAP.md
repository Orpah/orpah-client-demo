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
| b | **PC 侧 USB→模块的传输**（不改固件）。⚠ 关键未知项见 §二 |
| c | 第一版**固件**：CH32V203 裸机主循环 + host 数据口（SPI 从机/主机关系待定）+ §8.2 选级 + 无 RTC（`ts=0`）+ `cap.rtc` 声明；ATECC608B 先用**软件 P-256** 顶（SE 驱动在 d） |
| d | 换上**定制载板** + 真实 **ATECC608B 驱动**（Slot 0 私钥不可导出 / Slot 5 HMAC）+ 产线烧录流程（协议 §6.2） |
| e | 一体板：+ 低功耗/取能（能量轴实测标定，SPEC F-11）、结构/天线、Gerber/PNP 交付 |

## 二、b 步的关键未知项：**PC ↔ TX-AH 板的物理数据通路**

TX-AH 的 fmac 固件**AT 层没有用户数据命令**（`halow-demo` 真机实测），所以 payload 必须走：

| 候选 | 数据面 | 需要 | 待确认 |
|---|---|---|---|
| **① USB→SPI 桥**（CH341A/CH347A） | MACBUS `DATA_TX`/`DATA_RX`（与 `host_bus.py` 同一套帧语义） | USB→SPI 适配器 + 接线（含 INT/流控） | SPI 时序/流控、INT 怎么读、MACBUS 寄存器级协议（`halow-demo/simulator/docs/spi_protocol.md` 与 `firmware/Periph/spi_slave.c` 是现成的**从机**侧实现，可反推主机侧；`tools/sim_config.py` 有实验性的同类做法） |
| ② RJ45 透明桥（WNB 固件 + TH-RJ45 载板） | 以太网 L2 | 两块 TH-RJ45 + 网线 | `0x88B5`/广播是否透传、MTU、RSSI 哪端可读 |

**未定之前不写"看着已支持"的传输实现**（本仓规则 §1）。定了之后，客户端仿真器侧只需
换 `DeviceSim(client=…)` 这一个参数，**设备逻辑一行不改**。

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
