# TX-AH 主机口走 UART（mac_bus）——b 步候选通路的新首选

> 2026-09-16。背景与三条通路的可行性见 `ROADMAP.md` §二「2026-09-16 复核」；
> 本文件记**怎么落地**：固件、接线、协议要点、判据、未做。

## 一、为什么是 UART（而不是 SPI/SDIO）

模组固件 FMAC v2.4.1.5 的 `mac_bus` 只实现了 **SDIO / UART / USB**；
**没有 SPI 主机口实现**（手册说「SPI 与 SDIO 是同一固件」= SDIO 控制器的 SPI 模式，
线上是 SD/SDIO-over-SPI，要原厂主控驱动）。
三条里只有 **UART** 的线上协议我们**有源码**（HGIC 帧），所以主机侧（PC 或以后的
CH32V203）能自己实现 —— 这也是 b 步里唯一"不用等原厂"的一条。

## 二、固件（已编好，未烧）

在 `halow-demo` 仓的 `TXW8301` 目录（模组固件侧）：

```powershell
cd F:\git\halow-demo\TXW8301
python tools\fmac_macbus_switch.py status     # 现在开的是哪个主机口
python tools\fmac_macbus_switch.py uart       # 切 UART（改 project_config.h 两行宏，自动 .bak-sdio 备份）
python tools\fmac_macbus_switch.py restore    # 还原
```

- 2026-09-16 已切到 UART 并**编过**：产物 `FMAC_SDK\project\txw8301_v2.4.1.5-39777_2026.9.16_.bin`
  （365584 B，sha1 `06ea7ec83074…`），特征串 = `uart bus fixlen` + `mac_bus_uart_attach`
  （旧 bin 全是 `mac_bus_sdio_attach`）。
- 构建/烧录/回退的完整说明与两条环境坑（make 要用 CDK 自带的 3.82.90 + msys sh）见
  `halow-demo/TXW8301/README.md`。
- 烧录（**由用户执行**）：串口连 AT 口 → `at+fwupg` → SecureCRT 脚本「发送 Xmodem」选那个 bin。
- ⚠ **未烧录、未上机**：这一节只说明"能编出来且宏切对了"。

## 三、接线

### 3.1 CH347F-EVT 的排针（哪一排是什么）

| 排针 | 内容 |
|---|---|
| **P2** | **UART0**（7 针，丝印 `DTR0 CTS0 RTS0 TXD0 RXD0 GND 3V3`）← **数据口用这排** |
| **P3** | **UART1**（7 针，丝印 `DTR1 CTS1 RTS1 TXD1 RXD1 GND 3V3`）← AT/打印口用它 |
| P4 | SPI（`SCS0 SCK MISO MOSI`） |
| P5 | I2C（`VIO SCL SDA GND`），旁边 JP1 = VIO 选择（**短接 = `VIO` 与板 `3V3` 同轨**；2026-09-16 用户实测该轨静态 **3.28 V**） |
| P6 | JTAG/SWD（`TMS TCK TDO TDI TRST SRST`） |
| P7 | `GND /KEY GND` |
| P8 | 板载 **SPI FLASH**（`SCS1 … SCS0`）—— **别接模组** |
| P1 | USB（接 PC） |

**P2 / P3 的脚号（板级原理图 `CH347SCH.pdf` 第 1 页；板子丝印上不印脚号）**

| 脚号 | P2（UART0） | P3（UART1） |
|---|---|---|
| 1 | 3V3 | 3V3 |
| 2 | **GND** | **GND** |
| 3 | **RXD0** | **RXD1** |
| 4 | **TXD0** | **TXD1** |
| 5 | RTS0 / GP1 | RTS1（与 SDA 复用） |
| 6 | CTS0 / GP0 | CTS1（与 SCL 复用） |
| 7 | DTR0 / GP2 | DTR1（与 SCS1 复用） |

⚠ 板子丝印的**上下顺序与脚号相反**（原理图 pin1=3V3，板面从上往下是 `DTR0 … 3V3`）——
这条是从两侧资料对照推出来的，**接线时以丝印名字为准**，不要按脚号数。

### 3.2 模组开发板侧：A10/A11 在哪、怎么引出来

模组在板上的名字**不叫 A10/A11**，叫 `AH D2` / `AH D3`（= SDIO 数据线名）。出处：
`泰芯AH模组开发板原理图`（`AH开发板原理图_20251015150638.pdf`）页 2 + 芯朋 `pin_function.c`
（`V1.2.3 09/12/2020  uart0 fix to A10/A11, uart1 fix to A12/A13`）。

| 模组脚（U1 `TX-AH-RX00P-XX`） | 板上网名 | 排针上的可达点 |
|---|---|---|
| pin 15 = **IOA10**（UART0_**RX**） | `AH D2` / `SD_D2` | **J5 pin 3**（经 R41 **1K，已贴** ✓）、CON3 pin 1（经 **R18**，**默认没贴** ✗） |
| pin 16 = **IOA11**（UART0_**TX**） | `AH D3` / `SD_D3` | **J5 pin 5**（经 R42 **1K，已贴** ✓）、CON3 pin 2（经 **R20**，**默认没贴** ✗） |
| pin 33 = **IOA12**（UART1 RX） | `IOA12` | **J4 pin 2**（"AH UART" 排针） |
| pin 34 = **IOA13**（UART1 TX） | `IOA13` | **J4 pin 3** |

**⚠ CON3 为什么不能直接引**：CON3 是"SDIO 飞线"用的 8 pin 排针，模组到它中间**串了 6 个电阻，
原理图上标注是 `NC/1K` = 默认不贴（贴的话 1K）** ⇒ 从 CON3 引线等于引到一段**断路**上。
那 6 个位置与对应的线（页 2 读出）：

| 设计号 | 哪条线 → CON3 脚 | 原理图标注 |
|---|---|---|
| **R18** | `SD_D2`（=A10）→ CON3 pin 1 | `NC/1K` |
| **R20** | `SD_D3`（=A11）→ CON3 pin 2 | `NC/1K` |
| R21 | `SD_CMD` → CON3 pin 3 | `NC/1K` |
| R23 | `SD_CLK` → CON3 pin 5 | `NC/1K` |
| R3 | `SD_D0` → CON3 pin 7 | `NC/1K` |
| R25 | `SD_D1` → CON3 pin 8 | `NC/1K` |

（CON3 其余两脚：pin 4 = `SVCC`、pin 6 = `GND`，没有串阻。）

⚠ **阻值口径不一致**：原厂《泰芯AH模组开发板使用说明》里写「SDIO 飞线焊 **22R**」、
「SPI(COM3) 飞线焊 **0R**」（同一组位置两处还不一样），而**原理图**标的是 **`NC/1K`**。
要用 CON3 就得按原理图/问原厂定 —— **我们这条路线不需要动它**（走 J5 即可）。

