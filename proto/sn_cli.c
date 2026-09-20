/* sn_cli.c — SN 内核的 **host 侧** CLI（对拍 / 调试用；不编进固件）
 *
 * 与 Python 参考实现对拍时，**只认 stdout 的固定格式**（下面每个子命令都写明了）。
 * 输出一律 ASCII，便于 Windows 控制台/管道直接读。
 *
 * 子命令：
 *   compute-luhn32 <ORG-UNIQUE>        -> 1 位校验码 / ERR <code>
 *   compute-mod97  <ORG-UNIQUE>        -> 2 位校验码 / ERR <code>
 *   verify-luhn32  <ORG-UNIQUE-CHECK>  -> 1 / 0
 *   verify-mod97   <ORG-UNIQUE-CHECK>  -> 1 / 0
 *   ok             <SN>                -> 1 / 0
 *   err            <SN>                -> empty / too-long / bad-format / -
 *   verify-sn      <SN>                -> 1 / 0
 *   parse          <SN>                -> cc<TAB>org<TAB>unique<TAB>check / ERR <reason>
 *   batch-luhn32                        (stdin 每行一个 ORG-UNIQUE) -> ORG-UNIQUE<TAB>CHECK
 *   batch-mod97                         (同上，2 位)
 *   selfcheck                           -> C 侧内置黄金/边界自检（不需要文件），末尾 PASS/FAIL
 *   selftest <vector-file>              行格式：ORG-UNIQUE<TAB>LUHN32<TAB>MOD97（# 注释）
 *   sn-selftest <vector-file>           行格式：SN<TAB>ok<TAB>err<TAB>verify（# 注释）
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sn.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: sn_cli <cmd> [arg]\n"
            "  compute-luhn32|compute-mod97 <ORG-UNIQUE>\n"
            "  verify-luhn32|verify-mod97 <ORG-UNIQUE-CHECK>\n"
            "  ok|err|verify-sn|parse <SN>\n"
            "  batch-luhn32|batch-mod97        (stdin)\n"
            "  selfcheck\n"
            "  selftest <vector-file> | sn-selftest <vector-file>\n");
}

/* 去掉行尾 CR/LF */
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

/* ------------------------------------------------------------------ */
/* 子命令                                                             */
/* ------------------------------------------------------------------ */
static int cmd_compute(const char *algo, const char *arg)
{
    char out[4];
    int rc;

    if (arg == NULL) { usage(); return 1; }
    if (strcmp(algo, "luhn32") == 0) {
        rc = sn_compute_check_luhn32(arg, out);
    } else {
        rc = sn_compute_check_mod97(arg, out);
    }
    if (rc != 0) {
        printf("ERR %s\n", "bad-input");
        return 0;
    }
    printf("%s\n", out);
    return 0;
}

static int cmd_batch(const char *algo)
{
    char line[256];
    char out[4];

    while (fgets(line, (int)sizeof(line), stdin) != NULL) {
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (strcmp(algo, "luhn32") == 0) {
            if (sn_compute_check_luhn32(line, out) != 0) { printf("%s\tERR\n", line); continue; }
        } else {
            if (sn_compute_check_mod97(line, out) != 0) { printf("%s\tERR\n", line); continue; }
        }
        printf("%s\t%s\n", line, out);
    }
    return 0;
}

/* C 侧内置黄金/边界自检（不依赖向量文件；真正的判据仍是 selftest 的向量文件）。
 *
 * ⚠ 这里的期望值**全部**来自 Python 参考实现的输出（见 test_vectors_sn.txt 的表头与首行），
 *   不是手算/记忆值。踩过的坑：曾把 "WH01-9AF3C1D2 → B" 当成 Luhn32 的黄金值 —— 那是
 *   **Damm32** 的黄金样本（另一个算法！），Luhn32 的真值是 **E**、Mod97 是 **21**
 *   （巧合的是输入 "T" 的 Luhn32 恰好是 B，更容易混）。
 */
static int cmd_selfcheck(void)
{
    static const char *golden = "WH01-9AF3C1D2";
    int fails = 0, total = 0;
    char out[4];

    total++;
    if (sn_compute_check_luhn32(golden, out) != 0 || strcmp(out, "E") != 0) {
        printf("FAIL golden luhn32: expect E got %s\n", out);
        fails++;
    }
    total++;
    if (sn_compute_check_mod97(golden, out) != 0 || strcmp(out, "21") != 0) {
        printf("FAIL golden mod97: expect 21 got %s\n", out);
        fails++;
    }
    total++;
    if (sn_verify_check_luhn32("WH01-9AF3C1D2-E") != 1) {
        printf("FAIL verify luhn32(WH01-9AF3C1D2-E) 应为 1\n");
        fails++;
    }
    total++;
    if (sn_verify_check_mod97("WH01-9AF3C1D2-21") != 1) {
        printf("FAIL verify mod97(WH01-9AF3C1D2-21) 应为 1\n");
        fails++;
    }
    total++;
    if (sn_verify_check("CN-WH01-9AF3C1D2-E") != 1) {
        printf("FAIL verify_check(CN-WH01-9AF3C1D2-E) 应为 1（1 位 → Luhn32）\n");
        fails++;
    }
    total++;
    if (sn_verify_check("CN-WH01-9AF3C1D2-21") != 1) {
        printf("FAIL verify_check(CN-WH01-9AF3C1D2-21) 应为 1（2 位 → Mod97）\n");
        fails++;
    }
    total++;
    if (sn_verify_check("CN-WH01-9AF3C1D2-B") != 0) {
        printf("FAIL 校验位改成 B（Damm32 的值，不是 Luhn32）后应为 0\n");
        fails++;
    }
    total++;
    if (sn_ok("cn-wh01-9af3c1d2") != 0 || strcmp(sn_err("cn-wh01-9af3c1d2"), "bad-format") != 0) {
        printf("FAIL 小写 SN 应为 bad-format（正则只收大写）\n");
        fails++;
    }
    total++;
    if (sn_ok("CN-WH01-9AF3C1D2") != 1 || sn_err("CN-WH01-9AF3C1D2") != NULL) {
        printf("FAIL CN-WH01-9AF3C1D2 应合法\n");
        fails++;
    }
    total++;
    if (sn_err("") == NULL || strcmp(sn_err(""), "empty") != 0) {
        printf("FAIL 空串应为 empty\n");
        fails++;
    }
    total++;
    if (sn_verify_check("CN-WH01-9AF3C1D2") != 1) {
        printf("FAIL 无校验位应直接通过\n");
        fails++;
    }

    printf("%s %d/%d\n", fails ? "FAIL" : "PASS", total - fails, total);
    return fails ? 2 : 0;
}

