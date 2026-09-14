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
  与客户端直接相关的两条：① TX-AH 的 fmac 固件**AT 层没有用户数据命令** → 业务 payload 只能走
  **host SDIO/SPI（MACBUS）**（这正是 `host_bus.py` 那套帧的语义）；② **真机一次只应答一条 AT 查询**
  （背靠背发会吞后面那条）→ 轮询/配置必须逐条错开。

## 1. 硬件与工具链（2026-09-14 用户定）

- **MCU = CH32V203**（QingKe V2 内核）、**安全元件 = ATECC608B**（协议 §5.3 的 SE）、
  **射频模组 = 泰芯 TX-AH**（AH-SDK V2 系列，与 `halow-demo` 的真机档案 `txah` 同族）。
- **工具链沿用 `halow-demo/simulator/firmware` 那套**：`riscv-none-elf-gcc`（MounRiver 自带），
  `-DWCH_INTERRUPT_FAST` + 启动文件里的硬件栈/嵌套配置 ⇒ **必须用 MounRiver 工具链**（xPack 版会跑飞）。
- **裸机约束照抄参考固件**：无 RTOS、`-nostdlib`、自包含寄存器定义、主频 8 MHz HSI（无 PLL）。
- **参考骨架 = `halow-demo/simulator/firmware/`**（Makefile / `ld/link.ld` / `startup/` / `Core/board.h`
  / `Periph/{gpio,uart,spi_slave}` / 状态机）：**复用其套路**（含 SPI1 从机 host 接口、UART2 虚拟空口、
  AT 引擎、迷你 printf），不要另起一套目录/驱动风格。
- **烧录由用户执行**（WCH-Link/SWD：`openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg
  -c "program build/xxx.bin 0x08000000 verify reset exit"`；或 MounRiver 下载按钮）。
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

- 文本一律 **UTF-8 无 BOM**；C 源码用 **LF**（与 `halow-demo` 一致，避免整文件 diff）。
- **不要用 PowerShell 改含中文的源码**：`Get-Content` 默认按 ANSI 读 → 会把 UTF-8 中文变成乱码且
  编译直接语法错。改源码一律用编辑器工具。
- 打印中文的脚本先 `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`（Windows 控制台是 GBK）。
- **`.gitignore` 必须显式写**：`.vscode/`、`build/`、`*.o/*.elf/*.bin/*.hex/*.map/*.lst/*.d`、
  `__pycache__/`、`*.pyc` —— 上游踩过"迁出子集时规则不跟着走 → 首次 `git add -A` 把 27 个 `.pyc`
  吃进库"。**首次提交前先看一眼 `git status --short`。**
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

- **2026-09-14**：本仓刚建立，**只有一个空提交之前的骨架**：`AGENTS.md`（本文件）+ `README.md`
  + `.gitignore`。**没有任何代码、没有硬件实测、没有上机验证。**
- **未做**：协议内核 C、固件主循环/状态机、SE 驱动、TX-AH host 数据口对接、与
  `orpah-over-halow`（服务端/仿真器）联调、与 `orpah-openwrt-demo`（路由器侧）对接、真机烧录实测。