（顺带：主 SDIO 通路上的 R6/R7/R8/R9/R10/R11 = **22R，已贴**，那是模块 ↔ TF 卡座/CON3 的串联电阻。）

**J5 = 2×4 2.54 mm（板载 USB-UART `CH340E` ↔ A10/A11 的跳线排）**

| J5 脚 | 接什么 | J5 脚 | 接什么 |
|---|---|---|---|
| 1 | VCC | 2 | VCC |
| **3** | **`AH D2` = A10**（经 R41 1K） | 4 | `CH340E_TX` |
| **5** | **`AH D3` = A11**（经 R42 1K） | 6 | `CH340E_RX` |
| 7 | **GND** | 8 | **GND** |

**J4 = "AH UART" 4 pin**：1=VCC、2=`IOA12`、3=`IOA13`、4=GND（AT/打印口；你刷机那根线走这里或
板上的 USB 座）。J10 = 单针 GND。

⇒ 所以数据口有两种接法：

- **A. 先不碰 CH347F**：把 **J5 的 1-2 / 3-4 / 5-6 / 7-8 都插上跳线帽** ⇒ 板载 USB-UART 直接连到
  A10/A11 ⇒ 你刷机那个 COM 口就是数据口，直接 `--port COMxx probe`。
  （此时 AT/打印口 `A12/A13` 就没接了；想同时看日志，把 `J4` 接另一路 USB-TTL。）
- **B. 终态（CH347F）**：**J5 的 pin 3 → CH347F P2 `TXD0`；pin 5 → P2 `RXD0`；pin 7(或 8) → P2 `GND`**，
  **J5 不插跳线帽**（别让板载 CH340E 跟 CH347F 抢同一对线 —— 两路一起驱动会互相打架）。
- ⚠ CON3 那个 SDIO 飞线排虽然也有 `SD_D2/SD_D3`，但**默认电阻没焊**（原厂说明要焊 22R/0R），
  不如走 J5。
- A10 在固件里带 **10 k 上拉**（RX 空闲高，正常）；板上那 1 K 串阻对 115200 无影响。

**2026-09-16 实测电阻**：`J5 pin3/pin5` → 模组 `A10/A11` = **1 kΩ**（= R41/R42）；
AT/打印口 `A12/A13` = **0 Ω**（直达）—— 两条路的差别就只差这 2 颗 1 kΩ。
**结论：1 kΩ 不是收发不灵的原因**，理由（可自己核）：

| 项 | 数值 | 说明 |
|---|---|---|
| 稳态压降 | **0** | UART 输入是高阻（模组 RX 另带 10 k 上拉），串阻里没有电流 |
| 高电平 | ≈ 3.3 V | 上拉与驱动在同一电源轨，不会分压 |
| 低电平 | 3.3 × 1k/(1k+10k) ≈ **0.30 V** | 远低于门限（≈0.9–1.0 V） |
| 边沿时间 | τ = 1k ×（线容 20–50 pF）≈ **20–50 ns** | 10–90% ≈ 2.2τ ≤ 110 ns；而 115200 一个位 = **8.68 µs** ⇒ 占比 <1.5% |

⇒ 什么时候才该担心 1 kΩ：波特率往上拉很多（≈921600 以上）**且**线很长/容性很大时
（1k × 200 pF = 200 ns 才开始吃掉位宽）。115200 下不必想它。

⚠ 但这个测量**证明不了**“pin3 = A10、pin5 = A11”：两颗串阻都是 1 kΩ，**对调也一样是 1 kΩ**。
要定死线序只有两条路：① `J5 pin3 ↔ pin5` 短接跑 `--ch347-com 0 loopback`（顺便也把 1 kΩ 算了进去，
⇐ 这是验证“1 kΩ 不影响”的最直接实验）；② 万用表法：目标脚串 **1 kΩ 到 GND** 后测电压 ——
**≈1.65 V** = 模组输出脚（TX = A11，驱动经 R42 1k 与我们的 1k 分压）、**≈0.28 V** = 模组输入脚
（RX = A10，3.3 V 经 10k 上拉再经两个 1k 下拉）—— 两者差得很开，一测就知道。

### 3.3 具体怎么接（数据口 = 模组 UART0 ↔ CH347F P2）

| 模组侧 | 模组脚 | 方向 | CH347F-EVT P2 上印着 | 脚号（参考） |
|---|---|---|---|---|
| `A10` | UART0_RX（模组**收**） | ← | **`TXD0`** | pin 4 |
| `A11` | UART0_TX（模组**发**） | → | **`RXD0`** | pin 3 |
| GND | 地 | — | **`GND`** | pin 2 |

**AT / 打印口（模组 UART1 = `A12`/`A13`）**：
**最省事的做法是保持你刷机时那根 USB-UART 不动**（它接的就是打印口 `A12/A13`）——
刷完 MACBUS_UART 固件后 AT 仍在这一路（`sys_config.h`：非 MACBUS_USB 时
`ATCMD_UARTDEV = HG_UART1`），所以不必再飞到 CH347F P3。
真要挪到 CH347F 也可以（P3：模组 `A12`(模组发) → **`RXD1`**；模组 `A13`(模组收) ← **`TXD1`**；GND），
但**一次只改一处**，先把数据口跑通再动它。

- 开发板上这些脚的落点见 §3.2（`A10/A11` 走 **J5 pin 3/pin 5 + GND**；`A12/A13` 走 **J4**）。
  从 **CON3** 引是**断路**（那 6 个串阻默认没贴）。
- **共地必须接**；**供电各管各的** —— 模组板按自己的电源、CH347F-EVT 走它的 USB，
  两者**只连 GND，不要把两边的 3V3 接在一起**（2026-09-16 定，用户改接线后落定）。
  理由（都是查得到的，不是估计）：
  - 模组板上 `VCC` 是**板上 DCDC 的输出节点**：原理图
    `TXW8301/docs/AH开发板原理图_20251015150638.pdf` 写着 `DCDC OUTPUT 3V3 FOR SYS` +
    `VCC(3V3) current at least 400mA. ripple<30mV` —— 它是**有明确源阻抗/纹波要求的电源输出**，
    不是"随便喂一下"的输入脚；
  - CH347F-EVT 的 `VIO` 与 `3V3` 经 **`JP1` 是同一条轨**（用户实测：`JP1` 短接 `3V3`↔`VIO`，
    该轨静态 **3.28 V**）；
  - 两边一接 = **两个稳压源并联在同一条 3.3V 轨上且没有仲裁**：谁电压高谁供电、另一个被倒灌；
    模组板 USB 没插时更是"倒着喂" DCDC 的输出。静态量得出来（3.28 V 看着挺好），
    **发射瞬间才露馅**，而症状正是"时好时坏"、且说不清当下谁在供电。
  - 若确实要让 CH347F 那侧供模组：先**拔掉模组板自己的电源**（二选一，**别两路同上**），
    再看发射时的实测：测点在**模组板 `VCC` 焊盘**，最低点别掉到 **3.0 V 以下**、纹波 **<30 mV**，
    同时盯打印口有没有复位日志 / AT 掉字符。
