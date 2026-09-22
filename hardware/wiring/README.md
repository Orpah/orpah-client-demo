# 台架夹具接线图（b 步 / c 步）

本目录放 **台面夹具的接线图** —— Fritzing 工程 + 它的 SVG 导出。
文字版接线表：b 步在 [`../../docs/txah-uart-macbus.md`](../../docs/txah-uart-macbus.md) §3（单块）与
§7（两块模块端到端）；**c 步（自家固件上机）在 [`../../docs/c3-2b-bench-bringup.md`](../../docs/c3-2b-bench-bringup.md) §1**。
本文件是那些表的**图版**，并记录**这套接线实测能达成什么**。

| 文件 | 说明 |
|---|---|
| `bstep-ch347f-txah-evb.fzz` / `.svg` | **单模块**夹具：CH347F ↔ 一块模组（P2 数据口 + P3 AT/打印口）。用来验桥与线 |
| `bstep-ch347f-2txah-evb.fzz` / `.svg` | **两块模块**夹具（b 步数据面）：CH347F 的 P2 带**客户端(STA)**、P3 带**对端(AP)**，两块各自的 USB 接 PC 看 AT/打印 |
| `bstep-ch347f-txah-evb-thrj45.fzz` / `.svg` | **客户端 ↔ T-Halow-RJ45** 夹具（2026-09-20 实测用的那套）：客户端 STA 只接 CH347F 的数据口；TH-RJ45 当 **AP**，它的 **RJ45 空着**，只用 USB-C 看 AT |
| `cstep-ch347f-txah-evb-nanoch32.fzz` / `.svg` | ★ **c 步台架**（2026-09-21 上机验证过的那套、**c4 上机也用这套**）：**nanoCH32V203（跑自家固件）** + TX-AH EVB + CH347F 两个 PC 窗口 |
| `cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz` / `.svg` | **c 步台架 + 安全元件**（c4-γ-2 用，2026-09-22）：上面那套**只多一件** —— 面包板上的 ATECC608B 转接板 + 两只 4.7 kΩ 上拉 |

**每张图都直接嵌在下面各自那一节里**（点图可看原尺寸 `.svg`）—— 看图不必装 Fritzing。

前两张图都**内嵌**用到的部件（自包含）；`.svg` 是同图的导出，文档里直接看不必装 Fritzing。
⚠ 第三张（`…-thrj45`）**没有内嵌 `T-Halow-RJ45` 部件**（它引用本机 Fritzing 的
`parts/user/T-Halow-RJ45.fzp`）⇒ 别人打开那个 `.fzz` 会**缺件**；分享只看它的 `.svg`（自包含）。

**接线正确性怎么复验（不靠肉眼数线）**：

```powershell
python tools\fzz_nets.py
```

脚本按 Fritzing 自己的存储方式把 `.fzz` 解成网表（含每条 wire 自身导通两端 + 部件内部 `<bus>`），
再与脚本里的**期望表**逐网比对 —— 期望表就是这两个夹具的接线规格（单一源）。
**2026-09-19 实跑：两张图均「全部对上 ✓」（退出码 0）。**

---

## ★ c 步台架（`cstep-ch347f-txah-evb-nanoch32`）—— 自家固件上机 / c4 上报也用这套

[![c 步台架：nanoCH32V203（自家固件）+ TX-AH EVB + CH347F 两个 PC 窗口](cstep-ch347f-txah-evb-nanoch32.svg)](cstep-ch347f-txah-evb-nanoch32.svg)

三块板、**三根 USB**、各自供电、**只共地**（图里没有电源线 —— 每块板走自己的 USB 取电）。
左边 `nanoCH32V203` = 跑**本仓固件**的那块 MCU，右边 `TX-AH EVB` = 模组，中间 `CH347F-EVT`
把两个 UART 变成两个 PC 窗口。

