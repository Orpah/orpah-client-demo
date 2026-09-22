/* hgic_uart.c — 模组数据口驱动（见 hgic_uart.h 的分工与理由）。
 *
 * 三层各司其职，**别互相渗透**：
 *   中断 `USARTx_IRQHandler` → `uart_irq_rx()` → 本文件 `hgic_rx_byte()`：**只入环**
 *   主循环 `hgic_uart_poll()`：排空环 → `hgic_parser_feed()`（proto/ 的帧层）→ 回调应用
 *   发送：`hgic_frame_*`（proto/ 组帧）→ `hgic_uart_send()`（本文件写出）
 *
 * ★3 **cookie 为什么按通道分两个计数器**（2026-09-22；★**已用对照实验证实**，不是猜的）：
 *   现场观察：`idsend` 那条 FRM2 被模组收下（`[mbus rx] 403 byte(s)`）**之后**，
 *   模组跟着打一条 `cookie err: last:110, new:116`；而中间那 5 条 CMD 探测（cookie 111..115）
 *   模组**一句都没抱怨**。
 *   ⇒ 假设：模组是**按数据通道**查顺序（拿上一条 FRM2 的 cookie 跟这一条比），
 *      而以前 CMD 与 FRM2 **共用**一个计数器 ⇒ 每条 FRM2 的 cookie 天然比上一条大一截。
 *   ⇒ 改法：**两个通道各一个计数器**，各自逐帧 +1。
 *   ★ **对照实验（2026-09-22，重刷上面这版后）**：复位后连发两条 `idsend` ——
 *      · 第 1 条（cookie `01 00`）：`[mbus rx] 403 byte(s)` + **`cookie err: last:271, new:1`**
 *        （预期的：模组的 last 还停在**复位前**那个值 271，而我们从头开始了）；
 *      · 第 2 条（cookie `02 00`）：`[mbus rx] 403 byte(s)` + **一句 err 都没有** ⇒
 *        模组的数据通道 last 已更新成 1，与 2 连续 ⇒ **假设成立**。
 *      · 同期 CMD 探测走 `11 00` / `12 00`（17/18，独立计数），模组照旧不抱怨。
 *   ⚠ **如实剩下的边界**：每次**复位**后第一条 FRM2 **必然**报一次 err（cookie 从头开始，
 *      而模组记着旧的大值；固件没有 NVM 去保存这个计数，也不该为此加）。
 *      本轮**没观察到它造成任何后果**（那条帧照样 `[mbus rx] 403 byte(s)` 收下）；
 *      「模组会不会因这次 err 丢掉该帧」**仍未验证** —— 要证得看它有没有上空口，
 *      而 AP/STA 主机口 → 空口那段本来就还没通（见 `../../hardware/wiring/README.md`）。
 *   为什么只能这么试：模组固件是**预编译**的（`app=2.4.1.5`，SDK 里没有这段源码）⇒
 *   查不了它的判据源码，只能"改一处 → 看日志"。`tools/txah_hgic.py::CookieCounter` 那边
 *   仍是**单计数器**（PC 侧脚本，另一回事），别把两处混起来改。
 */
#include "hgic_uart.h"

#include "board.h"
#include "uart.h"

#define RING_SIZE   1024u                       /* **必须是 2 的幂**（下面用掩码取模） */
#define RING_MASK   (RING_SIZE - 1u)

/* 上行暂存上限。理由：我们发的是**以太帧**（≤ 1518 B 含 FCS）+ 8 B 头。
 * ⚠ 不取 `HGIC_MAX_FRAME`(4096)：那是**模组接收缓冲**的尺寸，在 20 KB RAM 的片子上
 *   没必要为此留 4 KB 发送暂存。超了就返回 HGIC_E_CAP（不静默截断）。*/
#define HGIC_TX_MAX (HGIC_HDR_LEN + 1536u)

static uint8_t  s_ring[RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;
static volatile uint32_t s_ring_drop;

static hgic_parser_t s_parser;                  /* 4 KB+：放**静态**，不压栈 */
static uint8_t       s_txbuf[HGIC_TX_MAX];
/* ★ **两个** cookie 计数器，按**通道**分开（2026-09-22 真机观察后的假设，见文件头 ★3）。
 *   以前是一个计数器同时喂 CMD（每 3 s 探测）与 FRM2（`idsend`）。*/
static hgic_cookie_t s_cookie_ctl;              /* 控制面：`CMD`/`CMD2` */
static hgic_cookie_t s_cookie_data;             /* 数据面：`FRM2` */
static hgic_rx_fn    s_cb;
static hgic_uart_stats_t s_st;

static void zero_stats(void)
{
    uint8_t *p = (uint8_t *)&s_st;
    size_t i;

    for (i = 0; i < sizeof(s_st); i++) { p[i] = 0u; }
}

/* ---------------- 中断侧：只入环 ---------------- */
static void hgic_rx_byte(uint8_t b)
{
    uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);

    /* 只在这个字段上累加：主循环不会写它 ⇒ 不会与主循环争同一份数据 */
    s_st.rx_bytes++;
    if (next == s_tail) {
        s_ring_drop++;                          /* 环满：丢这个字节并计数（**可见**） */
        return;
    }
    s_ring[s_head] = b;
    s_head = next;
}

