# ORPAH 客户端固件（CH32V203 + TX-AH）—— c 步骨架
#
# 工具链：**必须用 MounRiver Studio 自带的那份**（独立 xPack 版中断模型不同、会跑飞）。
#   MRS2 内部路径（本机 2026-09-20 实测存在）：
#     F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/
#   前缀是 **riscv-none-embed-**（不是参考固件默认的 riscv-none-elf-）——
#   实测 `riscv-none-embed-gcc.exe --version` = "xPack GNU RISC-V Embedded GCC 8.2.0"。
#
# 用法：
#   make                                # -> build/orpah-client.elf / .bin
#   make RISCV_PREFIX='C:/别的/工具链/bin/riscv-none-elf-'
#   make clean
#
# ⚠ Windows 实测（2026-09-20，GNU Make 4.4.1 / chocolatey Windows32 版 + PowerShell）：
#   recipe 里的 POSIX 命令**不一定经过 sh** —— 对没有 shell 元字符的行，make 会直接
#   CreateProcess 那个程序 ⇒ `mkdir -p` / `ls -l` / `rm -rf` 都报「找不到指定的文件」。
#   而 `make SHELL='D:/Program Files/Git/bin/sh.exe'` 在这个组合下**没能改变**这一点
#   （`sh.exe` 确实在 D:/Program Files/Git/bin/ 里，但不在 PATH；把它加进 PATH 也没用）。
#   ⇒ 本仓的 recipe **不依赖 sh**：去掉 `ls`（改用 echo）、建/删目录**交给 Python**（`mkdirs.py` ✓）。
#
# ★★ 2026-10-12：**建/删构建目录交给 Python**（`mkdirs.py`，用户当天定 ✓ —— 见下面那段说明）：
#   shell 那几种写法都**实测过** ✗：`mkdir -p` 不是 Windows 程序；带引号的 `cmd /c "…"` 在本机 make 上
#   **不建目录**（正常环境 rc=2；PATH 不全的终端里会起交互式 cmd 卡住 ✗）；无引号的 `cmd /c …`
#   能过但**路径含空格就废** ✗。⇒ 用 `py -3 mkdirs.py`（**不传路径**，空格不再是问题 ✓、跨平台 ✓）。
#   ⚠ 代价：**构建需要 Python**（`py -3`；或 `make PY=python3`）—— `firmware/README.md` 已记明 ✓。
#
# ★ 另一条测出来的坑：变量赋值**不要写行内注释** —— `PROTO_DIR = ../proto   # 说明`
#   会把注释前的对齐空格留在值里 ⇒ 先决条件变成 `../proto   /sn.c`（找不到）而规则静静失效，
#   同时 `-I../proto   ` 又能编译（编译器忍受尾空格）⇒ 现象很迷惑。注释请单独一行。

RISCV_PREFIX ?= F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-

CC      = $(RISCV_PREFIX)gcc
OBJCOPY = $(RISCV_PREFIX)objcopy
SIZE    = $(RISCV_PREFIX)size

TARGET    = orpah-client
BUILD     = build
# 协议内核目录：与 PC 侧交叉测试用的是**同一份源码**（勿写行内注释，见文件头）
PROTO_DIR = ../proto
STARTUP   = startup/startup_ch32v203.S
LINK      = ld/link.ld

ARCH = -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore

CFLAGS = $(ARCH) -Os -ffunction-sections -fdata-sections \
         -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin \
         -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST \
         -ICore -IPeriph -I$(PROTO_DIR)

ASFLAGS = $(ARCH) -ICore -IPeriph

LDFLAGS = -nostartfiles -nostdlib -T $(LINK) \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

# libgcc：**必须排在目标文件之后**（见下面 .elf 规则：放在 $(OBJS) 后面）。
# 为何需要：32 位目标上 `jcs.c` / `sha256.c` 会用到 `__udivdi3`/`__umoddi3`/`__lshrdi3`
# （64 位除/模/右移），而 `-nostdlib` **不会自动带上 libgcc**。2026-09-20 实测：关掉
# `--gc-sections` 时不加它就链不上（不加也能编过，只因那时 proto 全是没人调用的死代码）。
LDLIBS = -lgcc

