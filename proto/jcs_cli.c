/* jcs_cli.c — JCS / b64url 的 **host 侧** CLI（对拍/调试用；不编进固件）
 *
 * 子命令：
 *   cases                              -> 打印内置用例名（每行一个）
 *   jcs <case>                         -> 该用例的 JCS 字节（hex）
 *   jcs-selftest <vector-file>         行格式：<case><TAB><jcs-hex>
 *   b64url <hex>                       -> base64url（无填充）
 *   b64url-selftest <vector-file>      行格式：<hex><TAB><b64url>
 *   selfcheck                          -> 不依赖文件的冒烟自检（末尾 PASS/FAIL）
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 *
 * ★ 内置用例的**期望值一律来自 Python 参考实现**（`run_cross_test.py --refresh` 生成的
 *   向量文件），这里只允许写"结构"，不允许写"我以为的字节"。
 *   踩过的坑见 proto/README.md（曾把 Damm32 的黄金值 B 当成 Luhn32 的期望值）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "jcs.h"
#include "b64url.h"
#include "msg.h"
#include "downlink.h"
#include "id_report.h"

#define HEXOUT_MAX 4096
#define LINE_MAX   6000

/* ------------------------------------------------------------------ */
/* 内置用例（名字必须与 run_cross_test.py 里的 JCS_CASES 一一对应）        */
/* ------------------------------------------------------------------ */
static const char *CASE_NAMES[] = {
    "minimal", "empty_arr", "flat_int", "big_int", "strings_basic",
    "strings_escape", "unicode_utf8", "nested", "arr_mixed", "bool_null",
    "key_order_case", "report_shape",
};
#define N_CASES ((int)(sizeof(CASE_NAMES) / sizeof(CASE_NAMES[0])))

static void seen_routers(jcs_ctx_t *c, jv_t *payload)
{
    /* 与向量里的固定两条一致（真机上由扫描结果填充，见 c3） */
    jv_t *arr = jcs_arr(c);
    jv_t *r1 = jcs_obj(c);
    jv_t *r2 = jcs_obj(c);

    jcs_put_str(c, r1, "bssid", "AA:BB:CC:DD:EE:FF");
    jcs_put_str(c, r1, "ssid", "ORPAHID_ZONE_A");
    jcs_put_int(c, r1, "rssi", -42);
    jcs_add(c, arr, r1);

    jcs_put_str(c, r2, "bssid", "11:22:33:44:55:66");
    jcs_put_str(c, r2, "ssid", "ORPAHID_ZONE_B");
    jcs_put_int(c, r2, "rssi", -71);
    jcs_add(c, arr, r2);

    jcs_put(c, payload, "seen_routers", arr);
}

