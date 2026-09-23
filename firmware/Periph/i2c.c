/* i2c.c — 硬件 I2C1 主机（寄存器级；只覆盖主机写/读与超时）。
 *
 * 寄存器序列照 STM32F1 参考手册的"主机发送/接收"流程（CH32V203 的 I2C 与之一致）：
 *   发送：PE=1 → START 等 SB → 写地址等 ADDR（读 SR1 再读 SR2 清 ADDR）→
 *         每字节等 TXE 后写 DR → 最后一字节等 **BTF**（真的移位出去了）→ STOP
 *   接收：START → 地址(W) → 字地址 → **RESTART** → 地址(R) → 等 ADDR →
 *         每字节等 RXNE 后读 DR（收完想结束的那字节给 NACK）→ STOP
 *
 * ★ 三条"不许省"的纪律（都是踩过同类坑总结的）：
 *   1) **每个等待都有超时**（`I2C_TIMEOUT_MS`）⇒ 超时返回 IT_TIMEOUT 并计数，不死等；
 *   2) **错误标志写 0 清**（BERR/ARLO/AF/OVR 与 TIM 的 INTFR 同类坑）⇒ 用读改写清，
 *      别整字写 0（会把别人也清了 / 或写 1 反而置位）；
 *   3) 时钟按 `SYSTEM_CLOCK_HZ` 算，**换主频/分频要改这里**（否则 NACK 或全 0xFF）。
 */
#include "i2c.h"

#include "board.h"
#include "gpio.h"

/* 主循环的 1 ms 时基（`Core/main.c` 定义；同 `Core/id_core.c` 的用法）。
 * 用它做**截止时刻**判断 —— 比"数循环次数"可读，也能回答"等了多久"。*/
extern volatile uint32_t g_tick_ms;

#define I2C_TIMEOUT_MS  20u
/* 100 kHz @ 8 MHz：CCR = 8e6 / (2 * 100e3) = 40；TRISE = FREQ + 1 = 9 */
#define I2C_FREQ_MHZ    (SYSTEM_CLOCK_HZ / 1000000UL)
#define I2C_CCR_100K    (I2C_FREQ_MHZ * 1000000UL / (2UL * SE_I2C_HZ))
#define I2C_TRISE       (I2C_FREQ_MHZ + 1UL)

#define IT_OK        (0)
#define IT_ARG       (-1)
#define IT_TIMEOUT   (-2)
#define IT_NACK      (-3)
#define IT_BUSERR    (-4)
#define IT_ARB       (-5)

static i2c_stats_t s_st;

/* 当前 I²C 速率（工装可改；缺省 = `board.h` 的 `SE_I2C_HZ`）。
 * ⚠ 唤醒令牌按手册必须 ≤100 kHz ⇒ `seclk` 就是为这条准备的。*/
static uint32_t s_hz = SE_I2C_HZ;

void i2c_set_hz(uint32_t hz)
{
    if (hz < 10000UL || hz > 400000UL) {
        return;                     /* 越界不动（宁可不改，也别把总线搞成怪的值）*/
    }
    s_hz = hz;
    i2c_init();                     /* 重算 CCR：单一源，不另写一份寄存器序列 */
}

uint32_t i2c_get_hz(void)
{
    return s_hz;
}

static int expired(uint32_t deadline)
{
    return (int32_t)(g_tick_ms - deadline) >= 0;
}

static void clear_err(void)
{
    uint32_t sr1 = I2C1->STAR1;

    I2C1->STAR1 = sr1 & ~(uint32_t)(I2C_STAR1_BERR | I2C_STAR1_ARLO |
                                    I2C_STAR1_AF | I2C_STAR1_OVR);
}

/* 等 STAR1 里某个（或某几个）位置起；mask 里任一位为 1 即返回。
 * `errbits` = 一出现就中止的错误位（AF/BERR/ARLO/OVR）。*/
