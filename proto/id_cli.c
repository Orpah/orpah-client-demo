/* id_cli.c — **主机侧** CLI：把 `id_build.c` 那条流水线（§8.2 选级 / 演示密钥 / nonce / 整帧）
 * 拿到 PC 上跑，并与 Python（上游 `orpah_id` + `orpah_proto`）逐字节对拍。
 *
 * ★ **不编进固件**（固件只调 `id_build.c`；本文件是用 stdio/string.h 的 host 工具）。
 *
 * 用法：
 *   id_cli keys-selftest  <vec>    # 行： keys  sn  gen  ec64  hmac64
 *   id_cli level-selftest <vec>    # 行： level se  sign hmac lvl reason
 *                                  #      mode  name se  sign hmac
 *                                  #      order n1,n2,n3,n4
 *   id_cli nonce-selftest <vec>    # 行： nonce sn  entropy ctr hex32
 *   id_cli frame-selftest <vec>    # 行： frame sn gen mode entropy ctr level reason frame-hex [fw]
 *   id_cli dev-frame <sn> <gen> <mode> <entropy> <ctr> [fw]
 *         → 打印： <report-hex> <TAB> <envelope-hex> <TAB> <frame-hex> <TAB> <level> <TAB> <reason>
 *   id_cli selfcheck              # C 侧不变量（含 §8.2 Step2 降级路径），不需要文件
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "id_build.h"
#include "id_keys.h"
#include "id_level.h"
#include "p256.h"

#define LINE_MAX 8192

static char g_line[LINE_MAX];
static jcs_ctx_t g_jc;
static char g_rep[2048];
static char g_env[2048];
static char g_frame[2048];
static char g_tmp[LINE_MAX];

static void chomp(char *s)
{
    size_t n = strlen(s);

    while (n > 0u && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int split_tabs(char *s, char *cols[], int ncols)
{
    int n = 0;
    char *p = s;

    for (;;) {
        if (n >= ncols) { return -1; }
        cols[n++] = p;
        p = strchr(p, '\t');
        if (p == NULL) { break; }
        *p++ = '\0';
    }
    return n;
}

static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[i * 2u] = H[(in[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = H[in[i] & 0x0Fu];
    }
    out[n * 2u] = '\0';
}

static int is_absent(const char *s)
{
    return (s == NULL || s[0] == '\0' || (s[0] == '-' && s[1] == '\0'));
}

static long from_hex_bytes(const char *hex, unsigned char *out, size_t cap)
{
    size_t n = 0;

    if (is_absent(hex)) { return 0; }
    while (hex[0] != '\0' && hex[1] != '\0') {
        int hi, lo;
        char a = hex[0], b = hex[1];

        if (a >= '0' && a <= '9') { hi = a - '0'; }
        else if (a >= 'a' && a <= 'f') { hi = a - 'a' + 10; }
        else if (a >= 'A' && a <= 'F') { hi = a - 'A' + 10; }
        else { return -1; }
        if (b >= '0' && b <= '9') { lo = b - '0'; }
        else if (b >= 'a' && b <= 'f') { lo = b - 'a' + 10; }
        else if (b >= 'A' && b <= 'F') { lo = b - 'A' + 10; }
        else { return -1; }
        if (n >= cap) { return -1; }
        out[n++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return (long)n;
}

/* ------------------------------------------------------------------ */
/* 单条流水线（供 dev-frame / frame-selftest 共用）                       */
/* ------------------------------------------------------------------ */
static int run_pipeline(const char *sn, int gen, const char *mode, unsigned long entropy,
                        unsigned long ctr, const char *fw, int sign_fail,
                        char *rephex, char *envhex, char *framehex,
                        int *level, const char **reason, idb_t *outb)
{
    idb_t b;
    size_t flen = 0u;
    int rc;

    if (idb_init(&b, &g_jc, g_rep, sizeof(g_rep), g_env, sizeof(g_env),
                 g_frame, sizeof(g_frame), sn, gen, is_absent(fw) ? NULL : fw, NULL,
                 (uint32_t)entropy) != 0) {
        return -1;
    }
    /* 让 nonce 落在指定的计数器上（= idn_soft_at(sn, entropy, ctr)，便于两侧复现）*/
    b.nonce.ctr = (uint32_t)(ctr > 0u ? (ctr - 1u) : 0u);
    b.test_sign_fail = sign_fail;

    rc = idb_build(&b, mode, &flen, level, reason);
    if (outb != NULL) { *outb = b; }
    if (rc != 0) {
        return -100 + rc;
    }
    /* ⚠ 用 b 记下的长度（输出**不带 NUL**），别用 strlen */
    to_hex((const unsigned char *)g_rep, b.last_rep_len, rephex);
    to_hex((const unsigned char *)g_env, b.last_env_len, envhex);
    to_hex((const unsigned char *)g_frame, flen, framehex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 各向量文件的 selftest                                                */
/* ------------------------------------------------------------------ */
static int cmd_keys_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    int nrow = 0, nbad = 0, lineno = 0;
    char ec[65], hmac[65];

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *c[8];
        int n;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') { continue; }
        n = split_tabs(g_line, c, 8);
        if (n != 5) { printf("FAIL %s:%d 列数=%d（要 5）\n", path, lineno, n); nrow++; nbad++; continue; }
        if (strcmp(c[0], "keys") != 0) { printf("FAIL %s:%d 未知 kind=%s\n", path, lineno, c[0]); nrow++; nbad++; continue; }
        {
            uint8_t d[32], k[32];

            nrow++;
            if (idk_demo_ec(c[1], (int)strtol(c[2], NULL, 10), d) != 0) {
                printf("FAIL %s:%d demo_ec ERR\n", path, lineno); nbad++; continue;
            }
            if (idk_demo_hmac(c[1], (int)strtol(c[2], NULL, 10), k) != 0) {
                printf("FAIL %s:%d demo_hmac ERR\n", path, lineno); nbad++; continue;
            }
            to_hex(d, 32u, ec);
            to_hex(k, 32u, hmac);
            if (strcmp(ec, c[3]) != 0) {
                printf("FAIL %s:%d EC 私钥不一致\n     C=%s\n  FILE=%s\n", path, lineno, ec, c[3]);
                nbad++; continue;
            }
            if (strcmp(hmac, c[4]) != 0) {
                printf("FAIL %s:%d HMAC 密钥不一致\n     C=%s\n  FILE=%s\n", path, lineno, hmac, c[4]);
                nbad++; continue;
            }
        }
    }
    fclose(fp);
    if (nbad == 0) { printf("PASS keys-selftest %d/%d 行（%s）\n", nrow, nrow, path); return 0; }
    printf("FAIL keys-selftest %d/%d 行不一致（%s）\n", nbad, nrow, path);
    return 2;
}

