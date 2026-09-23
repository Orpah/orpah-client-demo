/* main.c — ORPAH 客户端固件（c 步）
 *
 * c3-2b（2026-09-20）加上**模组数据口**（HGIC over UART），上机后能验三件事：
 *   1) 上电打横幅 + 心跳灯（c1 就有的“固件真的跑起来了”）；
 *   2) **上电自动探测数据口**：发 `GET_UART_FIXLEN` 并打印模组的**真应答** ——
 *      “写出去”不等于模块活着，收到 CMD 应答才算数据口双向通（同 `tools/hgic_bus.py` 的 `ping()`）；
 *   3) 模组送上来的帧在控制台打摘要（数据帧 = 以太头 dst/src/ethertype/len；控制面 = id/status）。
 * 控制台命令：`AT` / `ping` / `stat` / `send <hex>` / `help`。
 *
 * ★ 还没做的（留给后面的阶段，别在这里自由发挥）：
 *   · c4 §8.2 选级 + 无 RTC 声明（`ts=0` / `cap.rtc=false`）+ 自限频 + 已签 ID 上报；
 *   · 收上来的帧**还没交给协议栈**（c4 才接 `msg./jcs./id_report.`）—— 现在只打印摘要；
 *   · `payload.nonce` 的来源（规范要求 ATECC608B RNG，等 SE 接线）。
 *
 * ⚠ 控制台输出一律 **ASCII**（终端代码页可能是 GBK，中文会成乱码 —— 同 make recipe 那条纪律）。
 * ⚠ 时钟：8 MHz HSI、无 PLL（见 board.h）。**烧录由用户执行**（见 Makefile 的 flash 目标）。
 * ⚠ 引脚口径全在 `board.h`（`CONSOLE_*` / `MODULE_*` / `LED_HEART_*`）；本文件不写死引脚。
 * ⚠ **链接基址必须 0x00000000、`IRQn_Type` 必须带 +16、`mstatus` 要 0x1888、TIM `INTFR` 清标志写 0**
 *   —— 这四条是 2026-09-21 上机踩出来的（台架记录 `docs/c3-2b-bench-bringup.md` §4）。
 */
#include "board.h"
#include "gpio.h"
#include "uart.h"

#include "hgic.h"          /* proto/ 的帧层：hgic_ctrl_parse / HGIC_CMD_* / HGIC_T_* */
#include "hgic_uart.h"     /* 本仓的模组数据口驱动 */
#include "atecc.h"         /* ★ c4-γ-2：安全元件（banner 要打**实测**的 RNG 来源）*/
#include "i2c.h"           /* 工装 `seclk`：运行时改 I²C 速率 */
#include "id_core.h"       /* ★ c4-γ-1：已签 ID 上报任务（proto/id_build.c 的固件侧外壳）*/

/* ------------------------------------------------------------------ */
/* 控制台：收字节成行（回显 + 整行命令）                                  */
/* ------------------------------------------------------------------ */
#define CONSOLE_LINE_MAX 640            /* hex 串最多 640 字符 = 320 字节的帧 */

static char             g_line[CONSOLE_LINE_MAX];
static uint16_t         g_line_n;
static volatile uint8_t g_line_ready;
static volatile uint32_t g_rx_bytes;

static void console_rx(uint8_t b)
{
    g_rx_bytes++;
    if (b == '\r' || b == '\n') {
        if (g_line_n > 0u) {
            g_line[g_line_n] = '\0';
            g_line_n = 0u;
            g_line_ready = 1u;      /* ⚠ 主循环没来得及处理就再来一行 ⇒ 覆盖（人手速，够用）*/
        }
        return;
    }
    uart_putc(CONSOLE_UART, b);     /* 回显：串口通不通一眼看出 */
    if (g_line_n + 1u < (uint16_t)sizeof(g_line)) {
        g_line[g_line_n++] = (char)b;
    }
}

