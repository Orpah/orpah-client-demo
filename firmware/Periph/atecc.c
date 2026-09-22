/* atecc.c — ATECC608B 的 I²C 事务与唤醒。见 atecc.h 的时序出处与"如实"说明。
 *
 * 一次 `Random` 的完整动作（照 ATECC 的 I²C 规矩）：
 *   ① 唤醒：关 I2C 外设 → SDA(PB7) 拉低 ≥60 µs → 放开 → 等 ≥1500 µs → 开外设
 *            → 写"唤醒令牌" = 地址 0x00，收到 ACK 才算醒（这是权威做法：CryptoAuthLib
 *              `hal_check_wake()` 就是发 0x00 看 ACK，不是靠猜时间）
 *   ② 发命令：START + 0xC0 + **字地址 0x03** + 包（5 或 7 字节） + STOP
 *   ③ 收响应：START + 0xC0 + 0x03 + **RESTART** + 0xC1 → 先读 4 字节 count
 *            → 按 count 接着读 count-4 字节（**同一段事务里读完**，中间不能停）
 *   ④ 解析：`proto/atecc_msg.c`（长度/count/CRC 三道检查）
 *
 * ★ CRC 模式自动判定（为什么必须自动）：线上要不要挂 CRC 取决于芯片 Config 区
 *   `ChipMode` 的 I2C-CRC 位，而**我们手上只有 NDA 摘要手册、没有 Config 默认值**
 *   ⇒ 不猜：先按"不挂 CRC"发一条，若响应 count 不是 36，再按"挂 CRC"发一条；
 *   哪一种通就**记住**（`s_st.crc_mode`），后面都用它。判定的结果打在 `se` 命令上。
 */
#include "atecc.h"

#include "atecc_msg.h"
#include "board.h"
#include "crc16.h"
#include "gpio.h"
#include "i2c.h"
#include "id_nonce.h"          /* idn_hex_upper()（别在业务代码里另写一份 hex）*/
#include "uart.h"              /* 控制台（自检打印）*/

#define ATECC_WORD_ADDR         0x03u
#define ATECC_WAKE_TOKEN_ADDR   0x00u
#define ATECC_PWRUP_US          200u    /* > tPU(100 µs)，多给一倍余量 */
/* ★ 唤醒令牌的轮询次数与间隔（2026-09-22，依据=摘要手册表 2-2，见 `atecc_wake()` 的注释）：
 *   手册允许用"轮询"代替干等，而**使能自检时**要等 `tWHIST ≥ 20 ms` 才收令牌。
 *   6 次 × 间隔（`i2c_delay_us(2000)` 实测 ≈ 8~10 ms）≈ 50 ms ⇒ 覆盖 20 ms 有余量。*/
#define ATECC_WAKE_TRIES        6u
#define ATECC_WAKE_GAP_US       2000u

static atecc_stats_t s_st;

/* ★ 命令用的 7 位地址（缺省 = `i2c.h` 的 `I2C_ADDR_ATECC`）。
 * 为什么做成可设（2026-09-23）：手册第 13 页明写 **ATECC608B 的 I²C 地址是可编程的**
 * （"Programmable I2C address after data (secret) zone lock"），而公开资料里实测到的
 * 7 位地址有 **0x35**（`i2cdetect` 直接看得到）这类**不是 0x60** 的例子。
 * 我们以前把 0x60 写死 ⇒ 如果这颗不是 0x60，就会表现为"接线全对、永远 NACK"。
 * ⇒ `sewake` 扫到谁就用谁（`s_addr = 找到的地址`），控制台也可手动 `seaddr <hex>`。*/
static uint8_t s_addr = I2C_ADDR_ATECC;

void atecc_set_addr(uint8_t addr7)
{
    s_addr = (uint8_t)(addr7 & 0x7Fu);
}

uint8_t atecc_get_addr(void)
{
    return s_addr;
}
static int s_present;
static int s_probed;                    /* 探过了没（`atecc_init()` 幂等，见下）*/
static int s_crc_mode = -1;             /* -1 = 还没判出来 */

/* 主循环的 1 ms 时基（`Core/main.c` 定义；同 `Core/id_core.c` 的用法）——
 * 只给工装 `atecc_line_test()` 用，量半周期比忙等函数准。*/
