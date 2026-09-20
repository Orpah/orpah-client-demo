/* main.c — ORPAH 客户端固件（c 步骨架）
 *
 * 这一版**只做最小可验证的事**：
 *   1) 上电打横幅（控制台，115200 8N1）；
 *   2) 心跳灯翻转（"固件真的跑起来了"）；
 *   3) 控制台回显 + 认一条最简命令（`AT` → `OK`），用来确认串口收发正常。
 *
 * ★ 还没做的（留给后面的阶段，别在这里自由发挥）：
 *   · c2 协议内核（SN/Damm32/报文编解码/JCS + 交叉测试）→ 在 proto/，纯 PC 可验；
 *   · c3 HGIC 数据口（Periph/hgic.c：8 字节头 + FRM2 + CMD/EVENT）→ 与模组通话；
 *   · c4 §8.2 选级 + 无 RTC 声明（ts=0 / cap.rtc=false）+ 自限频 + 已签 ID 上报。
 *
 * ⚠ 时钟：8 MHz HSI、无 PLL（见 board.h）。**烧录由用户执行**（见 Makefile 的 flash 目标）。
 */
#include "board.h"
#include "gpio.h"
#include "uart.h"

/* ------------------------------------------------------------------ */
/* 控制台收字节（回显 + 极简命令）                                       */
/* ------------------------------------------------------------------ */
static volatile uint8_t g_last = 0;
static volatile uint32_t g_rx_bytes = 0;

static void console_rx(uint8_t b)
{
    g_last = b;
    g_rx_bytes++;
    uart_putc(CONSOLE_UART, b);            /* 回显：串口通不通一眼看出 */
}

/* ------------------------------------------------------------------ */
/* 1 ms 时基（照参考骨架：TIM2；后面 c3/c4 的节拍/超时都靠它）              */
/* ------------------------------------------------------------------ */
volatile uint32_t g_tick_ms;

static void tick_init(void)
{
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;

    TIM2->CTLR1 = 0;
    TIM2->PSC = SYSTEM_CLOCK_HZ / 1000 - 1;   /* -> 1 kHz */
    TIM2->ATRLR = 0;                          /* 周期 = 1 ms */
    TIM2->DMAINTENR |= TIM_DMAINTENR_UIE;
    TIM2->CTLR1 |= TIM_CTLR1_CEN | TIM_CTLR1_ARPE;
    NVIC_EnableIRQ(TIM2_IRQn);
}

static void banner(void)
{
    uart_printf(CONSOLE_UART, "\r\n");
    uart_printf(CONSOLE_UART, "[orpah-client] CH32V203 firmware skeleton (c1)\r\n");
    uart_printf(CONSOLE_UART, "[orpah-client] clock=%lu Hz HSI, console=%lu 8N1\r\n",
                (unsigned long)SYSTEM_CLOCK_HZ, (unsigned long)CONSOLE_BAUD);
    uart_printf(CONSOLE_UART, "[orpah-client] TODO: HGIC 数据口(c3) / 协议内核(c2) / 签名上报(c4)\r\n");
    uart_printf(CONSOLE_UART, "[orpah-client] 输入 AT + 回车 -> OK\r\n");
}

/* ------------------------------------------------------------------ */
/* SystemInit：startup 进 main 之前会调用它（必须存在，否则链接不过）        */
/* ------------------------------------------------------------------ */
void SystemInit(void)
{
    /* 复位后默认就是 HSI 8 MHz —— 这里什么都不用配。
     * 将来要用 HSE/PLL：在 RCC->CTLR 使能、等就绪，再配 RCC->CFGR0 与 Flash 等待周期。 */
}

int main(void)
{
    uint32_t last_beat = 0;

    /* GPIO/控制台时钟（uart_init 只配引脚与波特率，时钟得自己开） */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                      RCC_APB2Periph_AFIO;

    /* 心跳灯（先灭） */
    gpio_set_mode(LED_HEART_PORT, LED_HEART_PIN, GPIO_MODE_OUT_PP_2MHZ);
    gpio_set_pin(LED_HEART_PORT, LED_HEART_PIN, LED_HEART_ACTIVE_LOW ? 1 : 0);

    uart_init(CONSOLE_UART, GPIOA, CONSOLE_TX_PIN, CONSOLE_RX_PIN,
              CONSOLE_BAUD, CONSOLE_UART_IRQn, console_rx);

    tick_init();
    banner();

    for (;;) {
        /* 心跳：每 ~500 ms 翻一次灯（只看"在变"，不关心亮还是灭） */
        if ((int32_t)(g_tick_ms - last_beat) >= 500) {
            last_beat = g_tick_ms;
            gpio_set_pin(LED_HEART_PORT, LED_HEART_PIN,
                         (uint8_t)!gpio_get_pin(LED_HEART_PORT, LED_HEART_PIN));
        }

        /* 控制台最小命令：收到 'T'（AT 的 T）就回 OK —— 只为确认收发链路 */
        if (g_last == 'T') {
            g_last = 0;
            uart_printf(CONSOLE_UART, "\r\nOK (rx=%lu)\r\n", (unsigned long)g_rx_bytes);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 中断向量（名称必须与 startup 的向量表一致）                            */
/* ------------------------------------------------------------------ */
APP_IRQ void TIM2_IRQHandler(void)
{
    TIM2->INTFR &= ~TIM_INTFR_UIF;
    g_tick_ms++;
}

APP_IRQ void CONSOLE_UART_IRQ(void)
{
    uart_irq_rx(CONSOLE_UART);
}