static int cmd_level_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    int nrow = 0, nbad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *c[8];
        int n;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') { continue; }
        n = split_tabs(g_line, c, 8);
        if (n < 2) { printf("FAIL %s:%d 列数=%d\n", path, lineno, n); nrow++; nbad++; continue; }
        nrow++;
        if (strcmp(c[0], "level") == 0 && n == 6) {
            int se = (int)strtol(c[1], NULL, 10);
            int sg = (int)strtol(c[2], NULL, 10);
            int hm = (int)strtol(c[3], NULL, 10);
            const char *rs = NULL;
            int got = idl_pick(se, sg, hm, &rs);
            int want = (int)strtol(c[4], NULL, 10);

            if (got != want) {
                printf("FAIL %s:%d level 不一致 C=%d FILE=%d (se=%d sign=%d hmac=%d)\n",
                       path, lineno, got, want, se, sg, hm);
                nbad++; continue;
            }
            if (strcmp(rs, c[5]) != 0) {
                printf("FAIL %s:%d reason 不一致 C=%s FILE=%s\n", path, lineno, rs, c[5]);
                nbad++; continue;
            }
        } else if (strcmp(c[0], "mode") == 0 && n == 5) {
            int se = -9, sg = -9, hm = -9;
            int want_se = (int)strtol(c[2], NULL, 10);
            int want_sg = (int)strtol(c[3], NULL, 10);
            int want_hm = (int)strtol(c[4], NULL, 10);

            if (idl_mode_inputs(c[1], &se, &sg, &hm) != 0) {
                printf("FAIL %s:%d 不认识的模式 %s\n", path, lineno, c[1]);
                nbad++; continue;
            }
            if (se != want_se || sg != want_sg || hm != want_hm) {
                printf("FAIL %s:%d mode %s 输入不一致 C=(%d,%d,%d) FILE=(%d,%d,%d)\n",
                       path, lineno, c[1], se, sg, hm, want_se, want_sg, want_hm);
                nbad++; continue;
            }
        } else if (strcmp(c[0], "order") == 0 && n == 2) {
            int i;
            char got[128];

            got[0] = '\0';
            for (i = 0; i < IDL_MODE_COUNT; i++) {
                if (i > 0) { strcat(got, ","); }
                strcat(got, idl_mode_name(i));
            }
            if (strcmp(got, c[1]) != 0) {
                printf("FAIL %s:%d 模式表顺序不一致 C=%s FILE=%s\n", path, lineno, got, c[1]);
                nbad++; continue;
            }
        } else {
            printf("FAIL %s:%d 未知 kind=%s（列数=%d）\n", path, lineno, c[0], n);
            nbad++; continue;
        }
    }
    fclose(fp);
    if (nbad == 0) { printf("PASS level-selftest %d/%d 行（%s）\n", nrow, nrow, path); return 0; }
    printf("FAIL level-selftest %d/%d 行不一致（%s）\n", nbad, nrow, path);
    return 2;
}