/* ---------------- 帧层回调（**主循环上下文**） ---------------- */
static int on_frame(const hgic_hdr_t *h, const uint8_t *payload, size_t plen, void *ctx)
{
    (void)ctx;

    s_st.rx_frames++;
    if (h->type == HGIC_T_FRM || h->type == HGIC_T_FRM2) {
        s_st.rx_frm2++;
    } else if (h->type == HGIC_T_CMD || h->type == HGIC_T_CMD2 ||
               h->type == HGIC_T_EVENT || h->type == HGIC_T_EVENT2) {
        s_st.rx_ctrl++;
    }
    if (s_cb != NULL) {
        s_cb(h, payload, plen);                 /* 应用层决定怎么处理/打印 */
    }
    return 0;                                   /* 0 = 继续切缓冲里剩下的帧 */
}

void hgic_uart_init(hgic_rx_fn cb)
{
    s_head = 0u;
    s_tail = 0u;
    s_ring_drop = 0u;
    s_cb = cb;
    zero_stats();

    /* 固件**只收**模组→主机的帧（主机→模组那一半是回环自测才需要） */
    hgic_parser_init(&s_parser, HGIC_EXPECT_MODULE_TO_HOST);
    hgic_cookie_init(&s_cookie_ctl, 1u);        /* 两个通道各自从 1 开始逐帧 +1 */
    hgic_cookie_init(&s_cookie_data, 1u);

    uart_init(MODULE_UART, MODULE_GPIO_PORT, MODULE_TX_PIN, MODULE_RX_PIN,
              MODULE_BAUD, MODULE_UART_IRQn, hgic_rx_byte);
}

void hgic_uart_poll(void)
{
    uint8_t chunk[64];

    while (s_tail != s_head) {
        size_t n = 0u;

        while (s_tail != s_head && n < sizeof(chunk)) {
            chunk[n++] = s_ring[s_tail];
            s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
        }
        hgic_parser_feed(&s_parser, chunk, n, on_frame, NULL);
    }

    /* 解析器的三个"丢了东西"的计数只在 feed 里累加 ⇒ 每次搬过来（都**可见**） */
    s_st.garbage    = s_parser.garbage;
    s_st.bad_length = s_parser.bad_length;
    s_st.overrun    = s_parser.overrun;
}

int hgic_uart_send(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0u) {
        return HGIC_E_ARG;
    }
    uart_write(MODULE_UART, frame, (uint32_t)len);
    s_st.tx_frames++;
    return (int)len;
}

int hgic_uart_send_frm2(const uint8_t *eth, size_t ethlen)
{
    int n;

    if (eth == NULL || ethlen < 14u) {          /* 至少要有一个以太头 */
        return HGIC_E_ARG;
    }
    if (ethlen + HGIC_HDR_LEN > sizeof(s_txbuf)) {
        return HGIC_E_CAP;
    }
    n = hgic_frame_frm2(s_txbuf, sizeof(s_txbuf), eth, ethlen, hgic_cookie_next(&s_cookie_data));
    if (n < 0) {
        return n;
    }
    return hgic_uart_send(s_txbuf, (size_t)n);
}

int hgic_uart_send_cmd(uint16_t cmd_id, const void *params, size_t plen)
{
    uint8_t buf[HGIC_HDR_LEN + HGIC_CTRL_UNION + 8u];   /* 够放只读命令的少量参数 */
    int n;

    if (plen > sizeof(buf) - HGIC_HDR_LEN - HGIC_CTRL_UNION) {
        return HGIC_E_CAP;
    }
    n = hgic_frame_cmd(buf, sizeof(buf), cmd_id, params, plen, hgic_cookie_next(&s_cookie_ctl));
    if (n < 0) {
        return n;
    }
    return hgic_uart_send(buf, (size_t)n);
}

void hgic_uart_stats(hgic_uart_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    /* ★ **不要写 `*out = s_st`**：结构体赋值在 `-nostdlib` 下会变成对 `memcpy` 的调用，
     *   而固件没链 libc ⇒ `undefined reference to memcpy`（2026-09-20 实测踩到）。
     *   逐字段搬（字段就这几个，代价可忽略）。*/
    out->rx_bytes   = s_st.rx_bytes;
    out->rx_frames  = s_st.rx_frames;
    out->rx_frm2    = s_st.rx_frm2;
    out->rx_ctrl    = s_st.rx_ctrl;
    out->tx_frames  = s_st.tx_frames;
    out->garbage    = s_st.garbage;
    out->bad_length = s_st.bad_length;
    out->overrun    = s_st.overrun;
    out->ring_drop  = s_ring_drop;      /* 这一项只在**中断**里累加，单独读 */
}

uint8_t hgic_uart_last_cookie_ctl(void)
{
    return (uint8_t)(s_cookie_ctl.n & 0xFFu);
}

uint8_t hgic_uart_last_cookie_data(void)
{
    return (uint8_t)(s_cookie_data.n & 0xFFu);
}
