/* id_core.c — 已签 ID 上报任务（固件侧策略；流水线本体在 proto/id_build.c）。*/
#include "id_core.h"

#include "id_build.h"
#include "id_level.h"
#include "hgic_uart.h"
#include "uart.h"
#include "board.h"

/* 主循环的 1 ms 时基（`Core/main.c` 定义）。⚠ 只用来**量** build 耗时：
 * 不读它的话（比如拿 `idc_*` 的入参 `now_ms` 去减）差恒为 0 —— 实测踩过：
 * 输出里 `build_ms=0` 看着"很快"，而真机上 ES256 是**秒级**（假数字比没数字更坏）。*/
extern volatile uint32_t g_tick_ms;

/* ------------------------------------------------------------------ */
/* 静态状态（RAM 紧：一律 static，别放栈上）                              */
/* ------------------------------------------------------------------ */
static jcs_ctx_t s_jc;
static idb_t     s_b;
static char      s_rep[IDC_REPORT_CAP];
static char      s_env[IDC_ENV_CAP];
static char      s_frame[IDC_FRAME_CAP];

static uint32_t  s_boot_entropy;     /* 上电熵（`id` 里打出来，便于看撞没撞）*/
static char      s_mode[12];        /* §8.2 故障注入模式（auto/sign_fail/se_fail/no_key）*/
static uint32_t  s_next_due;        /* 下一个发送时刻（ms 时基）*/
static uint32_t  s_last_sent;       /* 上次真正发出的时刻*/
static uint8_t   s_have_sent;       /* 发过一次没有（第一次不受自限频约束）*/
static uint32_t  s_sent;            /* 真正发出去的条数*/
static uint32_t  s_held;            /* 被自限频**延后**的次数（不是丢弃）*/
static uint32_t  s_build_fail;      /* 组装失败次数*/
static uint32_t  s_tx_fail;         /* 交给模组失败次数*/
static int       s_ready;

static const char *s_last_why = "-";

/* ------------------------------------------------------------------ */
/* 工具（ASCII 输出；控制台代码页可能是 GBK）                            */
/* ------------------------------------------------------------------ */
static void print_hex_line(const char *p, size_t n)
{
    static const char H[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        char b[3];

        b[0] = H[((unsigned char)p[i] >> 4) & 0x0Fu];
        b[1] = H[(unsigned char)p[i] & 0x0Fu];
        b[2] = '\0';
        uart_printf(CONSOLE_UART, "%s", b);
        if ((i % 32u) == 31u) {
            uart_printf(CONSOLE_UART, "\r\n");
        }
    }
    uart_printf(CONSOLE_UART, "\r\n");
}

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

static int str_prefix(const char *s, const char *pre)
{
    while (*pre != '\0') {
        if (*s != *pre) { return 0; }
        s++; pre++;
    }
    return 1;
}

/* §8.2 的输入：本 demo 没有 SE ⇒ `se_ok` 由**软件 P-256 替身**顶着（见 id_core.h 的"如实"）。
 * d 步接上 ATECC608B 后：这里改成真的探测（I2C wake + Random 自检）即可，**其余一行不用动**。*/
static int _se_ok(void)
{
    return 1;
}