| 用途 | nano 侧 | 模组侧 | PC 侧 | 备注 |
|---|---|---|---|---|
| **数据口**（HGIC，二进制帧） | `PA2`(USART2_TX) → | `A10`（**模组 RX**） | — | 115200 8N1 |
| | `PA3`(USART2_RX) ← | `A11`（**模组 TX**） | — | 方向固定，别接反 |
| **窗口①控制台** | `PA9`/`PA10`(USART1) | — | CH347F **P2/UART0 = COM23** | 固件横幅、命令（`help`/`stat`/`id`…）、`[id] sent …` |
| **窗口②模组口** | — | `A12`(RX)/`A13`(TX) | CH347F **P3/UART1 = COM24** | 模组的 AT 与日志（`[mbus rx]/[mbus tx]`） |
| **安全元件**（可选，c4-γ-2 起） | `PB6`(SCL) / `PB7`(SDA) | ATECC608B `pin6`(SCL) / `pin5`(SDA) | — | I²C1 默认引脚；**外接 4.7 kΩ×2 上拉到 3V3**；SE 的 `pin8`=3V3、`pin4`=GND |

- ★ **c4-γ-1 上机只需要这套**：窗口① 看 `[id] sent level=… build_ms=…`，窗口② 看 `[mbus rx] …`
  （= 我们那条已签报文真的进了模组）。**不需要 TH-RJ45、也不需要 RJ45 上行**——
  c4 的判据是「固件产出的帧被上游接受」，用控制台 `idhex` 把 hex 拿回 PC 跑
  `python tools\check_report_hex.py <hex>` 即可（见 `../../docs/c3-2b-bench-bringup.md`）。
  · ✅ **2026-09-22：两个窗口都拿到了** —— 窗口① `[id] sent level=0 … len=395 build_ms=9135`、
    窗口② `[mbus rx] 403 byte(s) 2b 1a 09 00 93 01 74 00 ff ff …`（403 = 8 B 头 + 395 B 帧）。
    一条命令可同时判：`python tools\check_c4g1_bench.py --port COM23 --mod-port COM24`（见
    `../../firmware/README.md` 的 c4-γ-1 条）。
- ⚠ **跳线帽要拔掉**（板载 CH340E 与 `A12/A13` 断开）；CH347F 的 `3V3`/`VIO` **空着不接**
  （它与 JP1 同轨，接过去 = 两个 3.3 V 源并联）。
- ⚠ **别给数据口（`A10/A11`）敲 AT** —— 那条口跑 8 B HGIC 头的二进制帧；AT/日志在 `A12/A13`（COM24）。
- ⚠ 模组 UART 方向（实测 + 厂商 `pin_function.c` 双证据）：**`A10` = 模组 RX、`A11` = 模组 TX**；
  `A12` = 模组 RX、`A13` = 模组 TX（见 `../../docs/txah-uart-macbus.md` §5.1）。
- ★ **ATECC608B（安全元件）的接线与命令来源见 `../../docs/atecc608b-se.md`** ——
  `PB6/PB7` → I²C，**必须有 4.7 kΩ 上拉**（没有就不能可靠工作）；SOIC-8 转 DIP 后
  `pin5=SDA / pin6=SCL / pin4=GND / pin8=3V3`。**不需要** CH347F 参与（那是给 PC 看的窗口）。

## ★ c4-γ-2 台架（`cstep-ch347f-txah-evb-nanoch32-atecc608b`）—— 上面那套 + ATECC608B

[![c4-γ-2 台架：c 步那套 + 面包板上的 ATECC608B 与两只 4.7 kΩ 上拉](cstep-ch347f-txah-evb-nanoch32-atecc608b.svg)](cstep-ch347f-txah-evb-nanoch32-atecc608b.svg)

就是上面那一套台架，**只多了右下角面包板上那一件** —— 绿色的 SOIC-8 → DIP 转接板
（板上芯片印着 `CN`，就是 **ATECC608B**）和它旁边的**两只 4.7 kΩ 上拉**。
图上其余连线与上面那张**逐根相同**（网表核过：新增的只有 SE 那 4 根 + 4 个 NC 各自孤网，
其它一根没动）。

| 用途 | nano 侧 | SE 侧（转接板） | 备注 |
|---|---|---|---|
| I²C 时钟 | `PB6`（SCL） | `pin6` SCL | 100 kHz，见 `../../firmware/Periph/i2c.c` |
| I²C 数据 | `PB7`（SDA） | `pin5` SDA | 双向 |
| 供电 | `3V3` 轨 | `pin8` VCC | 与上拉同一轨 |
| 地 | `GND` 轨 | `pin4` GND | — |
| 上拉 | `3V3` 轨 | 两只 `4.7 kΩ`：一端接 3V3，另一端分别接 SDA / SCL | **必须有**，否则 I²C 不可靠 |
| 空脚 | — | `pin1/2/3/7`（NC） | 图上各自孤网，谁都别接 |