static int wait_flag(uint32_t mask, uint32_t errbits, uint32_t timeout_ms)
{
    uint32_t deadline = g_tick_ms + timeout_ms;

    for (;;) {
        uint32_t sr1 = I2C1->STAR1;

        if ((sr1 & errbits) != 0u) {
            if ((sr1 & I2C_STAR1_AF) != 0u) {
                s_st.nack++;
                clear_err();
                return IT_NACK;
            }
            if ((sr1 & I2C_STAR1_BERR) != 0u) { s_st.bus_err++; clear_err(); return IT_BUSERR; }
            if ((sr1 & I2C_STAR1_ARLO) != 0u) { s_st.arb_lost++; clear_err(); return IT_ARB; }
            if ((sr1 & I2C_STAR1_OVR) != 0u)  { clear_err(); return IT_BUSERR; }
        }
        if ((sr1 & mask) != 0u) {
            return IT_OK;
        }
        if (expired(deadline)) {
            s_st.timeout++;
            return IT_TIMEOUT;
        }
    }
}

void i2c_init(void)
{
    /* 外设 + 端口时钟（uart_init 也是自己开外设钟；这里连 GPIO 一起开，
     * 免得调用顺序换了就静默不工作）*/
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO;

    /* 开漏复用（I2C 必须开漏：高电平靠外部 4.7 k 上拉）*/
    gpio_set_mode(SE_I2C_PORT, SE_SCL_PIN, GPIO_MODE_AF_OD_50MHZ);
    gpio_set_mode(SE_I2C_PORT, SE_SDA_PIN, GPIO_MODE_AF_OD_50MHZ);

    i2c_disable();
    I2C1->CTLR1 |= I2C_CTLR1_SWRST;      /* 复位外设状态机（上电/异常后用）*/
    I2C1->CTLR1 &= ~(uint32_t)I2C_CTLR1_SWRST;
    /* FREQ 必须在 PE=0 时写；CCR/TRISE 同理 */
    I2C1->CTLR2 = (I2C1->CTLR2 & ~(uint32_t)I2C_CTLR2_FREQ_MASK) |
                  (I2C_FREQ_MHZ & I2C_CTLR2_FREQ_MASK);
    {
        /* 标准模式 CCR = fPCLK1 / (2 × 目标速率)；参考手册要求 CCR ≥ 4 */
        uint32_t ccr = (I2C_FREQ_MHZ * 1000000UL) / (2UL * s_hz);

        if (ccr < 4UL) { ccr = 4UL; }
        I2C1->CKCFGR = ccr;
    }
    I2C1->RTR = I2C_TRISE;
    I2C1->OADDR1 = 0u;                   /* 本端只当主机，不用自身地址 */
    i2c_enable();
}

void i2c_disable(void)
{
    I2C1->CTLR1 &= ~(uint32_t)I2C_CTLR1_PE;
}

void i2c_enable(void)
{
    I2C1->CTLR1 |= (uint32_t)(I2C_CTLR1_PE | I2C_CTLR1_ACK);
}

int i2c_probe(uint8_t addr7)
{
    int rc;

    /* ★ 顺序只能是「先发 START，再等 SB」（2026-09-22 上机踩坑，c4-γ-2 第一次上机）：
     *   `i2c_disable()`/`i2c_enable()` 会把外设状态机复位 ⇒ 进这个函数时 SB **必为 0**；
     *   而 SB 只由硬件在 START 真的发出去之后才置起 ⇒ 若在 START 之前等 SB，
     *   这一等就是把 20 ms 超时耗光、返回 IT_TIMEOUT —— 现象是
     *   `[se] wake: NO ACK (ok=0 fail=N)` 里 **ok 恒为 0**（从没成功过一次），
     *   而接线/供电全对也一样，白查三件事。**别把等待挪到 START 前面**。*/
    I2C1->CTLR1 |= I2C_CTLR1_START;
    rc = wait_flag(I2C_STAR1_SB, 0u, I2C_TIMEOUT_MS);
    if (rc != IT_OK) { s_st.last_step = 2u; return rc; }
    (void)I2C1->STAR1;                   /* 读 SR1 → 写 DR 才清 SB */
    I2C1->DATAR = (uint32_t)((uint32_t)addr7 << 1);
    rc = wait_flag(I2C_STAR1_ADDR, I2C_STAR1_AF, I2C_TIMEOUT_MS);
    if (rc == IT_OK) {
        (void)I2C1->STAR1;               /* 读 SR1 再读 SR2 = 清 ADDR */
        (void)I2C1->STAR2;
    } else {
        s_st.last_step = 3u;
    }
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return rc;
}