static int cmd_nonce_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    int nrow = 0, nbad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *c[8];
        int n;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') { continue; }
        n = split_tabs(g_line, c, 8);
        if (n != 5 || strcmp(c[0], "nonce") != 0) {
            printf("FAIL %s:%d 需要 5 列且 kind=nonce（实际 %d 列 kind=%s）\n",
                   path, lineno, n, (n > 0) ? c[0] : "-");
            nrow++; nbad++; continue;
        }
        nrow++;
        if (idn_soft_at(c[1], (uint32_t)strtoul(c[2], NULL, 10),
                        (uint32_t)strtoul(c[3], NULL, 10), g_tmp) != 0) {
            printf("FAIL %s:%d idn_soft_at ERR\n", path, lineno); nbad++; continue;
        }
        if (strcmp(g_tmp, c[4]) != 0) {
            printf("FAIL %s:%d nonce 不一致\n     C=%s\n  FILE=%s\n", path, lineno, g_tmp, c[4]);
            nbad++; continue;
        }
    }
    fclose(fp);
    if (nbad == 0) { printf("PASS nonce-selftest %d/%d 行（%s）\n", nrow, nrow, path); return 0; }
    printf("FAIL nonce-selftest %d/%d 行不一致（%s）\n", nbad, nrow, path);
    return 2;
}

static int cmd_frame_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    int nrow = 0, nbad = 0, lineno = 0;

    if (fp == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *c[12];
        int n;
        char rephex[LINE_MAX / 4], envhex[LINE_MAX / 4], frhex[LINE_MAX / 4];
        int level = -1;
        const char *reason = NULL;
        int rc;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') { continue; }
        n = split_tabs(g_line, c, 12);
        if (n < 9 || strcmp(c[0], "frame") != 0) {
            printf("FAIL %s:%d 需要 >=9 列且 kind=frame（实际 %d 列）\n", path, lineno, n);
            nrow++; nbad++; continue;
        }
        nrow++;
        /* ⚠ `idr_build` 的输出**不带结尾 NUL**（同 jcs_encode）⇒ 一律用返回的长度，
         *   **不能用 strlen 读它**（实测：同进程第 2 条起会读到上一行的残留，长度看着“像对”）。*/
        rc = run_pipeline(c[1], (int)strtol(c[2], NULL, 10), c[3],
                          strtoul(c[4], NULL, 10), strtoul(c[5], NULL, 10),
                          (n >= 10) ? c[9] : NULL, 0,
                          rephex, envhex, frhex, &level, &reason, NULL);
        if (rc != 0) {
            printf("FAIL %s:%d 流水线 ERR %d（idr_rc=%d）\n", path, lineno, rc, -200 + rc);
            nbad++; continue;
        }
        if (level != (int)strtol(c[6], NULL, 10)) {
            printf("FAIL %s:%d level 不一致 C=%d FILE=%s\n", path, lineno, level, c[6]);
            nbad++; continue;
        }
        if (strcmp(reason, c[7]) != 0) {
            printf("FAIL %s:%d reason 不一致 C=%s FILE=%s\n", path, lineno, reason, c[7]);
            nbad++; continue;
        }
        if (strcmp(frhex, c[8]) != 0) {
            printf("FAIL %s:%d 以太帧不一致（%u vs %u 字符）\n     C=%s\n  FILE=%s\n",
                   path, lineno, (unsigned)strlen(frhex), (unsigned)strlen(c[8]), frhex, c[8]);
            nbad++; continue;
        }
    }
    fclose(fp);
    if (nbad == 0) { printf("PASS frame-selftest %d/%d 行（%s）\n", nrow, nrow, path); return 0; }
    printf("FAIL frame-selftest %d/%d 行不一致（%s）\n", nbad, nrow, path);
    return 2;
}

