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
| c | 第一版**固件**：CH32V203 裸机主循环 + host 数据口（SPI 从机/主机关系待定）+ §8.2 选级 + 无 RTC（`ts=0`）+ `cap.rtc` 声明；ATECC608B 先用**软件 P-256** 顶（SE 驱动在 d）—— **进行中：c1 骨架已完成**（2026-09-20，见 §五） |
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
- **✓✓ 2026-09-20 同日：b 步「L2 全链路」也闭环了** —— 台架还是上面那套，但 **PC 同时扮
  Router + Server**（不再只是裸帧夹具），客户端跑**上游真实业务栈**（`client_sim.DeviceSim`
  + `client.ClientHost`）：

  * 拓扑：`ClientHost` ──CH347F/UART0(HGIC)── 客户端 TX-AH(STA) ──空口── TH-RJ45(AP，L2 透明桥)
    ──RJ45── 中间路由器(当交换机) ── 家用 Wi-Fi ── 这台 PC 的 Wi-Fi 网卡 ──(Npcap)─→ `RouterBridge`
    → 本机 `OrpahServer`（UDP 19447）。
  * **PC 侧两段传输**：`tools/hgic_bus.py`（客户端侧，API 与厂商 `SerialAtBus` 同形）+ `tools/l2bus.py`
    （有线侧，Npcap + scapy 收 `ether proto 0x88b5`）；**业务逻辑一行没改**（Router 用 `bus=` 注入）。
  * **实测（一键 = `python tools\demo_l2_hgic.py --iface <网卡 MAC>`，退出码 0 = 判据全过，见脚本 docstring ①–⑥）**：
    ① `REQ-CONNECT` → `[client] [rx 1] ORPAH-ACCESS-INFO` ✓；
    ② `REPORT` ×3：每次 `[client] 注入` → `[router] 上行` → `[server] 收到 ts/rssi/seq`
       → `[router] [down] TRACKING-STATUS` → `[client] [rx]` ✓；
    ③ `mark_tracked` → 走失表下发（Router 缓存 1 项）→ 再跑一拍 → `status=TRACKED` ✓；
    ④ 零丢弃：`router_dropped=0 / server_dropped=0` ✓（设备侧自限频的 `held` **另列**，
       那是「延后不是丢弃」，不当判据）。
  * ★ **这条同时补上了上面那句「未验证」**：从 **AP 侧 RJ45** 进去的帧**确实能下行到客户端** ——
    下行的 `ACCESS-INFO` / `TRACKING-STATUS` 就是 PC 从网卡发出去、经 RJ45 进 AP、过空口、
    到客户端数据口的。
  * ⚠ 两个必须记住的坑：
    · **Npcap 会抓到本机自己发出的帧** ⇒ 不处理的话 Router 会把自己发的下行当上行收回来（回路）。
      `l2bus.py` 默认 `drop_own=True`（丢「源 MAC = 本机网卡」的帧），且 Router 的 `self_mac` 用
      **同一块网卡的 MAC** —— 实测 `own_dropped` 恒等于 Router 发出的下行动数（首次 5、补上 ID 后 8）。
    · **上游 `router.py` 需要一处小改**才能挂在裸以太总线上：`bus=` 注入 + `set_transport()`
      （见 `orpah-over-halow` 那边的对应提交）。
  * ★ **拍间隔是按桶容量算出来的，不是拍的**：设备自限频是 0.6 s/条（= 1.67 条/秒），
    而一拍要发 3 条（REQ-CONNECT + REPORT + ID）⇒ 拍间隔得 ≳ 1.55 s 才不会被自己的闸门
    延后（默认取 **1.8 s**）。比这快**不会报错**，只会看到 `held > 0`（延后不是丢弃）。
  * **✓ 用户选①后补上：ID 验签（`ORPAH-ID-REPORT`）也进了夹具**（2026-09-20 实测）——
    设备侧换成上游 `client_sim.DeviceSim`（`cycle()` = REQ-CONNECT → REPORT → 已签 ID-REPORT，
    顺序与 `ui_server` 一致），密钥库用上游 `orpah_id.KeyStore` 登记**同一个**设备对象：
    · **⑤ 验签通过**：3 拍都是 `alg=ES256 level=0 trust=high accepted=True` ——
      **已签报文过空口 + 有线到 Server，服务端 ECDSA 验签通过**（= b 步判据「服务端收到并验签通过」）；
    · **⑥ 负对照**：把一条已签报文的 payload 改掉（连 nonce 一起换，免得先被 nonce 去重拦下）
      ⇒ `accepted=False error=signature_invalid` —— 证明验签**真在跑**，不是橡皮图章；
    · 附带看到 `[found 1] 发现走失`（Router 命中走失表后上报 FOUND，也走的真链路）。
    ★ **按用户选①补上（2026-09-20 同日）**：夹具默认改成**真机形态声明** —— `ts=0` +
    `cap.rtc=false`（无 RTC 的 CH32V203 就是这么声明的）+ 如实报储能电压 `battery_mv`
    （默认取 `energy.mv_of(CHARGE0_MJ, STORE_MJ)` = **3900 mV**，**演示映射不是实测**）。
    实测新增两条判据（10 项全过）：
    · **⑦ 无 RTC 声明生效**：`ts_src=server`、`ts_eff≈现在`、`cap_rtc=False`、`ts_ok=False`
      —— 设备发的 `ts=0` 被**如实忽略**，服务端用接收时刻（= SPEC §5.5「ts=0/缺失 → 不做时间基准」），
      而**不是**把它当成 1970 年或假装有设备时间；
    · **⑧ 电量报进签名里**：`battery_mv=3900` 原样到达 Server，且 `level=0` ⇒ `degraded_reason=None`
      （**没降级就不许编成因**；成因只能由签名内的 `hdr.level` + `battery_mv` 推导）。
    要打**有 RTC 的对照**：`--rtc true --real-ts`（那时应看到 `ts_src=device`）；电量可直接
    `--battery-mv <mV>` 指定（例如给个 ≤3300 的值看 `id_energy` 那条低电告警的输入端）。
  * ⚠ **仍未验证（如实）**：这台 PC **仍没有有线网卡**，跑的是**家用 Wi-Fi 的 L2 域**
    （中间路由器当交换机用）；真机形态（PC / OpenWrt 的**有线口**直连 AP 的 RJ45）没验。
    （ID-REPORT / 验签那一路已补上，见**上**条。）
