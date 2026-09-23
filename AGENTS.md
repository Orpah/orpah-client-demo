# ORPAH 客户端（硬件 + 固件）开发规则 — `orpah-client-demo`

本仓 = ORPAH 五仓里的**客户端侧**：**CH32V203 + SE(ATECC608B) + TX-AH 模组**（用户 2026-09-14 定）。
本文件是**硬规则**；踩坑与实测细节写进 `README.md` 或 `docs/`，这里只留"必须照着做/不能做"的部分。

## 0. 五仓分工与「唯一事实来源」（最要紧的一条）

| 仓库 | 内容 | 与本仓的关系 |
|---|---|---|
| `F:\git\Protocol` | 通信协议规范（`OrpahIDProtocol.md`、`orpah-over-halow/SPEC.md`） | **唯一权威**：固件严格遵循 |
| `F:\git\orpah-over-halow` | 业务全链路（服务端/Router 桥/定位/告警 + `host_bus.py`） | 客户端**参考其协议定义与仿真器接口**，并对它做联调 |
| `F:\git\halow-demo`（`simulator/`） | 空口/设备侧（`host/sim.py`、`tools/ui/`、`firmware/` CH32V203 参考固件） | 可复用其**测试数据**与**固件骨架** |
| `F:\git\orpah-openwrt-demo` | 路由器端 OpenWrt 软件 | 客户端与之对接测试 |
| **本仓** | **客户端硬件 + 固件** | — |

- **本仓不定义协议**：报文/字段/签名流程以 `Protocol` 为准。要改协议 → **回 `Protocol` 改** →
  再让固件跟上；发现"必须改协议才能实现"时**先停下与用户对齐**，不许在固件里私加字段或改语义。
- **接口单一源**：host 数据口帧格式认 `orpah-over-halow/host_bus.py`（= SPI MACBUS
  `DATA_TX`/`DATA_RX`，`AA 55 TYPE LEN_H LEN_L CRC`，CRC-8/ATM 0x07）+ SPEC §6；
  **不要在固件里另发明一套帧/字段**（否则联调时两边"都觉得自己对"）。
- **设备侧行为**（降级下限 L1、设计常态 60 s 连接周期、无 RTC 时 `ts=0`、能量/沉默归因…）
  认 `orpah-over-halow/AGENTS.md` §0 与 SPEC §5.2/§8；**不要另立一套口径**。
- **空口/真机事实认 `halow-demo/simulator/AGENTS.md`**（那边有大量 TX-AH/TH-RJ45 真机实测记录）。
  与客户端直接相关的两条：① **PC ↔ 模块的数据面走 UART 上的 `AT+TXDATA`**（用户 2026-09-14 定）：
  `AT+TXDATA=<len>` → 等 `OK` → 写**裸以太网帧**（含 14B 以太头，长度也含它）；下行 = `FRAME:RX <hex>` 行
  （需 `AT+SYSDBG=WNB,1`）。⚠ **仓里两份记录不一致**：`T-Halow-RJ45/docs/AT_cmd.md` 确有 `AT+TXDATA`，
  而 `halow-demo` 的真机实测写着 TX-AH 的 fmac 固件“AT 只有控制面、无用户数据命令”
  → **以实测为准**；上机首测先确认三件事（命令写法 / 下行格式 / 数据模式粘性），
  参考实现与判据见 `orpah-over-halow/{host_serial.py, docs/client_sim.md, demo_client_uart.py}`；
  ② **真机一次只应答一条 AT 查询**（背靠背发会吞后面那条）→ 轮询/配置必须逐条错开。

## 1. 硬件与工具链（2026-09-14 用户定）

- **MCU = CH32V203**（QingKe V2 内核）、**安全元件 = ATECC608B**（协议 §5.3 的 SE）、
  **射频模组 = 泰芯 TX-AH**（AH-SDK V2 系列，与 `halow-demo` 的真机档案 `txah` 同族）。
- **工具链沿用 `halow-demo/simulator/firmware` 那套**：`riscv-none-elf-gcc`（MounRiver 自带），
  `-DWCH_INTERRUPT_FAST` + 启动文件里的硬件栈/嵌套配置 ⇒ **必须用 MounRiver 工具链**（xPack 版会跑飞）。