- ⚠ `A10/A11`（= `SD_D2/SD_D3`）经板上 22R 也接到 **TF 卡座**：**卡里插着 TF 卡时会拉这两条线**，
  调试前**拔掉 TF 卡**。

> 🖼 **图版（2026-09-16）**：上面这套接线的 Fritzing 图在
> [`hardware/wiring/`](../hardware/wiring/README.md) —— `.fzz` 工程 + `.svg` 导出；
> README 里有**从 `.fzz` 抽出来的连线表**（含线色）。该图走的是"**两路都接 CH347F**"：
> 数据口 → `P2`，AT/打印口 → `P3`（即上面「真要挪到 CH347F 也可以」那条）；
> **电源不在图上**（按上一条口径：各自供电、只共地）。
- PC 侧用法（`tools/probe_txah_uart.py`）：
  ```powershell
  python tools\ch347_spi.py uart-list                                  # 看 UART 索引（0=UART0、1=UART1）
  python tools\probe_txah_uart.py --ch347-uart 0 listen --secs 10      # 数据口（UART0）只读解析
  python tools\probe_txah_uart.py --ch347-uart 1 listen --secs 10      # AT/打印口（UART1）
  ```
- ★ **CH347F-EVT 在本机是 VCP 模式**：它的两路 UART **同时**是普通串口 ——
  `USB-HiSpeed-SERIAL-A CH347F (COM23)` = `MI_00` = **UART0**、
  `USB-HiSpeed-SERIAL-B CH347F (COM24)` = `MI_02` = UART1（`MI_04` = SPI/I2C/JTAG）。
  **2026-09-16 实测（短接 P2 的 TXD0↔RXD0 自环）**：

  | 路径 | 自环结果 |
  |---|---|
  | **`--port COM23`（VCP COM 口 / pyserial）** | **✓ 原样读回**（桥 + 线都好） |
  | `--ch347-uart 0`（WCH DLL 的 `CH347Uart_*`） | ✗ 读不回 —— **VCP 模式下 DLL 的 UART API 发不出去** |

  ⇒ **用 COM 口**。工具里加了个自动找口的入口（COM 号会变，按 `MI_00/MI_02` 认）：
  ```powershell
  python tools\probe_txah_uart.py --ch347-com 0 loopback   # 自动找到 UART0 并自环
  python tools\probe_txah_uart.py --ch347-com 0 probe      # 数据口活性探测
  python tools\probe_txah_uart.py --ch347-com 1 listen --secs 10   # UART1（AT/打印口）只读
  ```
  （`--port COMx` 仍然可用；`--ch347-uart` 留作对照。）
- ⚠ **解释器/依赖**（本机踩过）：默认 `python` 是 **3.14 且没装 pyserial**，而
  `C:\Python313\python.exe` 有。`--port`（含 CH347F 的 VCP COM 口）**需要 pyserial**；
  `--ch347-uart`（走 WCH DLL）不需要。两条路：
  `python -m pip install pyserial`，或直接换解释器
  （`C:\Python313\python.exe tools\probe_txah_uart.py ...`）。
- ⚠ DLL 那条路的 `CH347Uart_Init` 里 `ByteTimeout`（单位 100 µs）我暂用 0；手册只写了单位、
  没写 0 的含义，**若"写得出、读不回"**，可加 `--byte-timeout 1`（或 2/5）再试。
  ⚠ 实测（2026-09-16）：DLL 里 **`CH347OpenDevice(0)`（SPI 功能）与 UART0 冲突** ——
  先开 SPI 再 `CH347Uart_Init(0)` 会失败；**UART0+UART1 同时可用**、**SPI + UART1 也可用**。
  所以：只用 UART 时不要 `open()` 设备（脚本已这么做）；要同时跑 SPI 探测就用 UART1 看日志。
- ⚠ 原厂 FAQ 提到「角色选择 **IOB2**：RMII/USB/UART 第 1 套方案，SDIO/SPI 第 2 套方案」，
  以及这两组脚与 SDIO 脚复用 —— **跳线/电阻怎么配要找原厂确认**（我们只负责固件侧）。

## 四、协议要点（单一源 = 模组 SDK 源码）

| 项 | 值 / 出处 |
|---|---|
| 帧头 | `struct hgic_hdr`：`magic(2) + type(1) + ifidx:4|flags:4(1) + length(2) + cookie(2)` = **8 B**，`__packed`（`sdk/include/lib/lmac/hgic.h`） |
| 方向 | `HGIC_HDR_TX_MAGIC 0x1A2B`（主机→模组）、`HGIC_HDR_RX_MAGIC 0x2B1A`（模组→主机）——小端上线的字节分别是 `2B 1A` / `1A 2B` |
| 定帧（UART 特有） | `uart_bus.c`：收满 2 字节比对 magic；**第 5、6 字节 = 帧长**（16bit 小端，**整帧长度**）；`rxcount >= frm_len` 即一帧结束；另有 `fixlen` 定长模式（`HGIC_CMD_SET_UART_FIXLEN` / `sys_cfgs.uart_fixlen`） |
| 数据帧 | `struct hgic_frm_hdr` = `hgic_hdr` + （rx_info / tx_info / 24B）→ 后面才是以太网帧 |
| 命令/事件 | `HGIC_HDR_TYPE_CMD(3)` / `EVENT(4)` / `CMD2(13)` / `EVENT2(14)`（id > 255 走 CMD2/EVENT2）；`HDR_CMDID()` 宏取 id |
| 模式 | `WIFIMGR_FRM_TYPE_RAW`（切 UART 时 `project_config.h` 里定的）= 固件负责以太网帧封装，主机侧就是**裸以太网帧** |
| 波特率 | `UARTBUS_DEV_BAUDRATE 115200`（源码注释：范围 57600~400k） |

⚠ 本仓模拟器/`host_bus.py` 里那套 `AA 55 …` 是**模拟版**宿主协议（`halow-demo/simulator/docs/spi_protocol.md`
已注明"真实芯片的 MACBUS 帧细节属于厂商私有协议"）—— 真实模组走的是**上面这张表**，两者别混。

## 五、首测判据（接好线 + 烧好固件之后）

**⚠ 先纠正一个容易误判的点：`listen` 读到 0 字节 ≠ 不通。** 数据口不是打印口 —— 模组没东西要发时
它就是安静的。所以要用**会应答的探测**，顺序如下：