- 这张 `.fzz` **自包含**（21 项：`part.*.fzp` ×4 + 四个视图 svg ×16，含 `ATECC608B`）✓
  —— 与「只看 `.svg` 就不必装 Fritzing」是同一句的两种说法。
- ⚠ 图里那块转接板**只有 `CN` 一个丝印**（件本身没印型号、图里也没加文字标注）⇒
  看图时认「`CN` + 8 个金焊盘」那一块；两只上拉也**没画阻值字**，值看上面表里的 `4.7 kΩ`。
- ⚠ 转接板是**横着放**的（脚号随元件转了 90°），所以脚号字在图上读起来是竖的 ——
  件本身画的是不旋转的水平字，转了是**放置**的结果。

## 接线（从 `.fzz` 里抽出来的真实连线，2026-09-16 修订）—— 单模块那张

[![单模块夹具接线图：CH347F ↔ 一块 TX-AH EVB](bstep-ch347f-txah-evb.svg)](bstep-ch347f-txah-evb.svg)

**单模块夹具**：CH347F `P2` → 模组数据口（`A10/A11`）、`P3` → AT/打印口（`A12/A13`），
共 **5 根线**（4 信号 + 1 地）；左上角那句 `fritzing` 水印是有意保留的。

**共 5 根**：4 根信号 + 1 根地。**图里没有电源线**（供电见下一节）。

| TX-AH EVB（`U2` = `TX-AH-R900PNR_rev_1`） | CH347F-EVT（`U3` = `CH347F`） | 线色 |
|---|---|---|
| `A10` = 模组 UART0 **RX**（模组收） | **`TXD0`**（CH347F UART0 TX = `P2`） | 蓝 `#418dd9` |
| `A11` = 模组 UART0 **TX**（模组发） | **`RXD0`**（CH347F UART0 RX = `P2`） | 绿 `#25cc35` |
| `A13` = 模组 UART1 TX（AT/打印口） | **`RXD1`**（CH347F UART1 RX = `P3`） | 橙 `#ef6100` |
| `A12` = 模组 UART1 RX | **`TXD1`**（CH347F UART1 TX = `P3`） | 紫 `#ab58a2` |
| `GND` | `GND` | 棕 `#8c3b00` |

- **一个 CH347F 同时管两端**：`P2`（UART0）走模组**数据口**，`P3`（UART1）走模组 **AT/打印口**
  ⇒ PC 上一个进程发数据、另一个口看日志；脚本里的 `--ch347f-com 0` / `--ch347f-com 1` 就是这两路。
- 方向口径（别接反）：`TXD0 → A10`、`A11 → RXD0`；`TXD1 → A12`、`A13 → RXD1`。
  模组侧 A10=UART0_RX / A11=UART0_TX 的出处见 `../../docs/txah-uart-macbus.md` §5.1。
- 图上 `A10/A11/A12/A13/GND` 都是 `TX-AH-R900PNR_rev_1` 里 **J4/J5 跳线区的焊盘**
  （各自独立网：它们与模组 38 个边脚**实测不通**，所以没有并成一条总线）；
  同区的 `VCC` 焊盘**本图不用**（供电各管各的）。

## 两块模块那张图（`bstep-ch347f-2txah-evb`）—— b 步数据面

[![两块模块夹具接线图：CH347F 同时带客户端(STA)与对端(AP)](bstep-ch347f-2txah-evb.svg)](bstep-ch347f-2txah-evb.svg)

**两块模块夹具**：CH347F 的 `P2` 带**客户端（STA）**、`P3` 带**接入点（AP）**；
两块各自的 `A12/A13` 接自己的 USB-UART 到 PC 看 AT/打印（图上标成 `… USB → WindTerm`）。

同样**没有电源线**（各管各的、只共地）；AT/打印口**不走 CH347F** —— 两块模块的 `J4`（= `A12/A13`）
各自接它们自己的 USB-UART 到 PC，图上用文字标成 `供电 刷固件 AT命令 打印日志` 连到 `WindTerm`。