# 建目录 / 删目录：**用 Python**（`mkdirs.py`，见下面那段）—— `mkdir -p` / `rm -rf` 都**不行** ✗
# （不是 Windows 程序，make 对没有 shell 元字符的行会直接 CreateProcess ⇒ 报「找不到指定的文件」；
#  `SHELL=` 也改变不了 ✓）。
# ★★ 建/删构建目录 = **Python 干**（`mkdirs.py`，用户 2026-10-12 定 ✓）—— 建目录本来就不是
#   C/C++ 的任务 ✗；shell 那几种写法实测都不行（见文件头）。★ 关键：**不把路径传给脚本** ✓
#   —— 目录按脚本自己所在位置算 ⇒ **路径含空格也没事** ✓（无引号 `cmd /c` 就死在这里 ✗）。
#   脚本可用 `make PY=python3` 换解释器 ✓；POSIX 也走同一条路（少一套分支 ✓）。
ifeq ($(OS),Windows_NT)
  PY ?= py -3
else
  PY ?= python3
endif
MKDIRS = $(PY) mkdirs.py

# 固件自己的源文件。
SRCS = Core/main.c \
       Core/id_core.c \
       Periph/gpio.c \
       Periph/uart.c \
       Periph/i2c.c \
       Periph/atecc.c \
       Periph/hgic_uart.c

# 协议内核（`../proto/`）：**与 PC 侧交叉测试的是同一份源码**（AGENTS §4 的口径）。
# 它们都是**纯代码**（无 stdio/malloc/浮点/string.h）⇒ 能直接 `-nostdlib` 链接。
# ★ c3-2b（2026-09-20）：模组串口胶水已就位 = `Periph/hgic_uart.c`（中断入环 → 主循环
#   `hgic_parser_feed`；发送用 `hgic_frame_*`）。它**已经在被调用**（main：上电探测 +
#   收帧打印）⇒ 不再是被 gc 丢掉的死代码，`-lgcc` 的必要性也从"将来"变成"现在"。
# ⚠ 引脚/端口口径仍是**待定**（`Core/board.h` 的 MODULE_* 宏）；定板后只改 board.h。
PROTO_SRCS = sn.c jcs.c b64url.c msg.c downlink.c sha256.c hmac.c id_report.c hgic.c
# ★ c4-β-3（2026-09-22）：`id_report.c` 的 level=0 路径要 P-256/ECDSA/RFC 6979。
# ★ c4-γ-1（2026-09-22）：设备侧**流水线**（选级/演示密钥/nonce）已接进主循环（`Core/id_core.c`）
#   ⇒ 这五个文件现在是**真被调用**的（不再是 gc 丢掉的死代码）。
PROTO_SRCS += id_build.c id_level.c id_keys.c id_nonce.c p256.c rfc6979.c ecdsa.c
# ★ c4-γ-2（2026-09-22）：ATECC608B 的**纯编解码**（不含硬件）也放 proto/ ⇒
#   它跟着 PC 侧交叉测试一起对拍（黄金向量见 proto/test_vectors_atecc.txt）。
PROTO_SRCS += atecc_msg.c crc16.c
OBJS = $(addprefix $(BUILD)/, $(SRCS:.c=.o))
OBJS += $(addprefix $(BUILD)/proto/, $(PROTO_SRCS:.c=.o))
OBJS += $(BUILD)/startup.o