0. **先确认 UART 版固件真在跑**（最硬的一条判据）：复位/上电模组，看 **AT/打印口**（你刷机用的那根线）
   有没有这一行：
   ```
   uart bus fixlen=…
   ```
   它由 `mac_bus_uart_attach()` 打印（`sdk/lib/bus/macbus/uart_bus.c`）。
   **没有这行 ⇒ 还在跑 SDIO 版固件**（或没刷进去）——那"数据口 0 字节"就完全正常，别再查线。
   顺手也确认 `AT+VERSION=?` 等 AT 查询仍正常（AT 一直在 UART1，不受这次切换影响）。
1. **CH347F 自环**：把 P2 的 `TXD0` 与 `RXD0` 用一根线短接，跑
   `python tools\probe_txah_uart.py --ch347-uart 0 loopback`
   → 读回 = 桥和这根线没问题。**不过就别怀疑模组**。
2. **数据口活性探测**（接回模组）：
   `python tools\probe_txah_uart.py --ch347-uart 0 probe`
   = 先被动听几秒，再**主动**发 `GET_UART_FIXLEN`(id=109) 命令帧（只读，`uart_bus.c` 里
   `uart_bus_proc_cmd` 会应答）→ **收到合法回帧 = 数据口活了** ✓
   （`id 108` = `SET_UART_FIXLEN` 会写 flash 改配置，**别当探针用**。）
3. 数据口活了以后，才轮到"发得出去/收得回来"：`send-eth` / 看模组自发帧。
4. 最后才是 ORPAH 的 b 步判据（周期上报 / 服务端验签通过 / 下行真到达 / 上游零丢弃，
   见 `ROADMAP.md` §〇）。

查的顺序（每层验过再往下）：**固件（第 0 步）→ 桥/线（第 1 步）→ 共地/供电/跳线/A10-A11 是否真引出
→ 波特率 115200 8N1**。

### 5.1 2026-09-16 实测到的"已排除项"与分层排查法

已排除（都有实测）：

| 项 | 证据 |
|---|---|
| 固件是 MACBUS_UART 版 | 打印口复位时出 `uart bus fixlen=0` |
| 模组活着、在打印 | `--ch347-com 1 listen` 10 秒读到 ~900 字节打印文本 |
| **CH347F UART1（P3）= 模组 AT/打印口** | 同上（SecureCRT 刷机用的就是这个口 = `MI_02` = COM24） |
| CH347F 的 UART 通路 + P2 的 TXD0/RXD0 + 那根线 | `--ch347-com 0 loopback`（短接 TXD0↔RXD0）**原样读回** ✓ |
| WCH DLL 的 UART API | ✗ 在 VCP 模式下写得出读不回 —— **改用 COM 口** |
| **`A13` = 模组的打印/AT 输出脚（= `UART1_TX`）** | `RXD0→A13` 5 秒读到 **197 字节**打印文本；同一根线接到 `A12` **0 字节** ⇒ 定案（与 `pin_function.c` 的 `A12=UART1_RX / A13=UART1_TX` 一致；此前“两侧资料说法不一致”就此排除） |
| **P2 的 `TXD0`/`RXD0` 两个方向都好 + 电平/共地/波特率都对** | **AT 往返成功**：`TXD0→A12`、`RXD0→A13`，发 `AT+SSID?` → 回 `[9302]SSID: 测试链路\r\nOK`。同一条线、同一路 CH347F，接到模组 `UART1` 就活 ⇒ 桥、线、电平、共地、115200 全都没问题 |
| **模组 `UART0` 的引脚方向（源码级）** | `sdk/chip/txw4002ack803/pin_function.c` 的 `uart_pin_func()`：`HG_UART0_DEVID` → `PIN_UART0_RX_A10_F4`(RX=**A10**) / `PIN_UART0_TX_A11_F4`(TX=**A11**)；`project_config.h` 里 `UARTBUS_DEV = HG_UART0_DEVID`、`ATCMD_UARTDEV = HG_UART1` ⇒ **`TXD0` 必须接 `A10`、`RXD0` 必须接 `A11`** |
| **两根线 + J5 两个落点 + 两颗 1 kΩ 都通** | `J5 pin3 ↔ pin5` 短接（回路 = `TXD0 → 线 → J5 pin3 → R41 → 短接线 → R42 → J5 pin5 → 线 → RXD0`）→ `--ch347-com 0 loopback` **原样读回** `55 aa 00 ff 2b 1a` ✓（2026-09-16；顺带把 1 kΩ 对 115200 的影响也一起证掉了） |
| ★ **我们发的帧完好到达模组 `UART0`** | 调试固件（§5.2）打印：`[mbus rx] 9 byte(s) 2b 1a 03 00 09 00 34 12 6d` —— 正好是 `probe` 发出去的那一帧（magic `2b 1a`、type `03`=CMD、len `09 00`、cookie `34 12`、cmd `6d`=109）。**这条一次性定死四件事**：① `TXD0` 所在的那个 `J5` 落点**就是模组 `A10`（`UART0_RX`）** ⇒ 所以另一个落点必是 `A11`，**线序不用再猜/不用对调**；② 波特率/极性/电平全对；③ 1 kΩ 真的不影响（每字节都对）；④ 模组 `UART0` 的接收+中断+任务链路都在跑 |

⚠ **`loopback` 的接法（本次踩过）**：它要求把**本路 `TXD0` 与 `RXD0` 直接短接**（就在 P2 那排上）。
线已经接去 `A12/A13`（模组 UART1）或 `A10/A11`（模组 UART0）时，`TXD0` 与 `RXD0` 之间**没有通路**，
「自环不通」是必然的、**不说明任何问题** —— 要验线序请用下面的"远端短接"。

⇒ 至此**我们这一侧全部清白**：桥、两根线、两个落点、两颗 1 kΩ、电平、共地、波特率、HGIC 帧格式
都已各自有实测或源码依据（上表）。**唯一还没验的是「模组侧到底收到没有」** —— 所以下一步不再是
在线上猜，而是直接去看模组自己怎么说：§5.2 的临时调试固件。

> 备用线索（万一需要）：模组 `UART0` 的寄存器基址 = `HG_UART0_BASE` = `PERIPH_BASE(0x40000000)`
> `+ 0x3000` ⇒ `RBR=0x40003000`、`IER=+0x04`、`LSR=+0x14`、`USR=+0x7C`；
> AT 表里有 `AT+REG_RD`/`AT+REG_WT`（`project/atcmd.c`），真要时可以直接读 `LSR` 看有没有数据。
> （寄存器偏移出自 `sdk/include/dev/uart/hguart.h` 的 `struct hguart_hw`。）

> 别忘两条环境项：调试时**拔掉 TF 卡**（`A10/A11` = `SD_D2/D3`，经板上 22R 也接着 TF 卡座）；
> **`J5` 上的跳线帽要拿掉**（省得板载 CH340E 跟 CH347F 抢同一对线）；`CON3` 是死路（`NC/1K` 没焊）。
> 备用线索：模组 AT 表里有 `AT+REG_RD` / `AT+REG_WT`（`project/atcmd.c`）—— 真需要时可以寄存器级
> 验 `UART0`，但那要另算寄存器地址，留作最后手段。