| 谁（按**端口**认，不按实例编号） | 接到 CH347F | 图上标注 | 角色（2026-09-16 定） |
|---|---|---|---|
| **`P2` / `TXD0`+`RXD0` 那一路** | 模组 `A10`↔`TXD0`、`A11`↔`RXD0` | `客户端（STA）` | **STA**（本仓的客户端） |
| **`P3` / `TXD1`+`RXD1` 那一路** | 模组 `A10`↔`TXD1`、`A11`↔`RXD1` | `接入点（AP）` | **AP**（临时当路由器） |
| 三块板 | `GND` 互连（同一个网） | — | — |

- ⚠ **实例编号（`U4`/`U5`）会随重画变**，所以规格**按端口写**（`tools\fzz_nets.py` 也是这么核的，
  模块侧不限实例名）。2026-09-19 用户重画后是：**`U5` = `P2` = 客户端(STA)**、
  **`U4` = `P3` = 对端(AP)** —— 与图上 `客户端（STA）`/`接入点（AP）` 两个标注一致 ✓。
- 逐网复验（`python tools\fzz_nets.py`）实跑：这五组**全部对上 ✓**。
- ⚠ **GND 在图上是两条线**（两块模块各拉一条到 CH347F 的 GND 脚），但 **CH347F 部件内部
  11 个 GND 脚是同一条 `<bus>`** ⇒ 网表里它们是**同一个网**，不是两段孤立的线。
- ⚠ **板子别接反**：按这张图，**P2（`COM23`）那一路必须是 STA**（`--tx-com 0` = 从 STA 发）。
  2026-09-16 实机那次两块是反的（AP 在 P2、STA 在 P3）⇒ 要么照图重接，要么按实际接法写
  `--tx-com/--rx-com`，但**记录要与实物一致**。

## 供电：两块板各管各的，只共地

模组板走自己的电源、CH347F-EVT 走它的 USB，两者**只连 GND**。理由（都是查得到的，不是估计，
细节见 [`../../docs/txah-uart-macbus.md`](../../docs/txah-uart-macbus.md) §3.3）：

- 模组板上 `VCC` 是**板上 DCDC 的输出节点**：原理图 `AH开发板原理图_20251015150638.pdf`
  写着 `DCDC OUTPUT 3V3 FOR SYS` + `VCC(3V3) current at least 400mA. ripple<30mV`
  —— 它是**有源阻抗/纹波要求的电源输出**，不是“随便喂一下”的输入脚；
- CH347F-EVT 的 `VIO` 与 `3V3` 经 `JP1` 是**同一条轨**（实测：`JP1` 短接 `3V3`↔`VIO`，
  该轨静态 **3.28 V**）；
- 两边一接 = **两个稳压源并联在同一条 3.3V 轨上且没有仲裁**：谁电压高谁供电、另一个被倒灌；
  静态量着挺好，**发射瞬间才露馅**，症状正是“时好时坏”且说不清当下谁在供电。

本图第一版曾有一根 `VCC → VIO`（红 `#cc1414`），**2026-09-16 已按上述口径去掉**。

## 用到的部件版本（重要）

`.fzz` 会把用到的部件**连图形一起打包**，所以这个文件自身就够复现一张同样的图。本目录两张图用的是：

- 两张图用的模组部件**不一样**（都来自 `fritzing-parts-langhua` 仓的 `fzpz/`，随图内嵌）：
  - 单模块那张 = **`TX-AH-R900PNR_rev_1`**（无黄帽版，J4/J5 焊盘是普通焊盘圆，点选省事）；
  - 两块模块那张 = **`TX-AH-R900PNR_1`**（原版，同样带可接线的 J4/J5；它的两个**黄帽只遮住
    `CH340E RX/TX` 那两个脚**，本图不用那两个脚）。
- `TX-AH-R900PNR_1`（原版）从 **2026-09-19** 起也有 J4/J5 connector 了 —— 此前只有 `_rev_1` 有，
  所以本文件早先写“原版画不出这张图”，那句已过期。
- `CH347F`（`P2`/`P3` 上的 `TXD0/RXD0/TXD1/RXD1/GND` 都在）。

> ⚠ 因为是**快照**：部件以后改版，这张图**不会自动跟着变**。要跟上就重新导入部件、重画连线再存一次；
> 反过来说，这也是它的优点 —— 这张图能把"当时那版"完整冻住。

## 怎么用

