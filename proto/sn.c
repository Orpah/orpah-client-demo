/* sn.c — ORPAH SN 内核实现（逐字移植 orpah-over-halow/orpah_id.py）
 *
 * 移植时对每一处 Python 语义都做了标注（None→-1、True/False→1/0、异常→错误码），
 * 任何"看起来更顺手"的改写都会破坏与 Python 的零偏差 —— 别改，先跑 run_cross_test.py。
 */
#include "sn.h"

static size_t sn_len(const char *s)
{
    size_t n = 0;
    if (s == NULL) return 0;
    while (s[n] != '\0') n++;
    return n;
}

/* Crockford Base32 字母表 —— 与 orpah_id.CROCKFORD 逐字相同 */
static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int sn_crockford_index(char ch)
{
    int i;
    char c = ch;

    /* Python crockford_index 先 ch.upper()（且只接受长度 1 的 str） */
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (i = 0; i < 32; i++) {
        if (CROCKFORD[i] == c) return i;
    }
    return -1;                       /* Python: None */
}

char sn_crockford_char(int n)
{
    if (n < 0 || n > 31) return '\0';   /* Python crockford_encode 会抛异常 */
    return CROCKFORD[n];
}

/* ------------------------------------------------------------------ */
/* 正则等价物：                                                          */
/*   ^[A-Z]{2}-[0-9A-HJKMNP-TV-Z]{2,6}-[0-9A-HJKMNP-TV-Z]{8,16}         */
/*    (-[0-9A-HJKMNP-TV-Z]{1,2})?$                                       */
/*   ★ 正则**只收大写**（Python 侧也没有 re.IGNORECASE）⇒ 小写 SN = bad-format */
/* ------------------------------------------------------------------ */
static int is_cc_char(char c)
{
    return (c >= 'A' && c <= 'Z') ? 1 : 0;      /* [A-Z]：CC 不套 Crockford 限制 */
}

/* [0-9A-HJKMNP-TV-Z]：数字，或 A-H / J / K / M / N / P-Q-R-S-T / V-Z */
static int is_crockford_class_char(char c)
{
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'A' && c <= 'H') return 1;
    if (c == 'J' || c == 'K' || c == 'M' || c == 'N') return 1;
    if (c >= 'P' && c <= 'T') return 1;
    if (c >= 'V' && c <= 'Z') return 1;
    return 0;
}

static int field_ok(const char *s, size_t n, int lo, int hi, int (*pred)(char))
{
    size_t i;
    if (n < (size_t)lo || n > (size_t)hi) return 0;
    for (i = 0; i < n; i++) {
        if (!pred(s[i])) return 0;
    }
    return 1;
}

/* 把 SN 切成最多 4 段（不含 CC 的说法见 sn_parse）；返回段数，非法返回 -1 */
static int split_sn(const char *sn, char seg[4][SN_UNIQ_MAX + 1], size_t seglen[4])
{
    size_t i = 0, start = 0, n = 0;
    int parts = 0;

    if (sn == NULL || sn[0] == '\0') return -1;

    n = sn_len(sn);
    for (i = 0; i <= n; i++) {
        if (sn[i] == '-' || sn[i] == '\0') {
            size_t len = i - start;
            if (parts >= 4) return -1;                    /* 超过 4 段：正则不允许 */
            if (len == 0) return -1;                      /* 空段：正则不允许 */
            if (len > SN_UNIQ_MAX) return -1;             /* 段太长（UNIQUE 上限 16） */
            for (size_t k = 0; k < len; k++) seg[parts][k] = sn[start + k];
            seg[parts][len] = '\0';
            seglen[parts] = len;
            parts++;
            start = i + 1;
        }
    }
    return parts;
}

