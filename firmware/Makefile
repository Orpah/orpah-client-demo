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
# ⚠ Windows 上 recipe 用的是 POSIX 命令（mkdir -p / rm -rf）⇒ make 要能找到一个 sh。
#   本机实测可用：make SHELL='D:/Program Files/Git/bin/sh.exe'

RISCV_PREFIX ?= F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-

CC      = $(RISCV_PREFIX)gcc
OBJCOPY = $(RISCV_PREFIX)objcopy
SIZE    = $(RISCV_PREFIX)size

TARGET  = orpah-client
BUILD   = build
STARTUP = startup/startup_ch32v203.S
LINK    = ld/link.ld

ARCH = -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore

CFLAGS = $(ARCH) -Os -ffunction-sections -fdata-sections \
         -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin \
         -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST \
         -ICore -IPeriph

ASFLAGS = $(ARCH) -ICore -IPeriph

LDFLAGS = -nostartfiles -nostdlib -T $(LINK) \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

# 骨架只有这三个。接下来的接线（c3-2，**尚未做**）：
#   · 把 proto/ 的纯代码编进来：../proto/hgic.c（+ msg.c/jcs.c/id_report.c…，都用不到 libc）
#   · 新增 Periph/ 下的模组串口胶水（USART2 ↔ HGIC：灌字节给 hgic_parser_feed、发 hgic_frame_*）
# ★ HGIC 的**帧层（纯逻辑）在 proto/hgic.{h,c}**，不在这里 —— 那样它才能在 PC 上
#   与 tools/txah_hgic.py 做交叉测试（见 proto/README.md「HGIC 帧层」一节）。
SRCS = Core/main.c \
       Periph/gpio.c \
       Periph/uart.c

OBJS = $(addprefix $(BUILD)/, $(SRCS:.c=.o))
OBJS += $(BUILD)/startup.o

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin

$(BUILD):
	@mkdir -p $(BUILD)/Core $(BUILD)/Periph

$(BUILD)/startup.o: $(STARTUP) | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LINK)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

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