- **看/改**：Fritzing 打开 `.fzz`（本图由 Fritzing 1.0.3b 保存）。
- **文档里引用**：用 `.svg` 就行（别人不必为了看这张图去装 Fritzing）。
- 图里那个 `PCB1`（空白矩形 PCB）没有任何连线，是装饰/占位，删掉不影响接线。
- ★ **左下角的 `fritzing` 水印是用户 2026-09-20 有意保留的** —— 明示"这图是用 Fritzing 一根线
  一根线画出来的"。**别当残留顺手删掉。**
- 图上除了连线，还有这些**文字标注**（2026-09-20 版，供读者对照）：
  `客户端（STA）` / `接入点（AP）`（哪块板是什么角色）、三根 `… USB → WindTerm`（标注
  `供电 刷固件 AT命令 打印日志` = 板载 `CH340E` 那条 **AT/打印口 UART1（`A12/A13`）**）、
  数据线上的 `HGIC/MACBUS二进制帧`（= **数据口 UART0（`A10/A11`）** 走 CH347F）、
  两块板之间的 `HaLow 空口(802.11ah) 908/916/924`，以及
  `注意：本方案仅验证了 STA→AP 方向`（就是下面 ★ 那一节的结论，图里只留一行指针）。

## ★ 这套接线能达成什么（2026-09-16 实机结果）

前提：载荷 = **`FRM2`（8 B HGIC 头）+ 完整以太帧**；两块各自 `AT+WIFIMODE=sta/ap` +
同 SSID + **同 `AT+CHAN_LIST`**；`AT+ENCRYPT=0`。（协议细节见 `../../docs/txah-uart-macbus.md` §四/§七）

| 半条链路 | 结果 |
|---|---|
| STA 主机口 → 空口（客户端**发**上行） | ✅ 通，逐字节到达对端（AP）主机口 |
| AP 空口 → AP 主机口（路由器侧**收**） | ✅ 通（就是上面那次实测的另一半） |
| **AP 主机口 → 空口**（路由器侧**发**下行） | ❌ **不通**：帧进了 AP 主机口（`[mbus rx]` 在），但 LMAC `tx cnt` 几乎不涨、STA 主机口 **0 字节** |
| STA 空口 → STA 主机口（客户端**收**下行） | ⚠ **未验**（对端发不出来，没机会测） |

- 结论：**这套两块 FMAC 的夹具够用来验证「客户端上行」与「客户端收空口」**，
  但**不能**当 b 步的完整下行源。不是帧格式/地址/cookie 的问题（7 类原因已逐项排除），
  是这版通用 FMAC v2.4.1.5 在 **AP 模式下不做「主机口 → 空口」转发**
  （`WIFI_WNBAP_SUPPORT 0`、`bridgeif.o` 没编进 map）；「RJ45 ↔ HaLow 二层透传」是
  **WNB 固件**那条产品线的能力（宿主是以太网 GMAC）。证据见 `../../docs/txah-uart-macbus.md` §7.4/§7.5。
- ~~**下一步（2026-09-16 定）**：路由器侧换 **T-Halow-RJ45 板**（WNB 固件、天生 RJ45↔HaLow），
  用它的 `AT+TXDATA` 当“下行帧源”~~ ⇒ **2026-09-20 更正：这条路线作废** ——
  两块 V2.4 构建的 AT **都没有数据面命令**（`AT+TXDATA`/`AT+RXDATA`/`AT+SOCKET`/`AT+DHCP` 全静默），
  且上游 `T-Halow-RJ45/docs/ethernet_bridge_linux.md` 明确说**别用它**
  （*“a manual, single-frame debug interface (it enters a sticky data-mode and **rewrites the EtherType**)”*）。
  实际做法与结果见下一节（**第三张图**）；同样，下面“在 AP 那块的 AT 口连续发 `AT+TX_*`”也**未验证且不推荐**。
- 只想先验“客户端收空口”、不换板：在 **AP 那块**的 AT 口连续发
  （`AT+TX_DST_ADDR` = STA 接口 MAC、`AT+TX_LEN`/`AT+TX_TYPE` 设好，再 `AT+TX_CONT=1` + `AT+TX_START=1`），
  在**客户端主机口**听 `1a 2b`；同时看客户端的 `AT+RX_PKTS` 是否增长。
- 一键判据（两块都在 CH347F 上时）：
  `python tools\probe_txah_uart.py xfer --tx-com 0 --rx-com 1 --text HELLO-ORPAH --secs 8`