int i2c_write(uint8_t addr7, const uint8_t *buf, uint32_t n)
{
    uint32_t i;
    int rc;

    if (buf == 0 && n != 0u) { return IT_ARG; }

    /* 等总线空闲（BUSY=0）。有器件把线钉住时这里会超时 —— **可见**，不死等。*/
    {
        uint32_t deadline = g_tick_ms + I2C_TIMEOUT_MS;

        while ((I2C1->STAR2 & I2C_STAR2_BUSY) != 0u) {
            if (expired(deadline)) { s_st.timeout++; s_st.last_step = 1u; return IT_TIMEOUT; }
        }
    }

    I2C1->CTLR1 |= I2C_CTLR1_ACK;

    /* ---- START ---- */
    I2C1->CTLR1 |= I2C_CTLR1_START;
    rc = wait_flag(I2C_STAR1_SB, 0u, I2C_TIMEOUT_MS);
    if (rc != IT_OK) { s_st.last_step = 2u; return rc; }
    s_st.start++;

    /* ---- 地址 + 写 ----
     * ★ 2026-09-23：这里原来**漏了“读 SR1”**（下面 `i2c_probe()` 有）。
     *   RM 的时序是“**读 SR1 → 写 DR 才清 SB**”，与探针路径不一致时会出现
     *   **同一个地址字节在探针里 ACK、在写路径里不过**的怪现象（现场 `se` 的
     *   `tx=0 nack` 在涨 就是这个形状）。三处（probe / write / read_begin 两段）现在一致。*/
    (void)I2C1->STAR1;
    I2C1->DATAR = (uint32_t)(((uint32_t)addr7 << 1) | 0u);
    rc = wait_flag(I2C_STAR1_ADDR, I2C_STAR1_AF, I2C_TIMEOUT_MS);
    if (rc != IT_OK) {
        s_st.last_step = 3u;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        return rc;
    }
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;

    /* ---- 数据 ---- */
    for (i = 0u; i < n; i++) {
        rc = wait_flag(I2C_STAR1_TXE, I2C_STAR1_AF | I2C_STAR1_BERR |
                                      I2C_STAR1_ARLO, I2C_TIMEOUT_MS);
        if (rc != IT_OK) {
            s_st.last_step = 4u;
            I2C1->CTLR1 |= I2C_CTLR1_STOP;
            return rc;
        }
        I2C1->DATAR = buf[i];
        s_st.tx_bytes++;
    }
    /* 最后一字节要等 BTF（真的移位出去了），否则 STOP 会截断它 */
    rc = wait_flag(I2C_STAR1_BTF, I2C_STAR1_AF | I2C_STAR1_BERR |
                                  I2C_STAR1_ARLO, I2C_TIMEOUT_MS);
    if (rc != IT_OK) { s_st.last_step = 5u; }
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return rc;
}

