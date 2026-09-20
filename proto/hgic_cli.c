/* hgic_cli.c — HGIC 帧层的 **host 侧** CLI（对拍 / 调试用；**不编进固件**）
 *
 * 与 Python 参考实现（`../tools/txah_hgic.py`）对拍时，**只认 stdout 的固定格式**。
 * 输出一律 ASCII（十六进制小写），便于 Windows 控制台/管道直接读。
 *
 * 子命令：
 *   frm2 <eth-hex> [cookie]                  -> <frame-hex> / ERR <code>
 *   cmd  <cmd_id> [params-hex] [cookie]      -> <frame-hex> / ERR <code>
 *   hdr  <frame-hex>                         -> rc magic type ifidx flags length cookie
 *                                               from_module len_ok  （空格分隔；rc≠0 时只有 rc）
 *   feed <any|rx|tx> <stream-hex> [chunk]    -> frames<TAB>garbage<TAB>bad_length<TAB>overrun
 *   ctrl <type> <from 0|1> <payload-hex>     -> rc kind id status data-hex
 *   selfcheck                                -> C 侧内置不变量（不需要文件），末尾 PASS/FAIL
 *   frame-selftest <vector-file>             行格式见 proto/README.md（kind a1..a6 frame-hex）
 *   parse-selftest <vector-file>             行格式见 proto/README.md（expect stream chunk
 *                                            frames garbage bad_length overrun）
 *   ctrl-selftest  <vector-file>             行格式见 proto/README.md（type from payload rc kind
 *                                            id status data-hex）
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hgic.h"

#define LINE_MAX 65536          /* 一行向量可能很长：4096 B 的流 = 8192 个十六进制字符 */

static char    g_line[LINE_MAX];
static char    g_tmp[LINE_MAX];
static char    g_want[LINE_MAX];
static uint8_t g_bin[HGIC_MAX_FRAME];

static void usage(void)
{
    fprintf(stderr,
            "usage: hgic_cli <cmd> [args]\n"
            "  frm2 <eth-hex> [cookie]\n"
            "  cmd  <cmd_id> [params-hex] [cookie]\n"
            "  hdr  <frame-hex>\n"
            "  feed <any|rx|tx> <stream-hex> [chunk]\n"
            "  ctrl <type> <from 0|1> <payload-hex>\n"
            "  selfcheck\n"
            "  frame-selftest|parse-selftest|ctrl-selftest <vector-file>\n");
}

