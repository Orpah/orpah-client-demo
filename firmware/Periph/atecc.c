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

#define ATECC_WORD_ADDR         0x03u   /* 命令：写到字地址 0x03 */
#define ATECC_WORD_ADDR_RESP    0x00u   /* ★ 响应：**从字地址 0x00 读**（见下）*/
#define ATECC_WORD_ADDR_RESP_ALT 0x03u  /* 兜底：旧写法（读 0x03）；两种都试，结果打出来 */
#define ATECC_WAKE_TOKEN_ADDR   0x00u

/* ★★ 字地址到底在哪一个（2026-09-23 复核 CryptoAuthLib 源码发现的 bug）
 *
 * 我们原先**读写都用 0x03**，而公开实现里是一张四元表：
 *   `0x00` = 响应（读）、`0x01` = sleep、`0x02` = idle、`0x03` = 命令（写）；
 *   同一个表在 1 线侧写得最清楚：`hal_swi_gpio.h` 的
 *     `ATCA_1WIRE_RESET_WORD_ADDR 0x00` / `_SLEEP_ 0x01` / `_SLEEP_ALTERNATE 0x02` /
 *     `_COMMAND_ 0x03`。
 *   而“读响应前先发 0x00”在 `lib/calib/calib_execution.c` 里能看到：非 SWI 设备时
 *     `word_address = 0;` → `atsend(iface, word_address, NULL, 0)`（“Send Word address
 *     to device...”，只发那一个字节）→ `atreceive(...)`。
 *   侧证：`hal_i2c_sleep()` 写 `0x01`、`hal_i2c_idle()` 写 `0x02`（多处 HAL，含
 *     `hal_uc3_i2c_asf.c` / `hal_sam_i2c_asf.c` / `hal_sam0_i2c_asf.c`）。
 *
 * ⇒ 读错字地址的表现**恰好**是我们在台架上看到的：
 *   **地址被 ACK、命令字节写进去了（`tx` 在涨）、但响应一个字节也读不回（`rx=0`）
 *     而 `nack` 在涨** —— 器件不认“从 0x03 读”。
 *   所以 `transact()` 现在**两种都试**（先 0x00、不行再 0x03），并把**哪个通了**记进
 *   `s_st.resp_waddr`（`se` 会打出来）—— 真机上一眼就能确认到底哪个对，不再靠猜。*/
#define ATECC_WORD_ADDR_TRY_N   2u
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

/* 只发“字地址 0x03 + 命令包”（不含接收）*/
static int cmd_write(const uint8_t *pkt, uint32_t pktlen)
{
    uint8_t wbuf[1 + ATECC_CMD_LEN_CRC];
    uint32_t i;

    wbuf[0] = ATECC_WORD_ADDR;
    for (i = 0u; i < pktlen; i++) {
        wbuf[1u + i] = pkt[i];
    }
    return (i2c_write(s_addr, wbuf, 1u + pktlen) == 0) ? 0 : ATECC_E_IO;
}

