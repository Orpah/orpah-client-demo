/* jcs.c — JCS 规范化实现（逐条对齐 Python json.dumps(ensure_ascii=False,
 *          sort_keys=True, separators=(",",":"))；见 jcs.h 的说明与 proto/README.md）
 *
 * 不使用 stdio / malloc / 浮点 —— 裸机 `-nostdlib` 可直接编。
 */
#include "jcs.h"

/* ------------------------------------------------------------------ */
/* 小工具（不用 libc）                                                   */
/* ------------------------------------------------------------------ */
static size_t j_len(const char *s)
{
    size_t n = 0;
    if (s == NULL) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static int j_strcmp(const char *a, const char *b)      /* 逐字节（UTF-8 下 == 码点序） */
{
    size_t i = 0;
    for (;;) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (ca < cb) ? -1 : 1;
        if (ca == '\0') return 0;
        i++;
    }
}

void jcs_init(jcs_ctx_t *c)
{
    if (c == NULL) return;
    c->n_nodes = 0;
    c->pool_used = 0;
    c->pool[0] = '\0';
}

static jv_t *j_new(jcs_ctx_t *c, jv_type_t t)
{
    jv_t *n;

    if (c == NULL || c->n_nodes >= JCS_MAX_NODES) return NULL;
    n = &c->nodes[c->n_nodes++];
    n->type = t;
    n->u.arr.n = 0;
    n->u.obj.n = 0;
    return n;
}

jv_t *jcs_obj(jcs_ctx_t *c) { return j_new(c, JV_OBJ); }
jv_t *jcs_arr(jcs_ctx_t *c) { return j_new(c, JV_ARR); }

/* 把字符串（含结尾 NUL）拷进 pool */
static const char *j_dup(jcs_ctx_t *c, const char *s)
{
    size_t n;
    char *p;

    if (c == NULL || s == NULL) return NULL;
    n = j_len(s) + 1;
    if (c->pool_used + n > JCS_POOL_SIZE) return NULL;
    p = &c->pool[c->pool_used];
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    c->pool_used += n;
    return p;
}

static int j_find_key(const jv_t *o, const char *key)
{
    int i;
    for (i = 0; i < o->u.obj.n; i++) {
        if (j_strcmp(o->u.obj.keys[i], key) == 0) return i;
    }
    return -1;
}

int jcs_put(jcs_ctx_t *c, jv_t *o, const char *key, jv_t *v)
{
    const char *k;

    if (c == NULL || o == NULL || key == NULL || v == NULL) return JCS_E_TYPE;
    if (o->type != JV_OBJ) return JCS_E_TYPE;
    if (o->u.obj.n >= JCS_MAX_MEMBERS) return JCS_E_CAP;
    if (j_len(key) > JCS_KEY_MAX) return JCS_E_KEYLEN;
    if (j_find_key(o, key) >= 0) return JCS_E_DUPKEY;   /* Python dict 不会重复 => 报了才安全 */
    k = j_dup(c, key);
    if (k == NULL) return JCS_E_POOL;
    o->u.obj.keys[o->u.obj.n] = k;
    o->u.obj.vals[o->u.obj.n] = v;
    o->u.obj.n++;
    return 0;
}

/* 标量节点构造器（数组元素必须用它） */
jv_t *jcs_int(jcs_ctx_t *c, long long v)
{
    jv_t *n = j_new(c, JV_INT);
    if (n != NULL) n->u.i = v;
    return n;
}

jv_t *jcs_str(jcs_ctx_t *c, const char *v)
{
    const char *s;
    jv_t *n;

    if (v == NULL) return NULL;
    s = j_dup(c, v);
    if (s == NULL) return NULL;
    n = j_new(c, JV_STR);
    if (n != NULL) n->u.s = s;
    return n;
}

jv_t *jcs_bool(jcs_ctx_t *c, int v)
{
    jv_t *n = j_new(c, JV_BOOL);
    if (n != NULL) n->u.b = v ? 1 : 0;
    return n;
}

jv_t *jcs_null(jcs_ctx_t *c)
{
    return j_new(c, JV_NULL);
}

int jcs_put_str(jcs_ctx_t *c, jv_t *o, const char *key, const char *v)
{
    jv_t *n = jcs_str(c, v);
    return (n == NULL) ? JCS_E_POOL : jcs_put(c, o, key, n);
}

int jcs_put_int(jcs_ctx_t *c, jv_t *o, const char *key, long long v)
{
    jv_t *n = jcs_int(c, v);
    return (n == NULL) ? JCS_E_NODES : jcs_put(c, o, key, n);
}

int jcs_put_bool(jcs_ctx_t *c, jv_t *o, const char *key, int v)
{
    jv_t *n = jcs_bool(c, v);
    return (n == NULL) ? JCS_E_NODES : jcs_put(c, o, key, n);
}

int jcs_put_null(jcs_ctx_t *c, jv_t *o, const char *key)
{
    jv_t *n = jcs_null(c);
    return (n == NULL) ? JCS_E_NODES : jcs_put(c, o, key, n);
}

int jcs_add(jcs_ctx_t *c, jv_t *a, jv_t *v)
{
    (void)c;
    if (a == NULL || v == NULL) return JCS_E_TYPE;
    if (a->type != JV_ARR) return JCS_E_TYPE;
    if (a->u.arr.n >= JCS_MAX_ITEMS) return JCS_E_CAP;
    a->u.arr.items[a->u.arr.n++] = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 输出缓冲                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char *buf;
    size_t cap;
    size_t n;
    int err;                    /* 0 = 正常；负值 = 错误码 */
} out_t;