### 5.2 临时调试固件：`UART0` RX 打印（2026-09-16，用来定"模组到底收没收到"）

**为什么**：§5.1 已把"我们这一侧"全部排除，只剩"字节到底有没有进模组 `UART0`"。模组侧没有
现成的观测手段（`AT+SYSDBG` 只支持 heap/top/lmac/umac/irq，`hgic_dbg` 在闭源库里是空宏），
所以只能自己往接收路径加打印。

**改了什么**（`sdk/lib/bus/macbus/uart_bus.c`，**共 4 处**，都带 `TEMP DEBUG` 注释）：

| 位置 | 内容 |
|---|---|
| `uart_bus_task()` 之前 | 新增 `uart_bus_dbg_dump(tag, buf, count)`：打出**标签 + 字节数 + 前 16 字节 hex**（自己拼 hex 串，只用 `%d`/`%s`，不赌 `os_printf` 支不支持 `%02x`） |
| `uart_bus_task()` 里 `bus.recv()` 之前 | `uart_bus_dbg_dump("[mbus rx]", …)` —— **放在任务里、不放在 ISR 里**（`os_printf` 走轮询控制台，中断上下文太重） |
| `uart_bus_write()` 开头 | `uart_bus_dbg_dump("[mbus tx]", …)` —— 看**模组往主机口写了什么**（回答"它到底回没回"）。放这里是因为该函数一开始就 `os_mutex_lock(..., osWaitForever)`：**能睡眠 ⇒ 必是任务上下文**，不是在 ISR 里 |
| `mac_bus_uart_attach()` 里 | 多打一行 `[mbus rx] debug build: UART0 rx dump on` —— **用于证明烧进去的确实是这版**（否则"没输出"分不清是"没收到"还是"没烧成"） |

**备份 / 回退**：原文件已备份为同目录 `uart_bus.c.bak-nodbg`。回退 = 删掉那 4 处
（或直接 `copy uart_bus.c.bak-nodbg uart_bus.c`）后重编即可。
（`FMAC_SDK` 是指向外部 SDK 的 junction 且在 `.gitignore` 里 —— 这些改动**不进本仓**，只在本机生效。）

**编译**（照 `halow-demo/TXW8301/README.md` 那套；`make` 必须用 CDK 自带的 3.82.90 + msys `sh.exe`）：

```powershell
$env:SHELL='F:\C-Sky\CDK\CSKY\MinGW\msys\1.0\bin\sh.exe'; $env:MAKESHELL=$env:SHELL
& 'F:\C-Sky\CDK\CSKY\MinGW\bin\make.exe' SHELL=sh.exe -C F:/git/halow-demo/TXW8301/FMAC_SDK/project -f cdkws.mk All
```

产物：`FMAC_SDK/project/txw8301_v2.4.1.5-39777_<日期>_.bin`（并复制成 `APP.bin`）。
**2026-09-16 已编过两版**：第一版（只 RX）365584 B；第二版（RX+TX）**366096 B**（15:10:41）。
两版都离线核对过 `APP.bin` 里确实有对应字符串（沿用之前查 `uart bus fixlen` 的同一手法）。

**烧录**：同以前 —— AT 口发 `at+fwupg` → SecureCRT 用 Xmodem 发 `APP.bin` → 自动重启。

**看什么**（日志在 AT/打印口 `UART1` = `A12/A13`，看日志时那路要接着）：

| 现象 | 含义 |
|---|---|
| 启动时出 `[mbus rx] debug build: UART0 rx dump on` | 调试固件确实在跑（后面所有判据的前提） |
| 发帧时出 `[mbus rx] 9 byte(s) 2b 1a 03 00 09 00 34 12 6d` | **帧到了 `UART0`** ✓（2026-09-16 已实测到） |
| 发帧时**一行都不出** | **字节根本没进 `UART0`** ⇒ 落点/引脚复用/被别的东西咬着 |
| 出 `[mbus rx] N byte(s)` 但字节是乱的 | 波特率/电平/极性有问题（哪种一眼能看出） |
| 紧接着出 `[mbus tx] N byte(s) …` | **模组确实往主机口回写了** ⇒ 那么"没收到"就在回程（反而说明命令处理是通的） |
| 只有 `[mbus rx]` 没有 `[mbus tx]` | **模组根本没回写 `UART0`** ⇒ 问题在"命令处理/分发"或"它认为现在不该往主机写"，不在线上 |

**已知边界**：它只能看到"UART0 收到了什么"，看不到"为什么没收到"；也看不到
模组→主机那半条回程线。一旦确认收到，下一步就是拿 `probe` 看回程帧。

**备用线索：`AT+BUS_WT`（让模组主动往主机口写）**。`AT+BUS_WT=?` 实测回
`+BUS_WT:bus write disable` + `OK` —— 是个开关（`AT+BUS_WT=0/1`）。它的实现**在闭源库里**
（`libs/liblmac.a` 里的字符串 `"%s:bus write %s"` + `enable`/`disable`），但 `libs/libwifi.a` 里有
`wifi_mgr_print2host` ⇒ **很可能就是"把打印/数据也写到主机口（mac bus）"的调试开关**（推断，未实测）。
用法：`AT+BUS_WT=1` 后在**数据口**跑 `--ch347-com 0 listen --secs 5 --dump-raw`：
- 有字节（magic 应为 **`1a 2b`** = `0x2B1A` 小端 = 模组→主机）⇒ 回程那半条线也是好的；
- 试完记得 `AT+BUS_WT=0` 关掉。
- ⚠ **2026-09-16 实测：`AT+BUS_WT=0/1/2` 三种取值下数据口都是 0 字节** —— 所以要么它不是
  "往主机口写"，要么它依赖打印开关（例如 LMAC 周期打印被 `AT+SYSDBG=LMAC,0` 关过）。

**闭源库字符串里挖到的两条线索**（2026-09-16，`libs/*.a` 里 extract 出来的，不是猜的语义，是原文）：

| 字符串 | 出处 | 怎么用 |
|---|---|---|
| `wifimgr host cmd:%d, ifidx=%d` | `libs/libwifi.a` | **主机命令真正进到 `wifi_mgr` 分发器时会打印这行**。所以发帧后先看 AT 口：**有** ⇒ 命令到了分发层（问题在"回写主机口"）；**没有** ⇒ 根本没进分发层（在 bus → wifi_mgr 那段就丢了） |
| `WiFi_Mgr: open:%d, nif:%d, host_alive:%d`（`wifi_mgr_status()`） | `libs/libwifi.a` | 分发/回写可能受 `open` / `host_alive` 约束 ⇒ 主机侧通常要先发 **`HGIC_CMD_DEV_OPEN`（`hgic.h` 命令表第 1 条，id=1）**。已列入下一轮要试的命令：`send-cmd 1`（DEV_OPEN）/ `send-cmd 43`（`GET_FW_INFO`）/ `send-cmd 20`（`GET_STATUS`） |