# ★★ 头文件依赖（2026-10-12 补，修一个**闸门空转**的毛病）：原先**一条头依赖都没有**
#   ⇒ 改 `Core/*.h` / `Periph/*.h` / `../proto/*.h` 之后 `make` 只报
#   "Nothing to be done for 'all'"，**一个 .o 都不重编** ✗ ⇒ `tools/run_checks.py` 那条
#   "固件编译（要求零 warning）"会**报 PASS 而其实什么都没编** ✗（自证陷阱的又一例）。
#   这里取**粗粒度**依赖（把所有头文件都当先决条件）：会多编几个文件，但不会有
#   "改了某些文件却没编"的静默漏 ✗。细粒度可用 gcc `-MMD` + include 那份 `.d`，
#   但多一份中间文件要管，收益不大。
#
#   ★★ 另外**必须**同时钉住默认目标：下面这条 `$(OBJS): $(HEADERS)` 是文件里**第一条规则**，
#     不钉的话 make 的默认目标就变成"第一个 .o" ✗ —— 实测（2026-10-12）现象是
#     `make` 只编 `build/Core/main.o` 就 **rc=0 退出**、`.elf` 根本不生成 ✗（很能骗人：
#     退出码 0 + 零 warning，看着像"编译通过"）。`.DEFAULT_GOAL` 那行就是为它加的 ✓。
.DEFAULT_GOAL := all

HEADERS = $(wildcard Core/*.h Periph/*.h $(PROTO_DIR)/*.h)
$(OBJS): $(HEADERS)

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex

$(BUILD):
	@$(MKDIRS) make

$(BUILD)/startup.o: $(STARTUP) | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# 协议内核在仓库另一目录 ⇒ 得用显式规则：`$(BUILD)/%.o: %.c` 会把 `../proto/x.c` 的目标
# 算成 `build/../proto/x.o`（= 把 .o 写回仓库，污染源码目录）。
$(BUILD)/proto/%.o: $(PROTO_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LINK)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	$(SIZE) $@

# ⚠ 这里**不要**用 `@ls -l $@`：make 在 Windows 上会直接去 exec `ls`（它不是 Windows 程序）
#   ⇒ 产物其实已生成，make 却退出码 2。改用 echo（cmd 的内建命令，这条路能跑）。
# ★ 这一行**用 ASCII**：make/cmd 的输出不经任何编码转换，中文在 cp936 控制台上会变乱码
#   （2026-09-20 实测：`已生成` 显示成 `宸茬敓鎴?`）。
$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "  built: $@  (raw binary; bytes = size text + data above)"

# ★ .hex：WCHISPTool 的“下载方式 USB”那条路用 .hex 是无歧义的（.hex 自带地址）；
#   .bin 则要依赖工具自己按 0x08000000 处理 —— 多一个产物就多一条可走的路。
$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@
	@echo "  built: $@  (intel hex; for WCHISPTool / MounRiver download)"

clean:
	@$(MKDIRS) clean

# ★★ 2026-10-12 用户定：**固件构建一律从零**（`make rebuild`）✓ —— 全量约 **3 s**（本机 / 26 个编译单元），
#   换来两件事：
#     ① “零 warning”的含意变成**整棵树都编过** ✓（增量时只保证“make 认为该编的那些” ✗）；
#     ② 彻底不靠“增量依赖写得全不全” ✓ —— 2026-10-12 之前正好漏了头依赖 ⇒ 改 `.h` 不重编 ✗、
#        而闸门还报 PASS ✗（就是这条约定要防的那类事 ✓）。
#   ⚠ 如实边界：3 s 是**本机 + 现规模**的数 ✓；固件长大后要重新捷量 ✓。
#   增量 `make` 仍可用（不删 ✓），但它**不作为交付/闸门口径** ✓。
rebuild:
	@$(MKDIRS) clean
	@$(MAKE) all

# 烧录**由用户执行**（本仓不代跑）
flash: $(BUILD)/$(TARGET).bin
	@echo "烧录（二选一，由你执行）："
	@echo "  A) WCH-Link / SWD："
	@echo "     openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg \\"
	@echo "            -c \"program $(BUILD)/$(TARGET).bin 0x08000000 verify reset exit\""
	@echo "  B) WCHISPTool（BOOT+RST 进刷机态，不需要 COM 口）"

.PHONY: all clean rebuild flash
