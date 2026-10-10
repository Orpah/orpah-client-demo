/* atecc.c — ATECC608B 的 I²C 事务与唤醒。见 atecc.h 的时序出处与"如实"说明。
 *
 * 一次 `Random` 的完整动作（照 ATECC 的 I²C 规矩）：
 *   ① 唤醒：关 I2C 外设 → SDA(PB7) 拉低 ≥60 µs → 放开 → 等 ≥1500 µs → 开外设
 *            → 写"唤醒令牌" = 地址 0x00，收到 ACK 才算醒（这是权威做法：CryptoAuthLib
 *              `hal_check_wake()` 就是发 0x00 看 ACK，不是靠猜时间）
 *   ② 发命令：START + 0xC0 + **字地址 0x03** + 包（5 或 7 字节） + STOP
 *   ③ 收响应：START + 0xC0 + 字地址 + **RESTART** + 0xC1 → 先读 **1 字节** count
 *            → 按 count 接着读 count-1 字节（**同一段事务里读完**，中间不能停）
 *   ④ 解析：`proto/atecc_msg.c`（长度/count/CRC 三道检查）
 *
 * ★ CRC 模式自动判定（为什么必须自动）：线上要不要挂 CRC 取决于芯片 Config 区
 *   `ChipMode` 的 I2C-CRC 位，而**我们手上只有 NDA 摘要手册、没有 Config 默认值**
 *   ⇒ 不猜：先按"不挂 CRC"发一条，若响应 count 不是 33，再按"挂 CRC"发一条；
 *   哪一种通就**记住**（`s_st.crc_mode`），后面都用它。判定的结果打在 `se` 命令上。
 */
#include "atecc.h"

#include "atecc_msg.h"
#include "board.h"
#include "crc16.h"
#include "gpio.h"
#include "i2c.h"
#include "id_nonce.h"          /* idn_hex_upper()（别在业务代码里另写一份 hex）*/
#include "sha256.h"            /* ★ d 步：签名器要先把预像哈希掉（哈希在 MCU、私钥在 SE ✓）*/
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

/* ★★ 2026-10-10：唤醒的**权威口径**（照 CryptoAuthLib `hal_i2c_start.c::hal_i2c_wake()`
 *   + `lib/hal/atca_hal.c::hal_check_wake()`）：
 *     ① 脉冲 = **对从机地址 0x00 写 0 字节** —— 库里那句注释就是"part will NACK,
 *        so don't check for status" ⇒ **它本来就不 ACK** ✗；
 *        （我们原来拿"令牌没 ACK"当"没醒"，判据本身就是错的 ✗）
 *     ② 等 tWHI（自检使能时是 tWHIST ≥ 20 ms，见下）；
 *     ③ **从字地址 0x00 读 4 字节**，应当正好是 `04 11 33 43`（被唤醒 ✓）
 *        或 `04 07 C4 40`（**自检失败** ✗）—— `hal_check_wake()` 就是核对这两个常量 ✓。
 *   ⚠⚠ 第 ③ 步**必须读掉**：那 4 字节是器件放进输出缓冲的**回复**，库里是**读掉**它的 ✓；
 *      我们原来只"探地址"**不读** ✗ ⇒ 它就留在缓冲里，之后读命令响应时读到的很可能
 *      **正是这串旧的唤醒应答** ✓（2026-10-10 实测：`04 11 33 43` 反复被读回来 ✗）。
 *   返回 0 = 拿到并读掉了正确的唤醒应答；-4 = 自检失败；其它 <0 = 没拿到（见下）。*/
static int wake_reply_read(uint8_t out4[4]);   /* 定义在 resp_read() 之后 */

