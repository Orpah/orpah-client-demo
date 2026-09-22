/* id_nonce.c — 软熵 nonce 后端（纯逻辑；无 stdio / 无 string.h / 无 malloc）。*/
#include "id_nonce.h"

#include "sha256.h"

#define IDN_PREFIX "orpah-nonce-soft-v1"

static unsigned long _slen(const char *s)
{
    unsigned long n = 0;

    if (s != 0) {
        while (s[n] != '\0') { n++; }
    }
    return n;
}

/* 无符号十进制（照 Python `str(int(x))`）；out 至少 11 字节；返回长度。*/
static unsigned long _udec(uint32_t v, char *out)
{
    char tmp[11];
    unsigned long n = 0;
    unsigned long i = 0;

    if (v == 0u) {
        tmp[n++] = '0';
    }
    while (v != 0u) {
        tmp[n++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    }
    while (n > 0u) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
    return i;
}

void idn_hex_upper(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789ABCDEF";
    size_t i;

    if (out == 0) { return; }
    if (in == 0) { out[0] = '\0'; return; }
    for (i = 0; i < n; i++) {
        out[i * 2u] = H[(in[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = H[in[i] & 0x0Fu];
    }
    out[n * 2u] = '\0';
}

int idn_soft_at(const char *sn, uint32_t boot_entropy, uint32_t ctr,
                char out[IDN_NONCE_LEN + 1])
{
    /* ⚠ 静态：单片 RAM 很紧 ⇒ 不把 200+ 字节的 SHA 上下文放栈上；代价是不可重入
     *   （固件单线程、CLI 单线程，够用）。*/
    static sha256_ctx_t c;
    static char e[11];
    static char k[11];
    static uint8_t dig[SHA256_DIGEST_LEN];
    unsigned long en, kn;
    const char *p;
    size_t snn;

    if (sn == 0 || out == 0 || sn[0] == '\0') {
        return IDN_E_ARG;
    }
    snn = (size_t)_slen(sn);
    if (snn > (size_t)IDN_SN_MAX) {
        return IDN_E_ARG;
    }
    en = _udec(boot_entropy, e);
    kn = _udec(ctr, k);

    sha256_init(&c);
    p = IDN_PREFIX;
    sha256_update(&c, p, _slen(p));
    sha256_update(&c, "|", 1);
    sha256_update(&c, sn, snn);
    sha256_update(&c, "|", 1);
    sha256_update(&c, e, en);
    sha256_update(&c, "|", 1);
    sha256_update(&c, k, kn);
    sha256_final(&c, dig);

    idn_hex_upper(dig, (size_t)IDN_NONCE_BYTES, out);      /* 只取前 16 字节 */
    return 0;
}

int idn_soft_init(idn_soft_t *st, const char *sn, uint32_t boot_entropy)
{
    size_t n;

    if (st == 0 || sn == 0 || sn[0] == '\0') {
        return IDN_E_ARG;
    }
    n = (size_t)_slen(sn);
    if (n > (size_t)IDN_SN_MAX) {
        return IDN_E_ARG;
    }
    /* 自写拷贝（proto/ 不许用 string.h）；`n <= IDN_SN_MAX` 已在上面查过 ⇒ 不会溢出 */
    {
        size_t i;

        for (i = 0; i < n; i++) {
            st->sn[i] = sn[i];
        }
        st->sn[n] = '\0';
    }
    st->boot_entropy = boot_entropy;
    st->ctr = 0u;
    return 0;
}

int idn_soft_next(idn_soft_t *st, char out[IDN_NONCE_LEN + 1])
{
    if (st == 0) {
        return IDN_E_ARG;
    }
    /* ★ 先自增再算：**第 0 号** nonce 用 ctr=1 ⇒ 计数器与"第几条"一致（好读数），
     *   且同一 boot 内严格递增 ⇒ 不会重复。*/
    st->ctr++;
    return idn_soft_at(st->sn, st->boot_entropy, st->ctr, out);
}