⚠ **2026-09-16 实测：上述三条命令（1 / 43 / 20）都到了 `UART0`（三条 `[mbus rx]` 都在），
但 AT 口没有出现 `wifimgr host cmd:…`，也没有任何 `[mbus tx]`** ⇒ 命令没被分发，模组也没回写。
根因线索见 §5.3。

### 5.3 ★ `WIFIMGR_FRM_TYPE`：最可能的根因（RAW 模式没有命令通道）

厂商文档《TXSDK_主控交互指南》§2（`TX_AH_SDK_2.4\doc\TXSDK_主控交互指南.pdf`）把主机口的帧类型
分三档：

| `wifimgr_frm_type` | 主机↔模组之间传什么 | 命令 / 事件 | 谁控制以太头/ethertype |
|---|---|---|---|
| `WIFIMGR_FRM_TYPE_ETHER = 0` | 完整以太帧 | ✗ | **我们**（可以就是 `0x88B5`） |
| `WIFIMGR_FRM_TYPE_HGIC` | 帧 + 24 B `hgic_frm_info` + 载荷 | **✓（数据+命令+事件三合一）** | 我们 |
| `WIFIMGR_FRM_TYPE_RAW` | 裸数据，**由固件完成以太网帧格式封装** | ✗ | 固件（我们看不见） |

而 `project_config.h` 里我们这一版**显式选了 `WIFIMGR_FRM_TYPE_RAW`**（注释原文：
"串口默认用RAW数据类型，**如果需要用Nonos驱动接口，注释掉这个宏**"）；
`project/sys_config.h` 的回落默认值恰恰是 **`WIFIMGR_FRM_TYPE_HGIC`**：

```c
#ifndef WIFIMGR_FRM_TYPE
#define WIFIMGR_FRM_TYPE WIFIMGR_FRM_TYPE_HGIC
#endif
```

⇒ **推断（待实测确认）**：RAW 是"纯数据管"，**主机侧的命令/事件通道在 RAW 下不存在** ——
这正好解释了实测到的全部现象：帧完好到达、`wifimgr host cmd:` 不打印、模组一个字节都不回写。
也就是说，**这不是坏了，是模式选错了**（至少对"我们要在主机侧驱动模组"这件事而言）。

**不重编的判定法**（`sys_status.dbg_umac` 由 `AT+SYSDBG=UMAC,1` 打开；那行 `wifimgr host cmd:` 很可能
受它控制）：

```
AT+SYSDBG=UMAC,1
python tools\probe_txah_uart.py --ch347-com 0 send-cmd 43
```

- AT 口**出现** `wifimgr host cmd:43, ifidx=0` ⇒ 命令其实进了分发器（RAW 也会分发）⇒ 根因不在这里；
- **还是不出现** ⇒ RAW 下根本不分发命令 ⇒ 需要换 `frm_type`。

**要换的话**（单变量）：把 `project_config.h` 里那行 `#define WIFIMGR_FRM_TYPE WIFIMGR_FRM_TYPE_RAW`
**注释掉**（回落到 `sys_config.h` 的 HGIC），重编烧录后 `send-cmd 43` 应当有回应。
⚠ 这一步**同时决定 b 步的数据格式**（上表第 4 列），属于接口决策，**先与用户对齐再改**。

**2026-09-16 已按用户选择执行（选 `HGIC`）**：

| 改哪 | 怎么改 | 回退 |
|---|---|---|
| `project_config.h` | 注释掉 `#define WIFIMGR_FRM_TYPE WIFIMGR_FRM_TYPE_RAW`（行首加 `//`），回落到 `sys_config.h` 的 `WIFIMGR_FRM_TYPE_HGIC` | 备份在 `project_config.h.bak-raw`，`copy` 回去即可 |
| `main.c` | `sys_wifi_init()` 里 `wifi_mgr_init(...)` 之前加一行**纯打印**（惰性，只为一眼确认烧的是哪档）：`[mbus cfg] frm_type=%d (0=ETHER 1=HGIC 2=RAW) bus=%d` | 删掉这一行 |

- 重新编译：`APP.bin` = 366096 B（2026-09-16 15:56），bin 内含 `[mbus rx]` / `[mbus tx]` / `[mbus cfg]`
  三个字符串（离线核对）。§5.2 的 RX/TX 调试打印**保留**（正好能看模组的回写）。
- `WIFI_DHCPC_SUPPORT 0` **未动**（保持单变量）。
- **上机判据（待做）**：① 启动看到 `[mbus cfg] frm_type=1`；② `send-cmd 43` 有回读；
  ③ AT 口出现 `wifimgr host cmd:43, ifidx=0` 与 `[mbus tx] …`。

**不重编的旁证**（验 RAW 下数据上行到底通不通）：数据口发一条数据帧，再看 AT 口的计数 ——
`AT+TX_PKTS` / `AT+TX_FAIL`（还有 `RX_PKTS`）；计数涨了说明"数据其实发出去了，只是没有命令通道"。
`send-eth` 的用法（`--frame-type frm2|frm`、`--with-frm-info`、`--no-ethernet`）：

```powershell
# 裸载荷（RAW 模式的正统写法：固件替你封以太头）
python tools\probe_txah_uart.py --ch347-com 0 send-eth --no-ethernet --wait 3 01 02 03 04 05 06 07 08
# 完整以太帧（广播 + 模组 MAC + ethertype 88b5）
python tools\probe_txah_uart.py --ch347-com 0 send-eth --wait 3 ff ff ff ff ff ff 4a 06 59 8d 74 40 88 b5 01 02 03 04 05 06 07 08
# 另一种头：FRM + 24B info
python tools\probe_txah_uart.py --ch347-com 0 send-eth --frame-type frm --with-frm-info --no-ethernet --wait 3 01 02 03 04 05 06 07 08
```

**2026-09-16 实测证据（`AT+SYSDBG=UMAC,1` 打开状态打印后，发 `send-cmd 43`）**：

```
[950981][mbus rx] 9 byte(s) 2b 1a 03 00 09 00 01 00 2b
[951047]    VIF3: Type:2, [4a:06:59:8d:74:40] running, WPA_COMPLETED, chan:1
[951047]          TX_DATA:1, TX_BEACON:13, TX_FAIL:0          ← 按 ~1 秒窗口统计的计数
[951070]WiFi_Mgr: open:0, nif:1, host_alive:65535
[951074]    if_recv:1, if_write:0, if_err:0, drv_aggsize:0
[951080]    cmdlist:0, up2host:0, cachedata:0
```

