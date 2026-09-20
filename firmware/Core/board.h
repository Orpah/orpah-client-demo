/* board.h — ORPAH 客户端（CH32V203 + TX-AH）板级配置
 *
 * 与参考骨架（halow-demo/simulator/firmware/Core/board.h）同结构：
 * 引脚/波特率/IRQ 属性都集中在这里，驱动与业务代码不写死引脚。
 *
 * ★★ 本文件里的引脚是 **待定** 项：c 步用哪块 CH32 开发板、怎么接线，由用户确定后
 *    只改本文件（+ README 的接线表），驱动/协议代码不受影响。
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
/*   参考板是 CH340C 接 USART1；本板待定 —— 先沿用 PA9/PA10。            */
/*   c 步的 b 步台架（CH347F）用的是另一条口，互不影响。                   */
/* ------------------------------------------------------------------ */
#define CONSOLE_UART         USART1
#define CONSOLE_UART_IRQn    USART1_IRQn
#define CONSOLE_UART_IRQ     USART1_IRQHandler
#define CONSOLE_BAUD         115200UL
#define CONSOLE_TX_PIN       9           /* PA9  (AF PP) */
#define CONSOLE_RX_PIN       10          /* PA10 (input) */

/* ------------------------------------------------------------------ */
/* 模组数据口 = TX-AH 的 UART0（刷 MACBUS_UART 固件后的 HGIC 帧，115200 8N1）*/
/*   模组侧是**固定**的：UART0 = IOA10(TX) / IOA11(RX)，见               */
/*   docs/txah-uart-macbus.md（b 步实测：8 字节 HGIC 头 + FRM2 数据帧）。   */
/*   ★ 待定：本板用哪个 USART 接过去 —— 先默认 USART2 = PA2/PA3。          */
/*   c3 才会真正用到（驱动 Periph/hgic.c 还没写）。                        */
/* ------------------------------------------------------------------ */
#define MODULE_UART          USART2
#define MODULE_UART_IRQn     USART2_IRQn
#define MODULE_UART_IRQ      USART2_IRQHandler
#define MODULE_BAUD          115200UL
#define MODULE_TX_PIN        2           /* PA2 -> 模组 IOA11 (RX) */
#define MODULE_RX_PIN        3           /* PA3 <- 模组 IOA10 (TX) */

/* ------------------------------------------------------------------ */
/* 心跳灯（"烧进去了没有"的最小可视判据）                                 */
/*   ★ 待定：不同开发板的用户灯不同；PC13 是最常见的一颗。                  */
/* ------------------------------------------------------------------ */
#define LED_HEART_PORT       GPIOC
#define LED_HEART_PIN        13          /* PC13, ACTIVE LOW */
#define LED_HEART_ACTIVE_LOW 1

/* ------------------------------------------------------------------ */
/* 中断属性：必须与 -DWCH_INTERRUPT_FAST + 启动文件里的硬件栈配合          */
/* ------------------------------------------------------------------ */
#if defined(__riscv) && defined(WCH_INTERRUPT_FAST)
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