int i2c_read_begin(uint8_t addr7, uint8_t word_addr)
{
    int rc;

    {
        uint32_t deadline = g_tick_ms + I2C_TIMEOUT_MS;

        while ((I2C1->STAR2 & I2C_STAR2_BUSY) != 0u) {
            if (expired(deadline)) { s_st.timeout++; s_st.last_step = 1u; return IT_TIMEOUT; }
        }
    }

    /* 阶段 1：写“字地址”（**由调用方给**：ATECC 命令写 `0x03`、
     *   **读响应用 `0x00`** —— 见 `atecc.c` 的长注释与出处；这里曾经两个方向都用 0x03，是真 bug）*/
    I2C1->CTLR1 |= I2C_CTLR1_START;
    rc = wait_flag(I2C_STAR1_SB, 0u, I2C_TIMEOUT_MS);
    if (rc != IT_OK) { s_st.last_step = 2u; return rc; }
    (void)I2C1->STAR1;                  /* ★ 同上：读 SR1 → 写 DR 才清 SB */
    I2C1->DATAR = (uint32_t)(((uint32_t)addr7 << 1) | 0u);
    rc = wait_flag(I2C_STAR1_ADDR, I2C_STAR1_AF, I2C_TIMEOUT_MS);
    if (rc != IT_OK) {
        s_st.last_step = 3u;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        return rc;
    }
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;

    rc = wait_flag(I2C_STAR1_TXE, I2C_STAR1_AF, I2C_TIMEOUT_MS);
    if (rc != IT_OK) {
        s_st.last_step = 6u;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        return rc;
    }
    I2C1->DATAR = word_addr;
    s_st.tx_bytes++;
    rc = wait_flag(I2C_STAR1_BTF, I2C_STAR1_AF | I2C_STAR1_BERR, I2C_TIMEOUT_MS);
    if (rc != IT_OK) {
        s_st.last_step = 6u;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        return rc;
    }

    /* 阶段 2：RESTART + 地址（读）*/
    I2C1->CTLR1 |= I2C_CTLR1_START;
    rc = wait_flag(I2C_STAR1_SB, 0u, I2C_TIMEOUT_MS);
    if (rc != IT_OK) { s_st.last_step = 2u; return rc; }
    s_st.start++;
    (void)I2C1->STAR1;                  /* ★ 同上：读 SR1 → 写 DR 才清 SB */
    I2C1->DATAR = (uint32_t)(((uint32_t)addr7 << 1) | 1u);
    rc = wait_flag(I2C_STAR1_ADDR, I2C_STAR1_AF, I2C_TIMEOUT_MS);
    if (rc != IT_OK) {
        s_st.last_step = 7u;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        return rc;
    }
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;        /* 先 ACK，最后一字节前再撤 */
    return IT_OK;
}

int i2c_read_byte(uint8_t *b, int ack)
{
    int rc;

    if (b == 0) { return IT_ARG; }
    if (ack) {
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
    } else {
        I2C1->CTLR1 &= ~(uint32_t)I2C_CTLR1_ACK;
    }
    rc = wait_flag(I2C_STAR1_RXNE, I2C_STAR1_BERR | I2C_STAR1_ARLO, I2C_TIMEOUT_MS);
    if (rc != IT_OK) { s_st.last_step = 8u; return rc; }
    *b = (uint8_t)(I2C1->DATAR & 0xFFu);
    s_st.rx_bytes++;
    return IT_OK;
}

void i2c_read_end(void)
{
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
}

void i2c_stats(i2c_stats_t *out)
{
    if (out == 0) { return; }
    /* ⚠ 不要写 `*out = s_st`：结构体赋值在 -nostdlib 下会变成 memcpy 调用（同 hgic_uart.c 的注释）*/
    out->start    = s_st.start;
    out->tx_bytes = s_st.tx_bytes;
    out->rx_bytes = s_st.rx_bytes;
    out->nack     = s_st.nack;
    out->timeout  = s_st.timeout;
    out->last_step = s_st.last_step;
    out->bus_err  = s_st.bus_err;
    out->arb_lost = s_st.arb_lost;
}

void i2c_delay_us(uint32_t us)
{
    volatile uint32_t i;

    /* 8 MHz、每轮 ~4 个周期 ⇒ 1 µs ≈ 2 轮。**乘 4 倍余量**：这里宁可长一点，
     * 因为用它的两处（tWLO ≥60 µs、tWHI ≥1500 µs）都是**最小宽度**约束。*/
    for (i = 0u; i < us * 8u; i++) {
        /* 空转 */
    }
}