int atecc_wake(void)
{
    uint32_t tries, i;
    int rc;
    uint8_t rep[4] = { 0u, 0u, 0u, 0u };

    /* ① 上电延时（若刚上电；重复调用只是多等 200 µs，无害）*/
    i2c_delay_us(ATECC_PWRUP_US);

    /* ★ 整段序列最多重做 `ATECC_WAKE_TRIES` 次（手册表 2-2 明确允许"轮询"；库里也是重试整段 ✓）。
     *   为什么**每轮都重做脉冲**：脉冲没被器件认到时，只重发令牌没用。
     *   ★ 为什么必须轮询（2026-09-22 逐字核摘要手册表 2-2）：同一张表给了两个情形 ——
     *     未使能自检 `tWHI ≥ 1500 µs`；**使能自检 `tWHIST ≥ 20 ms`**；两行都写着
     *     "SDA should be stable high for this entire duration **unless polling is implemented**" ✓
     *   ⇒ 只等 1.7 ms 且只发一次令牌时，**自检使能的正常芯片**也不应答应 ✓
     *   ⇒ `轮=` 打在 `se` 的判据行上："第几轮才拿到正确唤醒应答" = 这颗芯片到底要多久 ✓。*/
    for (tries = 0u; tries < ATECC_WAKE_TRIES; tries++) {
        wake_pulse();

        /* ③ 读「唤醒应答」并**读掉**（这才是权威判据 ✓）*/
        rc = wake_reply_read(rep);
        if (rc == 0) {
            s_st.wake_ok++;
            s_st.last_wake_tries = tries + 1u;
            return 0;
        }
        if (tries + 1u < ATECC_WAKE_TRIES) {
            i2c_delay_us(ATECC_WAKE_GAP_US);
        }
    }
    /* 失败时把**最后一轮读到的字节**留在 stats 里（现场判据：唤醒应答长什么样 ✓）*/
    s_st.last_resp_len = 4u;
    for (i = 0u; i < sizeof(s_st.last_resp); i++) {
        s_st.last_resp[i] = (i < 4u) ? rep[i] : 0u;
    }
    s_st.last_wake_tries = ATECC_WAKE_TRIES;
    s_st.wake_fail++;
    return ATECC_E_WAKE;
}

/* ------------------------------------------------------------------ */
/* 一条命令 → 一条响应（响应长度由**第 1 字节** count 决定）                      */
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