static int sn_structural_ok(const char *sn)
{
    char seg[4][SN_UNIQ_MAX + 1];
    size_t seglen[4];
    int parts = split_sn(sn, seg, seglen);

    if (parts != 3 && parts != 4) return 0;
    if (!field_ok(seg[0], seglen[0], SN_CC_LEN, SN_CC_LEN, is_cc_char)) return 0;
    if (!field_ok(seg[1], seglen[1], SN_ORG_MIN, SN_ORG_MAX, is_crockford_class_char)) return 0;
    if (!field_ok(seg[2], seglen[2], SN_UNIQ_MIN, SN_UNIQ_MAX, is_crockford_class_char)) return 0;
    if (parts == 4) {
        if (!field_ok(seg[3], seglen[3], 1, SN_CHECK_MAX, is_crockford_class_char)) return 0;
    }
    return 1;
}

int sn_ok(const char *sn)
{
    size_t n = sn_len(sn);
    if (n == 0 || n > SN_MAX_LEN) return 0;
    return sn_structural_ok(sn);
}

const char *sn_err(const char *sn)
{
    size_t n = sn_len(sn);

    if (sn == NULL || n == 0) return "empty";           /* Python: not sn → "empty" */
    if (n > SN_MAX_LEN) return "too-long";
    if (!sn_structural_ok(sn)) return "bad-format";
    return NULL;
}