/* ------------------------------------------------------------------ */
/* 小工具（CLI 在 PC 上跑，可以用 string.h）                          */
/* ------------------------------------------------------------------ */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* 按 TAB 切最多 ncols 段（原地改串）；返回段数 */
static int split_tabs(char *s, char *cols[], int ncols)
{
    int n = 0;
    char *p = s;

    if (ncols <= 0) return 0;
    cols[n++] = p;
    while (*p != '\0') {
        if (*p == '\t') {
            *p = '\0';
            if (n >= ncols) return n;
            cols[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* 按 ':' 切最多 ncols 段（同 split_tabs 的规则） */
static int split_colons(char *s, char *cols[], int ncols)
{
    int n = 0;
    char *p = s;

    if (ncols <= 0) return 0;
    cols[n++] = p;
    while (*p != '\0') {
        if (*p == ':') {
            *p = '\0';
            if (n >= ncols) return n;
            cols[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 十六进制串 → 字节；返回字节数，-1 = 非法/超长。空串 = 0 字节。 */
static int hex_to_bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n, i;

    if (hex == NULL) return -1;
    n = strlen(hex);
    if (n % 2 != 0) return -1;
    if (n / 2 > cap) return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = hexval((unsigned char)hex[i * 2]);
        int lo = hexval((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

/* 字节 → 十六进制小写串（out 需 2n+1） */
static void bin_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char D[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = D[(b[i] >> 4) & 0x0F];
        out[i * 2 + 1] = D[b[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* "-" = 空/缺省（0 字节）；否则按十六进制解。返回字节数，-1 = 非法。 */
static int hex_or_dash(const char *s, uint8_t *out, size_t cap)
{
    if (s == NULL || strcmp(s, "-") == 0) return 0;
    return hex_to_bin(s, out, cap);
}

static unsigned long dec_or_0(const char *s)
{
    if (s == NULL || strcmp(s, "-") == 0) return 0;
    return strtoul(s, NULL, 10);
}

static const char *expect_name(uint8_t mode)
{
    switch (mode) {
    case HGIC_CTRL_REQ:  return "req";
    case HGIC_CTRL_RESP: return "resp";
    default:             return "-";
    }
}

/* ------------------------------------------------------------------ */
/* 帧构造 / 解析 / 流 / 控制面 —— 一处的实现，各子命令与自检共用      */
/* ------------------------------------------------------------------ */

/* 造帧：out 收十六进制串（需 8193 字节）。返回整帧长度，负 = 错误码，-100 = 未知 kind。 */
static int frame_build(const char *kind, char *a[6], char *out)
{
    uint8_t frame[HGIC_MAX_FRAME];
    int n, plen;

    if (strcmp(kind, "hdr") == 0) {
        plen = hex_or_dash(a[5], g_bin, sizeof(g_bin));
        if (plen < 0) return HGIC_E_ARG;
        n = hgic_build(frame, sizeof(frame),
                       (uint16_t)strtoul(a[0], NULL, 16),
                       (uint8_t)strtoul(a[1], NULL, 10),
                       g_bin, (size_t)plen,
                       (uint16_t)strtoul(a[4], NULL, 10),
                       (uint8_t)strtoul(a[2], NULL, 10),
                       (uint8_t)strtoul(a[3], NULL, 10));
    } else if (strcmp(kind, "frm2") == 0) {
        plen = hex_or_dash(a[0], g_bin, sizeof(g_bin));
        if (plen < 0) return HGIC_E_ARG;
        n = hgic_frame_frm2(frame, sizeof(frame), g_bin, (size_t)plen,
                            (uint16_t)strtoul(a[1], NULL, 10));
    } else if (strcmp(kind, "cmd") == 0) {
        plen = hex_or_dash(a[1], g_bin, sizeof(g_bin));
        if (plen < 0) return HGIC_E_ARG;
        n = hgic_frame_cmd(frame, sizeof(frame),
                           (uint16_t)strtoul(a[0], NULL, 10),
                           g_bin, (size_t)plen,
                           (uint16_t)strtoul(a[2], NULL, 10));
    } else {
        return -100;
    }
    if (n < 0) return n;
    bin_to_hex(frame, (size_t)n, out);
    return n;
}

typedef struct {
    char  *out;
    size_t cap;
    size_t used;
    int    count;
    int    overflow;
} frames_buf_t;

static int on_frame(const hgic_hdr_t *h, const uint8_t *payload, size_t plen, void *ctx)
{
    frames_buf_t *fb = (frames_buf_t *)ctx;
    char head[64];
    char hex[HGIC_MAX_FRAME * 2 + 1];
    size_t need;

    if (plen > 0) {
        bin_to_hex(payload, plen, hex);
    } else {
        hex[0] = '-';
        hex[1] = '\0';
    }
    snprintf(head, sizeof(head), "%s%d:%u:%u:%u:%u:%u:",
             (fb->count > 0 ? ";" : ""), (int)h->from_module, (unsigned)h->type,
             (unsigned)h->length, (unsigned)h->cookie, (unsigned)h->ifidx,
             (unsigned)h->flags);
    need = strlen(head) + strlen(hex);
    if (fb->used + need + 1u > fb->cap) {
        fb->overflow = 1;
        return 1;                       /* 装不下：这一帧先丢掉，停止本次解析 */
    }
    memcpy(fb->out + fb->used, head, strlen(head));
    fb->used += strlen(head);
    memcpy(fb->out + fb->used, hex, strlen(hex));
    fb->used += strlen(hex);
    fb->out[fb->used] = '\0';
    fb->count++;
    return 0;
}

static uint8_t expect_of(const char *s)
{
    if (strcmp(s, "rx") == 0) return (uint8_t)HGIC_EXPECT_MODULE_TO_HOST;
    if (strcmp(s, "tx") == 0) return (uint8_t)HGIC_EXPECT_HOST_TO_MODULE;
    return (uint8_t)HGIC_EXPECT_ANY;
}

/* 喂一段字节流：frames_out 收 `<from>:<type>:<len>:<cookie>:<ifidx>:<flags>:<payload>`
 * （`;` 分隔，无帧时为空串）。返回 0，参数非法返回 -1。 */
static int stream_parse(const char *expect_s, const char *stream_hex, int chunk,
                        char *frames_out, size_t cap, uint32_t *garbage,
                        uint32_t *bad_len, uint32_t *overrun)
{
    hgic_parser_t p;
    frames_buf_t fb;
    int n, i;

    n = hex_or_dash(stream_hex, g_bin, sizeof(g_bin));
    if (n < 0) return -1;

    hgic_parser_init(&p, (hgic_expect_t)expect_of(expect_s));
    fb.out = frames_out;
    fb.cap = cap;
    fb.used = 0;
    fb.count = 0;
    fb.overflow = 0;
    frames_out[0] = '\0';

    if (chunk <= 0) {
        hgic_parser_feed(&p, g_bin, (size_t)n, on_frame, &fb);
    } else {
        for (i = 0; i < n; i += chunk) {
            size_t take = (size_t)((n - i < chunk) ? n - i : chunk);
            hgic_parser_feed(&p, g_bin + i, take, on_frame, &fb);
        }
    }
    *garbage = p.garbage;
    *bad_len = p.bad_length;
    *overrun = p.overrun;
    return fb.overflow ? -2 : 0;
}

/* 控制面解码 → 一行 `<rc>\t<kind>\t<id>\t<status>\t<data-hex>` */
static int ctrl_line(uint8_t type, uint8_t from, const uint8_t *payload, size_t len,
                     char *out, size_t cap)
{
    hgic_hdr_t h;
    hgic_ctrl_t c;
    char hex[HGIC_MAX_FRAME * 2 + 1];
    int rc;

    memset(&h, 0, sizeof(h));
    h.magic = (uint16_t)(from ? HGIC_MAGIC_MODULE_TO_HOST : HGIC_MAGIC_HOST_TO_MODULE);
    h.type = type;
    h.from_module = (uint8_t)(from ? 1u : 0u);
    h.length = (uint16_t)(HGIC_HDR_LEN + len);

    rc = hgic_ctrl_parse(&h, payload, len, &c);
    if (rc != 0) {
        snprintf(out, cap, "%d\t-\t-\t-\t-", rc);
        return rc;
    }
    if (c.data_len > 0) {
        bin_to_hex(c.data, c.data_len, hex);
    } else {
        hex[0] = '-';
        hex[1] = '\0';
    }
    if (c.has_status) {
        snprintf(out, cap, "%d\t%s\t%u\t%u\t%s", rc, expect_name(c.kind),
                 (unsigned)c.id, (unsigned)c.status, hex);
    } else {
        snprintf(out, cap, "%d\t%s\t%u\t-\t%s", rc, expect_name(c.kind),
                 (unsigned)c.id, hex);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* 一次性调试子命令                                                   */
/* ------------------------------------------------------------------ */
static int cmd_frm2(const char *eth_hex, const char *cookie_s)
{
    char *a[6];
    int n;

    a[0] = (char *)eth_hex;
    a[1] = (char *)(cookie_s ? cookie_s : "0");
    a[2] = a[3] = a[4] = a[5] = (char *)"-";
    n = frame_build("frm2", a, g_tmp);
    if (n < 0) { printf("ERR %d\n", n); return 0; }
    printf("%s\n", g_tmp);
    return 0;
}

static int cmd_cmd(const char *id_s, const char *params_hex, const char *cookie_s)
{
    char *a[6];
    int n;

    a[0] = (char *)id_s;
    a[1] = (char *)(params_hex ? params_hex : "-");
    a[2] = (char *)(cookie_s ? cookie_s : "0");
    a[3] = a[4] = a[5] = (char *)"-";
    n = frame_build("cmd", a, g_tmp);
    if (n < 0) { printf("ERR %d\n", n); return 0; }
    printf("%s\n", g_tmp);
    return 0;
}

static int cmd_hdr(const char *hex)
{
    hgic_hdr_t h;
    int n, rc;

    n = hex_to_bin(hex, g_bin, sizeof(g_bin));
    if (n < 0) { printf("ERR -10\n"); return 0; }
    rc = hgic_parse_hdr(g_bin, (size_t)n, &h);
    if (rc != 0) { printf("%d\n", rc); return 0; }
    printf("%d %04x %u %u %u %u %u %u %d\n", rc, (unsigned)h.magic, (unsigned)h.type,
           (unsigned)h.ifidx, (unsigned)h.flags, (unsigned)h.length, (unsigned)h.cookie,
           (unsigned)h.from_module, hgic_len_ok(h.length));
    return 0;
}

static int cmd_feed(const char *expect_s, const char *stream_hex, int chunk)
{
    uint32_t g = 0, b = 0, o = 0;
    int rc = stream_parse(expect_s, stream_hex, chunk, g_tmp, sizeof(g_tmp), &g, &b, &o);

    if (rc == -1) { printf("ERR -10\n"); return 0; }
    printf("%s\t%u\t%u\t%u%s\n", (g_tmp[0] ? g_tmp : "-"), (unsigned)g, (unsigned)b,
           (unsigned)o, (rc == -2 ? "\t(截断)" : ""));
    return 0;
}

static int cmd_ctrl(const char *type_s, const char *from_s, const char *payload_hex)
{
    int n = hex_or_dash(payload_hex, g_bin, sizeof(g_bin));

    if (n < 0) { printf("ERR -10\n"); return 0; }
    ctrl_line((uint8_t)strtoul(type_s, NULL, 10), (uint8_t)strtoul(from_s, NULL, 10),
              g_bin, (size_t)n, g_tmp, sizeof(g_tmp));
    printf("%s\n", g_tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 向量文件自检（判定口径：末尾一行以 PASS 开头且全程无 FAIL）          */
/* ------------------------------------------------------------------ */
static int selftest(const char *path, const char *kind)
{
    FILE *fp = fopen(path, "r");
    int lineno = 0, nrow = 0, nbad = 0;

    if (fp == NULL) {
        fprintf(stderr, "打不开向量文件：%s\n", path);
        return 1;
    }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *cols[16];
        int n;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') continue;

        if (strcmp(kind, "frame") == 0) {
            int rc;
            n = split_tabs(g_line, cols, 8);
            nrow++;
            if (n != 8) { printf("FAIL %s:%d 列数=%d（要 8）\n", path, lineno, n); nbad++; continue; }
            rc = frame_build(cols[0], &cols[1], g_tmp);
            if (rc < 0) { printf("FAIL %s:%d 造帧 ERR %d\n", path, lineno, rc); nbad++; continue; }
            if (strcmp(g_tmp, cols[7]) != 0) {
                printf("FAIL %s:%d kind=%s\n     C=%s\n   FILE=%s\n",
                       path, lineno, cols[0], g_tmp, cols[7]);
                nbad++;
            }
        } else if (strcmp(kind, "parse") == 0) {
            uint32_t g = 0, b = 0, o = 0;
            char got[24];
            int rc;
            n = split_tabs(g_line, cols, 7);
            nrow++;
            if (n != 7) { printf("FAIL %s:%d 列数=%d（要 7）\n", path, lineno, n); nbad++; continue; }
            rc = stream_parse(cols[0], cols[1], (int)dec_or_0(cols[2]), g_tmp,
                              sizeof(g_tmp), &g, &b, &o);
            if (rc < 0) { printf("FAIL %s:%d 流解析 ERR %d\n", path, lineno, rc); nbad++; continue; }
            snprintf(got, sizeof(got), "%u\t%u\t%u", (unsigned)g, (unsigned)b, (unsigned)o);
            if (strcmp((g_tmp[0] ? g_tmp : "-"), cols[3]) != 0) {
                printf("FAIL %s:%d frames\n     C=%s\n   FILE=%s\n", path, lineno,
                       (g_tmp[0] ? g_tmp : "-"), cols[3]);
                nbad++;
            }
            {
                char want[24];
                snprintf(want, sizeof(want), "%s\t%s\t%s", cols[4], cols[5], cols[6]);
                if (strcmp(got, want) != 0) {
                    printf("FAIL %s:%d counters\n     C=%s\n   FILE=%s\n", path, lineno, got, want);
                    nbad++;
                }
            }
        } else {                                        /* ctrl */
            n = split_tabs(g_line, cols, 8);
            nrow++;
            if (n != 8) { printf("FAIL %s:%d 列数=%d（要 8）\n", path, lineno, n); nbad++; continue; }
            {
                int plen = hex_or_dash(cols[2], g_bin, sizeof(g_bin));
                if (plen < 0) { printf("FAIL %s:%d payload 十六进制非法\n", path, lineno); nbad++; continue; }
                ctrl_line((uint8_t)strtoul(cols[0], NULL, 10),
                          (uint8_t)strtoul(cols[1], NULL, 10),
                          g_bin, (size_t)plen, g_tmp, sizeof(g_tmp));
                snprintf(g_want, sizeof(g_want), "%s\t%s\t%s\t%s\t%s",
                         cols[3], cols[4], cols[5], cols[6], cols[7]);
                if (strcmp(g_tmp, g_want) != 0) {
                    printf("FAIL %s:%d ctrl\n     C=%s\n   FILE=%s\n", path, lineno, g_tmp, g_want);
                    nbad++;
                }
            }
        }
    }
    fclose(fp);
    if (nbad > 0) {
        printf("FAIL %d/%d 行不一致（%s）\n", nbad, nrow, path);
        return 2;
    }
    printf("PASS %d/%d 行（%s）\n", nrow, nrow, path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* selfcheck：C 侧内置不变量（不依赖向量文件）                        */
/* ------------------------------------------------------------------ */
static int sc_bad = 0;

static void sc(const char *name, int ok)
{
    if (!ok) {
        printf("FAIL %s\n", name);
        sc_bad++;
    }
}

static void selfcheck(void)
{
    static const uint8_t eth[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
                                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                  0x88, 0xB5, 0x01, 0x02, 0x03};
    static uint8_t big[HGIC_MAX_FRAME];      /* 当“超长载荷”的源：与输出缓冲**分开** */
    uint8_t frame[HGIC_MAX_FRAME];
    hgic_hdr_t h;
    hgic_cookie_t ck;
    hgic_ctrl_t ct;
    int n, i;

    /* ① 长度合法性边界 */
    sc("len_ok(7)=0", hgic_len_ok(7) == 0);
    sc("len_ok(8)=1", hgic_len_ok(8) == 1);
    sc("len_ok(4096)=1", hgic_len_ok(4096) == 1);
    sc("len_ok(4097)=0", hgic_len_ok(4097) == 0);

    /* ② hdr 往返：ifidx/flags 拆装、长度=8+载荷、magic 判定 from_module */
    n = hgic_build(frame, sizeof(frame), HGIC_MAGIC_HOST_TO_MODULE, HGIC_T_FRM2,
                   eth, sizeof(eth), 0x1234, 5, 10);
    sc("build 长度=8+载荷", n == (int)(HGIC_HDR_LEN + sizeof(eth)));
    sc("UART 上前两字节=2B 1A（小端）", frame[0] == 0x2B && frame[1] == 0x1A);
    sc("type=FRM2(9)", frame[2] == HGIC_T_FRM2);
    sc("ifidx:4|flags:4 打包", frame[3] == (uint8_t)((5u << 4) | 10u));
    sc("parse_hdr 成功", hgic_parse_hdr(frame, (size_t)n, &h) == 0);
    sc("往返 magic", h.magic == HGIC_MAGIC_HOST_TO_MODULE);
    sc("往返 cookie", h.cookie == 0x1234);
    sc("往返 ifidx/flags", h.ifidx == 5 && h.flags == 10);
    sc("from_module=0（主机→模组）", h.from_module == 0);
    sc("载荷长度 = 整帧 − 8", hgic_payload_len(&h) == sizeof(eth));
    for (i = 0; i < (int)sizeof(eth); i++) {
        if (frame[HGIC_HDR_LEN + (size_t)i] != eth[i]) break;
    }
    sc("载荷字节原样", i == (int)sizeof(eth));

    /* ③ 短输入 / 缓冲不足 / 超长 */
    sc("parse_hdr 7 字节 → E_SHORT", hgic_parse_hdr(frame, 7, &h) == HGIC_E_SHORT);
    sc("build cap 不够 → E_CAP",
       hgic_build(frame, 4, HGIC_MAGIC_HOST_TO_MODULE, HGIC_T_FRM2, eth, sizeof(eth), 0, 0, 0)
       == HGIC_E_CAP);
    sc("build 超 4096 → E_LEN",
       hgic_build(frame, sizeof(frame), HGIC_MAGIC_HOST_TO_MODULE, HGIC_T_FRM2,
                  big, HGIC_MAX_FRAME, 0, 0, 0) == HGIC_E_LEN);

    /* ④ 命令帧：union 必须补满（参数从偏移 12 起），id>255 走 CMD2 */
    n = hgic_frame_cmd(frame, sizeof(frame), 108, NULL, 0, 7);
    sc("cmd(108) 整帧=12", n == 12);
    sc("cmd(108) type=CMD(3)", frame[2] == HGIC_T_CMD);
    sc("cmd(108) id 后补 3 个 0", frame[8] == 108 && frame[9] == 0 && frame[10] == 0 && frame[11] == 0);
    n = hgic_frame_cmd(frame, sizeof(frame), 300, NULL, 0, 7);
    sc("cmd(300) type=CMD2(13)", frame[2] == HGIC_T_CMD2);
    sc("cmd(300) id 小端 u16", frame[8] == (300 & 0xFF) && frame[9] == (300 >> 8));
    n = hgic_frame_cmd(frame, sizeof(frame), 108, (const void *)eth, 2, 7);
    sc("cmd 带参：参数在偏移 12", n == 14 && frame[12] == eth[0] && frame[13] == eth[1]);

    /* ⑤ cookie：逐帧 +1，15 位回绕 */
    hgic_cookie_init(&ck, 1);
    sc("cookie 首帧=1", hgic_cookie_next(&ck) == 1);
    sc("cookie 次帧=2", hgic_cookie_next(&ck) == 2);
    hgic_cookie_init(&ck, 0x7FFF);
    sc("cookie 到 0x7FFF", hgic_cookie_next(&ck) == 0x7FFF);
    sc("cookie 回绕到 0", hgic_cookie_next(&ck) == 0);

    /* ⑥ 控制面：带 status 的应答 / 短应答 / 请求 */
    {
        static const uint8_t resp[] = {108, 0, 3, 0, 0xAA, 0xBB, 0xCC};
        hgic_hdr_t hr;
        memset(&hr, 0, sizeof(hr));
        hr.type = HGIC_T_CMD;
        hr.from_module = 1;
        sc("ctrl 带 status/len 的应答",
           hgic_ctrl_parse(&hr, resp, sizeof(resp), &ct) == 0 && ct.has_status == 1
           && ct.id == 108 && ct.status == 0 && ct.data_len == 3 && ct.data[0] == 0xAA);
        sc("ctrl 短应答（只有 id）",
           hgic_ctrl_parse(&hr, resp, 1, &ct) == 0 && ct.has_status == 0 && ct.id == 108);
        hr.from_module = 0;
        sc("ctrl 请求 kind=req",
           hgic_ctrl_parse(&hr, resp, sizeof(resp), &ct) == 0 && ct.kind == HGIC_CTRL_REQ);
        sc("ctrl 空载荷 → E_ARG", hgic_ctrl_parse(&hr, resp, 0, &ct) == HGIC_E_ARG);
    }

    /* ⑦ 流解析：一次切一帧 / 逐字节喂等价 / 前置杂音记 garbage / 假 magic 记 bad_length
     * ★ 小端易错点：字节 `2B 1A` = magic 0x1A2B（主机→模组），字节 `1A 2B` = 0x2B1A（模组→主机）。
     *   固件收到的是**后者**，下面用 `hgic_build(MODULE_TO_HOST, ...)` 造。 */
    {
        hgic_parser_t p;
        frames_buf_t fb;
        char buf[1024];
        uint8_t mh[HGIC_MAX_FRAME];          /* 模组→主机（解析器 expect=rx 收的就是它） */
        int n_mh = hgic_build(mh, sizeof(mh), HGIC_MAGIC_MODULE_TO_HOST, HGIC_T_FRM2,
                              eth, sizeof(eth), 3, 3, 4);
        int n_hw = hgic_frame_frm2(frame, sizeof(frame), eth, sizeof(eth), 3);

        sc("两个方向的帧都造出来了", n_mh > 0 && n_mh == n_hw);

        fb.out = buf; fb.cap = sizeof(buf); fb.used = 0; fb.count = 0; fb.overflow = 0;
        buf[0] = '\0';
        hgic_parser_init(&p, HGIC_EXPECT_MODULE_TO_HOST);
        hgic_parser_feed(&p, mh, (size_t)n_mh, on_frame, &fb);
        sc("流解析：模组→主机的帧被切出", fb.count == 1 && p.garbage == 0u);

        fb.used = 0; fb.count = 0; buf[0] = '\0';
        hgic_parser_init(&p, HGIC_EXPECT_MODULE_TO_HOST);
        for (i = 0; i < n_mh; i++) {
            hgic_parser_feed(&p, mh + i, 1u, on_frame, &fb);
        }
        sc("流解析：逐字节喂等价于一次喂", fb.count == 1 && p.garbage == 0u);

        fb.used = 0; fb.count = 0; buf[0] = '\0';
        hgic_parser_init(&p, HGIC_EXPECT_MODULE_TO_HOST);
        hgic_parser_feed(&p, (const uint8_t *)"\x01\x02\x03", 3u, on_frame, &fb);
        hgic_parser_feed(&p, mh, (size_t)n_mh, on_frame, &fb);
        sc("流解析：前置杂音记 garbage=3", fb.count == 1 && p.garbage == 3u);

        {
            /* 长度 7 的非法的“假 magic”帧（用模组→主机那个 magic，才与 expect=rx 相符） */
            static const uint8_t fake[8] = {0x1A, 0x2B, 0x09, 0x00, 0x07, 0x00, 0x01, 0x00};
            fb.used = 0; fb.count = 0; buf[0] = '\0';
            hgic_parser_init(&p, HGIC_EXPECT_MODULE_TO_HOST);
            hgic_parser_feed(&p, fake, sizeof(fake), on_frame, &fb);
            sc("流解析：长度 7 非法 → bad_length=1", fb.count == 0 && p.bad_length == 1u);
        }

        fb.used = 0; fb.count = 0; buf[0] = '\0';
        hgic_parser_init(&p, HGIC_EXPECT_MODULE_TO_HOST);
        hgic_parser_feed(&p, frame, (size_t)n_hw, on_frame, &fb);
        /* 全都不是本方向的 magic ⇒ 除了留住的最后 1 字节（可能是半个 magic）都算杂音 */
        sc("流解析：expect=rx 时**不收**主机→模组的帧",
           fb.count == 0 && p.garbage == (uint32_t)n_hw - 1u);
    }

    if (sc_bad == 0) {
        printf("PASS selfcheck（不变量全过）\n");
    } else {
        printf("FAIL selfcheck：%d 项不成立\n", sc_bad);
    }
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "frm2") == 0 && argc >= 3) {
        return cmd_frm2(argv[2], argc >= 4 ? argv[3] : NULL);
    }
    if (strcmp(argv[1], "cmd") == 0 && argc >= 3) {
        return cmd_cmd(argv[2], argc >= 4 ? argv[3] : NULL, argc >= 5 ? argv[4] : NULL);
    }
    if (strcmp(argv[1], "hdr") == 0 && argc >= 3) return cmd_hdr(argv[2]);
    if (strcmp(argv[1], "feed") == 0 && argc >= 4) {
        return cmd_feed(argv[2], argv[3], argc >= 5 ? (int)dec_or_0(argv[4]) : 0);
    }
    if (strcmp(argv[1], "ctrl") == 0 && argc >= 5) return cmd_ctrl(argv[2], argv[3], argv[4]);

    if (strcmp(argv[1], "selfcheck") == 0) {
        selfcheck();
        return (sc_bad == 0) ? 0 : 2;
    }
    if (strcmp(argv[1], "frame-selftest") == 0 && argc >= 3) return selftest(argv[2], "frame");
    if (strcmp(argv[1], "parse-selftest") == 0 && argc >= 3) return selftest(argv[2], "parse");
    if (strcmp(argv[1], "ctrl-selftest") == 0 && argc >= 3) return selftest(argv[2], "ctrl");

    usage();
    return 1;
}