- 另一条（**未走**，留档）：用户 2026-09-16 曾选**第二块 TX-AH 当对端**
  （模块 A = STA ↔ CH347F `P2`；模块 B = AP ↔ CH347F `P3`；两块都刷同一版固件；判据 `xfer`）——
  那条路上**只有上行通、下行不通**（§7.4 表）。

## 三、依赖（装机清单）

- **工具链**：MounRiver Studio 自带的那份（`-DWCH_INTERRUPT_FAST` **必须**用它；独立 xPack 版中断
  模型不同、会跑飞）。**已装 MounRiver Studio 2**（用户 2026-09-14）—— **2026-09-20 实测**：
  编译器在 `F:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin\`，
  **前缀是 `riscv-none-embed-`**（不是 `riscv-none-elf-`；`--version` = *xPack GNU RISC-V Embedded GCC 8.2.0*，
  即 WCH 在 MRS 里自带的那一份）。`firmware/Makefile` 的 `RISCV_PREFIX` 默认就指向它。
  ⚠ Windows 上还得给 make 找一个 POSIX shell（recipe 用 `mkdir -p`/`rm -rf`）：
  实测 `make SHELL='D:/Program Files/Git/bin/sh.exe'`（Git 自带）可用。
- **烧录**：① **WCHISPTool V3.3**（芯片内置 bootloader，BOOT+RST 进刷机态，**不需要 COM 口**）
  —— b 步夹具就是用它刷的；② WCH-Link（SWD）+ OpenOCD `interface/wch-link.cfg` + `target/ch32v20x.cfg`，
  或 MounRiver 下载按钮。**烧录一律由用户执行**。
- **WCH EVT 包**：`CH32V20xEVT`（<https://file.wch.cn/download/file?id=385>）—— b 步的
  `EVT/EXAM/USB/USBD/SimulateCDC` 就是从中编译出来的；本机在 `D:\Downloads\CH32V20xEVT`。
- **参考固件**：`halow-demo/simulator/firmware/`（CH32V203，裸机、无 RTOS、`-nostdlib`）。
- **协议**：`Protocol/docs/OrpahIDProtocol.md`（SN/校验/签名/密钥/降级/限频）、
  `Protocol/docs/orpah-over-halow/SPEC.md`（报文/走失表/覆盖/能量/设计常态 60 s）。

## 四、未做（如实）

- 本仓现在有：`tools/`（b 步 PC 侧夹具，**真机跑通**，见 §二）、`firmware/`（**c1 骨架：能编译出
  `.elf`/`.bin`，但未上机**）、`docs/`、`hardware/`（接线图）。**自家固件未上机**。
- b 步：**物理通路已重定为 UART macbus（CH347F 两路 UART + HGIC）**；**控制面 + 回程**（2026-09-16）、
  **数据面端到端**（上行过空口 + 下行到达主机口，2026-09-20）与
  **L2 全链路闭环**（PC 同时扮 Router + Server，2026-09-20 同日）**都已真机跑通** ——
  一键判据 `python tools\hgic_loop_test.py`（裸帧）与 `python tools\demo_l2_hgic.py`（全链路），
  详见 `docs/txah-uart-macbus.md` §7.4/§八 与 `ROADMAP.md` §二。
  仅剩：**真机形态的有线口**（PC/OpenWrt 有线网卡直连 AP 的 RJ45）没验 —— 本机没有有线网卡，
  跑的是家用 Wi-Fi 的 L2 域；另 `ID-REPORT`/验签那一路未进本夹具。
- c 步：**c1（骨架 + 能编译）已完成 / 未上机**；c2（协议内核 C + 交叉测试）、c3（HGIC 数据口）、
  c4（选级 + 无 RTC + 自限频 + 已签上报）未做 —— 见 §五。CH32 板 / ATECC608 未接线；
  ATECC608B 驱动、产线烧录、低功耗与取能标定全未做。
- 客户端侧的 `seen_routers`（设备看到哪些路由器）在仿真器里仍是**演示写死值**，
  真机要等空口侧给出可用读数（且 `xport` 不在签名内，见 SPEC F-12）。

## 五、c 步进度（2026-09-20 起）

| 阶段 | 内容 | 状态 |
|---|---|---|
| **c1** | 本仓 `firmware/` 骨架：Makefile / `ld` / startup / `Core{board.h,main.c}` / `Periph{gpio,uart}` | **✅ 完成（未上机）** |
| c2 | 协议内核 C 实现 + 与 Python 的**交叉测试**（同一批黄金向量，纯 PC） | **✅ 完成（见下）** |
| **c3** | HGIC 数据口（UART ↔ TX-AH：8 字节头 + `FRM2` + `CMD`/`EVENT`） | **帧层 ✅ 完成（未上机）**；`proto/*.c` **已编进固件**（2026-09-20）；剩 UART2 胶水 + 烧录 |
| c4 | §8.2 选级 + 无 RTC（`ts=0`/`cap.rtc=false`）+ 自限频 + 已签 ID 上报 | **进行中**：**降级（HS256）已达服务端验签通过**（见下 c4-α）；ES256 待拍板 |
| c5 | 低功耗 / 取能标定 | 推后到 d/e |

**c1 实测判据（2026-09-20）**：`make clean && make` **exit=0** ⇒ `build/orpah-client.elf` 9644 B、
`.bin` 2108 B（`text 2046` ⚠ `bss` 20224 ≈ 整个 RAM，是 `link.ld` 里 `.heap (NOLOAD)` 从 RAM 中段
预留到顶端 − 256 造成的，**不是真用掉**；`sp = _eusrstack` 从顶端向下长）。
**未上机**：横幅 / 心跳灯 / `AT`→`OK` 这三条是"烧进去应该看到什么"的判据，**由用户执行后确认**。

**c1 顺手更正的两条事实**（细节都写进 `firmware/README.md`）：

1. **工具链前缀是 `riscv-none-embed-`**（MRS2 内嵌路径，见 §三），**不是**参考 Makefile 默认的
   `riscv-none-elf-`；本机实测可用（`xPack GNU RISC-V Embedded GCC 8.2.0`）。
2. **参考 Makefile 的 `.bin` 目标是坏的**：它写 `objcopy -O binary $@ $<`（把**输出**当输入）
   ⇒ 本仓已改成 `$< $@`。（`halow-demo` 那边**没动** —— 不在本次范围，要同步修得你点头。）

**c4-α（降级链 HS256）：✅ 完成（2026-09-20）** —— `proto/id_report.{h,c}`：

* 设备侧组装：`hdr{alg,level}` + `payload{sn,ts,nonce,seen_routers[,cap][,battery_mv][,firmware]}`
  → 预像（JCS）→ 签名 → 报文 JSON（插入序）；**level 1/2 = HMAC-SHA256**、**level 3 = 不签名**；
  **level=0（ES256）返回 `IDR_E_ES256`：明确报未实现，不假装签了**。
* 实测（`python proto\run_cross_test.py` → **exit 0**，交叉测试 **12 组**）：
  · `id-report-selftest` **5/5**：与 Python `Device.report()` + `encode_msg()` **逐字节一致**（含外层信封）；
  · ★ **服务端验签通过**：把 **C 产出的报文**交给上游 `orpah_id.verify_report()`+`KeyStore`
    真验一遍 ⇒ **4 条 `level=1/2` 全部 `accepted=True` 且级别对得上**；
    `level=3`（`alg=none`）服务端判定 `accepted=True / trust=none / coverage_only=True`（只陈述，不当判据）。

⇒ 也就是说，c 步判据里的「**服务端收到并验签通过**」**已经先离线达成**（还没上机 —— 上机要做 c3-2：
把 HGIC 帧层接到 CH32 的 UART2 上 + 由你烧录）。
★ 一个必须知道的边界：向量里的 `ts` 是**固定值**（提交的文件必须确定性），所以验签时把 `now` **钉到向量那个 ts**；
`ts=0`（真机无 RTC 的正常路径）则不传 `now`。**时间窗是另一个机制**，由上游测试负责。
（踩坑经过：我曾硬编码一个抄来的 `ts`，过一会儿再跑就回 `timestamp_out_of_window`，签名其实早就过了。）

**c2（SN 内核 + JCS + b64url + 报文信封 + 下行解码 + SHA-256 + HMAC）：✅ 完成（2026-09-20）** —— `proto/`：

| 文件 | 作用 |
|---|---|
| `proto/sn.{h,c}` | SN 内核：Crockford / `sn_ok` / `sn_err` / `sn_parse` / Luhn32 / Mod97 / `sn_verify_check`（**无 malloc/stdio ⇒ 可直接编进固件**） |
| `proto/jcs.{h,c}` | **RFC 8785 JCS 规范化**（我们用到的那部分）+ 迷你 JSON arena 构建器；`jcs_preimage()` = `orpah_id.preimage_of`；`jcs_encode_raw()` = 信封的**插入序**编码（同样无 stdio/malloc/浮点） |
| `proto/msg.{h,c}` | 链路报文构造器：`msg_req_connect` / `msg_report` / `msg_id_report`（对齐 `orpah_proto._base`+`build_*`） |
| `proto/downlink.{h,c}` | **下行解码**：`dl_decode()`（同 `decode_msg`）+ `dl_type/dl_str/dl_int/dl_truthy`（Python 真值语义）；JSON 解析器在 `jcs.c`（**有意比 Python 严**，边界见 `proto/README.md`） |
| `proto/sha256.{h,c}` | **SHA-256**（FIPS 180-4，无 stdio/malloc）；§5.1 的 `SHA-256(preimage)` 与将来 RFC 6979 确定性 k 的前置 |
| `proto/hmac.{h,c}` | **HMAC-SHA256**（RFC 2104）= §5.1 的**降级 HS256**；键超分组先哈希、用完清栈上的密钥派生值 |
| `proto/b64url.{h,c}` | base64url（无填充）= `orpah_id.b64url_encode` |
| `proto/sn_cli.c` / `jcs_cli.c` | host 侧 CLI（对拍/调试用，**不编进固件**） |
| `proto/run_cross_test.py` | 一键对拍：**C ↔ 向量文件 ↔ Python 三方比对** |
| `proto/test_vectors_*.txt`（8 份） | 63 / 19 / 12 / 17 / 8 / 11 / 16 / 13 行（**Python 生成，勿手改**） |

实测判据：`python proto\run_cross_test.py` → **exit 0**，输出 `SN selfcheck 11/11`、`selftest 63/63`、
`sn-selftest 19/19`、`JCS 4/4` + `jcs-selftest 12/12`、`b64url-selftest 17/17`、`msg-selftest 8/8`、
`dl-selftest 11/11`、`SHA/HMAC selfcheck 3345/3345`、`sha256-selftest 16/16`、**`hmac-selftest 13/13`**、
`HGIC selfcheck` + `frame-selftest 14/14` + `parse-selftest 12/12` + `ctrl-selftest 13/13`，
**十二份快照**与 Python **逐行一致**。
★ SHA-256 与 HMAC 的向量里都包含**真实签名预像**（`jcs({"hdr","payload"})` 那串字节），
HMAC 那条用的是**真实降级路径**（32B 演示 HMAC 密钥 × 真实预像 = §5.1 的 HS256）——
c4 要签/要做 MAC 的就是这两串，先把它们钉死。
★ 一个必须分清的点：**签名预像走 JCS（排序）、链路信封走插入序**（`encode_msg` 不带 `sort_keys`）——
两者混了就是“能发出去但服务端验不过”。
★ 另一个实打实的坑：报文用例里用 `-` 表示“字段不给”，而 **RSSI 是负数** ⇒ `-55` 被误判成缺省，
`msg-selftest` 当场挂 4/8；正确做法是只有**整字段**等于 `-` 才当缺省。

★ **顺手纠正的一个真事实**：SN 校验位走的是 **Luhn32（1 位）/ Mod97（2 位）**，**不是 Damm32** ——
`verify_check()` 按校验位长度分流，Damm32 是 Phase 2 的替代算法、**当前不启用**。
本仓 AGENTS 里“黄金样本 `WH01-9AF3C1D2 → B`”那句说的是 **Damm32**；**Luhn32 真值是 `E`**、
Mod97 是 `21`（我一开始凭记忆把 B 当成 Luhn32 期望值 ⇒ 自检当场失败。
教训：**期望值只能来自 Python 输出**）。

**c3-1（HGIC 帧层）：✅ 完成（2026-09-20，未上机）** —— `proto/hgic.{h,c}` + `proto/hgic_cli.c`：

* 内容：8 B 头的组/解（**小端**、`ifidx:4|flags:4`）、数据帧 `FRM2`、命令帧 `CMD`/`CMD2`（**4 B union 补满**，
  参数在偏移 12）、cookie 计数器（**15 位回绕**、逐帧 +1 —— 模组做顺序检查）、控制面解码
  （带 `status/len` 的应答 / **短应答** / 请求与事件）、**流式定帧 + 重同步**（杂音 / 假 magic / 截断 / 逐字节喂）。
* 实测判据（同一条命令，**exit 0**）：`frame-selftest` **14/14**、`parse-selftest` **12/12**、
  `ctrl-selftest` **13/13** 与 **本仓** `tools/txah_hgic.py` **逐行一致**（HGIC 的单一源在本仓，
  **不需要上游仓库** ⇒ 那三组在没有 `orpah-over-halow` 时照样跑）+ `selfcheck` 不变量全过。
* ★ 写在哪：**帧层放 `proto/`（纯逻辑、可交叉测试）**，不放 `Periph/` —— 与 c2 同一约定；
  `Periph/` 留给硬件胶水（USART2 ↔ HGIC）。已在 `firmware/Makefile` 注释里更正（原先写的是 `Periph/hgic.c`）。
* ⚠ 两个真踩到的坑：① **magic 是小端 u16** —— 字节 `2B 1A` = `0x1A2B`（主机→模组）、
  字节 `1A 2B` = `0x2B1A`（模组→主机）；我自检里就拿错了方向（4 项红），而**向量组当时已经证明实现是对的**
  ⇒ 错的是测试数据（教训同 c2：期望值/用例也要拿证据对齐）；② C 的解析缓冲有上界（4096）而 Python 没有
  ⇒ 多一个 `overrun` 计数器（向量里恒 0，收录它就是钉住“合法数据永不触发它”）。
* **已做一半（c3-2a，2026-09-20）**：把 `proto/*.c` **编进固件**（`firmware/Makefile` 新增
  `PROTO_DIR`/`PROTO_SRCS` + `build/proto/` 独立目录规则 —— 不能靠 `$(BUILD)/%.o: %.c`，
  那会把 `../proto/x.c` 的目标算成 `build/../proto/x.o`，等于把 .o 写回仓库）。
  实测：9 个 proto 源全编过（`-Wall -Wextra` **无告警**）、链接成功；`size` = **text/data/bss
  2046/0/20224**，**与接线前逐字节相同** ⇒ `--gc-sections` 把没人调用的 proto 代码全丢了。
  ★ 但这是**弱证据**：真正的「proto 能不能 `-nostdlib` 链接」要等状态机真调用它们才算数。
  ★★ **于是把它当真测了一次（同日）**：把 `--gc-sections` 关掉重链 ⇒ **失败**，缺的是
  **libgcc** 的 `__udivdi3`/`__umoddi3`（`jcs.c` 的 enc/parse）与 `__lshrdi3`（`sha256.c`
  的 `sha256_final`）—— 32 位目标上做 64 位除/模/右移要用它们。当前 LDFLAGS 是 `-nostdlib`
  **不带 `libgcc`** ⇒ 现在能编过、只是因为 proto 全是**死代码**被 gc 丢掉了。
  实测：**追加 `-lgcc` 后全量保留也能链**（`text 15118 / data 0 / bss 20224`；flash 64 K 够用，
  bss 与接线前逐字节相同 ⇒ proto **没有静态数据**，全是调用方 arena/栈）。
  ⇒ **已按用户拍板加上 `-lgcc`**（`firmware/Makefile` 的 `LDLIBS`，**排在目标文件之后**）；
  同一轮把 `mkdir -p` / `rm -rf` 换成 `cmd /c`（不依赖 sh）⇒ **干净树上 `make clean && make` 可跑**。
* **未做（= c3-2b，要上机）**：`Periph/` 下的 UART2 胶水（**引脚口径待定**）+ 用户烧录。
* ⚠ `firmware/Makefile` 两条 Windows 实测坑（已写进该文件头）：
  ① recipe 里**没有 shell 元字符**的行，make 会**直接 exec** 那个程序（不经 sh）⇒ `mkdir -p` /
     `ls -l` / `rm -rf` 全都报「找不到指定的文件」；而 `make SHELL='D:/Program Files/Git/bin/sh.exe'`
     在这个组合下**没能改变**这一点（`sh.exe` 确实在，但不在 PATH；加进 PATH 也没用）。
     本仓已去掉 recipe 里的 `ls`；**`mkdir -p` / `rm -rf` 仍在**（干净树上会挂）—— 要改得用户点头。
  ② 变量赋值**别写行内注释**：注释前的对齐空格会留在值里 ⇒ 先决条件变成 `../proto   /sn.c`
     （找不到）⇒ 规则**静默失效**；同时 `-I../proto   ` 又能编译（编译器容忍尾空格）⇒ 现象很迷惑。

⚠ **剩下的两块**：① **c3-2 上机接线**（UART2 ↔ HGIC + 把 `proto/*.c` 编进固件 + 烧录）；
② **ECDSA P-256（level=0）** ⇒ 要先拍两个板（软件 P-256 的来源、`k`/nonce 方案，见下）。

**c4 开工前要拍的两个板**（属“不能自由发挥”的工程/安全取舍，2026-09-20 提出）：

- **软件 P-256 从哪来**：① 自己写（量大易错，不建议）；② **vendor 一个宽松许可实现**
  （如 micro-ecc，BSD-2）—— 但按 §0c「只提交我自己创建的文件」，**第三方源码要先经你同意**
  并随附 LICENSE；③ 先做**不签名的骨架**（c1–c3），签名留到 c4。
- **两处随机**（已实测确认 CH32V203 **没有 TRNG**：EVT 全树 + `nanoCH32V203` 资料里查不到）：
  `payload.nonce`（防重放，签名内 16 B）与 ECDSA 的每次签名 k（**k 用不好会直接泄漏私钥**）。
  倾向：k 用 **RFC 6979 确定性 nonce**（不需要熵）；`payload.nonce` 用上电熵 + 计数器、
  并**如实标注"非生产强度"**，d 步换 ATECC608B 自带的硬件 RNG。
