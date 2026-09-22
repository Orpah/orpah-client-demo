/* id_keys.c — 演示密钥材料（纯逻辑；无 stdio / 无 string.h / 无 malloc）。*/
#include "id_keys.h"

#include "sha256.h"

/* Python: b"orpah-demo-hmac-v1|" + sn + b"|" + str(int(gen)) */
#define IDK_HMAC_PREFIX "orpah-demo-hmac-v1"

static unsigned long _slen(const char *s)
{
    unsigned long n = 0;

    if (s != 0) {
        while (s[n] != '\0') { n++; }
    }
    return n;
}

/* 十进制（照 `str(int(gen))`：负数带 '-'）。out 至少 12 字节；返回长度。*/
static unsigned long _dec(int gen, char *out)
{
    char tmp[12];
    unsigned long n = 0;
    unsigned int v;
    int neg = (gen < 0);
    unsigned long i = 0;

    v = neg ? (unsigned int)(-(long)gen) : (unsigned int)gen;
    if (v == 0u) {
        tmp[n++] = '0';
    }
    while (v != 0u) {
        tmp[n++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    }
    if (neg) {
        out[i++] = '-';
    }
    while (n > 0u) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
    return i;
}

int idk_demo_ec(const char *sn, int gen, uint8_t out32[P256_BYTES])
{
    if (sn == 0 || out32 == 0 || sn[0] == '\0') {
        return IDK_E_ARG;
    }
    /* 曲线私钥的派生式**不在这里重写** —— 一处实现，见 p256.c（同上游 derive_demo_privkey）。*/
    return p256_demo_priv_from_sn(out32, sn, gen);
}

int idk_demo_hmac(const char *sn, int gen, uint8_t out32[32])
{
    /* ⚠ 静态：单片 RAM 很紧（CH32V203 = 20 KB），别把 200+ 字节的 SHA 上下文放栈上；
     *   也意味着本函数**不可重入**（固件单线程 + CLI 单线程，够用；要并发得自己加锁）。*/
    static sha256_ctx_t c;
    static char g[12];
    unsigned long gn;

    if (sn == 0 || out32 == 0 || sn[0] == '\0') {
        return IDK_E_ARG;
    }
    gn = _dec(gen, g);

    sha256_init(&c);
    sha256_update(&c, IDK_HMAC_PREFIX, _slen(IDK_HMAC_PREFIX));
    sha256_update(&c, "|", 1);
    sha256_update(&c, sn, _slen(sn));          /* ensure_ascii=False ⇒ 原样 UTF-8 字节 */
    sha256_update(&c, "|", 1);
    sha256_update(&c, g, gn);
    sha256_final(&c, out32);
    return 0;
}
