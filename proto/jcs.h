/* jcs.h — RFC 8785 JCS 规范化（我们用到的那部分）+ 迷你 JSON 构建器
 *
 * 单一源：`orpah-over-halow/orpah_id.py`
 *     def jcs(obj): return json.dumps(obj, ensure_ascii=False,
 *                                     sort_keys=True, separators=(",", ":")).encode("utf-8")
 *     def preimage_of(report): return jcs({"hdr": report["hdr"], "payload": report["payload"]})
 *
 * ⚠ **必须与 Python 逐字节一致** —— 差一个字节，服务端就判 `signature_invalid`。
 *   所以这里不"顺便优化"，只照 Python 的行为实现：
 *     · 键**按字典序**（Python `sort_keys` 用的是码点序；对 UTF-8 来说**字节序 == 码点序**，
 *       故直接用逐字节比较。⚠ 非 BMP 字符（>U+FFFF）在真 JCS 里按 UTF-16 码元排，
 *       与 Python 会不同 —— 我们的报文键全是 ASCII，用不到，但**这条差异要记住**）
 *     · 无空格：分隔符就是 `,` 与 `:`
 *     · 字符串 `ensure_ascii=False`：非 ASCII 原样 UTF-8 输出；只转义 `"`、`\`、控制字符
 *     · 数字**只有整数**（我们的报文不用浮点）⇒ 本实现**不提供浮点**，从根上避免
 *       "Python 的最短往返表示"那种跨语言不一致
 *
 * ⚠ 不使用 stdio / malloc：裸机固件（`-nostdlib`）可直接用；内存由调用方给（arena）。
 */
#ifndef ORPAH_JCS_H
#define ORPAH_JCS_H

#include <stddef.h>

#define JCS_MAX_NODES   40
#define JCS_MAX_MEMBERS 12
#define JCS_MAX_ITEMS   6
#define JCS_POOL_SIZE   640
#define JCS_KEY_MAX     40

typedef enum { JV_NULL = 0, JV_BOOL, JV_INT, JV_STR, JV_ARR, JV_OBJ } jv_type_t;

typedef struct jv jv_t;
struct jv {
    jv_type_t type;
    union {
        int b;                                  /* JV_BOOL */
        long long i;                            /* JV_INT  */
        const char *s;                          /* JV_STR  */
        struct { jv_t *items[JCS_MAX_ITEMS]; int n; } arr;
        struct { const char *keys[JCS_MAX_MEMBERS]; jv_t *vals[JCS_MAX_MEMBERS]; int n; } obj;
    } u;
};

typedef struct {
    jv_t nodes[JCS_MAX_NODES];
    int n_nodes;
    char pool[JCS_POOL_SIZE];
    size_t pool_used;
} jcs_ctx_t;

/* 错误码（负值） */
#define JCS_E_NODES   (-1)      /* 节点用满 */
#define JCS_E_POOL    (-2)      /* 字符串池用满 */
#define JCS_E_CAP     (-3)      /* 成员/元素用满 */
#define JCS_E_TYPE    (-4)      /* 类型不对（例如往数组里 put 键值） */
#define JCS_E_DUPKEY  (-5)      /* 重复键（Python dict 不可能出现 => 必须报错） */
#define JCS_E_OUT     (-6)      /* 输出缓冲不够 */
#define JCS_E_KEYLEN  (-7)      /* 键太长 */

void  jcs_init(jcs_ctx_t *c);
jv_t *jcs_obj(jcs_ctx_t *c);                    /* 失败返回 NULL */
jv_t *jcs_arr(jcs_ctx_t *c);                    /* 失败返回 NULL */

/* 标量节点（**数组元素必须用它**：数组元素没有键，没法走 jcs_put_int 之类） */
jv_t *jcs_int(jcs_ctx_t *c, long long v);
jv_t *jcs_str(jcs_ctx_t *c, const char *v);
jv_t *jcs_bool(jcs_ctx_t *c, int v);
jv_t *jcs_null(jcs_ctx_t *c);

int   jcs_put(jcs_ctx_t *c, jv_t *o, const char *key, jv_t *v);
int   jcs_put_str(jcs_ctx_t *c, jv_t *o, const char *key, const char *v);
int   jcs_put_int(jcs_ctx_t *c, jv_t *o, const char *key, long long v);
int   jcs_put_bool(jcs_ctx_t *c, jv_t *o, const char *key, int v);
int   jcs_put_null(jcs_ctx_t *c, jv_t *o, const char *key);
int   jcs_add(jcs_ctx_t *c, jv_t *a, jv_t *v);

/* 规范化输出（**不加 NUL**，返回字节数；out 预留 NUL 的话调用方自己加）
 * 失败返回负的错误码。cap 不够返回 JCS_E_OUT（不写半截）。*/
int   jcs_encode(jcs_ctx_t *c, const jv_t *root, char *out, size_t cap);

/* **不排序**的 JSON 输出 —— 对应 `orpah_proto.encode_msg` 那个信封：
 *     json.dumps(msg, ensure_ascii=False, separators=(",", ":"))
 *   ⚠ 它**没有** sort_keys ⇒ 键按**插入序**。链路报文（REQ-CONNECT/REPORT/ID-REPORT）
 *     走这个；**只有签名预像**才用 JCS（jcs_encode）—— 两者不能混。
 *   其余行为（转义/整数/无空格）与 jcs_encode 完全一致。*/
int   jcs_encode_raw(const jv_t *root, char *out, size_t cap);

/* 签名预像 = JCS({"hdr":hdr,"payload":payload}) —— 对应 orpah_id.preimage_of */
int   jcs_preimage(jcs_ctx_t *c, const jv_t *hdr, const jv_t *payload,
                   char *out, size_t cap);

#endif /* ORPAH_JCS_H */
