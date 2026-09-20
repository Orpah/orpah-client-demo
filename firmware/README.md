# firmware/ — ORPAH 客户端固件（CH32V203 + TX-AH）

**状态：c1 骨架（2026-09-20）—— 能编译出 `.elf`/`.bin`，但【未上机】**。
上机与烧录由用户执行；本仓不把"编译过"写成"跑过"。

## 这是什么

c 步的第一版固件骨架：CH32V203 裸机（无 RTOS、无 libc、`-nostdlib`）+ 控制台 + 1 ms 时基 + 心跳灯。
**只有骨架**：协议内核（c2）、HGIC 数据口（c3）、签名上报（c4）都还没写（见 `../ROADMAP.md` §五）。

## 目录

```
firmware/
├── Makefile                    # riscv-none-embed-gcc 构建（工具链路径见下）
├── ld/link.ld                  # 64K Flash / 20K RAM；栈顶 _eusrstack，.heap 用 NOLOAD 预留
├── startup/startup_ch32v203.S  # QingKe V2 启动：向量表 + 复位 + 清 bss + csrw 0x804,0x3
├── Core/
│   ├── ch32v20x.h              # 寄存器定义（自包含，无 CMSIS）
│   ├── board.h                 # 引脚/波特率/IRQ 属性（★ 引脚是待定项，用户定板后只改这里）
│   └── main.c                  # SystemInit + main：时钟/控制台/心跳/最小命令
└── Periph/
    ├── gpio.c/h                # gpio_set_mode / gpio_set_pin / gpio_get_pin
    └── uart.c/h                # 中断收发 + 迷你 printf（uart_init/uart_putc/uart_write/uart_printf）
```

## 构建

**必须用 MounRiver Studio 自带的那份工具链**（`-DWCH_INTERRUPT_FAST` 依赖它的
`interrupt("WCH-Interrupt-fast")` + 启动文件里的硬件栈配置；独立 xPack 版**会跑飞**）。

本机 2026-09-20 实测可用的路径与版本：

```
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/
  riscv-none-embed-gcc.exe  → "xPack GNU RISC-V Embedded GCC 8.2.0"
```

⚠ 前缀是 **`riscv-none-embed-`**，不是 `riscv-none-elf-`。`Makefile` 的 `RISCV_PREFIX`
默认就指向上面这个路径；换机器用 `make RISCV_PREFIX='.../bin/riscv-none-embed-'` 覆盖。

⚠ Windows 上 make 的 recipe 要 POSIX 命令（`mkdir -p` / `rm -rf`）⇒ 需要一个 sh。
实测可用：

```bash
# Git Bash 或任何带 sh 的环境
make SHELL='D:/Program Files/Git/bin/sh.exe'
# 或在 Git Bash 里
make
```

产物（实测尺寸）：

```
build/orpah-client.elf   9644 B
build/orpah-client.bin   2108 B   ← 烧这个
   text 2046 / data 0 / bss 20224
```

> `bss` 看着接近整个 RAM（20 KB）**不是真用掉了**：`link.ld` 里 `.heap (NOLOAD)` 从中段
> 一直预留到 `RAM 顶端 − 256`，`size` 把它算进 bss；`sp = _eusrstack` 从 RAM 顶端向下长。

## 烧录（**由用户执行**）

```bash
# A) WCH-Link / SWD
openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg \
        -c "program build/orpah-client.bin 0x08000000 verify reset exit"
# B) WCHISPTool（BOOT+RST 进刷机态，不需要 COM 口）；或 MounRiver 的下载按钮
make flash      # 只打印上面两条提示，不代跑
```

## 上电后应该看到什么（判据）

1. 控制台（115200 8N1）打出 `[orpah-client] CH32V203 firmware skeleton (c1)`；
2. 心跳灯每 ~500 ms 翻一次；
3. 键 `AT` + 回车 → 回 `OK (rx=N)`。

## 引脚（★ 待定：等确定用哪块 CH32 开发板）

| 用途 | 本固件默认 | 说明 |
|---|---|---|
| 控制台（打印/调试） | **USART1 = PA9(TX)/PA10(RX)** | 参考板是 CH340C 接 USART1；本板待定 |
| 模组数据口（HGIC） | **USART2 = PA2(TX)/PA3(RX)**，115200 8N1 | 模组侧是**固定**的：TX-AH 的 UART0 = IOA10(TX)/IOA11(RX)（见 `../docs/txah-uart-macbus.md`）；c3 才用到 |
| 心跳灯 | **PC13**（低有效） | 不同开发板的用户灯不同 |

接线方式与电气细节见 `../hardware/wiring/`。

## 与参考固件的关系（来源已注明）

骨架的**套路与部分基础设施文件**来自 [halow-demo/simulator/firmware](https://github.com/Orpah/halow-demo)
—— 那是**我们自己仓的代码**（非第三方），按 `../AGENTS.md` §1「沿用参考骨架的套路，不另起一套风格」复用：

| 文件 | 处理 |
|---|---|
| `ld/link.ld`、`startup/startup_ch32v203.S`、`Core/ch32v20x.h`、`Periph/gpio.*`、`Periph/uart.*` | **原样搬过来**（只删掉了模拟器专有的 SPI/灯/拨码） |
| `Core/board.h` | **重写**：模拟器的 SPI/LED/DIP 换成 ORPAH 的控制台 + 模组数据口 + 心跳灯 |
| `Core/main.c` | **重写**：横幅 + 心跳 + 最小命令（`AT` → `OK`） |
| `Makefile` | 照抄结构，改两处：① `RISCV_PREFIX` 默认指向 MRS2 内的 `riscv-none-embed-`；② 修掉一个 bug —— 参考版写的是 `objcopy -O binary $@ $<`（把**输出**当输入），所以它的 `.bin` 目标其实跑不通，本版改成 `$< $@` |

## 下一步（别在这里自由发挥，按阶段来）

- **c2 协议内核**：`proto/` 下做 SN 解析 / Damm32 / 报文编解码（JCS 规范化 + 签名预像 + `b64url`），
  与 Python 参考实现**同一批黄金向量零偏差**（`run_cross_test.py`，纯 PC 可验，**不需要硬件**）。
- **c3 HGIC 数据口**：`Periph/hgic.c`（8 字节头 + `FRM2` + `CMD`/`EVENT`），与模组通话。
- **c4 选级 / 无 RTC / 自限频 / 已签上报**：判据 = 服务端验签通过（b 步台架已能判，见 `../tools/demo_l2_hgic.py`）。

两个**待用户拍板**的项（见 `../ROADMAP.md` §五）：软件 P-256 的来源（自写 / vendor + LICENSE / 先不签名）、
以及两处随机（`payload.nonce`；ECDSA 的 k 建议 RFC 6979）。