## ★ 第三张图：客户端(STA) ↔ T-Halow-RJ45(AP) —— **2026-09-20 实测：下行也通了**

[![客户端(STA) ↔ T-Halow-RJ45(AP) 夹具接线图](bstep-ch347f-txah-evb-thrj45.svg)](bstep-ch347f-txah-evb-thrj45.svg)

**第三张图**（2026-09-20 实测用的那套）：客户端（STA）只接 CH347F 的数据口；
**TH-RJ45 当 AP，它的 RJ45 空着**（只用 USB-C 看 AT）。⚠ 这张 `.fzz` **没有内嵌 `T-Halow-RJ45` 部件**
（引用的是本机 Fritzing 的 `parts/user/T-Halow-RJ45.fzp`）⇒ 别人打开会缺件，**分享只看它的 `.svg`**。

```
客户端 TX-AH EVB（FMAC，STA）  ←空口 908.0MHz/bw8/open→  T-Halow-RJ45（WNB，AP）
PC ──CH347F UART0── 客户端 UART0(A10/A11)   数据口（HGIC）
PC ──USB-UART───── 客户端 UART1(A12/A13)   AT/打印口（逐帧 [mbus rx]/[mbus tx] 日志）
PC ──USB-UART───── TH-RJ45 的 USB-C         AP 的 AT 口（STA1: / rx1: / tx1:）
TH-RJ45 的 **RJ45 空着**（本方案不用它；下行由 AP 侧协议栈自己产生）
```

| 步骤 | 实测 |
|---|---|
| PC 发 3 条以太帧（ARP / DHCP DISCOVER / 0x88b5，`FRM2` + 完整以太帧） | 客户端 AT 口逐条 `[mbus rx] 50 / 294 / 41 byte(s)` ✓（= 8B HGIC 头 + 载荷） |
| 上行过空口 | AP per-STA `rx1_cnt 11 → 12` ✓；**且 DHCP 应答本身已反证上行到过 AP** |
| **下行到达** | 数据口收到 `FRM2`：`src=d6:a2:2a:82:67:c0`（**AP 的 MAC**）的 **DHCP 应答**（UDP 67→68）✓ —— 这条补上了上面表里“STA 收下行”那格 |

- 一键复现：`python tools\hgic_loop_test.py`（默认 `--ap-port COM8 --at-port COM6 --data-port COM23`；
  退出码 0 = 闭环成立）；**每条帧都以模组自己的 `[mbus rx] <8+len> byte(s)` 日志确认为准，否则重发**
  —— 兜住 CH347F 的 VCP 周期性抽风（I/O 报 `PermissionError(13)`）。
- 细节与五条硬规矩（**改角色后不能复位** / `AT+RSSI` 恒 0 不能判关联 / 判下行别用 `tx1` /
  两块 V2.4 的 AT 无数据面命令 / CH347F 的 VCP 会抽风）见
  [`../../docs/txah-uart-macbus.md`](../../docs/txah-uart-macbus.md) §八。
- ⚠ **仍未验证**：从 **AP 侧主机口（TH-RJ45 的 RJ45）注入**的帧能不能下行 —— 本机没有有线网卡，没接。
  （router 侧的产品形态走的正是这条路；AP 的 RJ45 ↔ 空口是厂商文档写的 L2 透明桥。）

## 边界（如实）

- 这两张图记录的是**这套台面夹具怎么接**，不等于链路已验证：上表是**已有实测**的那部分，
  「下行」那两格仍是**未通/未验**。
- 图上只有“接线”，**没有**画：两块板各自的 USB 供电与 USB 线（刷机、看日志用，图上只写了 WindTerm）、
  以及调试时要**拔掉 TF 卡**这条环境项。
- **跳线帽保持缺省位置**（不要拔、也不要挪）：缺省那组 = 板载 `CH340E` ↔ `A12/A13`（AT/打印口），
  **不碰数据口 `A10/A11`**，与 CH347F 不冲突（2026-09-16 实测更正；
  挪到"上-中"那组才会与 CH347F 抢线 —— 那是拿板载 USB 当数据口的用法）。
- 两条容易混的：① **AT 层没有发数据命令**（`AT+TXDATA` 在 FMAC 固件上静默）—— 那是 AT 控制面的事，
  不等于主机口不能发数据；② 载荷必须是**完整以太帧**，裸载荷不通（模组是二层桥，靠目的 MAC 决定发到哪）。