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
| P5 | I2C（`VIO SCL SDA GND`），旁边 JP1 = VIO 选择 |
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
- **共地必须接**；模组 3.1~3.3 V 自己供电，**别用 CH347F 的 `3V3`（pin 1）去带模组**
  （发射瞬间电流不够，会"时好时坏"）。
- ⚠ `A10/A11`（= `SD_D2/SD_D3`）经板上 22R 也接到 **TF 卡座**：**卡里插着 TF 卡时会拉这两条线**，
  调试前**拔掉 TF 卡**。
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
| **P2 的 `RXD0` + 那根线（走到 `J4` 那段）** | 同上：接上就能读到打印 ⇒ RX 路好，且**共地也没问题** |

⇒ 剩下的疑点只有两段：**① P2 的 `TXD0` → 模组收（`A12`）；② `P2 → A10/A11` 数据口那一段**
（板上落点 J5 pin3/pin5）。**分层排查法**（都借已知好信号，不用烙铁）：

1. **AT 往返**（验证 TX 路）：`TXD0→A12` 与 `RXD0→A13` **两根同时接**，从 COM23 发一条**已验证会回**的
   AT（用 `at+SSID?`；`AT+VERSION=?` 在本次实测里没有任何回应，不代表模块坏），预期在 COM23 的回读里
   看到应答（会混在打印文本中间）。⚠ 只接 `TXD0` 而把 `RXD0` 拔了的话，回读当然是空的 —— 那**不算失败**。
2. **远端短接**（验证两根线 + J5 落点接触）：在模组/J5 那侧把 `A10` 与 `A11` 短接，跑
   `--ch347-com 0 loopback`。通了 ⇒ 两根线、J5 两个落点、P2、桥**全都是好的**，问题在模组侧；
   不通 ⇒ 是线/接触问题（先解决它，别去动固件）。
3. ①②都通过而数据口 `probe` 仍无回应 ⇒ **给固件加 RX 调试打印**（在
   `sdk/lib/bus/macbus/uart_bus.c` 的接收路径里把收到的字节数/前几字节 `os_printf` 出来），
   重编烧录后从打印口就能看出"帧到底有没有到模组"。

> 别忘两条环境项：调试时**拔掉 TF 卡**（`A10/A11` = `SD_D2/D3`，经板上 22R 也接着 TF 卡座）；
> **`J5` 上的跳线帽要拿掉**（省得 CH340E 也挂在同一对线上）；`CON3` 是死路（`NC/1K` 没焊）。

## 六、未做（如实）

- **固件已烧、已由启动打印确认**（`hgSDK-v2.4.1.5-39777 … build time:Sep 16 2026 …` + `[44]uart bus fixlen=0`）；
  **接线在做**，但**数据口（`A10/A11`）至今没有任何一问一答的实测**（见 §5.1）。
- **PC 侧联调脚本已就绪但只跑过离线自测**：`tools/txah_hgic.py`（HGIC 编解码，自测 13 项全过）
  + `tools/probe_txah_uart.py`（listen / send-eth / send-cmd / raw）。真机行为待接线后跑。
- **RAW 模式下载荷到底带不带以太头、要不要那 24 字节 info**：不确定，靠真机试
  （工具两种都支持，会打印实际发送内容）。
- SPI 电气探测（`docs/ch347f-txah-spi-probe.md`）仍待接线后跑；若 SPI 也通，再回头比较两条路。
