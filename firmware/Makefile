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
#   ⇒ 本仓三条 recipe 已改成**不依赖 sh**：去掉 `ls`（改用 echo）、`mkdir -p`/`rm -rf`
#     换成 `cmd /c`（见下面的 MKDIR/RMDIR；POSIX 平台仍用 mkdir/rm）⇒ 干净树上 `make` 可跑。
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

# 建目录 / 删目录：**不能用 `mkdir -p` / `rm -rf`** —— make 对没有 shell 元字符的行会直接
# CreateProcess 那个程序（不经 sh），而 `mkdir`/`rm` 都不是 Windows 程序（实测都报「找不到
# 指定的文件」；`SHELL=` 也改变不了）。改用 **cmd 这个真程序**（走 `cmd /c`）；
# POSIX 平台（make 的 OS 不是 Windows_NT）仍用 mkdir/rm。
ifeq ($(OS),Windows_NT)
  MKDIR = cmd /c "if not exist $(subst /,\,$(1)) mkdir $(subst /,\,$(1))"
  RMDIR = cmd /c "if exist $(subst /,\,$(1)) rmdir /s /q $(subst /,\,$(1))"
else
  MKDIR = mkdir -p $(1)
  RMDIR = rm -rf $(1)
endif

# 固件自己的源文件。
SRCS = Core/main.c \
       Core/id_core.c \
       Periph/gpio.c \
       Periph/uart.c \
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
OBJS = $(addprefix $(BUILD)/, $(SRCS:.c=.o))
OBJS += $(addprefix $(BUILD)/proto/, $(PROTO_SRCS:.c=.o))
OBJS += $(BUILD)/startup.o

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex

$(BUILD):
	@$(call MKDIR,$(BUILD))
	@$(call MKDIR,$(BUILD)/Core)
	@$(call MKDIR,$(BUILD)/Periph)
	@$(call MKDIR,$(BUILD)/proto)

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
	@$(call RMDIR,$(BUILD))

# 烧录**由用户执行**（本仓不代跑）
flash: $(BUILD)/$(TARGET).bin
	@echo "烧录（二选一，由你执行）："
	@echo "  A) WCH-Link / SWD："
	@echo "     openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg \\"
	@echo "            -c \"program $(BUILD)/$(TARGET).bin 0x08000000 verify reset exit\""
	@echo "  B) WCHISPTool（BOOT+RST 进刷机态，不需要 COM 口）"

.PHONY: all clean flash
