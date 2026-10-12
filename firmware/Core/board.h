/* board.h — ORPAH 客户端（CH32V203 + TX-AH）板级配置
 *
 * 与参考骨架（halow-demo/simulator/firmware/Core/board.h）同结构：
 * 引脚/波特率/IRQ 属性都集中在这里，驱动与业务代码不写死引脚。
 *
 * ★ 引脚口径 **2026-09-21 上机实测确定**（台架 = nanoCH32V203 + TX-AH EVB + CH347F，
 *   接线表/判据/四个真凶见 `docs/c3-2b-bench-bringup.md`）。换板子只改本文件
 *   （含每个端口的**时钟位**）+ README 的接线表，驱动/协议代码不受影响。
 */
#ifndef __BOARD_H__
#define __BOARD_H__

#include "ch32v20x.h"

/* ------------------------------------------------------------------ */
/* 主频：8 MHz HSI、无 PLL（与参考固件一致：最简最稳，115200 误差 ~0.6%）  */
/* ------------------------------------------------------------------ */
#define SYSTEM_CLOCK_HZ      8000000UL

/* ------------------------------------------------------------------ */
/* 控制台（打印 / 调试）                                                */
/*   台架上接 CH347F 的 `P2/UART0`（Windows 侧 = COM23，换口/换机就变）。 */
/* ------------------------------------------------------------------ */
#define CONSOLE_UART         USART1
#define CONSOLE_UART_IRQn    USART1_IRQn
#define CONSOLE_UART_IRQ     USART1_IRQHandler
#define CONSOLE_BAUD         115200UL
#define CONSOLE_TX_PIN       9           /* PA9  (AF PP) */
#define CONSOLE_RX_PIN       10          /* PA10 (input) */

/* ------------------------------------------------------------------ */
/* 模组数据口 = TX-AH 的 UART0（刷 MACBUS_UART 固件后的 HGIC 帧，115200 8N1）*/
/*   模组侧是**固定**的：UART0 = IOA10(RX) / IOA11(TX)，见             */
/*   docs/txah-uart-macbus.md（b 步实测：8 字节 HGIC 头 + FRM2 数据帧）。   */
/*   ★ 2026-09-21 上机实测：本板用 USART2 = PA2/PA3，交叉接线：           */
/*     PA2(TX) → 模组 A10(**模块的 RX**)、PA3(RX) ← 模组 A11(**模块的 TX**)  */
/*   驱动 = `Periph/hgic_uart.c`（中断入环 + 主循环喂 `proto/hgic.c` 的帧层）。*/
/* ------------------------------------------------------------------ */
#define MODULE_UART          USART2
#define MODULE_UART_IRQn     USART2_IRQn
#define MODULE_UART_IRQ      USART2_IRQHandler
#define MODULE_BAUD          115200UL
#define MODULE_GPIO_PORT     GPIOA       /* 下面两个引脚所在的端口 */
#define MODULE_TX_PIN        2           /* PA2 -> 模组 A10 (模块的 RX) */
#define MODULE_RX_PIN        3           /* PA3 <- 模组 A11 (模块的 TX) */

/* ⚠ `MODULE_UART_IRQ` 与 `CONSOLE_UART_IRQ` **不能是同一个** —— 若撞了，会直接报
 *   “redefinition of `USART2_IRQHandler`”（因为两个宏会展开成同一个函数名）。
 *   这正好当**编译期护栏**用：换板子改引脚时配错，编的时候就红，不会到板上才发现。*/

/* ------------------------------------------------------------------ */
/* 心跳灯（"烧进去了没有"的最小可视判据）                                 */
/*   ★ 板载蓝灯 `D1` = **PA15**、**低电平点亮**（2026-09-21 由原理图确认）：
 *     原理图那一列是 `3V3 → R3(10K) → D1(BLUE) → PA15` ⇒ 拉低才亮。
 *     旁证：板上出厂 demo `demo/blink_1000.bin` 反汇编后驱动的正是
 *     `GPIOA` 掩码 `0x8000`（= PA15）⇒ 两条独立证据一致。
 *     （之前外接灯接在 `C13`/PC13 上，也不是不能用 —— 把下面两行改回
 *      `GPIOC` / `13` 即可；只是 PA15 少一根外接线。）*/
#define LED_HEART_PORT       GPIOA
#define LED_HEART_PIN        15          /* PA15, ACTIVE LOW（3V3→10K→LED→PA15）*/
#define LED_HEART_ACTIVE_LOW 1
/* 心跳灯所在端口的时钟使能位 —— 跟着 `LED_HEART_PORT` 一起改
 * （上电“第一口呼吸”要在任何其它外设之前先开它）*/