- **b 步的串口测试夹具已跑通**（用户 2026-09-14 实测；细节见 `docs/nano-ch32v203-uart-bridge.md`）：
  nanoCH32V203（**板载原生 USB、无 CH340**，Win11 下出不了 COM 口）刷 WCH 官方 `SimulateCDC`
  例程（MounRiver 编译 `EVT/EXAM/USB/USBD/SimulateCDC` → WCHISPTool V3.3、BOOT+RST 进刷机态）
  ⇒ 变成「USB CDC ↔ USART2(PA2/PA3)」的 **USB-UART 桥**，出现 **COM32**（115200 8N1）。
  接线：nano `A2`/`A3`/`5V`/`G` ↔ TX-AH `IOA13`(J4 pin3)/`IOA12`(J4 pin2)/`J2 VCC`/`J2 GND`。
  **J4 引脚定义（用户 2026-09-14 给的「AH UART」原理图，已确认）：1=VCC、2=IOA12、3=IOA13、4=GND**
  —— 板上那排丝印从左到右是 `GND/A13/A12/VCC` ⇒ **该排编号自右向左**（同段原理图里
  J5 = 2×4、J10 = 单针 GND）。
  `AT+SSID?` 有正确回应 ⇒ **AT 控制面已通**。
  ★★ **上机首测（2026-09-14 实测）：数据面走不通** —— `AT+TXDATA` 四种写法全部静默无应答，
  模块版本 `v2.4.1.5-38247`（第 4 位 = 5）= **fmac 固件，AT 只有控制面**，与
  `halow-demo/simulator/AGENTS.md` 2026-09-07 真机结论一致 ⇒ **b 步通路待重定**
  （候选：主机 SPI/SDIO MACBUS + USB→SPI 桥 / 换固件 / 数据面走网口的板），见 `ROADMAP.md` §二。
  ★ 另：`AT+SYSDBG=LMAC,0` 可关掉周期 LMAC 刷屏（我们的 `SerialAtBus` 开机该发一条，未改）；
  `resync()`（1700 字节填充）真机**有效**。
  ★ COM32 是 Windows 现分配的号（换口/换机就变，别写进脚本常量）；
  ★ **SimulateCDC 是测试夹具，不是本仓固件**（本仓固件从 c 步开始，未写）。
- **裸机约束照抄参考固件**：无 RTOS、`-nostdlib`、自包含寄存器定义、主频 8 MHz HSI（无 PLL）。
- **★ 固件四条硬规则（2026-09-21 上机踩出来的，改 `firmware/` 前必看；细节 = `docs/c3-2b-bench-bringup.md`）**：
  ① **链接基址必须 `0x00000000`**（`0x08000000` 的镜像烧进去**一条指令都不执行** —— 灯不亮、串口全静默）；
  ② **`IRQn_Type` 必须带 `+16` 异常号偏移**（`WWDG=16 … TIM2=44, USART1=53, USART2=54`）——
     `NVIC_EnableIRQ` 把值**直接当 PFIC 位号**用，写错就是"中断一个都进不来"；
  ③ **`mstatus` 写 `0x1888`**（MPP=0b11 留机器模式），WCH 的 `0x88`（用户模式）下 `main` 里
     **任何 CSR 访问都出事、写 PFIC 静默失效**；
  ④ **TIM 的 `INTFR` 是 write-all-bits**（清标志**写 0**，写 1 反而置位 ⇒ ISR 死风暴）
     且 **`ATRLR` 不能为 0**（不产生更新事件）。
  另：**烧录下载完不会自动运行，必须按一次 `RST`**（不按 = "刷完什么也没有"，最易误判成固件坏了）。
- **参考骨架 = `halow-demo/simulator/firmware/`**（Makefile / `ld/link.ld` / `startup/` / `Core/board.h`
  / `Periph/{gpio,uart,spi_slave}` / 状态机）：**复用其套路**（含 SPI1 从机 host 接口、UART2 虚拟空口、
  AT 引擎、迷你 printf），不要另起一套目录/驱动风格。
- **★ 平台层是「两份独立副本」，改一侧必须同步另一侧**（用户 2026-09-21 判定：不抽公共库、
  不做 submodule，两仓各自独立）。这 **7 个文件**：`ld/link.ld`、`startup/startup_ch32v203.S`、
  `Core/ch32v20x.h`、`Periph/gpio.{c,h}`、`Periph/uart.{c,h}` —— 参考仓那套**从未上机**、
  本仓这套**已上机验证**（见 `docs/c3-2b-bench-bringup.md` 的四个真凶）⇒
  **在一侧发现的平台层 bug，改完要在另一侧也改掉**（2026-09-21 就是这么回灌的：
  `halow-demo` 的 `d981896`/`8da9c48`/`cb1b7a1`），并各自标清「已上机 / 未上机」。