- `if_recv:1` ⇒ **帧确实进了 `wifi_mgr`**（被当成"接口输入"收下了）；
- `if_write:0` ⇒ `wifi_mgr` **一次都没往主机写**；
- `open:0` ⇒ 主机侧没有"打开设备"（且 `send-cmd 1` = `DEV_OPEN` 也没能把它打开）；
- `TX_DATA:1`（`VIF3` = AP 侧）⇒ **很可能是把我们那条"命令"当数据发到空口去了**（推断；要与 `AT+TX_PKTS` 对照才坐实）；
- 全程**没有** `wifimgr host cmd:`。

⇒ 与上面的推断一致：**RAW 下主机口是纯数据管，命令不会被分发**。要命令/事件通道就得换
`WIFIMGR_FRM_TYPE_HGIC`（见上）。

## 5.4 ★★ 2026-09-16 打通：HGIC 生效后「命令 + 事件 + 回程」全部验证

换成 `WIFIMGR_FRM_TYPE_HGIC` 后**当场就通了**（同一套线、同一个 CH347F、同一版调试固件）：

| 观测 | 内容 |
|---|---|
| 启动 | `[mbus cfg] frm_type=1 (0=ETHER 1=HGIC 2=RAW) bus=4` + `[mbus rx] debug build: UART0 rx dump on` |
| `send-cmd 43`（GET_FW_INFO） | **回读 40 字节**；AT 口同时出现 `wifimgr host cmd:43, ifidx=0`、`use UUID for MAC`、`[mbus tx] 40 byte(s) …`、`resp cmd, ret:28` |
| 模组→主机 **周期性事件** | `type=EVENT flags=1 len=16`，载荷 = `07 00 00 00 8b 00 00 00`，约每 5 s 一条 —— `hgic.h` 里 **`HGIC_EVENT_TX_BITRATE = 7`**，值 u32 ≈ 130~139 |
| `send-cmd 1` / `send-cmd 20` | 只回**一个 `cmd_id`**（9 B，无 status/len）⇒ 这两个 id 当前没有数据应答（**如实说明，不当成错误**） |

**由此确定的协议事实**（已写进 `tools/txah_hgic.py`，并用真机字节做黄金样本自测）：

1. `struct hgic_ctrl_hdr` = 8 B 帧头 + **4 B union** = **12 B**（`HGIC_CTRL_HDR_LEN`）；
   模组端取参数是 `data = (uint8 *)(ctrl + 1)` ⇒ **参数/数据都从帧内偏移 12 开始**。
   ⚠ 所以 `cmd_frame()` 现在**补满 4 B union**（原来 1 字节 `cmd_id` 就完了，**带参命令的参数会落错位置**）——
   `send-cmd 43` 发的是 `2b 1a 03 00 0c 00 01 00 2b 00 00 00`（12 B 头）。
   **2026-09-16 已上机复验 ✓**（`python tools\probe_txah_uart.py --ch347-com 0 send-cmd 43`）：
   回读 `1a 2b 03 00 28 00 01 00 | 2b 00 1c 00 …` —— 40 B、`status=0`、`data=28 B`、
   `FW: app=2.4.1.5 svn=39777 chip_id=0x4002 mac=4a:06:59:8d:74:40`，与启动日志一致；
   即**换成 12 B 头后回包与之前逐字节相同**（这条曾是 PC 侧唯一未验证的改动，现结）。
2. **CMD 应答载荷** = `cmd_id(1) | status(1) | len(2, LE) | data(len)`
   （实测 40 = 8 + 1 + 1 + 2 + **28**，与模组日志 `resp cmd, ret:28` 一致；
   另与 `uart_bus_proc_cmd` 里 "`ret = 2` = 数据长度" 的约定吻合）。
3. 43 返回的 28 B 就是 `struct hgic_fw_info`：

   | 字段 | 实测 | 交叉验证 |
   |---|---|---|
   | `version` | `05 01 04 02` → **2.4.1.5** | 与启动横幅 `hgSDK-v2.4.1.5-39777` 一致 |
   | `svn_version` | **39777** | 固件名 `…-39777` |
   | `chip_id` / `cpuid` | `0x4002` / `0x0001` | — |
   | `mac[6]` | **4a:06:59:8d:74:40** | 启动日志 `lmac_cfg_set_mac:4a:06:59:8d:74:40` |
   | `smt_dat` | **124022701** | 启动日志 `SMT_DAT: 124022701` |

**工具升级**（同一提交）：`txah_hgic.py` 加 `CMD_NAMES`/`EVENT_NAMES`、`ctrl_info()`（请求/应答/事件三分）、
`fw_info_decode()`、`describe()` 输出人类可读；`probe_txah_uart.py listen` 会把**连续重复的同一条事件合并**
并给「按类型统计」。自测 `python tools\txah_hgic.py selftest` 现 24 项（含上表全部黄金样本）。

**仍未定**（下一步）：

- **数据面**：`send-eth` 两种写法（裸载荷 / 完整以太帧）都**没有回读** —— 这在"没有对端"时是正常的。
  要判"有没有真发到空口"：看 AT 口 `IEEE80211 Status` 里的 `VIF3 … TX_DATA:`，或读 `AT+TX_PKTS` / `AT+TX_FAIL`。
- **模组当前是 AP 模式（`VIF3 Type:2 running, WPA_COMPLETED, chan:1`）+ 没有关联的 STA**；
  b 步要真跑起来得把模组配成 **STA 连到 HaLow AP**（对端是谁：T-Halow-RJ45 板 / ORPAH Router / 第二块模块 —— **待与用户确认**）。
- RAW 模式的载荷约定（"固件封以太头"到底封成什么）**不再需要查**：已改用 HGIC。

## 六、未做（如实）

- **控制面（命令 + 事件 + 回程）已在真机跑通**（§5.4）：`send-cmd 43` 一问一答回 40 B；
  换 **12 B 控制头**后回包**逐字节相同**（2026-09-16 复验，见 §5.4）。
- **数据面（用户数据上行/下行）仍没有真机结果**：`send-eth` 只有"发了但没对端"的记录 ——
  两块模块那一节（§7）要做的正是这件事。
- **临时调试打印还在码里**（`uart_bus.c` 四处 `TEMP DEBUG` + `main.c` 的 `[mbus cfg]` 标记）：
  两半均已上机（RX 见 §5.1、TX 见 §5.4 的 `[mbus tx] 40 byte(s)`），**正式固件应删掉**
  （备份/回退见 §5.2）。⚠ **目前烧在板上的是带调试打印的那版**（`APP.bin` 366096 B）。
- **RAW 模式下载荷到底带不带以太头、要不要那 24 字节 info**：**不再需要查** —— 已改用 HGIC（§5.3）。
- **SPI 电气探测**（`docs/ch347f-txah-spi-probe.md`）仍待做；UART 路已通，优先级降低
  （除非数据面在 UART 上走不通）。