static int cmd_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[3];
        char out[4];
        int n;

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        n = split_tabs(line, cols, 3);
        if (n != 3) {
            printf("FAIL line %d: 需要 3 列（ORG-UNIQUE<TAB>LUHN32<TAB>MOD97）\n", lineno);
            bad++;
            total++;
            continue;
        }
        total++;
        if (sn_compute_check_luhn32(cols[0], out) != 0 || strcmp(out, cols[1]) != 0) {
            printf("FAIL line %d: luhn32(%s) C=%s PY=%s\n", lineno, cols[0], out, cols[1]);
            bad++;
        }
        if (sn_compute_check_mod97(cols[0], out) != 0 || strcmp(out, cols[2]) != 0) {
            printf("FAIL line %d: mod97(%s) C=%s PY=%s\n", lineno, cols[0], out, cols[2]);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

static int cmd_sn_selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    int total = 0, bad = 0, lineno = 0;

    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *cols[4];
        const char *err;
        int n, ok, verify;

        lineno++;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        n = split_tabs(line, cols, 4);
        if (n != 4) {
            printf("FAIL line %d: 需要 4 列（SN<TAB>ok<TAB>err<TAB>verify）\n", lineno);
            bad++;
            total++;
            continue;
        }
        total++;
        ok = sn_ok(cols[0]);
        err = sn_err(cols[0]);
        verify = sn_verify_check(cols[0]);

        if (ok != atoi(cols[1])) {
            printf("FAIL line %d: sn_ok(%s) C=%d PY=%s\n", lineno, cols[0], ok, cols[1]);
            bad++;
        }
        /* err 列：Python 的 None 记作 "-" */
        if (strcmp(err == NULL ? "-" : err, cols[2]) != 0) {
            printf("FAIL line %d: sn_err(%s) C=%s PY=%s\n", lineno, cols[0],
                   err == NULL ? "-" : err, cols[2]);
            bad++;
        }
        if (verify != atoi(cols[3])) {
            printf("FAIL line %d: sn_verify_check(%s) C=%d PY=%s\n",
                   lineno, cols[0], verify, cols[3]);
            bad++;
        }
    }
    fclose(fp);
    printf("%s %d/%d\n", bad ? "FAIL" : "PASS", total - bad, total);
    return bad ? 2 : 0;
}

int main(int argc, char **argv)
{
    const char *cmd;
    const char *arg;

    if (argc < 2) { usage(); return 1; }
    cmd = argv[1];
    arg = (argc >= 3) ? argv[2] : NULL;

    if (strcmp(cmd, "compute-luhn32") == 0) return cmd_compute("luhn32", arg);
    if (strcmp(cmd, "compute-mod97") == 0) return cmd_compute("mod97", arg);
    if (strcmp(cmd, "verify-luhn32") == 0) {
        if (arg == NULL) { usage(); return 1; }
        printf("%d\n", sn_verify_check_luhn32(arg));
        return 0;
    }
    if (strcmp(cmd, "verify-mod97") == 0) {
        if (arg == NULL) { usage(); return 1; }
        printf("%d\n", sn_verify_check_mod97(arg));
        return 0;
    }
    if (strcmp(cmd, "ok") == 0) {
        if (arg == NULL) { usage(); return 1; }
        printf("%d\n", sn_ok(arg));
        return 0;
    }
    if (strcmp(cmd, "err") == 0) {
        const char *e;
        if (arg == NULL) { usage(); return 1; }
        e = sn_err(arg);
        printf("%s\n", e == NULL ? "-" : e);
        return 0;
    }
    if (strcmp(cmd, "verify-sn") == 0) {
        if (arg == NULL) { usage(); return 1; }
        printf("%d\n", sn_verify_check(arg));
        return 0;
    }
    if (strcmp(cmd, "parse") == 0) {
        sn_parts_t p;
        if (arg == NULL) { usage(); return 1; }
        if (!sn_parse(arg, &p)) {
            const char *e = sn_err(arg);
            printf("ERR %s\n", e == NULL ? "parse-failed" : e);
            return 0;
        }
        printf("%s\t%s\t%s\t%s\n", p.cc, p.org, p.unique, p.check);
        return 0;
    }
    if (strcmp(cmd, "batch-luhn32") == 0) return cmd_batch("luhn32");
    if (strcmp(cmd, "batch-mod97") == 0) return cmd_batch("mod97");
    if (strcmp(cmd, "selfcheck") == 0) return cmd_selfcheck();
    if (strcmp(cmd, "selftest") == 0) {
        if (arg == NULL) { usage(); return 1; }
        return cmd_selftest(arg);
    }
    if (strcmp(cmd, "sn-selftest") == 0) {
        if (arg == NULL) { usage(); return 1; }
        return cmd_sn_selftest(arg);
    }

    usage();
    return 1;
}