extern volatile uint32_t g_tick_ms;

/* ------------------------------------------------------------------ */
/* 唤醒                                                              */
/* ------------------------------------------------------------------ */
/* ② + ③：只做"脉冲 + tWHI"（**不判 ACK**）。
 * 单独抽出来是因为工装 `atecc_diag_probe()` 要先看"**不把没 ACK 当成器件不在**"会怎样。*/
static void wake_pulse(void)
{
    /* ② 唤醒脉冲：SDA 拉低 ≥60 µs。**先关外设**，否则它会把这一下当成 START。*/
    i2c_disable();
    gpio_set_mode(SE_I2C_PORT, SE_SDA_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_set_pin(SE_I2C_PORT, SE_SDA_PIN, 0u);
    i2c_delay_us(100u);                 /* tWLO 下界 60 µs；忙等不可靠 ⇒ 给 100 µs */

    /* ③ 放开 SDA（开漏输出 1 = 交给上拉）、回到复用功能，等 tWHI ≥1500 µs */
    gpio_set_pin(SE_I2C_PORT, SE_SDA_PIN, 1u);
    gpio_set_mode(SE_I2C_PORT, SE_SDA_PIN, GPIO_MODE_AF_OD_50MHZ);
    i2c_delay_us(1700u);
    i2c_enable();
}

int atecc_wake(void)
{
    uint32_t tries;
    int rc;

    /* ① 上电延时（若刚上电；重复调用只是多等 200 µs，无害）*/
    i2c_delay_us(ATECC_PWRUP_US);

    /* ★ 整段唤醒序列最多重做 `ATECC_WAKE_TRIES` 次（手册表 2-2 明确允许"轮询"）。
     *   为什么**每轮都重做脉冲**、而不是只重发令牌：脉冲没被器件认到时，
     *   只重发令牌是没用的（器件根本没进入"要醒"的状态）。
     *
     *   ★ 为什么必须轮询（2026-09-22 把摘要手册表 2-2 逐字核出来的，以前漏了这条）：
     *     同一张表给的是**两个情形** ——
     *       未使能自检：`tWHI  ≥ 1500 µs`
     *       **使能自检：`tWHIST ≥ 20   ms`**
     *     两行的说明都写着 "SDA should be stable high for this entire duration
     *     **unless polling is implemented**"；而 `tPU` 那行还补了一句
     *     "The power-up delay will be significantly longer if power-on self test
     *     is enabled in the Configuration zone."
     *   ⇒ 只等 1700 µs 且**只发一次**令牌时，一颗**完全正常**的芯片（自检开）也会回 NACK
     *     —— 现象就是 `[se] wake: NO ACK` 而 `nack` 逐帧 +1、`timeout=0`
     *     （总线全好、器件不应答），正是我们上机看到的那一版。
     *   ⇒ `tries=` 打在 `se` 的判据行上："第几次才 ACK"就是**这颗芯片到底要多久**的实测值。*/
    for (tries = 0u; tries < ATECC_WAKE_TRIES; tries++) {
        wake_pulse();

        /* ④ 唤醒令牌：0x00 字节（器件应答 ACK 才算醒）*/
        rc = i2c_probe(ATECC_WAKE_TOKEN_ADDR);
        if (rc == 0) {
            s_st.wake_ok++;
            s_st.last_wake_tries = tries + 1u;
            return 0;
        }
        if (tries + 1u < ATECC_WAKE_TRIES) {
            i2c_delay_us(ATECC_WAKE_GAP_US);
        }
    }
    s_st.last_wake_tries = ATECC_WAKE_TRIES;
    s_st.wake_fail++;
    return ATECC_E_WAKE;
}

/* ------------------------------------------------------------------ */
/* 一条命令 → 一条响应（响应长度由前 4 字节 count 决定）                  */
/* ------------------------------------------------------------------ */
static int transact(const uint8_t *pkt, uint32_t pktlen,
                    uint8_t *resp, uint32_t respcap, uint32_t *resplen)
{
    uint8_t wbuf[1 + ATECC_CMD_LEN_CRC];
    uint32_t i, count;
    int rc;

    if (pkt == 0 || resp == 0 || resplen == 0 || pktlen > sizeof(wbuf) - 1u) {
        return ATECC_E_ARG;
    }
    s_st.cmd++;

    /* ② 发命令：字地址 + 包 */
    wbuf[0] = ATECC_WORD_ADDR;
    for (i = 0u; i < pktlen; i++) {
        wbuf[1u + i] = pkt[i];
    }
    rc = i2c_write(s_addr, wbuf, 1u + pktlen);
    if (rc != 0) {
        return ATECC_E_IO;
    }

    /* ③ 收响应：先 4 字节 count（这一段事务**要保持打开**）*/
    rc = i2c_read_begin(s_addr, ATECC_WORD_ADDR);
    if (rc != 0) {
        return ATECC_E_IO;
    }
    for (i = 0u; i < 4u; i++) {
        rc = i2c_read_byte(&resp[i], 1);
        if (rc != 0) {
            i2c_read_end();
            return ATECC_E_IO;
        }
    }
    count = atecc_msg_count(resp);
    if (resplen != 0) {
        *resplen = 4u;                  /* 先记着；读满再改 */
    }
    if (count < 4u || count > respcap) {
        /* 长度离谱：**把实际读到的前几字节留下来**（现场一眼看出是哪一步不对）。
         * 这里必须收尾（NACK + STOP），否则总线会一直被占着。*/
        s_st.last_resp_len = 4u;
        for (i = 0u; i < 4u && i < sizeof(s_st.last_resp); i++) {
            s_st.last_resp[i] = resp[i];
        }
        for (i = 4u; i < sizeof(s_st.last_resp); i++) {
            s_st.last_resp[i] = 0u;
        }
        i2c_read_end();
        return ATECC_E_RESP;
    }
    for (i = 4u; i < count; i++) {
        rc = i2c_read_byte(&resp[i], (i + 1u < count) ? 1 : 0);
        if (rc != 0) {
            i2c_read_end();
            return ATECC_E_IO;
        }
    }
    i2c_read_end();
    *resplen = count;
    return 0;
}

/* 取一条 32 字节随机数（`with_crc` 决定线上带不带 CRC）*/
static int random_try(uint8_t *out32, int with_crc)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    uint8_t resp[ATECC_RESP_LEN_CRC];
    uint32_t pklen, replen = 0u;
    int rc, mrc;

    pklen = (uint32_t)atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, with_crc);
    if (pklen == 0u) {
        return ATECC_E_ARG;
    }
    rc = atecc_wake();
    if (rc != 0) {
        return rc;
    }
    rc = transact(pkt, pklen, resp, sizeof(resp), &replen);
    if (rc != 0) {
        return rc;
    }
    mrc = atecc_msg_resp_random(resp, (size_t)replen, out32);
    if (mrc != 0) {
        /* 长度/count/CRC 对不上 —— 把原始字节留档（现场判据）*/
        uint32_t i;

        s_st.last_resp_len = replen;
        for (i = 0u; i < sizeof(s_st.last_resp); i++) {
            s_st.last_resp[i] = (i < replen) ? resp[i] : 0u;
        }
        return ATECC_E_RESP;
    }
    return 0;
}

