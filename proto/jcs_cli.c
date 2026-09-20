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
    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 1;
}