- **烧录由用户执行**（WCH-Link/SWD：`openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg
  -c "program build/xxx.bin 0x00000000 verify reset exit"`；或 MounRiver 下载按钮 / WCHISPTool）。
  **本仓不许把"烧过了/上机验证过"写成既成事实** —— 没实测的一律标「未验证」。

## 2. 密钥与签名纪律（协议 §5/§6；错一次就全盘验不过）

- **1 把 ECDSA P-256 私钥（Slot 0）+ 1 把 32 字节 HMAC 密钥（降级用）**；**一代终身、不轮换**
  （协议 §6.3）⇒ **不写轮换代码**（demo 里那种 `rotate/grace/retired` 不是设备模型）。
- **私钥不可导出**（SE 内生成/烧入）；**仓库里绝不放真实私钥、密钥文件、出厂密钥**。
  测试只能用**派生演示密钥**并注明是演示。
- **签名预像 / JSON 规范化 / `b64url`** 必须与 Python 参考实现**逐字节一致**：这几处任何改动都会让
  已入库的密钥全部验不过（`signature_invalid`）—— 改完必须跑交叉测试（见 §4）。
- 产线烧录流程（§6.2）是**流程文档**，不在固件里实现"联网换钥/在线激活"。

## 3. 时间、电量与降级（免电池终端）

- **无 RTC → `ts=0`**（服务端用接收时刻），`cap.rtc` 三态**如实声明**（已签声明，谎报会被
  `id_cap_mismatch` 抓）；**绝不改报文里的 `ts`** —— 它在签名预像里，改了验签必失败。
- **设计常态 = 每 60 s 连一次 HaLow Router**（SPEC §5.2 末段）；比它慢 = 已降速，要如实标。
- **降级下限是 L1（HS256），永不降 L3**（§8.3 里 L3 不能确认人在场）→ 宁可如实沉默，也不发
  无法确认在场的报。
- 电量写进**已签**上报的 `battery_mv`（设备不能抵赖"我快没电了"）；"没电了"与"异常失联"是
  两类（服务端据此分流告警），固件侧不许把两者混成一个行为。
- 自限频（§5.8 设备那一环）语义是**延后**不是丢弃 —— 丢自己的业务报 = 漏报。

## 4. 自检口径：「单一源 + 交叉验证」（本仓最该坚持的一条）

- 协议内核（SN 解析/Damm32/报文编解码/签名预像/CRC）**不许只靠人眼对着文档写**：
  沿用 `orpah-over-halow/c/` 的套路 —— 同一批**黄金样本/测试向量**（入库的 `test_vectors*.txt`），
  **Python 参考实现 vs C 实现零偏差**，用 `run_cross_test.py` 一键跑（PC 上离线即可）。
- 本仓将来加 `run_checks.py` 时，判定口径与上游一致：**退出码 0 且输出无 `FAIL`/`Traceback`**
  （有些脚本自己吞异常还会往下跑）。
- **常量/阈值不许拍脑袋**：照上游做法先量（零假设分布/实测），并在注释里写"怎么量出来的"。
  常态周期这类基准要有**单一源**（一处定义，多处引用）。
- 真机结论只认**实测**（哪块板、哪版固件、什么命令、什么现象）；没做过就写"未做"。

## 5. 仓库与编码约定

- **许可 = Apache-2.0**（用户 2026-09-14 定；`LICENSE` 与 `orpah-over-halow` 逐字节相同）。
- **路线图见 `ROADMAP.md`**（真机三步走 a→e；每步只换一样东西，判据固定为
  「周期上报 + 服务端验签通过 + 下行真到达 + 上游零丢弃」）。
- 文本一律 **UTF-8 无 BOM**；C 源码用 **LF**（与 `halow-demo` 一致，避免整文件 diff）。
- **不要用 PowerShell 改含中文的源码**：`Get-Content` 默认按 ANSI 读 → 会把 UTF-8 中文变成乱码且
  编译直接语法错。改源码一律用编辑器工具。
- 打印中文的脚本先 `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`（Windows 控制台是 GBK）。
- **`.gitignore` 必须显式写**：`.vscode/`、`build/`、`*.o/*.elf/*.bin/*.hex/*.map/*.lst/*.d`、
  `__pycache__/`、`*.pyc` —— 上游踩过"迁出子集时规则不跟着走 → 首次 `git add -A` 把 27 个 `.pyc`
  吃进库"。**首次提交前先看一眼 `git status --short`。**