int atecc_random(uint8_t out32[ATECC_RANDOM_BYTES])
{
    int rc;

    if (out32 == 0) {
        return ATECC_E_ARG;
    }
    /* 已判出模式：直接走那一种 */
    if (s_crc_mode >= 0) {
        rc = random_try(out32, s_crc_mode);
        if (rc == 0) { s_st.ok++; return 0; }
        s_st.fail++;
        s_st.last_err = (uint32_t)(-rc);
        return rc;
    }
    /* 还没判出来：先试不带 CRC，再试带 CRC（判据 = 响应长度 36 / 38）*/
    rc = random_try(out32, 0);
    if (rc == 0) {
        s_crc_mode = 0;
        s_st.crc_mode = 0u;
        s_st.ok++;
        return 0;
    }
    rc = random_try(out32, 1);
    if (rc == 0) {
        s_crc_mode = 1;
        s_st.crc_mode = 1u;
        s_st.ok++;
        return 0;
    }
    s_st.fail++;
    s_st.last_err = (uint32_t)(-rc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 初始化 / 自检                                                       */
/* ------------------------------------------------------------------ */
int atecc_init(void)
{
    uint8_t r[ATECC_RANDOM_BYTES];

    /* 幂等：main 为了在 banner 里打**实测**的 RNG 来源会先探一次，idc_init() 还会再叫一次
     *   ⇒ 不能真探两次（每次都要唤醒+一条 Random）。这里直接复用上次的结论。*/
    if (s_probed) {
        return s_present ? 0 : ATECC_E_RESP;
    }
    s_probed = 1;
    i2c_init();
    s_present = 0;
    if (atecc_random(r) == 0) {
        s_present = 1;
    }
    return s_present ? 0 : ATECC_E_RESP;
}

int atecc_present(void)
{
    return s_present;
}

void atecc_stats(atecc_stats_t *out)
{
    if (out == 0) {
        return;
    }
    out->wake_ok       = s_st.wake_ok;
    out->wake_fail     = s_st.wake_fail;
    out->last_wake_tries = s_st.last_wake_tries;
    out->cmd           = s_st.cmd;
    out->ok            = s_st.ok;
    out->fail          = s_st.fail;
    out->crc_mode      = s_st.crc_mode;
    out->last_err      = s_st.last_err;
    out->last_resp_len = s_st.last_resp_len;
    {
        size_t i;

        for (i = 0u; i < sizeof(out->last_resp); i++) {
            out->last_resp[i] = s_st.last_resp[i];
        }
    }
}

int atecc_nonce_hex(char out[ATECC_NONCE_BYTES * 2 + 1], void *ctx)
{
    uint8_t r[ATECC_RANDOM_BYTES];

    (void)ctx;
    if (out == 0) {
        return ATECC_E_ARG;
    }
    if (atecc_random(r) != 0) {
        return ATECC_E_RESP;
    }
    idn_hex_upper(r, ATECC_NONCE_BYTES, out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 自检（控制台 `se` 调的，就是给现场看的判据）                          */
/* ------------------------------------------------------------------ */
int atecc_selftest(void)
{
    uint8_t r[ATECC_RANDOM_BYTES];
    char hex[ATECC_NONCE_BYTES * 2 + 1];
    i2c_stats_t ist;
    int rc;
    uint32_t i;

    rc = atecc_wake();
    uart_printf(CONSOLE_UART, "\r\n[se] wake: %s (ok=%u fail=%u tries=%u/%u)\r\n",
                (rc == 0) ? "ACK" : "NO ACK", (unsigned)s_st.wake_ok,
                (unsigned)s_st.wake_fail, (unsigned)s_st.last_wake_tries,
                (unsigned)ATECC_WAKE_TRIES);
    if (rc != 0) {
        uart_printf(CONSOLE_UART, "[se] 没应答 ⇒ 查三件事：3V3 供电 / SDA(PB7)-SCL(PB6) 接线 "
                                  "/ 4.7k 上拉\r\n");
        /* ★ 只读诊断（2026-09-22 加，c4-γ-2 第一次上机）：把**总线层**的计数打出来，
         *   让下一次上机**一次**分清"软件还是硬件" —— 光看 `NO ACK` 是分不出的：
         *     nack > 0            ⇒ 总线走通了、地址也发出去了，只是**器件没应答**
         *                            ⇒ 才轮到查供电/接线/上拉（硬件）；
         *     nack = 0, timeout>0 ⇒ 连 START/时钟都没走完 ⇒ 查 SCL 有没有被钉住、外设使能没；
         *     两个都是 0          ⇒ 压根没走到总线（`i2c_probe()` 之前就返回了）。
         *   计数是**累计**的（含开机时 `atecc_init()` 那次探针），只回答"有没有过 NACK"。*/
        i2c_stats(&ist);
        uart_printf(CONSOLE_UART, "[se] i2c: start=%u tx=%u rx=%u nack=%u timeout=%u hz=%u addr=0x%02x\r\n",
                    (unsigned)ist.start, (unsigned)ist.tx_bytes, (unsigned)ist.rx_bytes,
                    (unsigned)ist.nack, (unsigned)ist.timeout,
                    (unsigned)(i2c_get_hz() / 1000u), (unsigned)s_addr);
        return rc;
    }
    rc = atecc_random(r);
    uart_printf(CONSOLE_UART, "[se] cmd=%u ok=%u fail=%u crc_mode=%s\r\n",
                (unsigned)s_st.cmd, (unsigned)s_st.ok, (unsigned)s_st.fail,
                (s_st.crc_mode == 0u) ? "no-crc(响应 36 B)" :
                (s_st.crc_mode == 1u) ? "crc(响应 38 B)" : "unknown");
    if (rc != 0) {
        uart_printf(CONSOLE_UART, "[se] Random FAILED rc=%d last_resp_len=%u raw=",
                    rc, (unsigned)s_st.last_resp_len);
        for (i = 0u; i < sizeof(s_st.last_resp); i++) {
            uart_printf(CONSOLE_UART, "%02x", s_st.last_resp[i]);
        }
        uart_printf(CONSOLE_UART, "\r\n");
        return rc;
    }
    idn_hex_upper(r, ATECC_NONCE_BYTES, hex);
    uart_printf(CONSOLE_UART, "[se] Random(0x1B) 32 B ok; 前 16 B = %s\r\n", hex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 工装：不信"唤醒令牌没 ACK"就等于器件不在（把握手拆开逐条看）              */
/* ------------------------------------------------------------------ */
/* 工装：扫 7 位地址 0x08~0x7F（0x00~0x07 是 I²C 保留区，不扫）。返回第一个应答的地址，0 = 无。*/
static uint8_t diag_scan(const char *tag)
{
    uint8_t a, found = 0u;
    uint32_t n = 0u;

    uart_printf(CONSOLE_UART, "[sewake] %s：扫 0x08~0x7F -> ", tag);
    for (a = 0x08u; a <= 0x7Fu; a++) {
        if (i2c_probe(a) == 0) {
            uart_printf(CONSOLE_UART, "0x%02x(ACK) ", (unsigned)a);
            if (found == 0u) { found = a; }
            n++;
        }
    }
    if (n == 0u) {
        uart_printf(CONSOLE_UART, "**无任何器件应答**\r\n");
    } else {
        uart_printf(CONSOLE_UART, "（共 %u 个）\r\n", (unsigned)n);
    }
    return found;
}

int atecc_diag_probe(void)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    uint8_t resp[ATECC_RESP_LEN_CRC];
    uint8_t r32[ATECC_RANDOM_BYTES];
    uint8_t hit;
    char hex[ATECC_NONCE_BYTES * 2 + 1];
    uint32_t i, pklen, replen = 0u;
    int rc;

    /* 为什么做这条（2026-09-23 现场：接线/供电/总线/时钟/时序全部实测排除，仍永远 NACK）：
     *   · 手册第 13 页：**608B 的 I²C 地址是可编程的** ⇒ 不一定是 0x60；
     *     公开实测里就有 **0x35**（`i2cdetect` 看得到）、而 0x35/0x6A 在 CryptoAuthLib
     *     的地址表里被归为另一个型号。**我们以前把 0x60 写死，从来没扫过其他地址**。
     *   · 另外"地址 0x00 的唤醒令牌"是**器件在 Sleep 态**才应答的东西；器件若一直在
     *     Idle，它对 0x00 会回 NACK —— 而旧固件在这种情形下**从来没试过后面的命令**。
     * ⇒ 这条工装：① 先扫一遍（器件若没睡，无需唤醒）；② 脉冲 + tWHI 后再扫一遍
     *   （器件若在 Sleep，这一步把它叫醒）；③ 对找到的地址**直接发 Random**。
     *   扫到谁就把 `s_addr` 设成谁 ⇒ 后面的 `se` 也跟着用这个地址。*/
    uart_printf(CONSOLE_UART, "\r\n[sewake] I2C=%u kHz，命令地址当前=0x%02x\r\n",
                (unsigned)(i2c_get_hz() / 1000u), (unsigned)s_addr);

    hit = diag_scan("① 未脉冲");
    if (hit == 0u) {
        wake_pulse();
        hit = diag_scan("② 脉冲+tWHI 后");
    }
    if (hit == 0u) {
        uart_printf(CONSOLE_UART, "[sewake] 全地址扫描也无人应答 ⇒ 器件侧确实没反应"
                                 "（供电/焊接/坏件 —— 但已不可能是「地址写错」）\r\n");
        return ATECC_E_WAKE;
    }
    s_addr = hit;
    uart_printf(CONSOLE_UART, "[sewake] ★ 器件在 0x%02x ⇒ 已把命令地址改成它，接着直接发 Random\r\n",
                (unsigned)hit);

    pklen = (uint32_t)atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, 0);
    rc = transact(pkt, pklen, resp, sizeof(resp), &replen);
    if (rc != 0) {
        uart_printf(CONSOLE_UART, "[sewake] Random transact 失败 rc=%d（last_resp_len=%u）\r\n",
                    rc, (unsigned)s_st.last_resp_len);
        return rc;
    }
    uart_printf(CONSOLE_UART, "[sewake] 收到 %u B：", (unsigned)replen);
    for (i = 0u; i < replen && i < 12u; i++) {
        uart_printf(CONSOLE_UART, "%02x", resp[i]);
    }
    uart_printf(CONSOLE_UART, "\r\n");
    if (atecc_msg_resp_random(resp, (size_t)replen, r32) == 0) {
        idn_hex_upper(r32, ATECC_NONCE_BYTES, hex);
        uart_printf(CONSOLE_UART, "[sewake] ★★ 器件回了合法 Random！前 16 B = %s\r\n", hex);
        return 0;
    }
    uart_printf(CONSOLE_UART, "[sewake] 响应解析不过（长度/CRC/count 对不上）——可能线上带 CRC\r\n");
    return ATECC_E_RESP;
}

/* ------------------------------------------------------------------ */
/* 工装：把 SCL/SDA 当 GPIO 开漏翻转（沿路逐点量通断）                     */
/* ------------------------------------------------------------------ */
int atecc_line_test(uint32_t seconds)
{
    uint32_t n, deadline, half = 250u;      /* 半周期 250 ms ⇒ ~2 Hz（200 ms/div 也好看）*/

    if (seconds == 0u) { seconds = 10u; }
    if (seconds > 30u) { seconds = 30u; }

    /* 交出引脚：关外设 + 两条线都配成**开漏输出**。
     * 开漏而不是推挽：高电平仍由外部 4.7 k 上拉给出 ⇒ 与真跑 I²C 时的电平同一套。*/
    i2c_disable();
    gpio_set_mode(SE_I2C_PORT, SE_SCL_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_set_mode(SE_I2C_PORT, SE_SDA_PIN, GPIO_MODE_OUT_OD_2MHZ);

    uart_printf(CONSOLE_UART, "\r\n[line] SCL(PB%u) **2 Hz** / SDA(PB%u) **1 Hz**，翻转 %u s\r\n"
                             "       两条线用**不同频率**是有意的：逐只点芯片的脚时，\r\n"
                             "       快翻转的那只 = SCL 网、慢翻转的那只 = SDA 网\r\n"
                             "       （恒定高 = VCC、恒定低 = GND、不动 = NC）—— 不需要知道脚号\r\n",
                (unsigned)SE_SCL_PIN, (unsigned)SE_SDA_PIN, (unsigned)seconds);

    for (n = 0u; n < seconds * 4u; n++) {
        uint8_t scl = (n & 1u) ? 1u : 0u;           /* 每 250 ms 翻一次 = 2 Hz */
        uint8_t sda = ((n >> 1) & 1u) ? 1u : 0u;    /* 每 500 ms 翻一次 = 1 Hz（SCL 的一半）*/

        gpio_set_pin(SE_I2C_PORT, SE_SCL_PIN, scl);
        gpio_set_pin(SE_I2C_PORT, SE_SDA_PIN, sda);
        uart_printf(CONSOLE_UART, "[line] %u/%u  SCL=%u SDA=%u\r\n",
                    (unsigned)(n + 1u), (unsigned)(seconds * 4u),
                    (unsigned)scl, (unsigned)sda);
        deadline = g_tick_ms + half;
        while ((int32_t)(g_tick_ms - deadline) < 0) { }
    }

    i2c_init();                     /* 恢复：复用开漏 + 外设（单一源在 i2c.c）*/
    uart_printf(CONSOLE_UART, "[line] done（已恢复 I2C 复用；接着敲 `se` 继续）\r\n");
    return 0;
}
