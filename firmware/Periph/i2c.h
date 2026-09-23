/* i2c.h — 硬件 I2C1 主机（**只做 ATECC608B 需要的那点事**）
 *
 * 为什么用**硬件 I2C** 而不是位带（用户 2026-09-22 选）：
 *   · 时序交给外设，我们只等标志位；位带要自己数循环，8 MHz 下更脆。
 *   代价：往 `Core/ch32v20x.h` 加了 I2C 寄存器定义 —— 那是**与
 *   `halow-demo/simulator/firmware` 共享的 7 个平台层文件之一**（AGENTS §1），
 *   所以这份改动要同步到那一侧（已记在 `ch32v20x.h` 的注释里）。
 *
 * 口径（引脚/速率都从 `Core/board.h` 取，本文件不写死）：
 *   I2C1 = **PB6(SCL) / PB7(SDA)**、开漏、外接 4.7 kΩ 上拉到 3V3、100 kHz。
 *
 * ★ **所有等待都有上限**（`I2C_TIMEOUT_MS`），超时**返回错误并计数**，绝不死等：
 *   台架上"线没接对"是常态，死等只会让人以为板子挂了；报错 + 计数器才能定位。
 */
#ifndef __I2C_H__
#define __I2C_H__

#include <stdint.h>

#include "ch32v20x.h"

/* ATECC608B 的 7 位地址（出厂固定 0x60；8 位写地址 = 0xC0）*/
#define I2C_ADDR_ATECC   0x60u

typedef struct {
    uint32_t start;      /* 成功的 START 次数 */
    uint32_t tx_bytes;   /* 写出去的字节数 */
    uint32_t rx_bytes;   /* 读进来的字节数 */
    uint32_t nack;       /* 地址/数据被 NACK（AF）的次数 */
    uint32_t timeout;    /* 等标志超时的次数（**超时必须可见**）*/
    uint32_t bus_err;    /* BERR */
    uint32_t arb_lost;   /* ARLO */
    uint8_t  last_step;  /* ★ **最近一次失败卡在哪一步**（现场一眼定位，别再猜）：
                          *   0 = 没失败过；1 = 等 BUSY 清零超时；2 = 等 SB 超时（START 没发出去）；
                          *   3 = 写方向的地址阶段失败（NACK/超时）；4 = 数据字节阶段失败；
                          *   5 = 最后一字节 BTF 失败；6 = 读方向：字地址字节阶段失败；
                          *   7 = 读方向：RESTART 后的地址阶段失败；8 = 读字节（RXNE）失败。*/
} i2c_stats_t;

/* 初始化 I2C1（含 GPIOB/AFIO 与 I2C1 的外设时钟；100 kHz @ APB1=SYS_CLOCK）。
 * ★ 假设 APB1 = `SYSTEM_CLOCK_HZ`（8 MHz 无 PLL、无分频）。若将来改主频/分频，
 *   这里要跟着改，否则波特率会不对（现象：地址 NACK / 读出全 0xFF）。*/
void i2c_init(void);

/* 关掉外设（PE=0）/ 重新打开。ATECC 的**唤醒脉冲**要把 SDA 拉低当普通 GPIO 用，
 * 那时必须先把外设关掉 —— 否则外设会把那个下降沿当成一次 START（见 atecc.c）。*/
void i2c_disable(void);
void i2c_enable(void);

/* 只发地址看有没有 ACK（返回 0 = 有器件应答）。
 * `read_dir` != 0 ⇒ 发**读方向**（`addr<<1 | 1`）—— 真从机两个方向都该 ACK，
 * 所以“W 方向 ACK 但 R 方向 NACK”是用来分辨“真器件”与“伪/偶然 ACK”的一条判据。*/
int i2c_probe_dir(uint8_t addr7, int read_dir);
int i2c_probe(uint8_t addr7);

/* ★ 工装（2026-09-23）：运行时改 I²C 速率。
 * 为什么需要：手册要求**唤醒令牌 ≤100 kHz**，而我们把 CCR 算成**恰好 100 kHz** ——
 * 主频用的是内部 HSI（±1% 量级），只要有一点正偏差就**越界**，而越界时器件会
 * **默默忽略唤醒令牌**（现象：总线全好、`timeout=0`、永远 NACK）。
 * ⇒ 现场要能一键降到 50 kHz 对比（`seclk 50`）。hz 会被夹到 [10k, 400k]。*/
void     i2c_set_hz(uint32_t hz);
uint32_t i2c_get_hz(void);

/* START + addr|W + buf[0..n) + STOP。返回 0 = OK，<0 = 错误码（见 i2c.c 的 IT_*）。*/
int i2c_write(uint8_t addr7, const uint8_t *buf, uint32_t n);

/* "读" 的一整段事务：START + addr|W + 字地址 0x03 + **RESTART** + addr|R。
 * 调用序列必须是 `read_begin()` → 若干 `read_byte()` → `read_end()`；
 * 中间**不能**停（ATECC 的响应要在同一段事务里连着读完，见 atecc.c 的说明）。*/
int  i2c_read_begin(uint8_t addr7, uint8_t word_addr);
int  i2c_read_byte(uint8_t *b, int ack);
void i2c_read_end(void);

void i2c_stats(i2c_stats_t *out);

/* 忙等延时（**近似**，只保证"至少"：8 MHz 下按 ~1 µs ≈ 2 次循环预算，再乘余量）。
 * ⚠ 别拿它当精确计时用 —— 它只服务"最小宽度"类约束（如 ATECC 的 tWLO ≥60 µs）。*/
void i2c_delay_us(uint32_t us);

#endif /* __I2C_H__ */
