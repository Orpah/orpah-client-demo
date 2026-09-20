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
#   ⇒ 本仓已把 recipe 里的 `ls` 去掉（改用 make 原语）；**`mkdir -p` / `rm -rf` 还在**，
#     干净树上会挂 —— 要改得你点头。当下可用：先自行建好 build/{Core,Periph,proto}，再 `make`。
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

# 固件自己的源文件。
SRCS = Core/main.c \
       Periph/gpio.c \
       Periph/uart.c

# 协议内核（`../proto/`）：**与 PC 侧交叉测试的是同一份源码**（AGENTS §4 的口径）。
# 它们都是**纯代码**（无 stdio/malloc/浮点/string.h）⇒ 能直接 `-nostdlib` 链接。
# ★ 已就位：c2（SN/JCS/b64url/报文/下行/SHA-256/HMAC/已签报文）+ c3-1（HGIC 帧层）。
# **尚未做**：Periph/ 下的模组串口胶水（USART2 ↔ HGIC：灌字节给 `hgic_parser_feed`、
#   发 `hgic_frame_*`）—— 它的引脚口径**待定**；现在只是把它们编进来（还没人调）。
PROTO_SRCS = sn.c jcs.c b64url.c msg.c downlink.c sha256.c hmac.c id_report.c hgic.c

OBJS = $(addprefix $(BUILD)/, $(SRCS:.c=.o))
OBJS += $(addprefix $(BUILD)/proto/, $(PROTO_SRCS:.c=.o))
OBJS += $(BUILD)/startup.o

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin

$(BUILD):
	@mkdir -p $(BUILD)/Core $(BUILD)/Periph $(BUILD)/proto

$(BUILD)/startup.o: $(STARTUP) | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# 协议内核在仓库另一目录 ⇒ 得用显式规则：`$(BUILD)/%.o: %.c` 会把 `../proto/x.c` 的目标
# 算成 `build/../proto/x.o`（= 把 .o 写回仓库，污染源码目录）。
$(BUILD)/proto/%.o: $(PROTO_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LINK)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	$(SIZE) $@

# ⚠ 这里**不要**用 `@ls -l $@`：make 在 Windows 上会直接去 exec `ls`（它不是 Windows 程序）
#   ⇒ 产物其实已生成，make 却退出码 2。改用 echo（cmd 的内建命令，这条路能跑）。
# ★ 这一行**用 ASCII**：make/cmd 的输出不经任何编码转换，中文在 cp936 控制台上会变乱码
#   （2026-09-20 实测：`已生成` 显示成 `宸茬敓鎴?`）。
$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "  built: $@  (raw binary; bytes = size text + data above)"

clean:
	rm -rf $(BUILD)

# 烧录**由用户执行**（本仓不代跑）
flash: $(BUILD)/$(TARGET).bin
	@echo "烧录（二选一，由你执行）："
	@echo "  A) WCH-Link / SWD："
	@echo "     openocd -f interface/wch-link.cfg -f target/ch32v20x.cfg \\"
	@echo "            -c \"program $(BUILD)/$(TARGET).bin 0x08000000 verify reset exit\""
	@echo "  B) WCHISPTool（BOOT+RST 进刷机态，不需要 COM 口）"

.PHONY: all clean flash