static void o_byte(out_t *o, char ch)
{
    if (o->err) return;
    if (o->n >= o->cap) { o->err = JCS_E_OUT; return; }
    o->buf[o->n++] = ch;
}

static void o_str(out_t *o, const char *s)
{
    if (s == NULL) { o->err = JCS_E_TYPE; return; }
    while (*s != '\0') o_byte(o, *s++);
}

static void o_ll(out_t *o, long long v)
{
    char tmp[24];
    int k = 0;
    unsigned long long mag;

    /* 手写整数转换：JSON 整数就是十进制最短表示（无前导零、无正号） */
    if (v < 0) {
        o_byte(o, '-');
        mag = (unsigned long long)(-(v + 1)) + 1ULL;   /* 避免 INT64_MIN 取负溢出 */
    } else {
        mag = (unsigned long long)v;
    }
    if (mag == 0) { o_byte(o, '0'); return; }
    while (mag > 0 && k < (int)sizeof(tmp)) {
        tmp[k++] = (char)('0' + (int)(mag % 10ULL));
        mag /= 10ULL;
    }
    while (k > 0) o_byte(o, tmp[--k]);
}

static void o_hex4(out_t *o, unsigned int v)
{
    static const char H[] = "0123456789abcdef";      /* Python json 用小写 */
    o_byte(o, '\\');
    o_byte(o, 'u');
    o_byte(o, H[(v >> 12) & 0xF]);
    o_byte(o, H[(v >> 8) & 0xF]);
    o_byte(o, H[(v >> 4) & 0xF]);
    o_byte(o, H[v & 0xF]);
}

/* JSON 字符串：ensure_ascii=False ⇒ 非 ASCII 原样 UTF-8；控制字符转义 */
static void o_json_str(out_t *o, const char *s)
{
    o_byte(o, '"');
    while (*s != '\0') {
        unsigned char ch = (unsigned char)*s++;
        switch (ch) {
        case '"':  o_str(o, "\\\""); break;
        case '\\': o_str(o, "\\\\"); break;
        case '\b': o_str(o, "\\b"); break;
        case '\f': o_str(o, "\\f"); break;
        case '\n': o_str(o, "\\n"); break;
        case '\r': o_str(o, "\\r"); break;
        case '\t': o_str(o, "\\t"); break;
        default:
            if (ch < 0x20) o_hex4(o, ch);
            else           o_byte(o, (char)ch);      /* 含 >=0x80：原样 UTF-8 字节 */
            break;
        }
    }
    o_byte(o, '"');
}

/* ------------------------------------------------------------------ */
/* 递归编码（对象成员先排序）                                            */
/* ------------------------------------------------------------------ */
static void enc(out_t *o, const jv_t *v);

static void enc_obj(out_t *o, const jv_t *v)
{
    int idx[JCS_MAX_MEMBERS];
    int n = v->u.obj.n;

    for (int i = 0; i < n; i++) idx[i] = i;
    /* 插入排序：键按逐字节字典序（= Python 的码点序） */
    for (int i = 1; i < n; i++) {
        int cur = idx[i], j = i - 1;
        while (j >= 0 && j_strcmp(v->u.obj.keys[idx[j]], v->u.obj.keys[cur]) > 0) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = cur;
    }

    o_byte(o, '{');
    for (int i = 0; i < n; i++) {
        if (i) o_byte(o, ',');
        o_json_str(o, v->u.obj.keys[idx[i]]);
        o_byte(o, ':');
        enc(o, v->u.obj.vals[idx[i]]);
    }
    o_byte(o, '}');
}

static void enc(out_t *o, const jv_t *v)
{
    if (o->err) return;
    if (v == NULL) { o->err = JCS_E_TYPE; return; }
    switch (v->type) {
    case JV_NULL: o_str(o, "null"); break;
    case JV_BOOL: o_str(o, v->u.b ? "true" : "false"); break;
    case JV_INT:  o_ll(o, v->u.i); break;
    case JV_STR:  o_json_str(o, v->u.s); break;
    case JV_ARR:
        o_byte(o, '[');
        for (int i = 0; i < v->u.arr.n; i++) {
            if (i) o_byte(o, ',');
            enc(o, v->u.arr.items[i]);
        }
        o_byte(o, ']');
        break;
    case JV_OBJ:  enc_obj(o, v); break;
    default:      o->err = JCS_E_TYPE; break;
    }
}

int jcs_encode(jcs_ctx_t *c, const jv_t *root, char *out, size_t cap)
{
    out_t o;

    (void)c;
    if (out == NULL || cap == 0) return JCS_E_OUT;
    o.buf = out;
    o.cap = cap;
    o.n = 0;
    o.err = 0;
    enc(&o, root);
    if (o.err) return o.err;
    return (int)o.n;
}

int jcs_preimage(jcs_ctx_t *c, const jv_t *hdr, const jv_t *payload,
                 char *out, size_t cap)
{
    jv_t *root;
    int rc;

    if (c == NULL || hdr == NULL || payload == NULL) return JCS_E_TYPE;
    root = jcs_obj(c);
    if (root == NULL) return JCS_E_NODES;
    rc = jcs_put(c, root, "hdr", (jv_t *)hdr);          /* 顺序无关：JCS 会排序 */
    if (rc != 0) return rc;
    rc = jcs_put(c, root, "payload", (jv_t *)payload);
    if (rc != 0) return rc;
    return jcs_encode(c, root, out, cap);
}
