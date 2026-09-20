/* p256.c — NIST P-256：素域（Montgomery 乘）+ Jacobian 群运算 + 标量乘 + 公钥派生
 *
 * 单一源 / 常数来历 / 有意不做的边界 ⇒ 见 p256.h 的文件头。
 * 判定方式：`p256-selftest` 用 **Python(OpenSSL) 派生的公钥**逐字节比对
 * （常数写错、域运算错、点运算错、标量乘错 —— 任一处错都过不了）。
 */
#include "p256.h"

#include "sha256.h"                 /* 仅用于演示私钥派生 */

/* ------------------------------------------------------------------ */
/* 常数（LE32 limb；来历见 p256.h）                                    */
/* ------------------------------------------------------------------ */
/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const uint32_t P_V[P256_NL] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
};
/* -p^{-1} mod 2^32 = 1（p 的低 32 位是 0xFFFFFFFF ⇒ p ≡ -1 mod 2^32）*/
#define P_INV32 1u

/* R^2 mod p（R = 2^256）；用于把 plain 值转进/转出 Montgomery 域 */
static const uint32_t R2_V[P256_NL] = {
    0x00000003u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFBu,
    0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFDu, 0x00000004u
};

/* b */
static const uint32_t B_V[P256_NL] = {
    0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
    0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u
};
/* Gx */
static const uint32_t GX_V[P256_NL] = {
    0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
    0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u
};
/* Gy */
static const uint32_t GY_V[P256_NL] = {
    0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
    0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u
};
/* n（= 上游 orpah_id._P256_ORDER，逐位相同）*/
static const uint32_t N_V[P256_NL] = {
    0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
};

/* ------------------------------------------------------------------ */
/* limb 小工具（不用 memcpy/memset：固件 -nostdlib；也没有结构体赋值） */
/* ------------------------------------------------------------------ */
static void copy_v(uint32_t *d, const uint32_t *s)
{
    int i;
    for (i = 0; i < P256_NL; i++) {
        d[i] = s[i];
    }
}

static int cmp_v(const uint32_t *a, const uint32_t *b)
{
    int i;
    for (i = P256_NL - 1; i >= 0; i--) {
        if (a[i] != b[i]) {
            return (a[i] > b[i]) ? 1 : -1;
        }
    }
    return 0;
}