- 两块模块端到端的四组载荷/帧头约定、模组 B 的 MAC 是否不同、是否需要 `AT+PAIR`、加密下的行为：
  见 §7.5。

## 七、两块模块端到端（b 步数据面；用户 2026-09-16 选“第二块 TX-AH 当对端”）

### 7.1 接线（一个 CH347F 管两端，PC 上一条命令就能测）

| 谁 | 接到 | 说明 |
|---|---|---|
| 模块 A（客户端侧，做 **STA**） | CH347F **P2 = UART0**（`MI_00`） | `P2 TXD0 → J5 pin3(A10)`、`P2 RXD0 ← J5 pin5(A11)`、`GND → pin7/8`；**J5 跳线帽拿掉** |
| 模块 B（对端，做 **AP**） | CH347F **P3 = UART1**（`MI_02`） | 同样三根线接到 **B 板自己的 J5 pin3/pin5 + GND** |
| 两块板的 AT/打印口 | 各自的 USB-UART（刷机那根） | 看日志用；**不要**占用 CH347F |
| 三块板 | **共地** | CH347F + A 板 + B 板 GND 连一起 |

> CH347F 的 UART0 + UART1 可共存（2026-09-16 实测 ✓，见 §3.3 附近）；两块板**各自供电、只共地**
> （**别把两边 3V3 接一起** —— 理由见 §3.3 那条）。

### 7.2 两块都要刷**我们这版固件**

`MACBUS_UART` + `WIFIMGR_FRM_TYPE_HGIC` + 调试打印（§5.2）。第二块若是**出厂 SDIO 固件**，
必须按 §5.2 的流程重编 + `at+fwupg` 烧一遍 —— 否则它没有 UART macbus，数据口是死的。

**✅ 2026-09-16 实测（用户提供 B 板启动日志）：第二块不用重刷** ——
`hgSDK-v2.4.1.5-39777, app-0, build time:Sep 16 2026 15:55:48`（与 A 同一版）+
`[43][mbus cfg] frm_type=1 (0=ETHER 1=HGIC 2=RAW) bus=4` + `[47]uart bus fixlen=0` +
`[49][mbus rx] debug build: UART0 rx dump on`；
且 `[5030][mbus tx] 12 byte(s) 1a 2b 04 00 0c 00 00 00 1f 00 00 00`
⇒ **它的数据口（`UART0`）已在往主机方向发事件**（`type=EVENT`、事件 id=0x1f、无数据）。

### 7.3 AT 配置（各自 AT 口；写法出自《泰芯AH-SDK_V2.x AT指令使用说明_V1.4》§3.1/§4.1）

两块都先设射频与信道（示例取文档 §4.1 的写法）：

```
AT+BSS_BW=8                        # 8M 带宽
AT+CHAN_LIST=9080,9160,9240        # 或 AT+CHANNEL=1
AT+SSID=ORPAH_AH_TEST
AT+ENCRYPT=0                       # 先用不加密，最简
```

再分别设角色（**这一步是两块唯一不同的地方**）：

```
模块 B（对端）：AT+WIFIMODE=ap
模块 A（客户端）：AT+WIFIMODE=sta
```

两边 `AT+RST` 后：

- 文档原话：**“如果 AP 和 STA 都设置了 SSID 等参数，就不用启动 PAIR 了，会依靠 SSID”** ⇒ 本测试**不需要** `AT+PAIR`；
- 自检：模块 A 的 AT 口/状态打印里应看到连上（`WPA_COMPLETED` / `CONECTED` 之类）；
  `AT+SYSCFG` 可回读当前参数，`AT+STA_INFO` 看关联到的 STA。

### 7.4 判据：一条命令（`xfer`）

```powershell
python tools\probe_txah_uart.py xfer --tx-com 0 --rx-com 1 --text HELLO-ORPAH --secs 8
```

- `--tx-com 0` = P2 那路（模块 A 的数据口）发；`--rx-com 1` = P3 那路（模块 B 的数据口）收；
- 期望输出 `=> ✓ 载荷原样到达对端`（并打印对端收回的原始帧）；
- 先离线看要发什么：加 `--dry-run`（只组帧、不开串口）。

**载荷/帧头约定还没定**（这正是本次要量的东西）：HGIC 模式下数据帧用 `FRM2`(8B 头) 还是
`FRM`(8B + 24B info)，载荷是**裸数据**还是**完整以太帧**。四种组合依次试：

```powershell
# ① FRM2 + 裸载荷   ② FRM2 + 完整以太帧   ③ FRM + 24B info + 裸载荷   ④ FRM + 24B info + 完整以太帧
python tools\probe_txah_uart.py xfer --tx-com 0 --rx-com 1 --no-ethernet --text HELLO
python tools\probe_txah_uart.py xfer --tx-com 0 --rx-com 1 -- ff ff ff ff ff ff 4a 06 59 8d 74 40 88 b5 01 02 03 04 05
python tools\probe_txah_uart.py xfer --tx-com 0 --rx-com 1 --no-ethernet --frame-type frm --with-frm-info --text HELLO
python tools\probe_txah_uart.py xfer --tx-com 0 --rx-com 1 --frame-type frm --with-frm-info -- ff ff ff ff ff ff 4a 06 59 8d 74 40 88 b5 01 02 03 04 05
```

哪一组能“原样到达”，哪一组就是 b 步数据面的约定；**若对端收到的载荷外面多包了一层**，
`xfer` 会把它原样打出来（那层就是要认的封装）。两块的 AT 口同时应看到 `[mbus tx] …`（B 块往主机口写）。

### 7.5 待实测 / 未做

- 上述四组约定的真机结果（本轮没做）；模组 B 的 MAC 是否与 A 不同（启动日志 `use UUID for MAC`，
  预期不同，待确认）；是否需要 `AT+PAIR`；加密（`AT+ENCRYPT=1 + AT+KEY`）下的行为。
- 一切以实测为准：**没跑过的都不写“已通”**。

**已确认（2026-09-16）/ 两个可疑点**：

- ✅ **两块 MAC 确实不同**：A `4a:06:59:8d:74:40`、B `69:6e:6b:00:00:00`。
  ⚠ 但 **B 的 `0x69` 最低位 = 1 ⇒ 按 802 定义是组播地址**（合法单播应 LSB=0，A 的 `0x4a` 就是）；
  BSSID/源地址不应是组播 —— **若 A 关联不上 B，先怀疑它**（改成合法单播 MAC 再试）。
  注：A 那边日志里有 `use UUID for MAC`，B 的启动日志里没有。
- ⚠ B 的启动日志 `[1]syscfg: invalid magic_num=0, addr=fe000` ⇒ **B 的 syscfg 区是空的、参数全默认**
  ⇒ §7.3 的 SSID/带宽/信道/加密必须**显式设一遍**；设完 `AT+RST` 后这条应该消失。
