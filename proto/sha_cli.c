/* sha_cli.c — SHA-256 的 **host 侧** CLI（对拍/调试用；不编进固件）
 *
 * 子命令：
 *   sha256 <input-hex>              -> 32 字节摘要（hex）
 *   sha256-selftest <vector-file>   行格式：<input-hex><TAB><digest-hex>
 *   selfcheck                      -> **分块自洽**（一次算 vs 分多段喂，必须逐位相同）
 *                                      尾部填充/缓冲边界最容易错，这里每个分点都试
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 * ★ 期望值一律来自 Python hashlib 生成的向量文件；selfcheck 只检查**纯 C 自洽**，
 *   不写任何“我以为的摘要值”（见 proto/README.md 的踩坑 ①）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sha256.h"
#include "hmac.h"

#define HEXOUT_MAX 8192
#define LINE_MAX   8192

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

/* 按 TAB 切最多 ncols 段（原地改串） */
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

static int cmd_sha256(const char *in_hex)
{
    static unsigned char raw[HEXOUT_MAX / 2];
    unsigned char dig[SHA256_DIGEST_LEN];
    char hex[3 + SHA256_DIGEST_LEN * 2];
    long n = from_hex(in_hex, raw, sizeof(raw));

    if (n < 0) { printf("ERR hex\n"); return 0; }
    sha256(raw, (size_t)n, dig);
    to_hex(dig, sizeof(dig), hex, sizeof(hex));
    printf("%s\n", hex);
    return 0;
}

static int cmd_sha_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    static unsigned char raw[HEXOUT_MAX / 2];
    int total = 0, bad = 0, lineno = 0;
    unsigned char dig[SHA256_DIGEST_LEN];
    char hex[3 + SHA256_DIGEST_LEN * 2];

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *tab;
        long n;
        const char *in_hex;
        const char *want;

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        tab = strchr(line, '\t');
        if (tab == NULL) {
            printf("FAIL line %d: 需要 2 列（input-hex<TAB>digest-hex）\n", lineno);
            total++; bad++;
            continue;
        }
        *tab = '\0';
        in_hex = line;
        want = tab + 1;
        total++;

        n = from_hex(in_hex, raw, sizeof(raw));
        if (n < 0) {
            printf("FAIL line %d: input hex 非法（长度 %u）\n", lineno,
                   (unsigned)strlen(in_hex));
            bad++;
            continue;
        }
        sha256(raw, (size_t)n, dig);
        to_hex(dig, sizeof(dig), hex, sizeof(hex));
        if (strcmp(hex, want) != 0) {
            printf("FAIL line %d（input %ld 字节）:\n  C =%s\n  PY=%s\n", lineno, n, hex, want);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

/* 分块自洽：同一条输入，"一次算" 与 "分成多段依次喂" 必须完全相同。
 * 填充/缓冲边界最容易错，所以小长度把每个分点都试一遍。 */
#define SC_DATA_MAX 1000

static int cmd_selfcheck(void)
{
    static unsigned char data[SC_DATA_MAX];
    static unsigned char ref[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];
    int fails = 0, total = 0;

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (unsigned char)((i * 7 + 3) % 251);
    }

    /* ① 一次算 vs 分两段 */
    for (size_t len = 0; len <= 300; len += (len < 130 ? 1 : 13)) {
        sha256(data, len, ref);
        for (size_t split = 0; split <= len; split += (len < 70 ? 1 : 11)) {
            sha256_ctx_t c;
            sha256_init(&c);
            sha256_update(&c, data, split);
            sha256_update(&c, data + split, len - split);
            sha256_final(&c, got);
            total++;
            if (memcmp(ref, got, SHA256_DIGEST_LEN) != 0) {
                printf("FAIL 分块不一致：len=%u split=%u\n",
                       (unsigned)len, (unsigned)split);
                fails++;
            }
        }
    }

    /* ② 逐字节喂满整个缓冲，必须等于一次算 */
    {
        sha256_ctx_t c;
        sha256_init(&c);
        for (size_t i = 0; i < sizeof(data); i++) {
            sha256_update(&c, &data[i], 1);
        }
        sha256_final(&c, got);
        sha256(data, sizeof(data), ref);
        total++;
        if (memcmp(ref, got, SHA256_DIGEST_LEN) != 0) {
            printf("FAIL 逐字节喂 %u 字节与一次算不一致\n", (unsigned)sizeof(data));
            fails++;
        }
    }

    /* ③ 零长更新不该改变状态 */
    {
        sha256_ctx_t c;
        sha256_init(&c);
        sha256_update(&c, data, 100);
        sha256_update(&c, NULL, 0);
        sha256_update(&c, data + 100, 0);
        sha256_final(&c, got);
        sha256(data, 100, ref);
        total++;
        if (memcmp(ref, got, SHA256_DIGEST_LEN) != 0) {
            printf("FAIL 零长更新改变了状态\n");
            fails++;
        }
    }

    /* ④ HMAC 不变量：长于分组（>64B）的键，等价于先用它的 SHA-256 当键
     *    （RFC 2104 的定义本身；不需要外部真值就能查实现有没有走错分支） */
    {
        unsigned char k65[65], kh[32];
        unsigned char mac1[32], mac2[32];
        for (size_t i = 0; i < sizeof(k65); i++) k65[i] = (unsigned char)(i + 1);
        sha256(k65, sizeof(k65), kh);
        hmac_sha256(k65, sizeof(k65), data, 40, mac1);
        hmac_sha256(kh, sizeof(kh), data, 40, mac2);
        total++;
        if (memcmp(mac1, mac2, 32) != 0) {
            printf("FAIL HMAC 不变量：65B 键 ≠ 其 SHA-256 当键\n");
            fails++;
        }
        /* 再来一条：空消息的 HMAC 必须与“空指针 + 0 长度”一致 */
        hmac_sha256(kh, sizeof(kh), NULL, 0, mac1);
        hmac_sha256(kh, sizeof(kh), data, 0, mac2);
        total++;
        if (memcmp(mac1, mac2, 32) != 0) {
            printf("FAIL HMAC：空消息两种写法不一致\n");
            fails++;
        }
    }

    printf("%s %d/%d\n", fails ? "FAIL" : "PASS", total - fails, total);
    return fails ? 2 : 0;
}