- **★ 只提交「我自己创建的文件」**（2026-09-18 用户定，**适用于所有项目**）：我写的源码/文档/脚本 =
  我们的成果（有版权），可以入库；**不是我创建的一律不提交** —— 厂商手册 PDF、SDK 源码、固件 bin、
  安装包、下载的第三方素材**都没有版权**，入库即无授权传播。提交前 `git rev-parse --show-toplevel`
  确认仓库，**禁止 `git add -A`**（一律显式写路径），提交后 `git show --stat` 核一遍清单。
  ★ 反面教训（2026-09-16）：就是在这个仓提交文档时，终端丢掉了命令开头的 `Set-Location` → `git add -A`
  落到了 `F:\git\tianlu`（19 个厂商 PDF + 整个 SDK 投放物，6937 文件被提交成 `e2e8a89`）；
  当晚我还核查了错的仓库并误报「没有发生」。2026-09-18 已 `reset --mixed` 撤销 + 清对象。
- **push 永远由用户自己做**；改公共/接口（帧格式、协议字段、目录结构）前**先与用户对齐**。

## 6. 协作纪律（沿用用户 2026-09-06 定的防过拟合规则）

1. **Done 即停**：用户认可的结果，除非明确要求，不再"顺手优化/兜底/加固"。
2. **一次一改**：一次只动一个变量，等确认再动下一个；不连续叠加补丁。
3. **过拟合信号 = 停手信号**：修一个小问题要叠第 2 个以上补偿性改动 ⇒ 停下、回退、重新对齐。
4. **以实测/用户观察为准**：自己渲染/推演的结果不能当"事实"。
5. **口径不确定先问清**（物理/协议事实），不要自由发挥。

## 7. 目录约定（提案；**落代码时再建**，空目录 git 也跟不住）

```
orpah-client-demo/
├── firmware/     # CH32V203 固件（Makefile/启动/驱动/业务状态机）
├── proto/        # 协议内核 C 实现 + 测试向量 + 跨语言交叉测试（与 Python 参考零偏差）
├── tools/        # 上位机/联调脚本（host 数据口、串口、真机轮询）
├── docs/         # 硬件说明、接线、实测记录、踩坑
└── hardware/     # 原理图/BOM/结构（如有）
```

## 8. 现状与未做（如实）

- **2026-09-14**：本仓骨架已就位：`AGENTS.md`（本文件）+ `README.md` + `ROADMAP.md` + `.gitignore`
  + `LICENSE`(Apache-2.0)。当时**没有任何代码、没有硬件实测、没有上机验证。**
- **2026-09-22（c4-γ-1）**：设备侧**流水线**已接进固件主循环 —— `proto/id_build.{h,c}`（选级 → 演示密钥
  → nonce → `idr_build` → 链路信封 → 以太帧；**与 PC 侧交叉测试同一份源码**）+ `Core/id_core.c`（周期/
  自限频/控制台）。控制台新增 `id` / `idsend` / `idhex` / `idmodes` / `idlevel <m>`；上机判据工具
  `tools/check_report_hex.py`（把 `idhex` 的帧交给上游 `verify_report` 判一次）。判据（PC）：
  `python proto\run_cross_test.py` → exit 0，**15 组**，含★上游接受 C 产出的**整帧**（level=0/1/2）。
  ★ 两条**硬教训**（已写进 `proto/README.md`）：① 结构体里的“已派生/已填充”标志**必须显式清零**，
  否则同进程第 2 次调用会拿栈垃圾当密钥（一次一进程的 CLI 恰好掩盖它）；② jcs 家族输出**不带结尾
  NUL** ⇒ 只能按返回长度读，`strlen()` 会读到上一行残留。
  ⚠ **未上机**；`se_ok` 是**软件 P-256 替身**（level=0 属演示级）、nonce 是**软熵后端**（非生产强度），
  两者都如实打在启动横幅/控制台 `id` 上。
- **2026-09-22（c4-γ-1 台架 + c4-γ-2 代码）**：γ-1 **上机两条判据都过** —— 上游 `verify_report` 接受
  （level=0/trust=high，`build_ms≈9.1 s`）+ **模组侧确认** COM24 `[mbus rx] 403 byte(s)`（8 B HGIC 头
  + 395 B 帧）。顺带实测把一条旧口径纠正了：**模组的 cookie 顺序检查是「按数据通道」**（只比上一条
  `FRM2`，命令帧不参与）⇒ 固件把 CMD 与 FRM2 的计数器**拆开**（`Periph/hgic_uart.c` 文件头 ★3）。
  **c4-γ-2（代码已就位、PC 判据已过、★上机未做）**：`payload.nonce` 换成 **ATECC608B `Random(0x1B)`** ——
  新增 `Periph/i2c.c`（硬件 I2C1 @ `PB6/PB7`）、`Periph/atecc.c`（唤醒脉冲/事务/自检/nonce provider）、
  `proto/crc16.c` + `proto/atecc_msg.c`（**纯函数层**，可 PC 对拍）、`id_build` 的 **nonce provider 注入点**
  （不注入 = 软熵 ⇒ PC 侧仍可复现）；交叉测试 **16 组** exit 0。
  ★ **纪律提醒**：命令集事实取自**公开的 CryptoAuthLib**（我们那份 `ATECC608B.pdf` 是 **NDA 摘要版**、
  没有命令集）—— 每条都得**标出处**（见 `docs/atecc608b-se.md` 第 3 节）；且**签名仍是软件替身**，
  别把「nonce 来自 SE」写成「已启用 SE」。