int sn_parse(const char *sn, sn_parts_t *out)
{
    char seg[4][SN_UNIQ_MAX + 1];
    size_t seglen[4];
    int parts;

    if (out == NULL) return 0;
    out->cc[0] = out->org[0] = out->unique[0] = out->check[0] = '\0';
    if (!sn_ok(sn)) return 0;                           /* Python: parse_sn 先 sn_ok */

    parts = split_sn(sn, seg, seglen);
    if (parts < 3) return 0;

    {   /* 拷贝（长度已由 sn_ok 保证） */
        size_t i;
        for (i = 0; i < seglen[0] && i < SN_CC_LEN; i++) out->cc[i] = seg[0][i];
        out->cc[seglen[0] <= SN_CC_LEN ? seglen[0] : SN_CC_LEN] = '\0';

        for (i = 0; i < seglen[1] && i < SN_ORG_MAX; i++) out->org[i] = seg[1][i];
        out->org[seglen[1] <= SN_ORG_MAX ? seglen[1] : SN_ORG_MAX] = '\0';

        for (i = 0; i < seglen[2] && i < SN_UNIQ_MAX; i++) out->unique[i] = seg[2][i];
        out->unique[seglen[2] <= SN_UNIQ_MAX ? seglen[2] : SN_UNIQ_MAX] = '\0';

        if (parts == 4) {
            for (i = 0; i < seglen[3] && i < SN_CHECK_MAX; i++) out->check[i] = seg[3][i];
            out->check[seglen[3] <= SN_CHECK_MAX ? seglen[3] : SN_CHECK_MAX] = '\0';
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* CHECK 校验码                                                         */
/* ------------------------------------------------------------------ */

/* 对字母数字串（**跳过 '-'**）取模 97；A=10..Z=35。
 * ★ 与 Python _mod97 一致：只认大写 A-Z 与 0-9，其它字符**静默跳过**。 */
static int mod97(const char *s)
{
    int r = 0;
    size_t i = 0;

    if (s == NULL) return 0;
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];
        if (ch == '-') continue;
        if (ch >= '0' && ch <= '9') {
            r = (r * 10 + (ch - '0')) % 97;
        } else if (ch >= 'A' && ch <= 'Z') {
            r = (r * 100 + (ch - 'A' + 10)) % 97;
        }
    }
    return r;
}

/* Luhn mod 32 校验和（从右往左隔位翻倍）—— 对应 Python _luhn_sum */
static int luhn_sum32(const int *digits, int n)
{
    int s = 0, double_it = 0, k;
    for (k = n - 1; k >= 0; k--) {
        int d = digits[k];
        if (double_it) {
            int d2 = d * 2;
            s += d2 / 32 + d2 % 32;
        } else {
            s += d;
        }
        double_it = !double_it;
    }
    return s;
}

/* 把串里的 Crockford 字符收成数字（**跳过 '-'**）；含非法字符返回 -1 */
static int sn_collect_digits(const char *s, int *digits, int cap)
{
    int nd = 0;
    size_t i;

    if (s == NULL) return -1;
    for (i = 0; s[i] != '\0'; i++) {
        int d;
        if (s[i] == '-') continue;
        d = sn_crockford_index(s[i]);
        if (d < 0) return -1;                 /* Python 遇 None 会在求和时抛异常 */
        if (nd >= cap) return -1;
        digits[nd++] = d;
    }
    return nd;
}

int sn_compute_check_luhn32(const char *org_unique, char out[2])
{
    int digits[SN_MAX_LEN + 2];
    int nd, c;

    if (out == NULL) return -1;
    nd = sn_collect_digits(org_unique, digits,
                           (int)(sizeof(digits) / sizeof(digits[0])) - 1);
    if (nd < 0) return -1;

    for (c = 0; c < 32; c++) {
        digits[nd] = c;                       /* Python: digits + [c] */
        if (luhn_sum32(digits, nd + 1) % 32 == 0) {
            out[0] = sn_crockford_char(c);
            out[1] = '\0';
            return 0;
        }
    }
    out[0] = '0';                             /* Python 的兜底分支（理论不可达） */
    out[1] = '\0';
    return 0;
}

int sn_compute_check_mod97(const char *org_unique, char out[3])
{
    char buf[SN_ORG_MAX + SN_UNIQ_MAX + 4];
    size_t n, i;
    int v;

    if (org_unique == NULL || out == NULL) return -1;
    n = sn_len(org_unique);
    if (n + 3 > sizeof(buf)) return -1;

    /* ★ 照 Python 原样：**先把 "00" 追加到串尾**，再整体取模。
     *   不做任何代数化简 —— 化简看着等价，但会把"以后规范改了就没法对拍"的后路堵死。 */
    for (i = 0; i < n; i++) buf[i] = org_unique[i];
    buf[n] = '0';
    buf[n + 1] = '0';
    buf[n + 2] = '\0';

    v = 98 - mod97(buf);                      /* Python: 98 - _mod97(...) */
    out[0] = (char)('0' + (v / 10) % 10);     /* Python: "%02d" % v（v ∈ 2..98）*/
    out[1] = (char)('0' + (v % 10));
    out[2] = '\0';
    return 0;
}

int sn_verify_check_luhn32(const char *org_unique_check)
{
    int digits[SN_MAX_LEN + 2];
    int nd = sn_collect_digits(org_unique_check, digits,
                               (int)(sizeof(digits) / sizeof(digits[0])));

    if (nd < 0) return 0;                     /* Python: any None → False */
    return (luhn_sum32(digits, nd) % 32 == 0) ? 1 : 0;
}

int sn_verify_check_mod97(const char *org_unique_check)
{
    if (org_unique_check == NULL) return 0;
    return (mod97(org_unique_check) == 1) ? 1 : 0;   /* Python: _mod97(...) == 1 */
}

int sn_verify_check(const char *sn)
{
    sn_parts_t p;
    char body[SN_ORG_MAX + SN_UNIQ_MAX + SN_CHECK_MAX + 4];
    size_t n = 0, clen;

    if (!sn_parse(sn, &p)) return 0;
    clen = sn_len(p.check);
    if (clen == 0) return 1;                            /* 0 位 = 无校验 = 通过 */
    if (clen > SN_CHECK_MAX) return 0;

    /* body = "ORG-UNIQUE-CHECK"（**不含 CC**） */
    n = 0;
    for (size_t i = 0; p.org[i] != '\0'; i++) body[n++] = p.org[i];
    body[n++] = '-';
    for (size_t i = 0; p.unique[i] != '\0'; i++) body[n++] = p.unique[i];
    body[n++] = '-';
    for (size_t i = 0; p.check[i] != '\0'; i++) body[n++] = p.check[i];
    body[n] = '\0';

    if (clen == 1) return sn_verify_check_luhn32(body);
    return sn_verify_check_mod97(body);
}