static uint32_t add_v(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t carry = 0u;
    int i;
    for (i = 0; i < P256_NL; i++) {
        uint64_t s = (uint64_t)a[i] + (uint64_t)b[i] + (uint64_t)carry;
        r[i] = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
    return carry;
}

static uint32_t sub_v(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t borrow = 0u;
    int i;
    for (i = 0; i < P256_NL; i++) {
        uint64_t s = (uint64_t)a[i] - (uint64_t)b[i] - (uint64_t)borrow;
        r[i] = (uint32_t)s;
        borrow = (uint32_t)((s >> 32) & 1u);
    }
    return borrow;
}

/* r = a + x（x < 2^32）；返回进位 */
static uint32_t add_small(uint32_t *r, const uint32_t *a, uint32_t x)
{
    uint32_t carry = x;
    int i;
    for (i = 0; i < P256_NL && carry != 0u; i++) {
        uint64_t s = (uint64_t)a[i] + (uint64_t)carry;
        r[i] = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
    for (; i < P256_NL; i++) {
        r[i] = a[i];
    }
    return carry;
}

/* a -= x（x < 2^32）；假定 a >= x */
static void sub_small(uint32_t *a, uint32_t x)
{
    uint32_t borrow = x;
    int i;
    for (i = 0; i < P256_NL && borrow != 0u; i++) {
        uint64_t s = (uint64_t)a[i] - (uint64_t)borrow;
        a[i] = (uint32_t)s;
        borrow = (uint32_t)((s >> 32) & 1u);
    }
}

/* ------------------------------------------------------------------ */
/* 素域                                                               */
/* ------------------------------------------------------------------ */
void p256_fe_zero(p256_fe_t *r)
{
    int i;
    for (i = 0; i < P256_NL; i++) {
        r->v[i] = 0u;
    }
}

void p256_fe_one(p256_fe_t *r)
{
    p256_fe_zero(r);
    r->v[0] = 1u;
}

void p256_fe_from_bytes(p256_fe_t *r, const uint8_t be[P256_BYTES])
{
    int i;
    for (i = 0; i < P256_NL; i++) {
        const uint8_t *q = be + (P256_NL - 1 - i) * 4;      /* 最低 limb 取最后 4 字节 */
        r->v[i] = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                  ((uint32_t)q[2] << 8) | (uint32_t)q[3];
    }
}

void p256_fe_to_bytes(uint8_t be[P256_BYTES], const p256_fe_t *a)
{
    int i;
    for (i = 0; i < P256_NL; i++) {
        uint32_t w = a->v[P256_NL - 1 - i];
        be[i * 4 + 0] = (uint8_t)(w >> 24);
        be[i * 4 + 1] = (uint8_t)(w >> 16);
        be[i * 4 + 2] = (uint8_t)(w >> 8);
        be[i * 4 + 3] = (uint8_t)w;
    }
}

int p256_fe_is_zero(const p256_fe_t *a)
{
    int i;
    for (i = 0; i < P256_NL; i++) {
        if (a->v[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

int p256_fe_cmp(const p256_fe_t *a, const p256_fe_t *b)
{
    return cmp_v(a->v, b->v);
}

void p256_fe_add(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b)
{
    uint32_t t[P256_NL];
    uint32_t carry = add_v(t, a->v, b->v);
    /* a,b < p ⇒ a+b < 2p ⇒ 至多减一次 p；有进位时 t < p（见 p256.c 注释/对拍） */
    if (carry != 0u || cmp_v(t, P_V) >= 0) {
        (void)sub_v(t, t, P_V);
    }
    copy_v(r->v, t);
}

void p256_fe_sub(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b)
{
    uint32_t t[P256_NL];
    uint32_t borrow = sub_v(t, a->v, b->v);
    if (borrow != 0u) {
        (void)add_v(t, t, P_V);
    }
    copy_v(r->v, t);
}

/* Montgomery 乘：r = a*b*R^{-1} mod p（内部用；对外语义是 plain，见 fe_mul） */
static void mont_mul(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b)
{
    uint32_t t[2 * P256_NL + 2];
    int i, j, k;

    for (i = 0; i < 2 * P256_NL + 2; i++) {
        t[i] = 0u;
    }
    /* t = a * b（8×8 → 16 limbs） */
    for (i = 0; i < P256_NL; i++) {
        uint64_t carry = 0u;
        for (j = 0; j < P256_NL; j++) {
            uint64_t s = (uint64_t)a->v[i] * (uint64_t)b->v[j] +
                         (uint64_t)t[i + j] + carry;
            t[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        k = i + P256_NL;
        while (carry != 0u && k < 2 * P256_NL + 2) {
            uint64_t s = (uint64_t)t[k] + carry;
            t[k] = (uint32_t)s;
            carry = s >> 32;
            k++;
        }
    }
    /* Montgomery 归约（SOS）：NL 轮，每轮把 t[i] 消成 0 */
    for (i = 0; i < P256_NL; i++) {
        uint32_t m = t[i] * P_INV32;
        uint64_t carry = 0u;
        for (j = 0; j < P256_NL; j++) {
            uint64_t s = (uint64_t)m * (uint64_t)P_V[j] +
                         (uint64_t)t[i + j] + carry;
            t[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        k = i + P256_NL;
        while (carry != 0u && k < 2 * P256_NL + 2) {
            uint64_t s = (uint64_t)t[k] + carry;
            t[k] = (uint32_t)s;
            carry = s >> 32;
            k++;
        }
    }
    /* 结果在 t[NL..]；标准结论：< 2p ⇒ 至多减一次 p */
    for (i = 0; i < P256_NL; i++) {
        r->v[i] = t[P256_NL + i];
    }
    if (t[2 * P256_NL] != 0u || cmp_v(r->v, P_V) >= 0) {
        (void)sub_v(r->v, r->v, P_V);
    }
}

void p256_fe_mul(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b)
{
    p256_fe_t t, r2;
    copy_v(r2.v, R2_V);
    mont_mul(&t, a, b);          /* = a*b*R^{-1}（在 Montgomery 域里"乘一下"）*/
    mont_mul(r, &t, &r2);        /* × R^2 ⇒ 回到 plain 域的 a*b */
}

void p256_fe_sqr(p256_fe_t *r, const p256_fe_t *a)
{
    p256_fe_mul(r, a, a);
}

void p256_fe_inv(p256_fe_t *r, const p256_fe_t *a)
{
    /* a^(p-2) mod p（p 是素数 ⇒ 费马小定理）。a=0 时按 0 处理（不抛）。 */
    uint32_t e[P256_NL];
    p256_fe_t base, acc;
    int i;

    if (p256_fe_is_zero(a)) {
        p256_fe_zero(r);
        return;
    }
    copy_v(e, P_V);
    sub_small(e, 2u);                     /* e = p - 2 */
    copy_v(base.v, a->v);
    p256_fe_one(&acc);
    for (i = 255; i >= 0; i--) {
        p256_fe_sqr(&acc, &acc);
        if (((e[i / 32] >> (uint32_t)(i % 32)) & 1u) != 0u) {
            p256_fe_mul(&acc, &acc, &base);
        }
    }
    copy_v(r->v, acc.v);
}

/* a = p - 3（P-256 的曲线系数） */
static void fe_a(p256_fe_t *r)
{
    copy_v(r->v, P_V);
    sub_small(r->v, 3u);
}

/* ------------------------------------------------------------------ */
/* 群（Jacobian；z == 0 表示无穷远点）                                */
/* ------------------------------------------------------------------ */
typedef struct { p256_fe_t x, y, z; } jac_t;

static void jac_copy(jac_t *r, const jac_t *s)
{
    copy_v(r->x.v, s->x.v);
    copy_v(r->y.v, s->y.v);
    copy_v(r->z.v, s->z.v);
}

static void jac_set_inf(jac_t *r)
{
    p256_fe_zero(&r->x);
    p256_fe_one(&r->y);
    p256_fe_zero(&r->z);
}

static int jac_is_inf(const jac_t *p)
{
    return p256_fe_is_zero(&p->z);
}

static void jac_from_affine(jac_t *r, const p256_fe_t *x, const p256_fe_t *y)
{
    copy_v(r->x.v, x->v);
    copy_v(r->y.v, y->v);
    p256_fe_one(&r->z);
}

/* 倍点（通用 a）：X3 = M^2-2S, Y3 = M(S-X3)-8Y^4, Z3 = 2YZ，其中 S=4XY^2, M=3X^2+aZ^4 */
static void jac_double(jac_t *r, const jac_t *p)
{
    p256_fe_t a_fe, y2, s, m, x3, y3, y4, t, u;

    if (jac_is_inf(p)) {
        jac_set_inf(r);
        return;
    }
    fe_a(&a_fe);
    p256_fe_sqr(&y2, &p->y);              /* Y^2 */
    p256_fe_mul(&s, &p->x, &y2);          /* X*Y^2 */
    p256_fe_add(&s, &s, &s);
    p256_fe_add(&s, &s, &s);              /* S = 4XY^2 */
    p256_fe_sqr(&m, &p->x);               /* X^2 */
    p256_fe_add(&t, &m, &m);
    p256_fe_add(&m, &m, &t);              /* 3X^2 */
    p256_fe_sqr(&u, &p->z);
    p256_fe_sqr(&u, &u);                  /* Z^4 */
    p256_fe_mul(&u, &u, &a_fe);
    p256_fe_add(&m, &m, &u);              /* M = 3X^2 + aZ^4 */
    p256_fe_sqr(&x3, &m);
    p256_fe_add(&t, &s, &s);              /* 2S */
    p256_fe_sub(&x3, &x3, &t);            /* X3 = M^2 - 2S */
    p256_fe_sqr(&y4, &y2);                /* Y^4 */
    p256_fe_sub(&t, &s, &x3);
    p256_fe_mul(&t, &m, &t);              /* M(S-X3) */
    p256_fe_add(&u, &y4, &y4);
    p256_fe_add(&u, &u, &u);
    p256_fe_add(&u, &u, &u);              /* 8Y^4 */
    p256_fe_sub(&y3, &t, &u);             /* Y3 */
    p256_fe_mul(&u, &p->y, &p->z);
    p256_fe_add(&u, &u, &u);              /* Z3 = 2YZ */
    copy_v(r->x.v, x3.v);
    copy_v(r->y.v, y3.v);
    copy_v(r->z.v, u.v);
}

/* 点加（Jacobian+Jacobian；含两点的所有退化情形） */
static void jac_add(jac_t *r, const jac_t *a, const jac_t *b)
{
    p256_fe_t z1z1, z2z2, u1, u2, s1, s2, h, rr, hh, hhh, v, x3, y3, z3, t;

    if (jac_is_inf(a)) {
        jac_copy(r, b);
        return;
    }
    if (jac_is_inf(b)) {
        jac_copy(r, a);
        return;
    }
    p256_fe_sqr(&z1z1, &a->z);
    p256_fe_sqr(&z2z2, &b->z);
    p256_fe_mul(&u1, &a->x, &z2z2);
    p256_fe_mul(&u2, &b->x, &z1z1);
    p256_fe_mul(&s1, &a->y, &z2z2);
    p256_fe_mul(&s1, &s1, &b->z);
    p256_fe_mul(&s2, &b->y, &z1z1);
    p256_fe_mul(&s2, &s2, &a->z);
    p256_fe_sub(&h, &u2, &u1);
    p256_fe_sub(&rr, &s2, &s1);
    if (p256_fe_is_zero(&h)) {
        if (p256_fe_is_zero(&rr)) {
            jac_double(r, a);           /* 同一点 */
        } else {
            jac_set_inf(r);             /* 互为逆元 */
        }
        return;
    }
    p256_fe_sqr(&hh, &h);
    p256_fe_mul(&hhh, &hh, &h);
    p256_fe_mul(&v, &u1, &hh);
    p256_fe_sqr(&x3, &rr);
    p256_fe_sub(&x3, &x3, &hhh);
    p256_fe_add(&t, &v, &v);
    p256_fe_sub(&x3, &x3, &t);                     /* X3 = R^2 - H^3 - 2V */
    p256_fe_sub(&t, &v, &x3);
    p256_fe_mul(&t, &rr, &t);
    p256_fe_mul(&s1, &s1, &hhh);
    p256_fe_sub(&y3, &t, &s1);                     /* Y3 = R(V-X3) - S1*H^3 */
    p256_fe_mul(&z3, &a->z, &b->z);
    p256_fe_mul(&z3, &z3, &h);                     /* Z3 = Z1*Z2*H */
    copy_v(r->x.v, x3.v);
    copy_v(r->y.v, y3.v);
    copy_v(r->z.v, z3.v);
}

/* 标量乘：Montgomery ladder（R0=k*P, R1=R0+P 恒成立 ⇒ 不会撞上"加倍"分支）。
 * ⚠ 有数据相关分支与访存 ⇒ **不是常量时间**（见 p256.h 的边界说明）。 */
static void jac_mul(jac_t *r, const jac_t *p, const uint8_t k_be[P256_BYTES])
{
    jac_t r0, r1;
    int i;

    jac_set_inf(&r0);
    jac_copy(&r1, p);
    for (i = 255; i >= 0; i--) {
        uint32_t bit = (uint32_t)((k_be[31 - i / 8] >> (uint32_t)(i % 8)) & 1u);
        if (bit == 0u) {
            jac_add(&r1, &r0, &r1);
            jac_double(&r0, &r0);
        } else {
            jac_add(&r0, &r0, &r1);
            jac_double(&r1, &r1);
        }
    }
    jac_copy(r, &r0);
}

/* Jacobian → 仿射，写出 X||Y（各 32 B 大端） */
static void jac_to_affine(uint8_t out64[2 * P256_BYTES], const jac_t *p)
{
    p256_fe_t zi, zi2, zi3, ax, ay;

    p256_fe_inv(&zi, &p->z);
    p256_fe_sqr(&zi2, &zi);
    p256_fe_mul(&zi3, &zi2, &zi);
    p256_fe_mul(&ax, &p->x, &zi2);
    p256_fe_mul(&ay, &p->y, &zi3);
    p256_fe_to_bytes(out64, &ax);
    p256_fe_to_bytes(out64 + P256_BYTES, &ay);
}

int p256_point_on_curve(const uint8_t x_be[P256_BYTES], const uint8_t y_be[P256_BYTES])
{
    p256_fe_t x, y, lhs, rhs, b, t;

    if (x_be == NULL || y_be == NULL) {
        return 0;
    }
    p256_fe_from_bytes(&x, x_be);
    p256_fe_from_bytes(&y, y_be);
    p256_fe_sqr(&lhs, &y);                 /* y^2 */
    p256_fe_sqr(&rhs, &x);
    p256_fe_mul(&rhs, &rhs, &x);           /* x^3 */
    fe_a(&t);
    p256_fe_mul(&t, &t, &x);               /* a*x */
    p256_fe_add(&rhs, &rhs, &t);
    copy_v(b.v, B_V);
    p256_fe_add(&rhs, &rhs, &b);           /* x^3 + a*x + b */
    return p256_fe_cmp(&lhs, &rhs) == 0 ? 1 : 0;
}

int p256_point_mul(uint8_t out64[2 * P256_BYTES], const uint8_t k_be[P256_BYTES],
                   const uint8_t px_be[P256_BYTES], const uint8_t py_be[P256_BYTES])
{
    p256_fe_t x, y;
    jac_t p, r;
    int i;

    if (out64 == NULL || k_be == NULL || px_be == NULL || py_be == NULL) {
        return P256_E_ARG;
    }
    for (i = 0; i < P256_BYTES; i++) {
        if (k_be[i] != 0u) {
            break;
        }
    }
    if (i == P256_BYTES) {
        return P256_E_ZERO;
    }
    if (!p256_point_on_curve(px_be, py_be)) {
        return P256_E_CURVE;
    }
    p256_fe_from_bytes(&x, px_be);
    p256_fe_from_bytes(&y, py_be);
    jac_from_affine(&p, &x, &y);
    jac_mul(&r, &p, k_be);
    if (jac_is_inf(&r)) {
        return P256_E_ZERO;
    }
    jac_to_affine(out64, &r);
    return 0;
}

int p256_pubkey_from_priv(uint8_t out65[65], const uint8_t d_be[P256_BYTES])
{
    uint8_t gxy[2 * P256_BYTES], xy[2 * P256_BYTES];
    p256_fe_t gx, gy;
    int rc, i;

    if (out65 == NULL || d_be == NULL) {
        return P256_E_ARG;
    }
    copy_v(gx.v, GX_V);
    copy_v(gy.v, GY_V);
    p256_fe_to_bytes(gxy, &gx);
    p256_fe_to_bytes(gxy + P256_BYTES, &gy);
    /* 用独立的输入/输出缓冲（不让 out65 同时当输入，省得依赖"先读后写"这种微妙前提）*/
    rc = p256_point_mul(xy, d_be, gxy, gxy + P256_BYTES);
    if (rc != 0) {
        return rc;
    }
    out65[0] = 0x04u;
    for (i = 0; i < 2 * P256_BYTES; i++) {
        out65[1 + i] = xy[i];
    }
    return 0;
}

void p256_order_bytes(uint8_t n_be[P256_BYTES])
{
    p256_fe_t t;
    if (n_be == NULL) {
        return;
    }
    copy_v(t.v, N_V);
    p256_fe_to_bytes(n_be, &t);
}

int p256_demo_priv_from_sn(uint8_t d_be[P256_BYTES], const char *sn, int gen)
{
    /* d = (SHA256("orpah-demo-ec-p256-v1|" + sn + "|" + gen) 大端整数) mod (n-1) + 1
     * ⚠ **只是演示**（真机私钥在 SE 内生成、不可导出）—— 与上游 derive_demo_privkey 逐位一致。*/
    static const char PREFIX[] = "orpah-demo-ec-p256-v1|";
    uint8_t buf[128];
    uint8_t dig[P256_BYTES];
    uint32_t n1[P256_NL], val[P256_NL];
    p256_fe_t v;
    size_t n = 0;
    int i;

    if (d_be == NULL || sn == NULL) {
        return P256_E_ARG;
    }
    for (i = 0; PREFIX[i] != '\0'; i++) {
        buf[n++] = (uint8_t)PREFIX[i];
    }
    for (i = 0; sn[i] != '\0'; i++) {
        if (n + 16 >= sizeof(buf)) {
            return P256_E_ARG;
        }
        buf[n++] = (uint8_t)sn[i];
    }
    buf[n++] = (uint8_t)'|';
    /* gen（十进制，非负） */
    {
        char rev[12];
        int k = 0;
        int g = (gen < 0) ? -gen : gen;
        if (g == 0) {
            rev[k++] = '0';
        }
        while (g > 0 && k < (int)sizeof(rev)) {
            rev[k++] = (char)('0' + (g % 10));
            g /= 10;
        }
        while (k > 0) {
            buf[n++] = (uint8_t)rev[--k];
        }
    }
    /* sha256() 返回 void（不会失败）—— 上一版按 int 写了，MSVC 当场报 C2186 */
    sha256(buf, n, dig);
    p256_fe_from_bytes(&v, dig);          /* 这里当"256 位大端整数"用（不是域元素）*/
    copy_v(val, v.v);
    copy_v(n1, N_V);
    sub_small(n1, 1u);                     /* n - 1 */
    /* val < 2^256 且 n-1 > 2^255 ⇒ 商至多 1 ⇒ 一次条件减即可 */
    if (cmp_v(val, n1) >= 0) {
        (void)sub_v(val, val, n1);
    }
    (void)add_small(val, val, 1u);         /* d = val + 1 ∈ [1, n-1] */
    copy_v(v.v, val);
    p256_fe_to_bytes(d_be, &v);
    return 0;
}