- **2026-09-21（更新）**：上面那条已过时 —— `firmware/` 已在真机上跑通（c3-2b）：控制台 + 1 ms 时基
  + 心跳灯（板载 `D1`=PA15）+ **模组数据口**（USART2 ↔ TX-AH，HGIC 双向通）；
  协议内核 `proto/` 与 Python 参考零偏差（`python proto\run_cross_test.py` → exit 0）。
  现场记录（接线/判据/**四个真凶**：链接基址 0x0、`IRQn_Type` +16、`mstatus` MPP、TIM `INTFR`）
  见 `docs/c3-2b-bench-bringup.md`；**c4（选级/无 RTC/自限频/已签上报）仍未上机**。
- **a 步（客户端设备仿真器）已在 `orpah-over-halow` 完成**（本仓无代码交付）：
  `client_sim.py`（`DeviceSim`，可换传输）+ `demo_client_sim.py`（端到端验收：验签通过 /
  下行到达 / 零丢弃）+ `docs/client_sim.md`（含 b–e 各步的判据与两条候选物理通路）。
- **2026-09-23（B 方案：CH347F 当独立 I²C 主机）**：为了让「SE 不应答」不再只由我们固件背锅，加了
  `tools/ch347_i2c.py` + `docs/ch347f-i2c-crosscheck.md` + 接线图 `hardware/wiring/cstep-ch347f-atecc608b.{fzz,svg}`
  （**图上没有 nano**；规格已登记进 `tools/fzz_nets.py`，**6 张图全部核对通过**）。当天实测：
  **正对照**（EVT 板载 24C02 `0x50`）能读能写 ⇒ 主机/DLL/总线/上拉没问题；但 `0x64` 那个器件
  **写命令 60/60 全部 ACK、读回来的字节永远一样（`04 FF 01 42`）、CRC 一次都没对上**，
  而且**换一颗芯片后数字逐字节不变** ⇒ **`0x64` 到底是不是 SE 仍未判定**（缺「把 SE 整块拔下来
  再读同一地址」的对照）。SE 供电实测 **3.29 V 稳定**（"没供电"已排除）；取电点现在是
  `P5` 第 4 脚 `VIO` —— **下次应改回 `P2` 末端的 3V3**（见该文档 §5）。
  ★ 教训：**同一时刻只允许一个进程**碰 CH347（当天两个后台工装互相踩，现象与"夹具脆弱"无法区分）。
- **未做**：b 步的物理传输（USB→SPI 走 MACBUS 或 RJ45 桥，**通路未定**）、固件主循环/状态机、
  SE（ATECC608B）驱动、产线烧录流程、低功耗与取能标定、与 `orpah-over-halow`（服务端/仿真器）
  联调、与 `orpah-openwrt-demo`（路由器侧）对接、真机烧录实测。

## 9. 文档/README：图文并茂（2026-09-21 用户定，适用于所有项目）

- **本仓每个 README（顶层 + 子目录）都要把「这样的图」嵌进正文**：接线图 / 原理框图 / 界面截图 /
  实物照片 —— 用户原话「**图对人的阅读吸引力更大**」；**别只给文字表格、也别只放一个链接**。
- 写法：markdown 相对路径直接嵌（`[![caption](x.svg)](x.svg)`；`.svg` GitHub 能渲染），
  图旁配一句 caption（“这张图画的是什么、该看哪几根线”）。
- **图要与文字规格一致**：图的权威内容 = **可复验的源**（见 §1 的网表核对口径）；
  手工加在导出 `.svg` 上的标注要写明「手加、重导出会丢」。
- 本仓现成素材：`hardware/wiring/*.svg`（b/c 步台架 4 张接线图）、
  `docs/c3-2b-bench-bringup.md`（上机记录，可配图）。