/* ------------------------------------------------------------------ */
/* 组装 + 发送                                                          */
/* ------------------------------------------------------------------ */
static int idc_build_and_send(uint32_t now_ms, const char *why)
{
    size_t flen = 0u;
    int level = -1;
    const char *reason = 0;
    uint32_t t0, dt;
    int rc, tx;

    t0 = now_ms;
    rc = idb_build(&s_b, s_mode, &flen, &level, &reason);
    /* ← 重读时基：`now_ms` 是入参、全程不变，拿它减自己恒得 0（实测踩过这个假测量）。
     * `dt == 0` 就是"不足 1 ms"（比如 level=3 不签名），**不要兜底成 1**。*/
    dt = g_tick_ms - t0;
    if (rc != 0) {
        s_build_fail++;
        uart_printf(CONSOLE_UART, "\r\n[id] build FAILED err=%d idr_rc=%d mode=%s\r\n",
                    s_b.last_err, s_b.last_idr_rc, s_mode);
        return 0;
    }
    tx = hgic_uart_send_frm2((const uint8_t *)s_frame, flen);
    if (tx < 0) {
        s_tx_fail++;
        uart_printf(CONSOLE_UART, "\r\n[id] tx FAILED rc=%d (frame %u B)\r\n",
                    tx, (unsigned)flen);
        return 0;
    }
    s_sent++;
    s_last_sent = now_ms;
    s_have_sent = 1u;
    s_last_why = reason;
    uart_printf(CONSOLE_UART, "\r\n[id] sent level=%d alg=%s why=%s nonce=%s len=%u"
                              " build_ms=%u (%s)\r\n",
                level, (level == 0) ? "ES256" : (level == 3) ? "none" : "HS256",
                reason, s_b.last_nonce, (unsigned)flen, (unsigned)dt, why);
    return 1;
}