/* ------------------------------------------------------------------ */
/* 1 ms 时基（照参考骨架：TIM2；后面 c3/c4 的节拍/超时都靠它）              */
/* ------------------------------------------------------------------ */
volatile uint32_t g_tick_ms;
volatile uint32_t g_loops;      /* 主循环圈数（`stat` 里看“主循环卡没卡”）*/

static void tick_init(void)
{
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;

    TIM2->CTLR1 = 0;
    TIM2->PSC   = SYSTEM_CLOCK_HZ / 1000000UL - 1UL;  /* -> 1 MHz 计数时钟 */
    TIM2->ATRLR = 1000UL - 1UL;                       /* -> 每 1000 计数 = 1 ms */
    TIM2->CNT   = 0u;
    /* ★ `INTFR` 是 **write-all-bits**：清标志要**写 0**（写 1 会置位！——见 ISR 里的说明）。*/
    TIM2->EVGR  = 1u;
    TIM2->INTFR = 0u;
    TIM2->DMAINTENR |= TIM_DMAINTENR_UIE;
    TIM2->CTLR1 |= TIM_CTLR1_CEN | TIM_CTLR1_ARPE;
    NVIC_EnableIRQ(TIM2_IRQn);
}

/* ------------------------------------------------------------------ */
/* 模组数据口（HGIC over UART）                                          */
/*   回呼在**主循环上下文**（驱动保证）⇒ 这里打印是安全的。                  */
/* ------------------------------------------------------------------ */
static uint8_t  g_ping_pending;          /* 等 GET_UART_FIXLEN 的应答 */
static uint32_t g_ping_seq;              /* 发出过几次探测 */
static uint32_t g_ping_ok;               /* 收到过几次应答（= 双向通的证据）*/
static uint32_t g_ping_deadline;         /* 超时（ms 时基）*/

static void print_mac(const uint8_t *m)
{
    static const char *H = "0123456789abcdef";
    char b[18];
    int i, j = 0;

    for (i = 0; i < 6; i++) {
        if (i != 0) { b[j++] = ':'; }
        b[j++] = H[(m[i] >> 4) & 0x0Fu];
        b[j++] = H[m[i] & 0x0Fu];
    }
    b[j] = '\0';
    uart_printf(CONSOLE_UART, "%s", b);
}

static void module_frame(const hgic_hdr_t *h, const uint8_t *payload, size_t plen)
{
    if (h->type == HGIC_T_FRM || h->type == HGIC_T_FRM2) {
        /* 数据帧 = 一整个以太帧（≥ 14 B 以太头）*/
        uart_printf(CONSOLE_UART, "\r\n[mod] FRM2 len=%u ifidx=%u flags=%u\r\n",
                    (unsigned)plen, (unsigned)h->ifidx, (unsigned)h->flags);
        if (plen >= 14u) {
            uart_printf(CONSOLE_UART, "[mod]   dst=");
            print_mac(payload);
            uart_printf(CONSOLE_UART, " src=");
            print_mac(payload + 6u);
            uart_printf(CONSOLE_UART, " type=0x%x\r\n",
                        (unsigned)(((unsigned)payload[12] << 8) | (unsigned)payload[13]));
        }
    } else {
        hgic_ctrl_t c;

        if (hgic_ctrl_parse(h, payload, plen, &c) != 0) {
            uart_printf(CONSOLE_UART, "\r\n[mod] ctrl type=%u (body 解不出)\r\n",
                        (unsigned)h->type);
            return;
        }
        uart_printf(CONSOLE_UART, "\r\n[mod] ctrl type=%u id=%u",
                    (unsigned)h->type, (unsigned)c.id);
        if (c.has_status) {
            uart_printf(CONSOLE_UART, " status=%u len=%u",
                        (unsigned)c.status, (unsigned)c.data_len);
        } else {
            uart_printf(CONSOLE_UART, " (short resp / req)");
        }
        uart_printf(CONSOLE_UART, "\r\n");
        if (c.id == HGIC_CMD_GET_UART_FIXLEN) {
            g_ping_ok++;
            g_ping_pending = 0u;
            uart_printf(CONSOLE_UART, "[mod] **DATA PORT IS UP**（模组应答了 GET_UART_FIXLEN）\r\n");
        }
    }
}