/* 从指定字地址读一条响应（先 1 字节 count，再按 count 读满）*/
static int resp_read(uint8_t waddr, uint8_t *resp, uint32_t respcap, uint32_t *resplen)
{
    uint32_t i, count;
    int rc;

    rc = i2c_read_begin(s_addr, waddr);
    if (rc != 0) {
        return ATECC_E_IO;
    }
    /* ★ 2026-10-10：**先只读 1 字节** —— I²C 上 count 就是 1 字节（出处见 `atecc_msg.h`）。
     *   原来一次读 4 字节再按"大端 count"算 ✗：器件回的 4 字节错误包 `04 FF 01 42`
     *   会被算成 `0x04FF0142` ⇒ 报"长度离谱"，真话（状态 0xFF）反而被吃掉。*/
    rc = i2c_read_byte(&resp[0], 1);
    if (rc != 0) {
        i2c_read_end();
        return ATECC_E_IO;
    }
    count = atecc_msg_count(resp);
    if (resplen != 0) {
        *resplen = 1u;                  /* 先记着；读满再改 */
    }
    if (count < ATECC_RSP_SIZE_MIN || count > respcap) {
        /* 长度离谱：**把实际读到的前几字节留下来**（现场一眼看出是哪一步不对）。
         * 这里必须收尾（NACK + STOP），否则总线会一直被占着。*/
        s_st.last_resp_len = 1u;
        s_st.last_resp[0] = resp[0];
        for (i = 1u; i < sizeof(s_st.last_resp); i++) {
            s_st.last_resp[i] = 0u;
        }
        i2c_read_end();
        return ATECC_E_RESP;
    }
    for (i = 1u; i < count; i++) {
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

/* ★ 2026-10-10：读「唤醒应答」并核对 —— 常量照 CryptoAuthLib `hal_check_wake()` ✓。
 *   返回 0 = `04 11 33 43`（被唤醒 ✓）；-4 = `04 07 C4 40`（**自检失败** ✗）；
 *   其它 <0 = 没读到 / 读到别的东西（字节留在 `out4` 供上层打印 ✓）。*/
static int wake_reply_read(uint8_t out4[4])
{
    static const uint8_t wake_ok[4]       = { 0x04u, 0x11u, 0x33u, 0x43u };
    static const uint8_t wake_selftest[4] = { 0x04u, 0x07u, 0xC4u, 0x40u };
    uint32_t got = 0u, i;

    if (resp_read(ATECC_WORD_ADDR_RESP, out4, 4u, &got) != 0) {
        return ATECC_E_IO;
    }
    if (got == 4u) {
        for (i = 0u; i < 4u; i++) {
            if (out4[i] != wake_ok[i]) {
                break;
            }
        }
        if (i == 4u) {
            return 0;                       /* 04 11 33 43 ✓ */
        }
        for (i = 0u; i < 4u; i++) {
            if (out4[i] != wake_selftest[i]) {
                break;
            }
        }
        if (i == 4u) {
            return ATECC_E_SELFTEST;        /* 04 07 C4 40 ✗（自检失败）*/
        }
    }
    return ATECC_E_RESP;
}
/* ★★ 2026-10-10 改：写成"轮询到器件答地址再读"，并把上限放到 **60 ms** ——
 *   因为执行时间**不是**我们原来以为的几毫秒：CryptoAuthLib 的表里 `Random` 是 **23 ms**
 *   （`lib/calib/calib_execution.c` 的 `device_execution_time_108[]`：`{ ATCA_RANDOM, 23}`；
 *     ATECC608 那张表同量级）✗ —— 原来只等 8 ms ⇒ **每次都在它还在算的时候去读** ✗。
 *   2026-10-10 实测（不烧录、走 `i2cwr`/`i2crd`）：下命令后 **10 ms 读地址被 NACK**
 *   （= 器件正忙、根本不答地址 ✓）；**210 ms 时它已经睡了** ⇒ 只有"**轮询到 ACK 立刻读**"
 *   这一条路走得通 ✓——而这正是 CryptoAuthLib 的做法（响应靠**轮询**，不是干等一个数 ✓）。
 *   返回 1 = 它答了地址（响应已备好）；0 = 60 ms 内一直不答 ⇒ **本轮不读**（交给上层重试）。*/
/* ★ 2026-10-11 改（与 PC 侧同一教训）：原来 60 ms（"2 × 23 ms"）**只够 `Random`** ✓；
 *   而 `GenKey` / `ECDH` / `Write` 要 ~40–50 ms ⇒ **PC 侧实测 `GenKey` 约 50 ms**
 *   （重试 11 次 × 5 ms 才读到 ✓），60 ms 太贴边 ✗ ⇒ 放到 250 ms（≈5× 余量）✓。
 *   代价很小：器件"**算完之前地址不 ACK**"✓ ⇒ 这 250 ms 只在**慢命令**上真正等到 ✓。
 *   出处：原厂 `lib/calib/calib_execution.c` 的执行时间表（Random 23 ms / ECDH·GenKey ~40 ms …）✓。*/
#define ATECC_READY_POLL_MS   250u
#define ATECC_READY_GAP_US    300u     /* 两次探地址之间（探一次只要 µs 量级 ✓）*/

static int wait_ready(uint32_t ms)
{
    uint32_t deadline = g_tick_ms + ms;

    do {
        if (i2c_probe(s_addr) == 0) {   /* 0 = 有器件应答 ⇒ 它备好了 */
            return 1;
        }
        i2c_delay_us(ATECC_READY_GAP_US);
    } while ((int32_t)(g_tick_ms - deadline) < 0);
    return 0;
}

/* ★ 2026-10-10：读响应**之前**看两条线有没有被器件放开。
 *   为什么：带 CRC 的那几次都以 **BERR（总线错位）**收场 ✗，而"器件还在忙/还按着线"
 *   正是 BERR 的典型成因；而 `wait_ready()` 的"探地址 ACK"会被**假 ACK 骗过** ✗
 *   （2026-10-10 那个假 `0x08` 就骗过了探针 ✓）。
 *   做法同 `diag_levels()`：两脚放开成**开漏写 1** 再读回（**不是内部上拉** ✓），读完恢复外设。*/
static int lines_free(uint8_t *scl, uint8_t *sda)
{
    i2c_disable();
    gpio_set_mode(SE_I2C_PORT, SE_SCL_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_set_mode(SE_I2C_PORT, SE_SDA_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_set_pin(SE_I2C_PORT, SE_SCL_PIN, 1u);
    gpio_set_pin(SE_I2C_PORT, SE_SDA_PIN, 1u);
    i2c_delay_us(50u);                  /* 给上拉一点时间（4.7 k × 线容 ≈ µs 级）*/
    *scl = gpio_get_pin(SE_I2C_PORT, SE_SCL_PIN);
    *sda = gpio_get_pin(SE_I2C_PORT, SE_SDA_PIN);
    i2c_init();                         /* 恢复复用开漏（单一源在 i2c.c）*/
    return (*scl != 0u && *sda != 0u) ? 1 : 0;
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
        uint8_t lv_scl = 0u, lv_sda = 0u;

        rc = cmd_write(pkt, pktlen);
        if (rc != 0) {
            return rc;
        }
        if (!wait_ready(ATECC_READY_POLL_MS)) {
            uart_printf(CONSOLE_UART,
                        "[se] 等器件备好超时（%u ms 内它一直不答地址）⇒ 本轮不读（留给重试）\r\n",
                        (unsigned)ATECC_READY_POLL_MS);
            return ATECC_E_IO;
        }
        /* ★ 读前判据：两线真被放开才读。若有线被按住 ⇒ **不读**（否则必是 BERR）⇒
         *   返回错误、交给上层 `random_try()` 重试（那里会重做唤醒序列）。*/
        if (!lines_free(&lv_scl, &lv_sda)) {
            uart_printf(CONSOLE_UART,
                        "[se] 读前电平：SCL=%u SDA=%u ✗ 有线被按住 ⇒ 本轮不读（留给重试）\r\n",
                        (unsigned)lv_scl, (unsigned)lv_sda);
            return ATECC_E_IO;
        }
        rc = resp_read(waddr[k], resp, respcap, resplen);
        if (rc == 0) {
            s_st.resp_waddr = k + 1u;                 /* 1 = 读 0x00 通了；2 = 读 0x03 通了 */
            return 0;
        }
    }
    return rc;
}

/* ★ 2026-10-10：写通了、读扑空 ⇒ 命令要**重试**。
 *   实测（见 `docs/atecc608b-se.md` §8.2）：同一颗器件会以**秒级**在醒/睡之间来回
 *   （循环里"2 轮命中 / 1 轮 NACK"交替），而刚醒时读回来的是**唤醒响应** `04 11 33 43`
 *   （count 离谱 ⇒ `atecc_msg_resp_random()` 直接判不合格）。
 *   ⇒ 一次不成就**重发命令**（每次都重做唤醒序列），别把"它刚好睡着"当成坏件。*/
#define ATECC_CMD_ATTEMPTS  3u

/* 取一条 32 字节随机数（`with_crc` 决定线上带不带 CRC）*/
static int random_try(uint8_t *out32, int with_crc)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    uint8_t resp[ATECC_RESP_LEN_CRC];
    uint32_t pklen, replen = 0u, i, attempt;
    i2c_stats_t a, b;
    int rc = ATECC_E_IO, mrc;

    pklen = (uint32_t)atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, with_crc);
    if (pklen == 0u) {
        return ATECC_E_ARG;
    }
    for (attempt = 0u; attempt < ATECC_CMD_ATTEMPTS; attempt++) {
        replen = 0u;
        /* ★ 令牌没 ACK **不等于**器件不在：器件若已经在 Idle（前一次唤醒已把它叫醒、
         *   或者它被配置成不自睡），它对地址 0x00 会回 **NACK**。
         *   ⇒ 这里**不早退**，照样把命令发出去，让**命令的结果**说话。*/
        i2c_stats(&a);
        (void)atecc_wake();
        rc = transact(pkt, pklen, resp, sizeof(resp), &replen);
        i2c_stats(&b);
        /* ★ 现场判据：把**这条包实际长什么样**与**这一轮走到哪**打出来（含重试轮次）。*/
        uart_printf(CONSOLE_UART, "[se] 试 %s 第%u次：包(%u B) ", with_crc ? "crc" : "no-crc",
                    (unsigned)(attempt + 1u), (unsigned)pklen);
        for (i = 0u; i < pklen; i++) {
            uart_printf(CONSOLE_UART, "%02x ", (unsigned)pkt[i]);
        }
        uart_printf(CONSOLE_UART,
                    "-> rc=%d tx=+%u nack=+%u timeout=+%u step=%u nackbyte=%u\r\n",
                    rc, (unsigned)(b.tx_bytes - a.tx_bytes), (unsigned)(b.nack - a.nack),
                    (unsigned)(b.timeout - a.timeout), (unsigned)b.last_step,
                    (unsigned)b.last_nack_byte);
        if (rc != 0) {
            continue;                       /* 可能是"它刚睡了/刚醒"⇒ 重试 */
        }
        mrc = atecc_msg_resp_random(resp, (size_t)replen, out32);
        if (mrc == 0) {
            return 0;
        }
        /* ★ 器件"说话"的最小形状 = **4 字节错误包**（count + 状态 + 2）⇒ 把状态
         *   **翻成文字**打出来。出处 `lib/calib/calib_command.c::isATCAError()`，
         *   措辞照库里原话（例：0xFF = bad crc / 命令没被正确收到，0x11 = 刚被唤醒）。*/
        {
            uint8_t st = 0u;

            if (atecc_msg_err_pkt_status(resp, (size_t)replen, &st) == 0) {
                uart_printf(CONSOLE_UART,
                            "[se] 器件回了 4 字节错误包：状态 0x%02x（%s）\r\n",
                            (unsigned)st, atecc_msg_status_name(st));
            }
        }
        /* 长度/count/CRC 对不上 —— 把原始字节留档（现场判据；唤醒响应 `04 11 33 43` 就落这里）*/
        s_st.last_resp_len = replen;
        for (i = 0u; i < sizeof(s_st.last_resp); i++) {
            s_st.last_resp[i] = (i < replen) ? resp[i] : 0u;
        }
    }
    return (rc != 0) ? rc : ATECC_E_RESP;
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
    /* 还没判出来：先试不带 CRC，再试带 CRC（判据 = 响应长度 33 / 35）*/
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
/* ★★ d 步：签名（Nonce + Sign）—— 私钥永远不出芯片                     */
/* ------------------------------------------------------------------ */
/* 两步（形状/CRC 见 `proto/atecc_msg.c` 的 `atecc_msg_nonce_load/sign`，字节来自
 * 2026-10-11 台架**实测**：`27 16 43 00 00 <摘要> 30 1A` + `07 41 A0 00 00 7B 85`）：
 *   ① `Nonce(0x16)`：pass-through 把 32 B **摘要**装进器件的消息摘要缓冲
 *      ⇒ 成功应答 = 4 字节状态包、**状态 0x00**（实测 `04 00 03 40` ✓）
 *   ② `Sign(0x41)`：用槽里的私钥签（param2 = 槽号）⇒ 响应 67 B = `43 | r||s(64) | CRC`
 * ⚠ 为什么两步都要重试：器件"算完之前地址是不 ACK 的"（`GenKey`/`ECDH` ≈ 40–50 ms）⇒
 *   `transact()` 里的 `wait_ready()`（250 ms）就是为它准备的；NACK 一律当"还没好" ✓。
 * ⚠ 失败时**保持槽号/结果原样**，让上层按 §8.2 Step2 降级（L0 → L1）—— 别在这里兜底 ✗。*/
int atecc_sign_digest(const uint8_t digest32[ATECC_NONCE_DIGEST_BYTES],
                      uint8_t out64[ATECC_SIGN_BYTES])
{
    uint8_t pkt[ATECC_CMD_LEN_NONCE_CRC];
    uint8_t resp[ATECC_RESP_LEN_SIG_CRC];
    uint32_t pklen, replen = 0u, attempt;
    uint8_t st = 0u;
    int rc, mrc;
    /* 线上要不要挂 CRC 已经由开机那次 `Random` 判定了（`s_crc_mode`）——
     *   还没判定过（理论上不会：`atecc_init()` 会先取一条）就按**带 CRC**（实测要带 ✓）。*/
    const int with_crc = (s_crc_mode == 0) ? 0 : 1;

    if (digest32 == 0 || out64 == 0) {
        return ATECC_E_ARG;
    }
    s_st.sign_slot = ATECC_SIGN_SLOT;

    /* ---- ① Nonce：装摘要 ---- */
    pklen = (uint32_t)atecc_msg_nonce_load(pkt, sizeof(pkt), digest32, with_crc);
    if (pklen == 0u) {
        return ATECC_E_ARG;
    }
    rc = ATECC_E_IO;
    for (attempt = 0u; attempt < ATECC_CMD_ATTEMPTS; attempt++) {
        replen = 0u;
        (void)atecc_wake();                 /* 器件若已醒，它对令牌回 NACK —— 不早退 ✓ */
        rc = transact(pkt, pklen, resp, sizeof(resp), &replen);
        if (rc != 0) {
            continue;
        }
        st = 0u;
        if (atecc_msg_err_pkt_status(resp, (size_t)replen, &st) == 0 && st == ATECC_STATUS_OK) {
            break;                          /* `04 00 …` = 摘要已装进去 ✓ */
        }
        /* 不是"成功的 4 字节状态包"（例如把上一条响应的 count 读回来了）⇒ 重试 */
        uart_printf(CONSOLE_UART, "[se] Nonce 第%u次：期望 4 字节状态包(状态 0)，实回 %u B"
                                  "（首字节 0x%02x）⇒ 重试\r\n",
                    (unsigned)(attempt + 1u), (unsigned)replen, (unsigned)resp[0]);
    }
    if (attempt >= ATECC_CMD_ATTEMPTS) {
        s_st.sign_fail++;
        uart_printf(CONSOLE_UART, "[se] Nonce 装摘要失败（%u 次都没拿到状态 0）\r\n",
                    (unsigned)ATECC_CMD_ATTEMPTS);
        return ATECC_E_IO;
    }

    /* ---- ② Sign：用槽里的私钥签 ---- */
    pklen = (uint32_t)atecc_msg_sign(pkt, sizeof(pkt), ATECC_SIGN_SLOT, with_crc);
    if (pklen == 0u) {
        return ATECC_E_ARG;
    }
    for (attempt = 0u; attempt < ATECC_CMD_ATTEMPTS; attempt++) {
        replen = 0u;
        (void)atecc_wake();
        rc = transact(pkt, pklen, resp, sizeof(resp), &replen);
        if (rc != 0) {
            continue;
        }
        mrc = atecc_msg_resp_sign(resp, (size_t)replen, out64);
        if (mrc == 0) {
            s_st.sign_ok++;
            return 0;
        }
        /* 4 字节错误包 ⇒ **把状态翻成文字**（现场判据：0x0F「cannot execute」最常见的原因是
         *   这个槽不是密钥槽 / 配置与预期不符，0x05 = ECC 计算错）。*/
        st = 0u;
        if (atecc_msg_err_pkt_status(resp, (size_t)replen, &st) == 0) {
            uart_printf(CONSOLE_UART, "[se] Sign 第%u次：器件回状态 0x%02x（%s）\r\n",
                        (unsigned)(attempt + 1u), (unsigned)st,
                        atecc_msg_status_name(st));
        } else {
            uart_printf(CONSOLE_UART, "[se] Sign 第%u次：响应形状不对（%u B，首字节 0x%02x）\r\n",
                        (unsigned)(attempt + 1u), (unsigned)replen, (unsigned)resp[0]);
        }
    }
    s_st.sign_fail++;
    s_st.last_err = (uint32_t)(-ATECC_E_RESP);
    return ATECC_E_RESP;
}

int atecc_sign_pre(uint8_t out64[ATECC_SIGN_BYTES], const void *pre, size_t prelen, void *ctx)
{
    uint8_t dg[SHA256_DIGEST_LEN];

    (void)ctx;
    if (out64 == 0 || pre == 0) {
        return ATECC_E_ARG;
    }
    /* 哈希在 MCU（快、无秘密），签名在 SE（私钥不出芯片）✓ —— 两者各管一段 */
    sha256(pre, prelen, dg);
    return atecc_sign_digest(dg, out64);
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
    out->sign_ok       = s_st.sign_ok;
    out->sign_fail     = s_st.sign_fail;
    out->sign_slot     = s_st.sign_slot;
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
    uart_printf(CONSOLE_UART,
                "\r\n[se] 唤醒判据 = 读回的 4 字节应等于 04 11 33 43：%s（ok=%u fail=%u 轮=%u/%u）\r\n",
                (rc == 0) ? "是 ✓" : "不是 ✗",
                (unsigned)s_st.wake_ok, (unsigned)s_st.wake_fail,
                (unsigned)s_st.last_wake_tries, (unsigned)ATECC_WAKE_TRIES);
    if (rc != 0) {
        uart_printf(CONSOLE_UART,
                    "[se] 最后一轮读到：%02x %02x %02x %02x（≠04 11 33 43）\r\n",
                    (unsigned)s_st.last_resp[0], (unsigned)s_st.last_resp[1],
                    (unsigned)s_st.last_resp[2], (unsigned)s_st.last_resp[3]);
        /* ★★ 2026-10-11 实测（同一颗已通的芯片，连按两次 `se`）：第 1 次 `是 ✓`；
         *   第 2 次这一行读到 **`23 00 00 00`** —— 那正是**上一条 `Random` 响应的 count**
         *   （`0x23` = 35 = 1 count + 32 数据 + 2 CRC）✓。
         *   机制：唤醒应答**只在“器件真的从睡眠被叫醒”时**才产生 ✓；器件本来就醒着时
         *   它不重新产生，于是我们读到的是**输出缓冲里的旧内容** ✗（不是电气故障 ✗）。
         *   ⇒ 原来那句 `⇒ 查 3V3 供电 / 接线 / 上拉` **会把人带偏** ✗（实测：报“不是”的
         *     同一次里，紧接着的 `Random` 是**成功**的 ✓）⇒ 判据改口径，只有两处算数：
         *     ① **随后那条命令的结果** ✓；② 下面 `i2c:` 行里的 **`rx` 有没有涨** ✓
         *     （`rx` 不涨 + `nack` 在涨 = 连地址都没人答 ⇒ 才轮到查供电/接线/上拉 ✓）。*/
        uart_printf(CONSOLE_UART,
                    "[se] ⚠ 这一行**只在「器件从睡眠里被叫醒」时才有意义**：它本来就醒着时，"
                    "读到的是输出缓冲里的旧内容（如上面的 `23 …` = 上一条响应的 count）"
                    "⇒ **不要据此查接线/供电** ✗\r\n");
        uart_printf(CONSOLE_UART,
                    "[se]    要看两处：① 随后那条命令**成不成功** ✓；② `i2c:` 行的 `rx` 有没有涨"
                    "（`rx` 不涨 + `nack` 涨 = 地址没人答 ⇒ 才查 3V3 供电 / SDA(PB7)-SCL(PB6) "
                    "接线 / 4.7k 上拉）\r\n");
        uart_printf(CONSOLE_UART,
                    "[se] 注：对地址 0x00 写 0 字节**本来就不会 ACK**（库里原话 "
                    "\"part will NACK, so don't check for status\"）⇒ 别拿它当\"器件不在\"✗\r\n");
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
                (s_st.crc_mode == 0u) ? "no-crc(响应 33 B)" :
                (s_st.crc_mode == 1u) ? "crc(响应 35 B)" : "unknown",
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

    /* ★★ d 步的签名判据：对**固定摘要**签一次，把 `r||s` 打出来 ⇒ **PC 侧用 Slot 0 的公钥
     *   独立验签** ✓（板上**不**自己验自己：那只证明自洽、证明不了对 ✗ —— 同 §8.7 的口径）。
     * 摘要用固定串 ⇒ 每次开机这条判据都可复现（PC 侧照样算 SHA-256("ORPAH d-step probe v1")）。*/
    {
        static const char msg[] = "ORPAH d-step probe v1";
        uint8_t dg[SHA256_DIGEST_LEN];
        uint8_t sig[ATECC_SIGN_BYTES];

        sha256(msg, sizeof(msg) - 1u, dg);
        rc = atecc_sign_digest(dg, sig);
        if (rc != 0) {
            uart_printf(CONSOLE_UART,
                        "[se] Sign(slot %u) FAILED rc=%d（sign_ok=%u fail=%u）\r\n",
                        (unsigned)ATECC_SIGN_SLOT, rc, (unsigned)s_st.sign_ok,
                        (unsigned)s_st.sign_fail);
            return rc;
        }
        uart_printf(CONSOLE_UART, "[se] Sign(slot %u) ok; r=", (unsigned)ATECC_SIGN_SLOT);
        for (i = 0u; i < (ATECC_SIGN_BYTES / 2u); i++) {
            uart_printf(CONSOLE_UART, "%02x", (unsigned)sig[i]);
        }
        uart_printf(CONSOLE_UART, "\r\n[se] s=");
        for (i = (ATECC_SIGN_BYTES / 2u); i < ATECC_SIGN_BYTES; i++) {
            uart_printf(CONSOLE_UART, "%02x", (unsigned)sig[i]);
        }
        uart_printf(CONSOLE_UART, "\r\n[se] sign_ok=%u sign_fail=%u（摘要 = SHA-256(\"%s\")）\r\n",
                    (unsigned)s_st.sign_ok, (unsigned)s_st.sign_fail, msg);
        uart_printf(CONSOLE_UART, "[se] ⚠ 板上不验自己：PC 侧拿 Slot 0 的公钥独立验"
                                  "（docs/atecc608b-se.md §8.10）\r\n");
    }
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
        /* ★ 2026-10-11 修（原 bug ✗：只凭“地址应答”就改命令地址 ⇒ 台上板载 24C02 在 0x50 先应答
         *   ⇒ 地址被换成 0x50 ⇒ 后面 Info/锁位/Random **全部对着 EEPROM** 做，报出**假**判据 ✗）。
         *   CA 器件的判据是**能答 CA 命令**（`Info(0x30)`）✓，不是“地址 ACK” ✗。
         *   从扫描得到的候选开始逐个试（只试到第一个能答 Info 的为止）。*/
        uint8_t a;
        uint8_t chosen = 0u;

        for (a = hit; a <= 0x7Fu && chosen == 0u; a++) {
            uint8_t save;
            int is_ca;

            if (diag_probe_n(a) < DIAG_PROBE_MIN) { continue; }
            save = s_addr;
            s_addr = a;
            is_ca = (diag_cmd4(ATECC_OP_INFO, 0x00u, 0x0000u, rev, &crc_used) == 0);
            if (is_ca) {
                chosen = a;
            } else {
                s_addr = save;
                uart_printf(CONSOLE_UART, "[sewake] ⚠ 0x%02x 地址应答但**不答 Info(0x30)**"
                                          " ⇒ 不是 CA 器件（多半是别的 I²C 器件，如板载 EEPROM）"
                                          " ⇒ 不认它 ✓\r\n", (unsigned)a);
            }
        }
        hit = chosen;
        if (chosen != 0u) {
            uart_printf(CONSOLE_UART, "[sewake] ★ 0x%02x **答了 Info(0x30)** ⇒ 认它，"
                                      "命令地址改成它 ✓（判据是能答 CA 命令，不是地址 ACK）\r\n",
                        (unsigned)chosen);
        } else {
            uart_printf(CONSOLE_UART, "[sewake] 有地址应答但**没人答 Info(0x30)** ⇒"
                                      " **不改命令地址**（仍是 0x%02x）✓，"
                                      "也不做下面 ③④ 的判据 ✗\r\n", (unsigned)s_addr);
        }
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
