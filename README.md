# orpah-client-demo — ORPAH 客户端（硬件 + 固件）

ORPAH（用无线技术找人）五仓里的**客户端侧**：**CH32V203 + 安全元件 ATECC608B + 泰芯 TX-AH
（802.11ah HaLow）模组**。目标是一台**免电池/低功耗**的客户端设备：周期性向 HaLow 路由器
上报「我还在」（Orpah ID 签名上报），必要时按电量降级，并配合服务端做定位与搜寻。

> **现状（2026-09-14）：本仓刚建立，只有规则与骨架（`AGENTS.md` / `README.md` / `.gitignore`）。**
> **没有代码、没有硬件实测、没有上机验证** —— 任何"已支持/已验证"的说法在目前都不成立。

## 五仓分工（改动该往哪个仓放）

| 仓库 | 内容 | 与本仓的关系 |
|---|---|---|
| `Protocol`（`F:\git\Protocol`） | 通信协议规范：`docs/OrpahIDProtocol.md`（SN/校验/签名/密钥/降级/限频）、`docs/orpah-over-halow/SPEC.md` | **唯一权威**，固件严格遵循 |
| `orpah-over-halow`（`F:\git\orpah-over-halow`） | 业务全链路：服务端（UDP 19447）、Router 桥、走失表、Orpah ID 验签、定位/告警、Web UI、`host_bus.py`、`client.py`（PC 参考客户端） | 参考其协议定义与仿真器接口；**与本仓联调** |
| `halow-demo`（`F:\git\halow-demo\simulator`） | 空口/设备侧：`host/sim.py`（空口 + host 数据口）、`tools/ui/`、`firmware/`（CH32V203 参考固件）、真机实测记录 | 复用其**测试数据**与**固件骨架** |
| `orpah-openwrt-demo`（`F:\git\orpah-openwrt-demo`） | 路由器端 OpenWrt 软件（daemon + 包壳 + LuCI） | **与本仓对接测试** |
| **本仓** | **客户端硬件 + 固件** | — |

## 硬件形态（2026-09-14 用户定）

- **MCU**：CH32V203（QingKe V2，64K Flash / 20K RAM 级别），裸机、无 RTOS、`-nostdlib`。
- **安全元件（SE）**：ATECC608B —— 协议 §5.3 指定的 ECDSA P-256 载体（私钥不可导出）。
- **射频模组**：泰芯 **TX-AH**（AH-SDK V2 系列）。⚠ 该模组的 fmac 固件**AT 层没有用户数据命令**
  → ORPAH 的报文只能走 **host SDIO/SPI（MACBUS）**，其帧格式与 `orpah-over-halow/host_bus.py`
  一致（`AA 55 TYPE LEN_H LEN_L CRC`，CRC-8/ATM 0x07）。这条来自 `halow-demo` 的真机实测。
- **工具链**：`riscv-none-elf-gcc`（**MounRiver** 自带；`-DWCH_INTERRUPT_FAST` 必须用它）。
- **烧录**：WCH-Link + OpenOCD（或 MounRiver 下载按钮）—— **由用户执行**。

## 固件要实现的（来自规范，逐条对着读）

| 主题 | 规范位置 | 客户端要做的事 |
|---|---|---|
| SN 编码/字符集 | `OrpahIDProtocol.md` §2 | 解析 `CC-ORG-UNIQUE[-CHECK]`，只认合法字符集，不套 Crockford 限制 |
| 校验码 Damm32 | §3 / 附录 B | 只算 `ORG-UNIQUE`（**不含 CC**） |
| Unicode 规范化 | §4 | 上报前按规范规范化字符串 |
| 签名与报文 | §5.1 / §5.4 | 组报文、算预像、ECDSA P-256 签名（SE）、`b64url` 编码 |
| 防重放/时间 | §5.5 | nonce/时间窗；**无 RTC 时 `ts=0`**，不改报文里的 `ts` |
| alg 白名单 | §5.6 | 只发白名单内的算法；ES256 优先，降级到 HS256 为下限 |
| 限频 | §5.8 | 设备侧自限频（**延后**而非丢弃） |
| 密钥 | §6.1/§6.2/§6.3/§6.4 | 1 把 P-256 私钥 + 1 把 HMAC，**一代终身不轮换**；私钥不可导出 |
| 降级 | §8.1/§8.2 | 四级降级流程；**永不降到 L3**（不能确认在场时宁可沉默） |
| 能量/覆盖 | `SPEC.md` §5.2 (E1–E4) | 由能量决定上报间隔与降级；设计常态 **60 s** 一次连接；沉默要能归因 |
| host 数据口 | `SPEC.md` §6 / `host_bus.py` | 帧格式与语义对齐 SPI MACBUS `DATA_TX`/`DATA_RX` |

## 目录（提案，落代码时再建）

```
orpah-client-demo/
├── firmware/     # CH32V203 固件（Makefile/启动/驱动/业务状态机）
├── proto/        # 协议内核 C 实现 + 测试向量 + 跨语言交叉测试
├── tools/        # 上位机/联调脚本（host 数据口、串口、真机轮询）
├── docs/         # 硬件说明、接线、实测记录、踩坑
└── hardware/     # 原理图/BOM/结构（如有）
```

## 构建与烧录（**照抄参考固件的做法；本仓尚未实测**）

```bash
# 参考 halow-demo/simulator/firmware 的构建方式（本仓 firmware/ 落地后同理）
cd firmware
make                        # -> build/xxx.elf / .bin
make RISCV_PREFIX="C:/MounRiver/MounRiver_Studio/toolchain/RISC-V GCC Toolchain/bin/riscv-none-elf-"
make clean
```

```bash
# 烧录（用户执行）
openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg \
        -c "program build/xxx.bin 0x08000000 verify reset exit"
```

## 自检（本仓口径）

- **协议内核必须跨语言零偏差**：同一批黄金样本/向量，Python 参考实现（`orpah-over-halow`）vs
  本仓 C 实现，一键交叉测试（沿用 `orpah-over-halow/c/run_cross_test.py` 的套路）。
- 将来加 `run_checks.py`：判定 = 退出码 0 **且** 输出无 `FAIL`/`Traceback`。

## 还没做（如实）

- 协议内核 C、固件主循环/状态机、SE 驱动、TX-AH host 数据口对接；
- 与 `orpah-over-halow`（服务端/仿真器）联调、与 `orpah-openwrt-demo`（路由器侧）对接；
- 真机烧录与实测、功耗实测（免电池取能曲线）、产线烧录流程落地。

## 许可

尚未定（`orpah-over-halow` 用 Apache-2.0）—— **与用户确认后再加 `LICENSE`**。