/* ------------------------------------------------------------------ */
/* C 侧不变量（不需要文件）—— 含 §8.2 Step2 的降级路径                     */
/* ------------------------------------------------------------------ */
#define SN_DEMO "CN-WH01-9AF3C1D2"

static int cmd_selfcheck(void)
{
    int bad = 0;
    int i;
    char rephex[1024], envhex[1024], frhex[4096];
    int level = -1;
    const char *reason = NULL;
    char n1[IDN_NONCE_LEN + 1], n2[IDN_NONCE_LEN + 1];
    idn_soft_t st;
    idb_t b;
    size_t flen = 0u;

    /* ① 模式表：四个模式的名字/顺序就是上游 LEVEL_MODES 的四个 */
    {
        static const char *want[4] = { "auto", "sign_fail", "se_fail", "no_key" };

        for (i = 0; i < IDL_MODE_COUNT; i++) {
            const char *g = idl_mode_name(i);

            if (g == NULL || strcmp(g, want[i]) != 0) { bad++; printf("FAIL 模式表[%d]\n", i); }
        }
        if (idl_mode_name(IDL_MODE_COUNT) != NULL || idl_mode_name(-1) != NULL) {
            bad++; printf("FAIL 模式表越界未返回 NULL\n");
        }
    }

    /* ② 选级：§8.2 的四条路径 */
    {
        struct { int se, sg, hm, want; const char *rs; } t[4] = {
            { 1, 1, 1, IDL_L0, "normal" },
            { 1, 0, 1, IDL_L1, "slot0_sign_failed" },
            { 0, 1, 1, IDL_L2, "se_unavailable" },
            { 0, 1, 0, IDL_L3, "no_key" }
        };

        for (i = 0; i < 4; i++) {
            const char *rs = NULL;
            int g = idl_pick(t[i].se, t[i].sg, t[i].hm, &rs);

            if (g != t[i].want || strcmp(rs, t[i].rs) != 0) {
                bad++; printf("FAIL 选级 (%d,%d,%d) -> %d/%s\n", t[i].se, t[i].sg, t[i].hm, g, rs);
            }
        }
    }

    /* ③ nonce：格式 + 同 boot 内不重复 + 随 sn / entropy 变化 */
    if (idn_soft_init(&st, SN_DEMO, 0x1234u) != 0) { bad++; printf("FAIL nonce init\n"); }
    if (idn_soft_next(&st, n1) != 0) { bad++; printf("FAIL nonce next\n"); }
    if (idn_soft_next(&st, n2) != 0) { bad++; printf("FAIL nonce next2\n"); }
    if (strlen(n1) != IDN_NONCE_LEN) { bad++; printf("FAIL nonce 长度=%u\n", (unsigned)strlen(n1)); }
    if (strcmp(n1, n2) == 0) { bad++; printf("FAIL nonce 同一 boot 内重复\n"); }
    for (i = 0; i < IDN_NONCE_LEN; i++) {
        if (!((n1[i] >= '0' && n1[i] <= '9') || (n1[i] >= 'A' && n1[i] <= 'F'))) {
            bad++; printf("FAIL nonce 不是大写十六进制: %s\n", n1);
            break;
        }
    }
    if (idn_soft_at(SN_DEMO, 0x1235u, 1u, g_tmp) != 0 || strcmp(g_tmp, n1) == 0) {
        bad++; printf("FAIL nonce 不随 entropy 变化\n");
    }
    if (idn_soft_at("CN-WH01-9AF3C1D3", 0x1234u, 1u, g_tmp) != 0 || strcmp(g_tmp, n1) == 0) {
        bad++; printf("FAIL nonce 不随 SN 变化\n");
    }
    /* 计数器上界附近也不撞（这里只查 8 条互不相同）*/
    {
        char prev[IDN_NONCE_LEN + 1];
        idn_soft_t s2;

        (void)idn_soft_init(&s2, SN_DEMO, 0u);
        prev[0] = '\0';
        for (i = 0; i < 8; i++) {
            char now[IDN_NONCE_LEN + 1];

            (void)idn_soft_next(&s2, now);
            if (strcmp(now, prev) == 0) { bad++; printf("FAIL nonce 第 %d 条与上一条相同\n", i); }
            strcpy(prev, now);
        }
    }

    /* ④ 流水线：四个模式各出一帧，且帧头正确（广播 dst / 0x88b5）*/
    {
        static const char *modes[4] = { "auto", "sign_fail", "se_fail", "no_key" };
        static const int   wantl[4] = { IDL_L0, IDL_L1, IDL_L2, IDL_L3 };

        for (i = 0; i < 4; i++) {
            int rc = run_pipeline(SN_DEMO, 1, modes[i], 0xABCDu, 1u, NULL, 0,
                                  rephex, envhex, frhex, &level, &reason, NULL);

            if (rc != 0) { bad++; printf("FAIL 流水线 %s ERR %d\n", modes[i], rc); continue; }
            if (level != wantl[i]) { bad++; printf("FAIL 流水线 %s level=%d\n", modes[i], level); continue; }
            if (strncmp(frhex, "ffffffffffff", 12) != 0) {
                bad++; printf("FAIL %s dst 不是广播\n", modes[i]);
            }
            if (frhex[24] != '8' || frhex[25] != '8' || frhex[26] != 'b' || frhex[27] != '5') {
                bad++; printf("FAIL %s ethertype != 0x88b5（%s）\n", modes[i], frhex + 24);
            }
        }
    }

    /* ⑤ §8.2 Step2 的**降级路径**：ES256 签名失败 → 自动降到 L1（测试钩子模拟 SE 签名失败）*/
    {
        idb_t b2;
        int rc = run_pipeline(SN_DEMO, 1, "auto", 0xABCDu, 1u, NULL, 1,
                              rephex, envhex, frhex, &level, &reason, &b2);

        if (rc != 0) {
            bad++; printf("FAIL 降级路径流水线 ERR %d\n", rc);
        } else {
            if (level != IDL_L1) { bad++; printf("FAIL 降级后 level=%d（要 1）\n", level); }
            if (strcmp(reason, "slot0_sign_failed") != 0) {
                bad++; printf("FAIL 降级后 reason=%s\n", reason);
            }
            if (b2.fell_back != 1u) {
                bad++; printf("FAIL 降级计数 fell_back=%u\n", (unsigned)b2.fell_back);
            }
            if (b2.last_idr_rc != 0) {
                bad++; printf("FAIL 降级后最后一次 idr_rc=%d\n", b2.last_idr_rc);
            }
        }
    }

    /* ⑥ ES256 私钥 = 上游演示派生（这里只查"非 0 且能派生出合法公钥"）*/
    {
        uint8_t d[32], pub[65];

        if (idk_demo_ec(SN_DEMO, 1, d) != 0 || p256_pubkey_from_priv(pub, d) != 0) {
            bad++; printf("FAIL 演示 EC 私钥/公钥\n");
        }
    }

    /* ⑦ 缓冲不够要**报错而不是截断**（固件 RAM 紧，这条是安全底线）*/
    {
        idb_t small;

        if (idb_init(&small, &g_jc, g_rep, sizeof(g_rep), g_env, sizeof(g_env),
                     g_frame, 20u, SN_DEMO, 1, NULL, NULL, 1u) != 0) {
            bad++; printf("FAIL 小帧缓冲 init 应当成功（20 > 14）\n");
        } else if (idb_build(&small, "auto", &flen, &level, &reason) == 0) {
            bad++; printf("FAIL 帧缓冲不够却成功返回\n");
        }
    }

    /* ⑧ 按**固件缓冲尺寸**（IDB_REC_*）跑四档："固件里够不够"要在上机前就知道 */
    {
        static jcs_ctx_t jc2;
        static char rep2[IDB_REC_REPORT_CAP];
        static char env2[IDB_REC_ENV_CAP];
        static char fr2[IDB_REC_FRAME_CAP];
        static const char *modes[4] = { "auto", "sign_fail", "se_fail", "no_key" };
        idb_t b3;
        size_t fl;
        int lv;
        const char *rs;

        for (i = 0; i < 4; i++) {
            if (idb_init(&b3, &jc2, rep2, sizeof(rep2), env2, sizeof(env2),
                         fr2, sizeof(fr2), SN_DEMO, 1, "0.1.0-c4g1", NULL, 0x5A5Au) != 0) {
                bad++; printf("FAIL 固件缓冲 init（%s）\n", modes[i]);
                continue;
            }
            if (idb_build(&b3, modes[i], &fl, &lv, &rs) != 0) {
                bad++; printf("FAIL 固件缓冲不够（%s）err=%d idr_rc=%d\n",
                             modes[i], b3.last_err, b3.last_idr_rc);
                continue;
            }
            if (fl > sizeof(fr2)) {
                bad++; printf("FAIL 帧长 %u > 缓冲 %u\n", (unsigned)fl, (unsigned)sizeof(fr2));
            }
        }
    }

    (void)b;
    if (bad == 0) {
        printf("PASS id-cli selfcheck（选级/nonce/流水线/降级路径/缓冲不够报错）\n");
        return 0;
    }
    printf("FAIL id-cli selfcheck：%d 项不成立\n", bad);
    return 2;
}

