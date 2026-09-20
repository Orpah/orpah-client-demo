/* sn.h — ORPAH SN 内核（C99；**无 malloc / 无 stdio**，可编进裸机固件）
 *
 * 单一源：逐字移植自上游参考实现 `orpah-over-halow/orpah_id.py`
 *   · Crockford 字母表 / SN_MAX_LEN / SN 正则 / parse_sn / sn_ok / sn_err
 *   · compute_check_luhn32 / verify_check_luhn32 / compute_check_mod97 / verify_check_mod97
 *   · verify_check（整串 SN 按**校验位长度**分流：0 位=通过、1 位=Luhn32、2 位=Mod97）
 *
 * ⚠ 本仓**不定义**协议/校验算法：与 Python 有任何出入都以 Python 为准（跑 run_cross_test.py）。
 * ⚠ 校验位只算 ORG-UNIQUE（**不含 CC**）—— 这是规范 §2 的硬规则。
 */
#ifndef ORPAH_SN_H
#define ORPAH_SN_H

#include <stddef.h>

#define SN_MAX_LEN     32       /* orpah_id.SN_MAX_LEN */
#define SN_CC_LEN      2
#define SN_ORG_MIN     2
#define SN_ORG_MAX     6
#define SN_UNIQ_MIN    8
#define SN_UNIQ_MAX    16
#define SN_CHECK_MAX   2

typedef struct {
    char cc[SN_CC_LEN + 1];
    char org[SN_ORG_MAX + 1];
    char unique[SN_UNIQ_MAX + 1];
    char check[SN_CHECK_MAX + 1];   /* 无校验位时为空串 */
} sn_parts_t;

/* Crockford Base32（去易混 I L O U） */
int  sn_crockford_index(char ch);       /* 0..31；非法 → -1（对应 Python 的 None） */
char sn_crockford_char(int n);          /* 0..31 → 字符；越界 → '\0' */

/* SN 合法性：对应 Python sn_ok / sn_err（后者合法返回 NULL） */
int          sn_ok(const char *sn);
const char  *sn_err(const char *sn);    /* "empty" / "too-long" / "bad-format" / NULL */

/* 解析：成功返回 1（对应 Python parse_sn 返回 dict / None） */
int  sn_parse(const char *sn, sn_parts_t *out);

/* 校验位：成功返回 0；输入含非法 Crockford 字符返回 -1 */
int  sn_compute_check_luhn32(const char *org_unique, char out[2]);
int  sn_compute_check_mod97(const char *org_unique, char out[3]);

/* 校验：1 = 通过，0 = 不通过（非法字符也算不通过，对应 Python 的 False） */
int  sn_verify_check_luhn32(const char *org_unique_check);
int  sn_verify_check_mod97(const char *org_unique_check);

/* 整串 SN 校验（含 CC 与可选校验位）—— 对外就调它 */
int  sn_verify_check(const char *sn);

#endif /* ORPAH_SN_H */