static int cmd_hmac(const char *key_hex, const char *msg_hex)
{
    static unsigned char key[HEXOUT_MAX / 2];
    static unsigned char msg[HEXOUT_MAX / 2];
    unsigned char mac[SHA256_DIGEST_LEN];
    char hex[3 + SHA256_DIGEST_LEN * 2];
    long kn = from_hex(key_hex, key, sizeof(key));
    long mn = from_hex(msg_hex, msg, sizeof(msg));

    if (kn < 0 || mn < 0) { printf("ERR hex\n"); return 0; }
    hmac_sha256(key, (size_t)kn, msg, (size_t)mn, mac);
    to_hex(mac, sizeof(mac), hex, sizeof(hex));
    printf("%s\n", hex);
    return 0;
}

static int cmd_hmac_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX];
    static unsigned char key[HEXOUT_MAX / 2];
    static unsigned char msg[HEXOUT_MAX / 2];
    int total = 0, bad = 0, lineno = 0;
    unsigned char mac[SHA256_DIGEST_LEN];
    char hex[3 + SHA256_DIGEST_LEN * 2];

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[3];
        long kn, mn;

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (split_tabs(line, cols, 3) != 3) {
            printf("FAIL line %d: 需要 3 列（key-hex<TAB>msg-hex<TAB>mac-hex）\n", lineno);
            total++; bad++;
            continue;
        }
        total++;
        kn = from_hex(cols[0], key, sizeof(key));
        mn = from_hex(cols[1], msg, sizeof(msg));
        if (kn < 0 || mn < 0) {
            printf("FAIL line %d: hex 非法（key %u 字符 / msg %u 字符）\n", lineno,
                   (unsigned)strlen(cols[0]), (unsigned)strlen(cols[1]));
            bad++;
            continue;
        }
        hmac_sha256(key, (size_t)kn, msg, (size_t)mn, mac);
        to_hex(mac, sizeof(mac), hex, sizeof(hex));
        if (strcmp(hex, cols[2]) != 0) {
            printf("FAIL line %d（key %ldB / msg %ldB）:\n  C =%s\n  PY=%s\n",
                   lineno, kn, mn, hex, cols[2]);
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
        fprintf(stderr, "usage: sha_cli <sha256 <hex>|sha256-selftest <f>|selfcheck>\n");
        return 1;
    }
    cmd = argv[1];
    arg = (argc >= 3) ? argv[2] : NULL;

    if (strcmp(cmd, "sha256") == 0) {
        if (arg == NULL) return 1;
        return cmd_sha256(arg);
    }
    if (strcmp(cmd, "sha256-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_sha_selftest(arg);
    }
    if (strcmp(cmd, "selfcheck") == 0) {
        return cmd_selfcheck();
    }
    if (strcmp(cmd, "hmac") == 0) {
        if (argc < 4) {
            fprintf(stderr, "hmac <key-hex> <msg-hex>\n");
            return 1;
        }
        return cmd_hmac(argv[2], argv[3]);
    }
    if (strcmp(cmd, "hmac-selftest") == 0) {
        if (arg == NULL) return 1;
        return cmd_hmac_selftest(arg);
    }
    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 1;
}