/* 从指定字地址读一条响应（先 4 字节 count，再按 count 读满）*/
static int resp_read(uint8_t waddr, uint8_t *resp, uint32_t respcap, uint32_t *resplen)
{
    uint32_t i, count;
    int rc;

    rc = i2c_read_begin(s_addr, waddr);
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

/* 发一条命令再读回响应。★ **响应字地址两种都试**（0x00 优先，见上面的长注释）：
 * 第一种读不通时**把命令重发一次**再用 0x03 读 —— 因为读失败的那次可能已经把命令取走了。*/
static int transact(const uint8_t *pkt, uint32_t pktlen,
                    uint8_t *resp, uint32_t respcap, uint32_t *resplen)
{
    static const uint8_t waddr[ATECC_WORD_ADDR_TRY_N] = {
        ATECC_WORD_ADDR_RESP, ATECC_WORD_ADDR_RESP_ALT
    };
    uint32_t k;
    int rc = ATECC_E_IO;

    if (pkt == 0 || resp == 0 || resplen == 0 || pktlen > ATECC_CMD_LEN_CRC) {
        return ATECC_E_ARG;
    }
    s_st.cmd++;

    for (k = 0u; k < ATECC_WORD_ADDR_TRY_N; k++) {
        rc = cmd_write(pkt, pktlen);
        if (rc != 0) {
            return rc;
        }
        rc = resp_read(waddr[k], resp, respcap, resplen);
        if (rc == 0) {
            s_st.resp_waddr = k + 1u;                 /* 1 = 读 0x00 通了；2 = 读 0x03 通了 */
            return 0;
        }
    }
    return rc;
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
    /* ★ 令牌没 ACK **不等于**器件不在：器件若已经在 Idle（前一次唤醒已把它叫醒、
     *   或者它被配置成不自睡），它对地址 0x00 会回 **NACK**。
     *   ⇒ 这里**不早退**，照样把命令发出去，让**命令的结果**说话（`se` 自己会打 wake 那一行）。*/
    (void)atecc_wake();
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
    out->resp_waddr    = s_st.resp_waddr;
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
        uart_printf(CONSOLE_UART, "[se] 令牌没 ACK **不等于器件不在**（器件若已在 Idle，"
                                  "它对 0x00 本来就会回 NACK）⇒ 照样发一条命令，看结果说话\r\n");
        /* ★ 只读诊断（2026-09-22 加，c4-γ-2 第一次上机）：把**总线层**的计数打出来，
         *   让下一次上机**一次**分清"软件还是硬件" —— 光看 `NO ACK` 是分不出的：
         *     nack > 0            ⇒ 总线走通了、地址也发出去了，只是**器件没应答**
         *                            ⇒ 才轮到查供电/接线/上拉（硬件）；
         *     nack = 0, timeout>0 ⇒ 连 START/时钟都没走完 ⇒ 查 SCL 有没有被钉住、外设使能没；
         *     两个都是 0          ⇒ 压根没走到总线（`i2c_probe()` 之前就返回了）。
         *   计数是**累计**的（含开机时 `atecc_init()` 那次探针），只回答"有没有过 NACK"。*/
        i2c_stats(&ist);
        uart_printf(CONSOLE_UART, "[se] i2c: start=%u tx=%u rx=%u nack=%u timeout=%u step=%u"
                                  " nackbyte=%u hz=%u addr=0x%02x\r\n",
                    (unsigned)ist.start, (unsigned)ist.tx_bytes, (unsigned)ist.rx_bytes,
                    (unsigned)ist.nack, (unsigned)ist.timeout, (unsigned)ist.last_step,
                    (unsigned)ist.last_nack_byte,
                    (unsigned)(i2c_get_hz() / 1000u), (unsigned)s_addr);
    }
    /* ★ 不再早退（2026-09-23）："令牌没 ACK"**不影响**要不要发命令 —— 走 `atecc_random()`
     *   的宽容路径（`random_try()` 不会因为令牌没 ACK 就退出）。
     *   这也是 `seaddr <任意地址>` + `se` 能当"**在别的地址上试命令**"用的前提：
     *   以前令牌一 NACK 就 `return`，命令阶段根本没走到，于是"0x60 上到底行不行"一直没答案。*/
    uart_printf(CONSOLE_UART, "[se] 命令发往 addr=0x%02x（重烧/复位后回到缺省 0x60——"
                              "先 `seaddr <hex>` 或用 `sewake` 扫一下）\r\n",
                (unsigned)s_addr);
    rc = atecc_random(r);
    uart_printf(CONSOLE_UART, "[se] cmd=%u ok=%u fail=%u crc_mode=%s resp_waddr=%s\r\n",
                (unsigned)s_st.cmd, (unsigned)s_st.ok, (unsigned)s_st.fail,
                (s_st.crc_mode == 0u) ? "no-crc(响应 36 B)" :
                (s_st.crc_mode == 1u) ? "crc(响应 38 B)" : "unknown",
                (s_st.resp_waddr == 1u) ? "0x00" :
                (s_st.resp_waddr == 2u) ? "0x03" : "unknown");
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
/* ★ 一次 ACK 不算命中（2026-09-23 实测教训）：现场出现过**仅一次**应答 0x64、
 *   随后 Info/Read/Random 全 NACK；而 `i2c_probe()` 只探一次 ⇒ 一次毛刺就冒充器件。
 *   现在每个地址探 `DIAG_PROBE_TRIES` 次、ACK 次数 ≥ `DIAG_PROBE_MIN` 才算命中；
 *   只 ACK 一两次的**也打出来**（当线索看），但不当器件。*/
#define DIAG_PROBE_TRIES  3u
#define DIAG_PROBE_MIN    2u

/* `sescan`（重复扫描统计）的上限：轮数上限 60（计数存 uint8_t），命中表只留 8 项
 * （现场基本是 0 个或 1 个地址；本芯片 RAM 只剩 ~4 KB 给栈，别开大数组）。*/
#define ATECC_SCAN_HIT_MAX    8u
#define ATECC_SCAN_MAX_ROUNDS 60u

/* 工装：把两条线放开（开漏输出写 1 = 只剩 4.7 k 上拉在拉）再读回真实电平。
 * 两个都该是 1；若有一个是 0 ⇒ 那根线被**某处按住**（器件拉低/虚焊搭到 GND/坏件），
 * 而 SDA 被按住时，第九个时钟采样到低 = 正是"偶然 ACK"的温床。*/
static void diag_levels(void)
{
    uint8_t scl, sda;

    i2c_disable();
    gpio_set_mode(SE_I2C_PORT, SE_SCL_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_set_mode(SE_I2C_PORT, SE_SDA_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_set_pin(SE_I2C_PORT, SE_SCL_PIN, 1u);
    gpio_set_pin(SE_I2C_PORT, SE_SDA_PIN, 1u);
    i2c_delay_us(50u);                   /* 给上拉一点时间（4.7 k × 线容 ≈ µs 级）*/
    scl = gpio_get_pin(SE_I2C_PORT, SE_SCL_PIN);
    sda = gpio_get_pin(SE_I2C_PORT, SE_SDA_PIN);
    i2c_init();                          /* 恢复复用开漏（单一源在 i2c.c）*/
    uart_printf(CONSOLE_UART, "[sewake] 空闲电平：SCL=%u SDA=%u（**都该是 1**；"
                             "有一个是 0 ⇒ 那根线被按住，后面的 ACK 就不一定可信）\r\n",
                (unsigned)scl, (unsigned)sda);
}

/* 对**单个地址**连探 `DIAG_PROBE_TRIES` 次，返回 ACK 次数（0..3）。
 * `read_dir` != 0 时发读方向（用来分辨"真器件"与"伪 ACK"：真从机两个方向都该 ACK）。
 * `diag_scan()` / `atecc_diag_scan_stats()` 共用它 ⇒ "一次探测回合"只有一份定义。*/
static uint32_t diag_probe_dir_n(uint8_t a, int read_dir)
{
    uint32_t hit = 0u, k;

    for (k = 0u; k < DIAG_PROBE_TRIES; k++) {
        if (i2c_probe_dir(a, read_dir) == 0) { hit++; }
    }
    return hit;
}

static uint32_t diag_probe_n(uint8_t a)
{
    return diag_probe_dir_n(a, 0);
}

/* 工装：扫 7 位地址 0x01~0x7F（**跳过 0x00** —— 那是唤醒令牌的地址，探它会把两种含义搞混）。
 * 返回第一个**确认过的**地址，0 = 无。*/
static uint8_t diag_scan(const char *tag)
{
    uint8_t a, found = 0u;
    uint32_t n = 0u;

    uart_printf(CONSOLE_UART, "[sewake] %s：扫 0x01~0x7F（每址 %u 探，≥%u 次 ACK 才算）-> ",
                tag, (unsigned)DIAG_PROBE_TRIES, (unsigned)DIAG_PROBE_MIN);
    for (a = 0x01u; a <= 0x7Fu; a++) {
        uint32_t hit = diag_probe_n(a);

        if (hit >= DIAG_PROBE_MIN) {
            /* ★ 命中时**顺便探一次读方向**：真从机两个方向都该 ACK。
             *   “W 方向 ACK 但 R 方向 NACK” ⇒ 这个 ACK 可疑（不是正常从机），
             *   这是把“器件真在”与“我们的 ACK 判定有毛病”分开的一条判据。*/
            uint32_t rhit = diag_probe_dir_n(a, 1);

            uart_printf(CONSOLE_UART, "0x%02x(ACK %u/%u, R %u/%u) ",
                        (unsigned)a, (unsigned)hit, (unsigned)DIAG_PROBE_TRIES,
                        (unsigned)rhit, (unsigned)DIAG_PROBE_TRIES);
            if (found == 0u) { found = a; }
            n++;
        } else if (hit != 0u) {
            uart_printf(CONSOLE_UART, "[0x%02x 仅 %u/%u ⇒ 偶然，不当器件] ",
                        (unsigned)a, (unsigned)hit, (unsigned)DIAG_PROBE_TRIES);
        }
    }
    if (n == 0u) {
        uart_printf(CONSOLE_UART, "**无任何器件应答**\r\n");
    } else {
        uart_printf(CONSOLE_UART, "（共 %u 个）\r\n", (unsigned)n);
    }
    return found;
}

/* 工装：发一条 5 字节形状的命令（Info / Read），**不带 CRC 试一次、再带 CRC 试一次**，
 * 把响应的 4 字节数据写进 `out4`。返回 0 = OK。*/
static int diag_cmd4(uint8_t op, uint8_t p1, uint16_t p2, uint8_t out4[4], int *used_crc)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    uint8_t resp[ATECC_RESP4_LEN_CRC];
    uint32_t got = 0u;
    size_t n;
    int i;

    for (i = 0; i < 2; i++) {
        if (op == ATECC_OP_INFO) {
            n = atecc_msg_info(pkt, sizeof(pkt), p1, i);
        } else {
            n = atecc_msg_read(pkt, sizeof(pkt), p1, p2, i);
        }
        if (n == 0u) {
            return ATECC_E_ARG;
        }
        if (transact(pkt, (uint32_t)n, resp, sizeof(resp), &got) != 0) {
            continue;
        }
        if (atecc_msg_resp_get4(resp, (size_t)got, out4) == 0) {
            if (used_crc != 0) { *used_crc = i; }
            return 0;
        }
    }
    return ATECC_E_RESP;
}

int atecc_diag_probe(void)
{
    uint8_t rev[4];
    uint8_t cfg[4];
    uint8_t r32[ATECC_RANDOM_BYTES];
    uint8_t hit;
    char hex[ATECC_NONCE_BYTES * 2 + 1];
    int rc, crc_used = 0, rev_ok = 0, cfg_ok = 0;

    /* 为什么做这条（2026-09-23 现场：接线/供电/总线/时钟/时序全部实测排除，仍永远 NACK）：
     *   · 手册第 13 页：**608B 的 I²C 地址是可编程的** ⇒ 不一定是 0x60；
     *     公开实测里就有 **0x35**（`i2cdetect` 看得到）、而 0x35/0x6A 在 CryptoAuthLib
     *     的地址表里被归为另一个型号。**我们以前把 0x60 写死，从来没扫过其他地址**。
     *   · 另外"地址 0x00 的唤醒令牌"是**器件在 Sleep 态**才应答的东西；器件若一直在
     *     Idle，它对 0x00 会回 NACK —— 而旧固件在这种情形下**从来没试过后面的命令**。
     * ⇒ 这条工装：① 先扫一遍（器件若没睡，无需唤醒）；② 脉冲 + tWHI 后再扫一遍
     *   （器件若在 Sleep，这一步把它叫醒）；③ 命中时读完版本（`Info`）再读配置区锁位
     *   （`Read` word 0x15 ⇒ 0x55 = 未锁）；④ 最后发 `Random`（c4-γ-2 真正要的）。
     *   **扫到谁就把 `s_addr` 设成谁** ⇒ 后面的 `se` 也跟着用这个地址。
     *   ⚠ 一个都扫不到时**不早退**：照样在旧地址上写一条命令、**照样打结论行** ——
     *   一颗芯片一行判据，明早逐颗试时不用再去分辨「没打结论」是哪种情形。*/
    uart_printf(CONSOLE_UART, "\r\n[sewake] I2C=%u kHz，命令地址当前=0x%02x\r\n",
                (unsigned)(i2c_get_hz() / 1000u), (unsigned)s_addr);
    diag_levels();                       /* 先看两线空闲电平（SDA 被按住 = 偶然 ACK 的温床）*/

    hit = diag_scan("① 未脉冲");
    if (hit == 0u) {
        wake_pulse();
        hit = diag_scan("② 脉冲+tWHI 后");
    }
    if (hit != 0u) {
        s_addr = hit;
        uart_printf(CONSOLE_UART, "[sewake] ★ 器件在 0x%02x ⇒ 已把命令地址改成它，接着逐项验：\r\n",
                    (unsigned)hit);
    } else {
        uart_printf(CONSOLE_UART, "[sewake] 两遍扫描都无人应答 ⇒ 器件侧确实没反应"
                                  "（供电/焊接/坏件 —— 但已不可能是「地址写错」）\r\n");
        uart_printf(CONSOLE_UART, "[sewake] 仍在 0x%02x 上直接写一条命令（不等 ACK）"
                                  "—— 万一扫描本身有毛病：\r\n", (unsigned)s_addr);
    }

    if (hit != 0u) {
        /* ③ Info：读器件版本（能读出来 = 器件真的在答）。*/
        rev_ok = (diag_cmd4(ATECC_OP_INFO, 0x00u, 0x0000u, rev, &crc_used) == 0);
        if (rev_ok) {
            uart_printf(CONSOLE_UART, "[sewake] ③ Info(0x30)  -> ok(%s)：%02x %02x %02x %02x"
                                      "   ← 器件版本\r\n",
                        crc_used ? "带CRC" : "无CRC",
                        rev[0], rev[1], rev[2], rev[3]);
        } else {
            uart_printf(CONSOLE_UART, "[sewake] ③ Info(0x30)  -> **失败**（器件不应答 Info）\r\n");
        }

        /* ④ Read 配置区 word 0x15（字节地址 0x54）⇒ LockValue：`0x55` = 未锁。*/
        cfg_ok = (diag_cmd4(ATECC_OP_READ, ATECC_ZONE_CONFIG, ATECC_CFG_ADDR_LOCK, cfg,
                            &crc_used) == 0);
        if (cfg_ok) {
            uart_printf(CONSOLE_UART, "[sewake] ④ Read cfg@0x54 -> ok(%s)：%02x %02x %02x %02x"
                                      "   ⇒ %s\r\n",
                        crc_used ? "带CRC" : "无CRC", cfg[0], cfg[1], cfg[2], cfg[3],
                        (cfg[0] == 0x55u) ? "**配置区 unlocked（0x55）**"
                                          : "**已锁（≠ 0x55）**");
        } else {
            uart_printf(CONSOLE_UART, "[sewake] ④ Read cfg@0x54 -> **失败**（读不到配置区）\r\n");
        }
    }

    /* ⑤ Random：这才是 c4-γ-2 真正要的那条。**总是试**（走 `random_try` 的宽容唤醒：
     *   令牌没 ACK 不早退）—— 这样两种情形下都有一行结论，明早一颗芯片一眼一句。*/
    rc = random_try(r32, 0);
    if (rc != 0) {
        rc = random_try(r32, 1);
    }
    if (rc == 0) {
        idn_hex_upper(r32, ATECC_NONCE_BYTES, hex);
        uart_printf(CONSOLE_UART, "[sewake] ⑤ Random(0x1B) -> ok：前 16 B = %s\r\n", hex);
    } else {
        uart_printf(CONSOLE_UART, "[sewake] ⑤ Random(0x1B) -> **失败** rc=%d\r\n", rc);
    }

    uart_printf(CONSOLE_UART, "[sewake] ===== 结论：addr=0x%02x rev=%s cfg=%s random=%s =====\r\n",
                (unsigned)((hit != 0u) ? hit : 0u),
                rev_ok ? "ok" : "无",
                cfg_ok ? ((cfg[0] == 0x55u) ? "unlocked" : "locked") : "读不到",
                (rc == 0) ? "OK" : "失败");
    return (rc == 0) ? 0 : ATECC_E_RESP;
}

/* ★ 工装（2026-09-23，现场需要）：**重复扫描统计** —— 一次 ACK 说明不了问题，
 * 但"答中几轮 / 一共跑几轮"一下就把三种情形分开：
 *   真实器件 = **每轮都答**；虚焊/接触不良 = **时有时无**（0 < k < 轮数，地址还可能变）；
 *   纯毛刺 = 偶发、地址每次不一样。
 * 为什么让固件做而不是拿示波器：那一下太短、又不常出现，**抓不到**；
 * 重复采样是固件最擅长的事（而且顺带把唤醒令牌的 ACK 率也统计了）。*/
int atecc_diag_scan_stats(uint32_t rounds)
{
    /* 命中表只存"答过"的地址（现场基本是 0 个或 1 个）⇒ 比 128 项数组省 RAM
     * （本芯片 RAM 只剩 ~4 KB 给栈，别乱开大数组）。*/
    struct { uint8_t addr; uint8_t cnt; } hit[ATECC_SCAN_HIT_MAX];
    uint32_t r, a, any = 0u, wake_ok = 0u, best_n = 0u;
    uint8_t best = 0u, i;

    if (rounds == 0u) { rounds = 10u; }
    if (rounds > ATECC_SCAN_MAX_ROUNDS) { rounds = ATECC_SCAN_MAX_ROUNDS; }
    for (i = 0u; i < ATECC_SCAN_HIT_MAX; i++) { hit[i].addr = 0u; hit[i].cnt = 0u; }

    uart_printf(CONSOLE_UART, "\r\n[sescan] %u 轮；每轮 = 完整唤醒序列 + 扫 0x01~0x7F"
                             "（每址 %u 探，≥%u 次算这一轮答了）\r\n",
                (unsigned)rounds, (unsigned)DIAG_PROBE_TRIES, (unsigned)DIAG_PROBE_MIN);
    diag_levels();

    for (r = 1u; r <= rounds; r++) {
        uint32_t nthis = 0u;
        int wrc = atecc_wake();

        if (wrc == 0) { wake_ok++; }
        for (a = 0x01u; a <= 0x7Fu; a++) {
            if (diag_probe_n((uint8_t)a) < DIAG_PROBE_MIN) { continue; }
            nthis++;
            for (i = 0u; i < ATECC_SCAN_HIT_MAX; i++) {
                if (hit[i].addr == (uint8_t)a) { hit[i].cnt++; break; }
            }
            if (i == ATECC_SCAN_HIT_MAX) {                 /* 表里没有 ⇒ 新地址 */
                for (i = 0u; i < ATECC_SCAN_HIT_MAX; i++) {
                    if (hit[i].cnt == 0u) {
                        hit[i].addr = (uint8_t)a;
                        hit[i].cnt = 1u;
                        break;
                    }
                }
                if (i == ATECC_SCAN_HIT_MAX) { nthis--; }  /* 表满：丢掉，但仍计入本轮提示 */
            }
        }
        uart_printf(CONSOLE_UART, "[sescan] 轮 %u/%u：唤醒令牌 %s（tries=%u），本轮命中 %u 个地址\r\n",
                    (unsigned)r, (unsigned)rounds,
                    (wrc == 0) ? "ACK" : "无 ACK",
                    (unsigned)s_st.last_wake_tries, (unsigned)nthis);
    }

    uart_printf(CONSOLE_UART, "[sescan] 唤醒令牌：%u/%u 轮 ACK\r\n",
                (unsigned)wake_ok, (unsigned)rounds);
    for (i = 0u; i < ATECC_SCAN_HIT_MAX; i++) {
        if (hit[i].cnt == 0u) { continue; }
        uart_printf(CONSOLE_UART, "[sescan]   0x%02x：%u/%u 轮\r\n",
                    (unsigned)hit[i].addr, (unsigned)hit[i].cnt, (unsigned)rounds);
        any++;
        if ((uint32_t)hit[i].cnt > best_n) { best_n = hit[i].cnt; best = hit[i].addr; }
    }
    if (any == 0u) {
        uart_printf(CONSOLE_UART, "[sescan] ===== 结论：连着 %u 轮一个地址都没答过"
                                 "（不是\"偶然没答\"，是**真没反应**）=====\r\n", (unsigned)rounds);
        return ATECC_E_RESP;
    }
    if (best_n == rounds) {
        uart_printf(CONSOLE_UART, "[sescan] ===== 结论：**稳定器件 0x%02x**（%u/%u 轮全答）"
                                 " ⇒ 接着敲 `sewake` 验 Info/锁/Random =====\r\n",
                    (unsigned)best, (unsigned)best_n, (unsigned)rounds);
        return 0;
    }
    uart_printf(CONSOLE_UART, "[sescan] ===== 结论：**不稳定**（最像器件的 0x%02x 只答 %u/%u 轮）"
                             " ⇒ 虚焊/接触不良的典型签名：先重烫那 8 只脚（SDA/SCL/GND 重点）"
                             "再跑一次 =====\r\n",
                (unsigned)best, (unsigned)best_n, (unsigned)rounds);
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