static int cmd_dev_frame(char **av, int n)
{
    char rephex[LINE_MAX / 4], envhex[LINE_MAX / 4], frhex[LINE_MAX / 4];
    int level = -1;
    const char *reason = NULL;
    int rc;

    if (n < 5) {
        fprintf(stderr, "dev-frame 需要 <sn> <gen> <mode> <entropy> <ctr> [fw]\n");
        return 1;
    }
    rc = run_pipeline(av[0], (int)strtol(av[1], NULL, 10), av[2],
                      strtoul(av[3], NULL, 10), strtoul(av[4], NULL, 10),
                      (n >= 6) ? av[5] : NULL, 0, rephex, envhex, frhex,
                      &level, &reason, NULL);
    if (rc != 0) {
        printf("ERR pipeline %d\n", rc);
        return 2;
    }
    printf("%s\t%s\t%s\t%d\t%s\n", rephex, envhex, frhex, level, reason);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sub;

    if (argc < 2) {
        fprintf(stderr, "usage: id_cli keys-selftest|level-selftest|nonce-selftest|frame-selftest <vec>\n"
                        "       id_cli dev-frame <sn> <gen> <mode> <entropy> <ctr> [fw]\n"
                        "       id_cli selfcheck\n");
        return 1;
    }
    sub = argv[1];
    if (strcmp(sub, "keys-selftest") == 0 && argc >= 3) { return cmd_keys_selftest(argv[2]); }
    if (strcmp(sub, "level-selftest") == 0 && argc >= 3) { return cmd_level_selftest(argv[2]); }
    if (strcmp(sub, "nonce-selftest") == 0 && argc >= 3) { return cmd_nonce_selftest(argv[2]); }
    if (strcmp(sub, "frame-selftest") == 0 && argc >= 3) { return cmd_frame_selftest(argv[2]); }
    if (strcmp(sub, "dev-frame") == 0) { return cmd_dev_frame(argv + 2, argc - 2); }
    if (strcmp(sub, "selfcheck") == 0) { return cmd_selfcheck(); }
    fprintf(stderr, "unknown/incomplete subcommand: %s\n", sub);
    return 1;
}