/* 返回根节点；名字不认识返回 NULL */
static jv_t *build_case(jcs_ctx_t *c, const char *name)
{
    if (strcmp(name, "minimal") == 0) {
        return jcs_obj(c);
    }
    if (strcmp(name, "empty_arr") == 0) {
        return jcs_arr(c);
    }
    if (strcmp(name, "flat_int") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_int(c, o, "a", 1);
        jcs_put_int(c, o, "b", -2);
        jcs_put_int(c, o, "c", 0);
        return o;
    }
    if (strcmp(name, "big_int") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_int(c, o, "ts", 1789879939LL);
        jcs_put_int(c, o, "neg", -9223372036854775807LL - 1LL);   /* INT64_MIN */
        return o;
    }
    if (strcmp(name, "strings_basic") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_str(c, o, "b", "hello");
        jcs_put_str(c, o, "a", "world");
        return o;
    }
    if (strcmp(name, "strings_escape") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_str(c, o, "q", "a\"b");
        jcs_put_str(c, o, "bs", "c\\d");
        jcs_put_str(c, o, "nl", "e\nf");
        jcs_put_str(c, o, "tab", "g\th");
        jcs_put_str(c, o, "ctl", "\x01x");
        return o;
    }
    if (strcmp(name, "unicode_utf8") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_str(c, o, "s", "\xe4\xb8\xad\xe6\x96\x87");   /* 中文（UTF-8 原样） */
        jcs_put_str(c, o, "e", "\xc3\xa9");                     /* é */
        return o;
    }
    if (strcmp(name, "nested") == 0) {
        jv_t *o = jcs_obj(c);
        jv_t *mid = jcs_obj(c);
        jv_t *inner = jcs_obj(c);
        jcs_put_int(c, inner, "y", 2);
        jcs_put_int(c, inner, "x", 3);
        jcs_put(c, mid, "a", inner);
        jcs_put_int(c, mid, "z", 1);
        jcs_put(c, o, "o", mid);
        return o;
    }
    if (strcmp(name, "arr_mixed") == 0) {
        /* {"a":[1,"x",true,null,{"k":2}]}
         * 数组元素没有键 ⇒ 用标量构造器 jcs_int/jcs_str/jcs_bool/jcs_null 直接造节点 */
        jv_t *o = jcs_obj(c);
        jv_t *arr = jcs_arr(c);
        jv_t *sub = jcs_obj(c);

        jcs_put_int(c, sub, "k", 2);
        jcs_add(c, arr, jcs_int(c, 1));
        jcs_add(c, arr, jcs_str(c, "x"));
        jcs_add(c, arr, jcs_bool(c, 1));
        jcs_add(c, arr, jcs_null(c));
        jcs_add(c, arr, sub);
        jcs_put(c, o, "a", arr);
        return o;
    }
    if (strcmp(name, "bool_null") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_bool(c, o, "t", 1);
        jcs_put_bool(c, o, "f", 0);
        jcs_put_null(c, o, "n");
        return o;
    }
    if (strcmp(name, "key_order_case") == 0) {
        jv_t *o = jcs_obj(c);
        jcs_put_int(c, o, "Z", 1);
        jcs_put_int(c, o, "a", 2);
        jcs_put_int(c, o, "A", 3);
        jcs_put_int(c, o, "1", 4);
        jcs_put_int(c, o, "_", 5);
        return o;
    }
    if (strcmp(name, "report_shape") == 0) {
        jv_t *hdr = jcs_obj(c);
        jv_t *payload = jcs_obj(c);
        jv_t *cap = jcs_obj(c);

        jcs_put_str(c, hdr, "typ", "orpah-id-report");
        jcs_put_int(c, hdr, "ver", 1);
        jcs_put_str(c, hdr, "alg", "ES256");
        jcs_put_int(c, hdr, "level", 0);

        jcs_put_str(c, payload, "sn", "CN-WH01-9AF3C1D2");
        jcs_put_int(c, payload, "ts", 0);
        jcs_put_str(c, payload, "nonce", "3F9A8B2C1D4E5F6A7B8C9D0E1F2A3B4C");
        seen_routers(c, payload);
        jcs_put_bool(c, cap, "rtc", 0);
        jcs_put(c, payload, "cap", cap);
        jcs_put_int(c, payload, "battery_mv", 3900);
        jcs_put_str(c, payload, "firmware", "c1-bench");

        /* 返回 {"hdr":…,"payload":…} 的**根**（这就是 preimage 的形状） */
        {
            jv_t *root = jcs_obj(c);
            jcs_put(c, root, "hdr", hdr);
            jcs_put(c, root, "payload", payload);
            return root;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* hex 工具                                                           */
/* ------------------------------------------------------------------ */
static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t to_hex(const unsigned char *in, size_t n, char *out, size_t cap)
{
    static const char H[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (o + 2 >= cap) return 0;
        out[o++] = H[(in[i] >> 4) & 0xF];
        out[o++] = H[in[i] & 0xF];
    }
    out[o] = '\0';
    return o;
}

static long from_hex(const char *s, unsigned char *out, size_t cap)
{
    size_t n = strlen(s), o = 0;
    if (n % 2 != 0) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_val(s[i]), lo = hex_val(s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        if (o >= cap) return -1;
        out[o++] = (unsigned char)((hi << 4) | lo);
    }
    return (long)o;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

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

/* ------------------------------------------------------------------ */
/* 子命令                                                             */
/* ------------------------------------------------------------------ */
static int cmd_jcs(const char *name)
{
    jcs_ctx_t c;
    char hex[HEXOUT_MAX];
    char buf[HEXOUT_MAX / 2];
    jv_t *root;
    int n;

    jcs_init(&c);
    root = build_case(&c, name);
    if (root == NULL) { printf("ERR unknown-case\n"); return 0; }
    n = jcs_encode(&c, root, buf, sizeof(buf));
    if (n < 0) { printf("ERR jcs-%d\n", n); return 0; }
    to_hex((const unsigned char *)buf, (size_t)n, hex, sizeof(hex));
    printf("%s\n", hex);
    return 0;
}

static int cmd_jcs_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[2];
        jcs_ctx_t c;
        char hex[HEXOUT_MAX], buf[HEXOUT_MAX / 2];
        jv_t *root;
        int n;

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (split_tabs(line, cols, 2) != 2) {
            printf("FAIL line %d: 需要 2 列（case<TAB>hex）\n", lineno);
            total++; bad++;
            continue;
        }
        total++;
        jcs_init(&c);
        root = build_case(&c, cols[0]);
        if (root == NULL) {
            printf("FAIL line %d: C 侧没有用例 %s\n", lineno, cols[0]);
            bad++;
            continue;
        }
        n = jcs_encode(&c, root, buf, sizeof(buf));
        if (n < 0) {
            printf("FAIL line %d: jcs_encode(%s) 错误 %d\n", lineno, cols[0], n);
            bad++;
            continue;
        }
        to_hex((const unsigned char *)buf, (size_t)n, hex, sizeof(hex));
        if (strcmp(hex, cols[1]) != 0) {
            printf("FAIL line %d: %s\n  C =%s\n  PY=%s\n", lineno, cols[0], hex, cols[1]);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

static int cmd_b64url_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[2];
        unsigned char raw[HEXOUT_MAX / 2];
        char out[HEXOUT_MAX];
        long n;
        int m;

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (split_tabs(line, cols, 2) != 2) {
            printf("FAIL line %d: 需要 2 列（hex<TAB>b64url）\n", lineno);
            total++; bad++;
            continue;
        }
        total++;
        n = from_hex(cols[0], raw, sizeof(raw));
        if (n < 0) {
            printf("FAIL line %d: hex 非法：%s\n", lineno, cols[0]);
            bad++;
            continue;
        }
        m = b64url_encode(raw, (size_t)n, out, sizeof(out));
        if (m < 0) {
            printf("FAIL line %d: b64url_encode 出错\n", lineno);
            bad++;
            continue;
        }
        out[m] = '\0';
        if (strcmp(out, cols[1]) != 0) {
            printf("FAIL line %d: hex=%s\n  C =%s\n  PY=%s\n", lineno, cols[0], out, cols[1]);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

static int cmd_selfcheck(void)
{
    jcs_ctx_t c;
    char buf[512];
    int fails = 0, total = 0, n;

    /* 只做"形状/错误码"级别的冒烟检查；字节级正确性一律靠向量文件 */
    total++;
    jcs_init(&c);
    {
        jv_t *o = jcs_obj(&c);
        jcs_put_int(&c, o, "b", 1);
        jcs_put_int(&c, o, "a", 2);
        n = jcs_encode(&c, o, buf, sizeof(buf));
        buf[n > 0 ? n : 0] = '\0';
        if (n <= 0 || strcmp(buf, "{\"a\":2,\"b\":1}") != 0) {
            printf("FAIL 键序：got %s\n", n > 0 ? buf : "(err)");
            fails++;
        }
    }
    total++;
    jcs_init(&c);
    {
        jv_t *o = jcs_obj(&c);
        int rc = jcs_put_int(&c, o, "a", 1);
        rc = jcs_put_int(&c, o, "a", 1);           /* 重复键必须报错 */
        if (rc != JCS_E_DUPKEY) {
            printf("FAIL 重复键应返回 JCS_E_DUPKEY，实得 %d\n", rc);
            fails++;
        }
    }
    total++;
    {
        unsigned char in[3] = { 0xFF, 0xFF, 0xFF };
        char out[8];
        int m = b64url_encode(in, 3, out, sizeof(out));
        out[m > 0 ? m : 0] = '\0';
        if (m != 4 || strcmp(out, "____") != 0) {   /* 0xFFFFFF → "____" */
            printf("FAIL b64url(ffffff) = %s（期望 ____）\n", m > 0 ? out : "(err)");
            fails++;
        }
    }
    total++;
    {
        unsigned char in[1] = { 0xFB };
        char out[8];
        int m = b64url_encode(in, 1, out, sizeof(out));
        out[m > 0 ? m : 0] = '\0';
        if (m != 2 || strcmp(out, "-w") != 0) {     /* 0xFB → "-w"（字母表含 -_） */
            printf("FAIL b64url(fb) = %s（期望 -w）\n", m > 0 ? out : "(err)");
            fails++;
        }
    }

    printf("%s %d/%d\n", fails ? "FAIL" : "PASS", total - fails, total);
    return fails ? 2 : 0;
}

/* ------------------------------------------------------------------ */
/* 链路报文（信封：**插入序**，与 run_cross_test.py 的 MSG_CASES 一一对应）*/
/* ------------------------------------------------------------------ */
#define ID_STUB_NONCE "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"   /* 32 hex = 16B，与 Python 镜像一致 */
#define ID_STUB_SIG   "STUB"

/* 内层已签报文的 **stub**（id-report 外壳用例用；不是真签名） */
static jv_t *id_stub(jcs_ctx_t *c, const char *sn)
{
    jv_t *rep = jcs_obj(c);
    jv_t *hdr = jcs_obj(c);
    jv_t *payload = jcs_obj(c);
    jv_t *seen = jcs_arr(c);

    if (rep == NULL || hdr == NULL || payload == NULL || seen == NULL) return NULL;
    jcs_put_str(c, hdr, "typ", "orpah-id-report");
    jcs_put_int(c, hdr, "ver", 1);
    jcs_put_str(c, hdr, "alg", "ES256");
    jcs_put_int(c, hdr, "level", 0);

    jcs_put_str(c, payload, "sn", sn);
    jcs_put_int(c, payload, "ts", 0);
    jcs_put_str(c, payload, "nonce", ID_STUB_NONCE);
    jcs_put(c, payload, "seen_routers", seen);

    jcs_put(c, rep, "hdr", hdr);
    jcs_put(c, rep, "payload", payload);
    jcs_put_str(c, rep, "sig", ID_STUB_SIG);
    return rep;
}

/* "-"（**整字段**）表示“不给”
 * ⚠ 不能用 s[0]=='-' 判断 —— RSSI 本来就是负数（-55 开头就是 '-'），
 *   ts/seq 将来也可能出现负值。这个坑实打过：4/8 用例挂在这里。 */
static int is_absent(const char *s)
{
    return (s == NULL || s[0] == '\0' || strcmp(s, "-") == 0);
}

static long long parse_ll(const char *s, long long dflt)
{
    if (is_absent(s)) return dflt;
    return strtoll(s, NULL, 10);
}

/* 按 7 列规格（kind + 5 参数 + hex）构造并编码；失败返回 NULL */
static const char *msg_case_hex(const char *kind, const char *a1, const char *a2,
                                const char *a3, const char *a4, const char *a5,
                                char *out, size_t outcap)
{
    static jcs_ctx_t c;            /* 单线程 CLI：静态避免把 arena 放栈上 */
    static char buf[4096];
    jv_t *m = NULL;
    int n;

    jcs_init(&c);

    if (strcmp(kind, "req-connect") == 0) {
        /* CLI 里的 "-" = 不给 ⇒ 交给构造器前先换成 NULL（构造器只认 NULL/空串） */
        m = msg_req_connect(&c, a1, parse_ll(a2, 0),
                            is_absent(a3) ? NULL : a3,
                            is_absent(a4) ? NULL : a4);
    } else if (strcmp(kind, "report") == 0) {
        int cap = MSG_CAP_NONE;
        int rssi_set = 0, rssi = 0;
        if (!is_absent(a4)) cap = (a4[0] == '1') ? MSG_CAP_TRUE : MSG_CAP_FALSE;
        if (!is_absent(a5)) { rssi_set = 1; rssi = (int)parse_ll(a5, 0); }
        m = msg_report(&c, a1, parse_ll(a2, 0), (int)parse_ll(a3, 1), cap, rssi_set, rssi);
    } else if (strcmp(kind, "id-report") == 0) {
        jv_t *stub = id_stub(&c, a1);
        if (stub != NULL) m = msg_id_report(&c, stub, a1, parse_ll(a2, 0));
    } else {
        return NULL;
    }
    if (m == NULL) return NULL;
    n = jcs_encode_raw(m, buf, sizeof(buf));      /* ★ 信封：不排序 */
    if (n < 0) return NULL;
    to_hex((const unsigned char *)buf, (size_t)n, out, outcap);
    return out;
}

static int cmd_msg(const char *kind, char **av, int n)
{
    char hex[HEXOUT_MAX];
    const char *s;

    if (n < 5) { fprintf(stderr, "msg 需要 5 个参数\n"); return 1; }
    s = msg_case_hex(kind, av[0], av[1], av[2], av[3], av[4], hex, sizeof(hex));
    if (s == NULL) { printf("ERR msg-case\n"); return 0; }
    printf("%s\n", s);
    return 0;
}

static int cmd_msg_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[7];
        char hex[HEXOUT_MAX];

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (split_tabs(line, cols, 7) != 7) {
            printf("FAIL line %d: 需要 7 列（kind + 5 参数 + hex）\n", lineno);
            total++; bad++;
            continue;
        }
        total++;
        if (msg_case_hex(cols[0], cols[1], cols[2], cols[3], cols[4], cols[5],
                         hex, sizeof(hex)) == NULL) {
            printf("FAIL line %d: 构造失败（kind=%s）\n", lineno, cols[0]);
            bad++;
            continue;
        }
        if (strcmp(hex, cols[6]) != 0) {
            printf("FAIL line %d: %s\n  C =%s\n  PY=%s\n", lineno, cols[0], hex, cols[6]);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

/* ------------------------------------------------------------------ */
/* 下行解码（与 run_cross_test.py 的 DL_CASES 一一对应）                  */
/* ------------------------------------------------------------------ */
/* 输出 7 个 TAB 分隔的字段（= 向量文件第 2..8 列）：
 *   valid<TAB>type<TAB>sn<TAB>ts<TAB>tracked<TAB>status<TAB>code */
static void dl_report_line(const char *json_hex, char *out, size_t outcap)
{
    static jcs_ctx_t c;
    unsigned char raw[HEXOUT_MAX / 2];
    jv_t *msg = NULL;
    long n;
    int tv = 0;
    long long ts = 0;
    const char *type_s, *sn_s, *status_s, *code_s;
    char tsbuf[32];
    const char *ts_s;

    jcs_init(&c);
    n = from_hex(json_hex, raw, sizeof(raw));
    if (n < 0 || dl_decode(&c, (const char *)raw, (size_t)n, &msg) != 0) {
        snprintf(out, outcap, "0\t-\t-\t-\t-\t-\t-");
        return;
    }
    type_s = dl_type(msg);
    sn_s = dl_str(msg, "sn");
    status_s = dl_str(msg, "status");
    code_s = dl_str(msg, "code");
    if (dl_int(msg, "ts", &ts)) {
        snprintf(tsbuf, sizeof(tsbuf), "%lld", ts);
        ts_s = tsbuf;
    } else {
        ts_s = "-";
    }
    snprintf(out, outcap, "1\t%s\t%s\t%s\t%s\t%s\t%s",
             type_s ? type_s : "-", sn_s ? sn_s : "-", ts_s,
             dl_truthy(msg, "tracked", &tv) ? (tv ? "1" : "0") : "-",
             status_s ? status_s : "-", code_s ? code_s : "-");
}

static int cmd_dl(const char *json_hex)
{
    char out[512];
    dl_report_line(json_hex, out, sizeof(out));
    printf("%s\n", out);
    return 0;
}

static int cmd_dl_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[8];
        char got[512], exp[512];

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (split_tabs(line, cols, 8) != 8) {
            printf("FAIL line %d: 需要 8 列（json-hex + 7 个期望字段）\n", lineno);
            total++; bad++;
            continue;
        }
        total++;
        dl_report_line(cols[0], got, sizeof(got));
        snprintf(exp, sizeof(exp), "%s\t%s\t%s\t%s\t%s\t%s\t%s",
                 cols[1], cols[2], cols[3], cols[4], cols[5], cols[6], cols[7]);
        if (strcmp(got, exp) != 0) {
            printf("FAIL line %d:\n  C =%s\n  PY=%s\n", lineno, got, exp);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

/* ------------------------------------------------------------------ */
/* 设备侧已签报文（id-report）：与 run_cross_test.py 的 IDR_CASES 一一对应  */
/* ------------------------------------------------------------------ */
/* 输出：<报文-hex><TAB><信封-hex>。
 * 参数固定 8 个：sn ts nonce level key-hex battery|- caprtc|- firmware|-
 *   ⚠ `key-hex` 的含义**按 level 分**（列的个数不变，免得改向量格式）：
 *     level 0   = **P-256 私钥 d**（32 B 大端 hex）——ES256 走 RFC 6979
 *     level 1/2 = **HMAC 降级密钥**（32 B hex）
 *     level 3   = 用不上（写 -）*/
static void idr_line(char **av, char *out, size_t outcap)
{
    static jcs_ctx_t c;
    static char repbuf[2048];
    static char envbuf[2048];
    unsigned char key[64];
    jv_t *rep_node = NULL;
    long kn;
    size_t rn = 0;
    int rc, en;
    int level = (int)parse_ll(av[3], -1);
    int cap = MSG_CAP_NONE;
    int batt_set = 0, batt = 0;

    if (!is_absent(av[6])) cap = (av[6][0] == '1') ? MSG_CAP_TRUE : MSG_CAP_FALSE;
    if (!is_absent(av[5])) { batt_set = 1; batt = (int)parse_ll(av[5], 0); }
    kn = from_hex(is_absent(av[4]) ? "" : av[4], key, sizeof(key));
    if (kn < 0) { snprintf(out, outcap, "ERR key-hex"); return; }

    jcs_init(&c);
    rc = idr_build(&c, av[0], parse_ll(av[1], 0), av[2], level,
                   cap, batt_set, batt, is_absent(av[7]) ? NULL : av[7],
                   NULL,
                   (level == 0) ? NULL : key, (level == 0) ? 0 : (size_t)kn,
                   (level == 0) ? key : NULL, (level == 0) ? (size_t)kn : 0,
                   repbuf, sizeof(repbuf), &rn);
    if (rc != 0) { snprintf(out, outcap, "ERR idr-%d", rc); return; }

    /* 再包一层链路信封（= orpah_proto.build_id_report） */
    if (json_parse(&c, repbuf, rn, &rep_node) != 0) {
        snprintf(out, outcap, "ERR re-parse");
        return;
    }
    {
        jv_t *env = msg_id_report(&c, rep_node, av[0], parse_ll(av[1], 0));
        if (env == NULL) { snprintf(out, outcap, "ERR envelope"); return; }
        en = jcs_encode_raw(env, envbuf, sizeof(envbuf));
        if (en < 0) { snprintf(out, outcap, "ERR env-%d", en); return; }
    }
    {
        char rhex[4096], ehex[4096];
        to_hex((const unsigned char *)repbuf, rn, rhex, sizeof(rhex));
        to_hex((const unsigned char *)envbuf, (size_t)en, ehex, sizeof(ehex));
        snprintf(out, outcap, "%s\t%s", rhex, ehex);
    }
}

static int cmd_idr(const char *kind_unused, char **av, int n)
{
    char out[LINE_MAX];
    (void)kind_unused;
    if (n < 8) { fprintf(stderr, "id-report 需要 8 个参数\n"); return 1; }
    idr_line(av, out, sizeof(out));
    printf("%s\n", out);
    return 0;
}

static int cmd_idr_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[10];
        char got[LINE_MAX], exp[LINE_MAX];

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (split_tabs(line, cols, 10) != 10) {
            printf("FAIL line %d: 需要 10 列（8 参数 + 报文 hex + 信封 hex）\n", lineno);
            total++; bad++;
            continue;
        }
        total++;
        idr_line(cols, got, sizeof(got));
        snprintf(exp, sizeof(exp), "%s\t%s", cols[8], cols[9]);
        if (strcmp(got, exp) != 0) {
            printf("FAIL line %d:\n  C =%s\n  PY=%s\n", lineno, got, exp);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

int main(int argc, char **argv)
{
    const char *cmd, *arg;

    if (argc < 2) {
        fprintf(stderr, "usage: jcs_cli <cases|jcs <case>|jcs-selftest <f>|"
                        "b64url <hex>|b64url-selftest <f>|selfcheck>\n");
        return 1;
    }
    cmd = argv[1];
    arg = (argc >= 3) ? argv[2] : NULL;

    if (strcmp(cmd, "cases") == 0) {
        for (int i = 0; i < N_CASES; i++) printf("%s\n", CASE_NAMES[i]);
        return 0;
    }
    if (strcmp(cmd, "jcs") == 0) {
        if (arg == NULL) return 1;
        return cmd_jcs(arg);
    }
    if (strcmp(cmd, "jcs-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_jcs_selftest(arg);
    }
    if (strcmp(cmd, "b64url") == 0) {
        unsigned char raw[HEXOUT_MAX / 2];
        char out[HEXOUT_MAX];
        long n;
        int m;
        if (arg == NULL) return 1;
        n = from_hex(arg, raw, sizeof(raw));
        if (n < 0) { printf("ERR hex\n"); return 0; }
        m = b64url_encode(raw, (size_t)n, out, sizeof(out));
        if (m < 0) { printf("ERR cap\n"); return 0; }
        out[m] = '\0';
        printf("%s\n", out);
        return 0;
    }
    if (strcmp(cmd, "b64url-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_b64url_selftest(arg);
    }
    if (strcmp(cmd, "selfcheck") == 0) {
        return cmd_selfcheck();
    }
    if (strcmp(cmd, "msg") == 0) {
        if (argc < 8) { fprintf(stderr, "msg <kind> <a1> <a2> <a3> <a4> <a5>\n"); return 1; }
        return cmd_msg(argv[2], &argv[3], argc - 3);
    }
    if (strcmp(cmd, "msg-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_msg_selftest(arg);
    }
    if (strcmp(cmd, "dl") == 0) {
        if (arg == NULL) return 1;
        return cmd_dl(arg);
    }
    if (strcmp(cmd, "dl-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_dl_selftest(arg);
    }
    if (strcmp(cmd, "id-report") == 0) {
        if (argc < 10) {
            fprintf(stderr, "id-report <sn> <ts> <nonce> <level> <key-hex> <battery|-> <caprtc|-> <firmware|->\n"
                            "  key-hex: level 0 = P-256 私钥 d（32B 大端）; level 1/2 = HMAC 密钥; level 3 = -\n");
            return 1;
        }
        return cmd_idr(cmd, &argv[2], argc - 2);
    }
    if (strcmp(cmd, "id-report-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_idr_selftest(arg);
    }
    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 1;
}