#define LED_HEART_CLK        RCC_APB2Periph_GPIOA

/* ------------------------------------------------------------------ */
/* 安全元件 ATECC608B（**I2C1**，c4-γ-2 起）                             */
/*   ★ 2026-09-22 接线口径（用户确认）：                                  */
/*     PB6 = SCL、PB7 = SDA —— I2C1 的**默认引脚**（AF 开漏），           */
/*     外接 **4.7 kΩ ×2 上拉到 3V3**（ATECC 的 I2C 必须有上拉）。          */
/*   ATECC608B SOIC-8 引脚：pin4 = GND、pin5 = SDA、pin6 = SCL、pin8 = VCC */
/*         （VCC 2.0~5.5 V，IO 1.8~5.5 V ⇒ 3.3 V 直接接，见摘要手册 §2.1）*/
/*   ⚠ 速率常量 `SE_I2C_HZ` 只被 `Periph/i2c.c` 用来算 CCR；改了要重算。   */
/* ------------------------------------------------------------------ */
/* ★ 2026-10-13：SE 的 I²C **可以二选一**（编译期开关，见下面的 `SE_I2C_USE_2`）——
 *   台架用 **I2C1 @ PB6/PB7**（c4-γ-2 起、已上机 ✓）、载板用 **I2C2 @ PB10/PB11**
 *   （口径见 `docs/carrier-board-spec.md` §3；**未上机** ✗）。
 *   ⇒ 下面 PORT/SCL/SDA 与新增的 `SE_I2C`/`SE_I2C_CLK` 全跟着开关走，
 *     所以 `Periph/i2c.c` 与 `Periph/atecc.c` 的**代码一行都不用改** ✓
 *     （它们只认这几个宏 ✓）。
 *   ⚠ 两种口都靠**板上的 4.7 kΩ ×2 上拉到 3V3** —— 载板上别忘 ✓。
 *   ⚠ I2C2 与 I2C1 同在 **APB1**（时钟位不同）；PB10/PB11 是 I2C2 的**默认** AF
 *     引脚 ⇒ **不需要** AFIO 重映射（I2C1 的 PB6/PB7 同理 ✓）。
 *   ★ 自检：`make rebuild CFLAGS=-DSE_I2C_USE_2=1` 必须也能**零 warning** 编过 ✓
 *     （载板档我这边没有板子，只能做到“编得过 + 代码里真的引用 I2C2”✓，
 *      真机验收仍需上机 ✗ —— 别把“编得过”读成“已上机”✗）。*/
#ifndef SE_I2C_USE_2
#define SE_I2C_USE_2         0
#endif
#if (SE_I2C_USE_2 != 0) && (SE_I2C_USE_2 != 1)
#error "SE_I2C_USE_2 只能是 0（台架 I2C1）或 1（载板 I2C2）"
#endif
#if SE_I2C_USE_2
#define SE_I2C               I2C2
#define SE_I2C_CLK           RCC_APB1Periph_I2C2
#define SE_I2C_PORT          GPIOB
#define SE_SCL_PIN           10          /* PB10 = I2C2_SCL */
#define SE_SDA_PIN           11          /* PB11 = I2C2_SDA */
#else
#define SE_I2C               I2C1
#define SE_I2C_CLK           RCC_APB1Periph_I2C1
#define SE_I2C_PORT          GPIOB
#define SE_SCL_PIN           6           /* PB6 = I2C1_SCL */
#define SE_SDA_PIN           7           /* PB7 = I2C1_SDA */
#endif
#define SE_I2C_HZ            100000UL

/* ------------------------------------------------------------------ */
/* 中断属性：必须与 -DWCH_INTERRUPT_FAST + 启动文件里的硬件栈配合          */
/* ------------------------------------------------------------------ */#if defined(__riscv) && defined(WCH_INTERRUPT_FAST)
#define APP_IRQ __attribute__((interrupt("WCH-Interrupt-fast")))
#elif defined(__riscv)
#define APP_IRQ __attribute__((interrupt))
#else
#define APP_IRQ
#endif

/* 兼容参考骨架里用 SIM_IRQ 写的代码（照搬过来的驱动不必改名） */
#ifndef SIM_IRQ
#define SIM_IRQ APP_IRQ
#endif

#endif /* __BOARD_H__ */