int idc_tick(uint32_t now_ms)
{
    if (!s_ready) {
        return 0;
    }
    /* 环绕安全：用"差距"判断，不直接比大小 */
    if ((int32_t)(now_ms - s_next_due) < 0) {
        return 0;
    }
    s_next_due = now_ms + IDC_INTERVAL_MS;
    return idc_build_and_send(now_ms, "periodic");
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */
int idc_init(uint32_t boot_entropy)
{
    int rc;

    s_boot_entropy = boot_entropy;
    rc = idb_init(&s_b, &s_jc, s_rep, sizeof(s_rep), s_env, sizeof(s_env),
                  s_frame, sizeof(s_frame), IDC_SN_DEFAULT, IDC_GEN_DEFAULT,
                  IDC_FW_VERSION, 0, boot_entropy);
    if (rc != 0) {
        uart_printf(CONSOLE_UART, "[id] init FAILED rc=%d\r\n", rc);
        return rc;
    }
    /* 默认 auto（= §8.2 全正常 → L0）；故障注入靠控制台 `idlevel` */
    {
        const char *m = idl_mode_name(0);

        s_mode[0] = (m != 0) ? m[0] : 'a';
        {
            int i = 1;

            while (m != 0 && m[i] != '\0' && i < (int)sizeof(s_mode) - 1) {
                s_mode[i] = m[i];
                i++;
            }
            s_mode[i] = '\0';
        }
    }
    s_next_due = IDC_INTERVAL_MS;      /* 上电后先走完一个周期再发第一条（给人看横幅的时间）*/
    s_last_sent = 0u;
    s_have_sent = 0u;
    s_ready = 1;
    (void)_se_ok();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 控制台                                                              */
/* ------------------------------------------------------------------ */
static void cmd_status(void)
{
    uart_printf(CONSOLE_UART, "\r\n[id] sn=%s gen=%d fw=%s\r\n",
                s_b.sn, s_b.gen, (s_b.firmware != 0) ? s_b.firmware : "-");
    uart_printf(CONSOLE_UART, "[id] se=%s (demo: software key; real SE in d-step)\r\n",
                _se_ok() ? "stand-in(software P-256)" : "unavailable");
    uart_printf(CONSOLE_UART, "[id] mode=%s  interval=%u ms  min_gap=%u ms\r\n",
                s_mode, (unsigned)IDC_INTERVAL_MS, (unsigned)IDC_MIN_GAP_MS);
    uart_printf(CONSOLE_UART, "[id] last_level=%d why=%s nonce=%s frame_len=%u\r\n",
                s_b.last_level, (s_last_why != 0) ? s_last_why : "-",
                (s_b.last_nonce[0] != '\0') ? s_b.last_nonce : "-",
                (unsigned)s_b.last_frame_len);
    uart_printf(CONSOLE_UART, "[id] built=%u sent=%u held=%u build_fail=%u tx_fail=%u"
                              " fell_back=%u\r\n",
                (unsigned)s_b.built, (unsigned)s_sent, (unsigned)s_held,
                (unsigned)s_build_fail, (unsigned)s_tx_fail, (unsigned)s_b.fell_back);
    /* ⚠ 如实：软熵后端只保证"同一 boot 内不重复"。**跨 boot 若上电熵撞了，整段 nonce 会重**
     *   ⇒ 服务端按重放丢弃（= 漏报）。把这个值打出来，好让人一眼看出撞了没。*/
    uart_printf(CONSOLE_UART, "[id] boot_entropy=0x%08x (soft; 跨 boot 撞了整段 nonce 会重)\r\n",
                (unsigned)s_boot_entropy);
    uart_printf(CONSOLE_UART, "[id] ts=0 cap.rtc=false (no RTC); battery_mv not written\r\n");
}

static void cmd_modes(void)
{
    int i;

    uart_printf(CONSOLE_UART, "[id] modes(§8.2 故障注入 -> 级别): ");
    for (i = 0; i < IDL_MODE_COUNT; i++) {
        int se = 1, sg = 1, hm = 1;
        const char *why = "-";

        (void)idl_mode_inputs(idl_mode_name(i), &se, &sg, &hm);
        uart_printf(CONSOLE_UART, "%s->L%d ", idl_mode_name(i), idl_pick(se, sg, hm, &why));
    }
    uart_printf(CONSOLE_UART, "\r\n");
}

int idc_console(const char *cmd, uint32_t now_ms)
{
    if (str_eq(cmd, "id")) {
        cmd_status();
        return 1;
    }
    if (str_eq(cmd, "idsend")) {
        if (s_have_sent && (uint32_t)(now_ms - s_last_sent) < IDC_MIN_GAP_MS) {
            /* §5.8 设备侧：**延后，不丢弃**（丢自己的业务报 = 漏报）*/
            s_held++;
            uart_printf(CONSOLE_UART, "\r\n[id] held (self-limit): next in %u ms\r\n",
                        (unsigned)(IDC_MIN_GAP_MS - (now_ms - s_last_sent)));
            return 1;
        }
        if (!idc_build_and_send(now_ms, "console")) {
            uart_printf(CONSOLE_UART, "[id] send FAILED (see above)\r\n");
        }
        return 1;
    }
    if (str_eq(cmd, "idhex")) {
        if (s_b.last_frame_len == 0u) {
            uart_printf(CONSOLE_UART, "\r\n[id] no frame yet (try idsend)\r\n");
            return 1;
        }
        uart_printf(CONSOLE_UART, "\r\n[id] last frame %u B (paste into"
                                  " tools/check_report_hex.py on the PC):\r\n",
                    (unsigned)s_b.last_frame_len);
        print_hex_line(s_frame, (size_t)s_b.last_frame_len);
        return 1;
    }
    if (str_eq(cmd, "idmodes")) {
        cmd_modes();
        return 1;
    }
    if (str_prefix(cmd, "idlevel")) {
        int se = 1, sg = 1, hm = 1;
        const char *why = "-";
        const char *arg = cmd + 7;
        int lvl;

        while (*arg == ' ') { arg++; }
        if (*arg == '\0') {
            cmd_modes();
            return 1;
        }
        if (idl_mode_inputs(arg, &se, &sg, &hm) != 0) {
            uart_printf(CONSOLE_UART, "\r\n[id] unknown mode: %s (idmodes)\r\n", arg);
            return 1;
        }
        /* 存模式名（照抄上游 LEVEL_MODES 的那几个字符串）*/
        {
            int i = 0;

            while (arg[i] != '\0' && i < (int)sizeof(s_mode) - 1) {
                s_mode[i] = arg[i];
                i++;
            }
            s_mode[i] = '\0';
        }
        lvl = idl_pick(se, sg, hm, &why);
        uart_printf(CONSOLE_UART, "\r\n[id] mode=%s -> L%d (%s)\r\n", s_mode, lvl, why);
        return 1;
    }
    return 0;
}