static void do_ping(void)
{
    int n = hgic_uart_send_cmd(HGIC_CMD_GET_UART_FIXLEN, NULL, 0);

    g_ping_seq++;
    if (n < 0) {
        uart_printf(CONSOLE_UART, "[mod] send GET_UART_FIXLEN failed rc=%d\r\n", n);
        return;
    }
    g_ping_pending = 1u;
    g_ping_deadline = g_tick_ms + 3000u;   /* 3 s 超时（同 PC 侧 ping 的默认）*/
    uart_printf(CONSOLE_UART, "[mod] tx GET_UART_FIXLEN #%u, waiting reply...\r\n",
                (unsigned)g_ping_seq);
}

static void print_stats(void)
{
    hgic_uart_stats_t st;

    hgic_uart_stats(&st);
    uart_printf(CONSOLE_UART, "\r\n[mod] rx_bytes=%u frames=%u (frm2=%u ctrl=%u)\r\n",
                (unsigned)st.rx_bytes, (unsigned)st.rx_frames,
                (unsigned)st.rx_frm2, (unsigned)st.rx_ctrl);
    uart_printf(CONSOLE_UART, "[mod] tx_frames=%u ring_drop=%u garbage=%u bad_len=%u overrun=%u\r\n",
                (unsigned)st.tx_frames, (unsigned)st.ring_drop,
                (unsigned)st.garbage, (unsigned)st.bad_length, (unsigned)st.overrun);
    uart_printf(CONSOLE_UART, "[mod] probe: tx=%u reply=%u\r\n",
                (unsigned)g_ping_seq, (unsigned)g_ping_ok);
    /* 上机判据（c3-2b 用过，留着很便宜）：`tick` 不动 = 1 ms 时基中断没跑；
     * `loops` 不动 = 主循环卡住；两条都不动 ⇒ 优先怀疑「中断根本没进来」。*/
    uart_printf(CONSOLE_UART, "[mod] tick=%u loops=%u\r\n",
                (unsigned)g_tick_ms, (unsigned)g_loops);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* `send <hex>`：把 hex 解成一条**以太帧**发出去（hex 由 PC 侧给，
 * 例：`python tools\eth_frames.py` 的 ARP 42 B / DHCP DISCOVER 286 B）。
 * 这样帧的构造**单一源仍在 PC 侧**，固件不重复实现造帧。*/
static void send_hex(const char *hex)
{
    static uint8_t buf[320];        /* ★ 静态：别在 20 KB RAM 的片子上压栈 */
    size_t n = 0u;
    int rc;

    while (hex[0] != '\0' && hex[1] != '\0' && n < sizeof(buf)) {
        int hi = hexval(hex[0]);
        int lo = hexval(hex[1]);

        if (hi < 0 || lo < 0) { break; }
        buf[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    if (n < 14u) {
        uart_printf(CONSOLE_UART, "\r\n[mod] need >=14 bytes (got %u)\r\n", (unsigned)n);
        return;
    }
    rc = hgic_uart_send_frm2(buf, n);
    uart_printf(CONSOLE_UART, "\r\n[mod] sent FRM2 %u byte(s) rc=%d\r\n", (unsigned)n, rc);
}

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

/* 前缀匹配（工装命令带参数时用；比逐字节比好读）。*/
static int str_starts(const char *s, const char *p)
{
    while (*p != '\0') {
        if (*s++ != *p++) { return 0; }
    }
    return 1;
}

static void print_help(void)
{
    uart_printf(CONSOLE_UART, "\r\ncommands:\r\n");
    uart_printf(CONSOLE_UART, "  AT           -> OK (console self-test)\r\n");
    uart_printf(CONSOLE_UART, "  ping         -> send GET_UART_FIXLEN, prove module is alive\r\n");
    uart_printf(CONSOLE_UART, "  stat         -> module-port counters (incl. dropped bytes)\r\n");
    uart_printf(CONSOLE_UART, "  send <hex>   -> tx one ethernet frame (hex, >=14 B)\r\n");
    uart_printf(CONSOLE_UART, "  id           -> signed-ID task status (level/why/nonce/counters)\r\n");
    uart_printf(CONSOLE_UART, "  idsend       -> build+send one ORPAH-ID-REPORT now (self-limited)\r\n");
    uart_printf(CONSOLE_UART, "  idhex        -> dump last frame as hex (paste into PC judge)\r\n");
    uart_printf(CONSOLE_UART, "  idmodes      -> 8.2 fault-injection modes -> level\r\n");
    uart_printf(CONSOLE_UART, "  idlevel <m>  -> set mode: auto|sign_fail|se_fail|no_key\r\n");
    uart_printf(CONSOLE_UART, "  seline [1-9] -> bench: toggle SCL/SDA as GPIO open-drain (~2 Hz)\r\n");
    uart_printf(CONSOLE_UART, "  sewake       -> bench: scan addr 0x01-0x7F (before/after pulse), then\r\n");
    uart_printf(CONSOLE_UART, "                  Info(0x30) + Read cfg lock + Random -> one verdict line\r\n");
    uart_printf(CONSOLE_UART, "  sescan [n]   -> bench: repeat full scan n rounds (default 10) -> rate table\r\n");
    uart_printf(CONSOLE_UART, "  seclk <khz>  -> bench: set I2C clock (wake token must be <=100 kHz)\r\n");
    uart_printf(CONSOLE_UART, "  seaddr <h>   -> bench: set device 7-bit addr in hex (e.g. 35)\r\n");
}

static void run_cmd(const char *cmd)
{
    while (*cmd == ' ') { cmd++; }
    if (str_eq(cmd, "AT")) {
        uart_printf(CONSOLE_UART, "\r\nOK (rx=%u)\r\n", (unsigned)g_rx_bytes);
    } else if (str_eq(cmd, "ping")) {
        do_ping();
    } else if (str_eq(cmd, "stat")) {
        print_stats();
    } else if (cmd[0] == 's' && cmd[1] == 'e' && cmd[2] == 'n' && cmd[3] == 'd' &&
               (cmd[4] == ' ' || cmd[4] == '\0')) {
        send_hex(cmd + 4);
    } else if (str_eq(cmd, "help")) {
        print_help();
    } else if (str_starts(cmd, "seline")) {
        /* 工装（2026-09-23）：`seline [1-9]` —— SCL/SDA 当 GPIO 开漏翻转几秒，
         * 供沿路逐点量通断（见 `Periph/atecc.h` 的说明）。不带参数 = 10 s。*/
        uint32_t sec = 10u;
        const char *a = cmd + 6;

        while (*a == ' ') { a++; }
        if (*a >= '1' && *a <= '9') { sec = (uint32_t)(*a - '0'); }
        (void)atecc_line_test(sec);
    } else if (str_starts(cmd, "sewake")) {
        /* 工装：把握手拆开看（不判唤醒 ACK、逐地址探测、最后直接发 Random）。*/
        (void)atecc_diag_probe();
    } else if (str_starts(cmd, "sescan")) {
        /* 工装：重复扫描统计（一轮 = 完整唤醒 + 扫一遍全地址）——
         * 用来分开"稳定器件 / 虚焊时有时无 / 纯毛刺"，不用示波器抓那一下。*/
        uint32_t rounds = 0u;
        const char *a = cmd + 6;

        while (*a == ' ') { a++; }
        while (*a >= '0' && *a <= '9') { rounds = rounds * 10u + (uint32_t)(*a - '0'); a++; }
        (void)atecc_diag_scan_stats(rounds);
    } else if (str_starts(cmd, "seclk")) {
        /* 工装：改 I²C 速率（唤醒令牌按手册必须 ≤100 kHz）。*/
        uint32_t khz = 0u;
        const char *a = cmd + 5;

        while (*a == ' ') { a++; }
        while (*a >= '0' && *a <= '9') { khz = khz * 10u + (uint32_t)(*a - '0'); a++; }
        if (khz >= 10u && khz <= 400u) {
            i2c_set_hz(khz * 1000u);
        }
        uart_printf(CONSOLE_UART, "\r\n[seclk] I2C 现在是 %u kHz\r\n",
                    (unsigned)(i2c_get_hz() / 1000u));
    } else if (str_starts(cmd, "seaddr")) {
        /* 工装：手动设 7 位器件地址（手册第 13 页：608B 的地址可编程）。*/
        uint32_t v = 0u, ndig = 0u;
        const char *a = cmd + 6;

        while (*a == ' ') { a++; }
        for (; *a != '\0' && ndig < 2u; a++) {
            uint32_t d;
            char ch = *a;

            if (ch >= '0' && ch <= '9') { d = (uint32_t)(ch - '0'); }
            else if (ch >= 'a' && ch <= 'f') { d = (uint32_t)(ch - 'a') + 10u; }
            else if (ch >= 'A' && ch <= 'F') { d = (uint32_t)(ch - 'A') + 10u; }
            else { break; }
            v = v * 16u + d;
            ndig++;
        }
        if (ndig > 0u && v <= 0x7Fu) {
            atecc_set_addr((uint8_t)v);
        }
        uart_printf(CONSOLE_UART, "\r\n[seaddr] 器件地址现在是 0x%02x\r\n",
                    (unsigned)atecc_get_addr());
    } else if (idc_console(cmd, g_tick_ms)) {
        /* 已签 ID 任务的控制台命令（id / idsend / idhex / idmodes / idlevel）*/
    } else if (*cmd != '\0') {
        uart_printf(CONSOLE_UART, "\r\n? unknown: %s (type help)\r\n", cmd);
    }
}

static void banner(void)
{
    uart_printf(CONSOLE_UART, "\r\n");
    uart_printf(CONSOLE_UART, "[orpah-client] CH32V203 firmware (c1 skeleton + c3-2b module port)\r\n");
    uart_printf(CONSOLE_UART, "[orpah-client] clock=%u Hz HSI, console=%u 8N1, module=%u 8N1\r\n",
                (unsigned)SYSTEM_CLOCK_HZ, (unsigned)CONSOLE_BAUD, (unsigned)MODULE_BAUD);
    uart_printf(CONSOLE_UART, "[orpah-client] module port = HGIC over UART (proto/hgic.c frame layer)\r\n");
    /* ★ 如实（c4-γ-2 起）：这两件事**必须分开说** ——
     *   ① `sign=` = **签名本体用的是什么**：本 demo 仍是**软件 P-256 替身**
     *      （SE 签名 / Slot0 在 d 步，见 ROADMAP §五）⇒ level=0 是**演示级**；
     *   ② `rng=` = **nonce 的来源**：接了 ATECC608B 就是它的 `Random(0x1B)`，
     *      没接/不应答则**如实**回退软熵（非生产强度）。
     *   ⚠ 不能写成一句“已启用 SE” —— 那是把①②混为一谈。*/
    uart_printf(CONSOLE_UART, "[orpah-client] id-task: sn=%s period=%u ms  sign=software P-256"
                              " (demo-grade; SE sign is d-step)\r\n",
                IDC_SN_DEFAULT, (unsigned)IDC_INTERVAL_MS);
    uart_printf(CONSOLE_UART, "[orpah-client] rng=%s\r\n",
                atecc_present() ? "ATECC608B Random(0x1B)"
                                : "soft-entropy (SE absent; non-production strength)");
    uart_printf(CONSOLE_UART, "[orpah-client] type 'help' for commands\r\n");
}

/* ------------------------------------------------------------------ */
/* ★ 最早期自检（c3-2b 上机调试加）：**只用 CPU + 心跳灯那一组引脚**（口径见
 *   `board.h` 的 `LED_HEART_*`），不碰任何其它外设。
 *   目的：把"固件到底跑没跑"与"某一步外设初始化失败"分开 ——
 *   灯闪 = 启动/时钟/Flash/GPIO 都好；不闪 = 比 GPIO 更早的地方就出事了。
 *   ⚠ 延时是**忙等粗估**（不精确、不用 TIM2）：只做视觉节拍，够用。*/
/* ------------------------------------------------------------------ */
static void blind_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 2000u;      /* 8 MHz、无 PLL，每次迭代几条指令 */

    while (n--) { }
}

static void led_write(uint8_t on)
{
    gpio_set_pin(LED_HEART_PORT, LED_HEART_PIN,
                 (uint8_t)(LED_HEART_ACTIVE_LOW ? !on : on));
}

static void led_blink(uint8_t times, uint32_t on_ms, uint32_t off_ms)
{
    uint8_t i;

    for (i = 0; i < times; i++) {
        led_write(1u);
        blind_delay_ms(on_ms);
        led_write(0u);
        blind_delay_ms(off_ms);
    }
}

/* 分段闪灯（c3-2b 上机定位用过，现已关掉）：每过一个初始化阶段就闪 N 下长闪。
 *   3 下**短**闪 = 第一口呼吸；之后：1 长 = console UART 好了、
 *   2 长 = 模组数据口好了、3 长 = 1 ms 时基好了。
 *   置 0 = 只留心跳；下次上机定位再开 1。*/
#define BRINGUP_MARKS 0
#if BRINGUP_MARKS
static void marks(uint8_t n) { led_blink(n, 600u, 300u); }
#else
static void marks(uint8_t n) { (void)n; }
#endif

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
    uint32_t last_ping = 0;

    /* ★★ 0) 第一口呼吸：只开**心跳灯所在端口**的时钟 + 闪 3 下（在任何其它外设之前）
     *   看到这 3 下 ⇒ 启动/时钟/Flash/GPIO 都正常，后面再出事就是外设初始化的事；
     *   一下都不闪 ⇒ 别再怀疑 UART/波特率，去查烧录/启动。
     *   ⚠ 端口必须跟着 `board.h` 的 `LED_HEART_PORT` 走（2026-09-21：灯从 PC13 改到板载
     *     PA15 后，这里如果还写 GPIOC，上电那 3 下就打在没时钟的脚上 = 白闪）。*/
    RCC->APB2PCENR |= LED_HEART_CLK;
    gpio_set_mode(LED_HEART_PORT, LED_HEART_PIN, GPIO_MODE_OUT_PP_2MHZ);
    led_write(0u);
    blind_delay_ms(300u);            /* 给眼睛一个"起点"，好数后面闪了几下 */
    led_blink(3u, 120u, 180u);

    /* GPIO/控制台时钟（uart_init 只配引脚与波特率，时钟得自己开） */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA |
                      RCC_APB2Periph_AFIO;

    /* 心跳灯（先灭） */
    led_write(0u);

    uart_init(CONSOLE_UART, GPIOA, CONSOLE_TX_PIN, CONSOLE_RX_PIN,
              CONSOLE_BAUD, CONSOLE_UART_IRQn, console_rx);
    marks(1u);                        /* 1 长闪 = console UART 初始化返回了 */

    hgic_uart_init(module_frame);     /* ★ 模组数据口（HGIC over UART）*/
    marks(2u);                        /* 2 长闪 = 模组数据口初始化返回了 */

    tick_init();
    marks(3u);                        /* 3 长闪 = 1 ms 时基起来了 */

    /* ★ c4-γ-2：**先**探一次安全元件（幂等）—— banner 里要打的是**实测**结果，
     *   不能写死；`idc_init()` 还会再叫一次，那时会直接复用这个结论。*/
    (void)atecc_init();

    banner();
    do_ping();                        /* ★ 上电就探测：模组应答才算数据口通 */

    /* ★ c4-γ-1：已签 ID 上报任务。
     * `boot_entropy` = 上电熵（**几十 bit、非生产强度**，见 proto/id_nonce.h）：
     *   取 TIM2 计数（刚跑起来，低位随机）+ ms 时基，混一下即可 —— 它只用来让
     *   **不同 boot 的 nonce 不撞**；真随机要等 d 步的 ATECC608B。*/
    idc_init((uint32_t)(TIM2->CNT) ^ (g_tick_ms * 2654435761u));

    for (;;) {
        g_loops++;                    /* `stat` 里能看出主循环在不在转 */
        hgic_uart_poll();             /* 数据口：越勤越好（中断只入环）*/

        /* 心跳：每 ~500 ms 翻一次灯（只看“在变”，不关心亮还是灭）*/
        if ((int32_t)(g_tick_ms - last_beat) >= 500) {
            last_beat = g_tick_ms;
            gpio_set_pin(LED_HEART_PORT, LED_HEART_PIN,
                         (uint8_t)!gpio_get_pin(LED_HEART_PORT, LED_HEART_PIN));
        }

        /* ★★ 周期探测（c3-2b 上机调试加）：每 3 s 从数据口发一次 ping。
         *   为什么不用“只开机发一次”：那是一次性证据，PC 侧要恰好在那一下听着才看得到；
         *   改成周期后 ⇒ 模组 AT/打印口**每 3 s** 会出现 `[mbus rx] 9 byte(s) …`，
         *   控制台也会周期出现结果 ⇒ **任何时刻都能验证数据口通不通**。*/
        if ((int32_t)(g_tick_ms - last_ping) >= 3000) {
            last_ping = g_tick_ms;
            do_ping();
        }

        /* 探测超时：**如实报“没应答”**（不静默等）*/
        if (g_ping_pending && (int32_t)(g_tick_ms - g_ping_deadline) >= 0) {
            g_ping_pending = 0u;
            uart_printf(CONSOLE_UART,
                        "[mod] no reply in 3s - check wiring / baud / module fw\r\n");
        }

        /* ★ c4-γ-1：已签 ID 上报（``IDC_INTERVAL_MS` 到点就发；组装/签名会阻塞一会儿，
         *   时长在 `[id] sent … build_ms=` 里如实打出来）*/
        idc_tick(g_tick_ms);

        /* 控制台整行命令 */
        if (g_line_ready) {
            g_line_ready = 0u;
            run_cmd(g_line);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 中断向量（名称必须与 startup 的向量表一致）                            */
/* ------------------------------------------------------------------ */
APP_IRQ void TIM2_IRQHandler(void)
{
    /* ★ `INTFR` 是 **write-all-bits（rw）**：**写 0 才清零、写 1 反而置位**。
     *   依据 = WCH 官方 `TIM_ClearITPendingBit()` 的写法 `TIMx->INTFR = ~TIM_IT;`
     *   （不是 STM32 那种 rc_w0 惯例）。
     *   ⚠ 2026-09-21 踩过：按 rc_w0 惯例改成“写 1”，结果**把 UIF 又置上** ⇒
     *     ISR 立刻重入 ⇒ 死风暴，连横幅都打不出来（主循环永远回不去）。*/
    TIM2->INTFR &= ~TIM_INTFR_UIF;
    g_tick_ms++;
}

APP_IRQ void CONSOLE_UART_IRQ(void)
{
    uart_irq_rx(CONSOLE_UART);
}

/* 模组数据口的中断：只把字节丢进驱动的环（见 Periph/hgic_uart.c 的说明）*/
APP_IRQ void MODULE_UART_IRQ(void)
{
    uart_irq_rx(MODULE_UART);
}
