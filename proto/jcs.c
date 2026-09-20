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

/* 十六进制位（解析 \uXXXX 用）；非十六进制返回 -1 */
static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
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
/* 递归编码（sorted=1：对象成员先排序（JCS）；sorted=0：保持插入序）        */
/* ------------------------------------------------------------------ */
static void enc(out_t *o, const jv_t *v, int sorted);

static void enc_obj(out_t *o, const jv_t *v, int sorted)
{
    int idx[JCS_MAX_MEMBERS];
    int n = v->u.obj.n;

    for (int i = 0; i < n; i++) idx[i] = i;
    if (sorted) {
        /* 插入排序：键按逐字节字典序（= Python 的码点序） */
        for (int i = 1; i < n; i++) {
            int cur = idx[i], j = i - 1;
            while (j >= 0 && j_strcmp(v->u.obj.keys[idx[j]], v->u.obj.keys[cur]) > 0) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = cur;
        }
    }

    o_byte(o, '{');
    for (int i = 0; i < n; i++) {
        if (i) o_byte(o, ',');
        o_json_str(o, v->u.obj.keys[idx[i]]);
        o_byte(o, ':');
        enc(o, v->u.obj.vals[idx[i]], sorted);
    }
    o_byte(o, '}');
}

static void enc(out_t *o, const jv_t *v, int sorted)
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
            enc(o, v->u.arr.items[i], sorted);
        }
        o_byte(o, ']');
        break;
    case JV_OBJ:  enc_obj(o, v, sorted); break;
    default:      o->err = JCS_E_TYPE; break;
    }
}

static int enc_any(const jv_t *root, char *out, size_t cap, int sorted)
{
    out_t o;

    if (out == NULL || cap == 0) return JCS_E_OUT;
    o.buf = out;
    o.cap = cap;
    o.n = 0;
    o.err = 0;
    enc(&o, root, sorted);
    if (o.err) return o.err;
    return (int)o.n;
}

int jcs_encode(jcs_ctx_t *c, const jv_t *root, char *out, size_t cap)
{
    (void)c;
    return enc_any(root, out, cap, 1);          /* sorted=1：JCS */
}

int jcs_encode_raw(const jv_t *root, char *out, size_t cap)
{
    return enc_any(root, out, cap, 0);          /* sorted=0：插入序（信封） */
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

/* ------------------------------------------------------------------ */
/* 对象取值                                                            */
/* ------------------------------------------------------------------ */
jv_t *jv_get(const jv_t *obj, const char *key)
{
    if (obj == NULL || key == NULL || obj->type != JV_OBJ) return NULL;
    for (int i = 0; i < obj->u.obj.n; i++) {
        if (j_strcmp(obj->u.obj.keys[i], key) == 0) return obj->u.obj.vals[i];
    }
    return NULL;
}

const char *jv_get_str(const jv_t *obj, const char *key)
{
    jv_t *v = jv_get(obj, key);
    return (v != NULL && v->type == JV_STR) ? v->u.s : NULL;
}

int jv_get_int(const jv_t *obj, const char *key, long long *out)
{
    jv_t *v = jv_get(obj, key);
    if (v == NULL || v->type != JV_INT) return 0;
    if (out != NULL) *out = v->u.i;
    return 1;
}

/* ------------------------------------------------------------------ */
/* JSON 解析（下行报文用；递归下降，无 malloc）                            */
/* ------------------------------------------------------------------ */
#define PARSER_MAX_DEPTH 8
#define PARSER_STR_MAX   256

typedef struct {
    jcs_ctx_t *c;
    const char *p;
    const char *end;
    int depth;
} pctx_t;

static void p_ws(pctx_t *q)
{
    while (q->p < q->end && (*q->p == ' ' || *q->p == '\t' ||\
                             *q->p == '\n' || *q->p == '\r')) {
        q->p++;
    }
}

static int p_peek(pctx_t *q, char *out)
{
    if (q->p >= q->end) return 0;
    *out = *q->p;
    return 1;
}

static int p_lit(pctx_t *q, const char *word)
{
    size_t n = j_len(word);
    if ((size_t)(q->end - q->p) < n) return JCS_E_PARSE;
    for (size_t i = 0; i < n; i++) {
        if (q->p[i] != word[i]) return JCS_E_PARSE;
    }
    q->p += n;
    return 0;
}

static int u_hex4(pctx_t *q, unsigned int *out)
{
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        int d;
        if (q->p >= q->end) return JCS_E_PARSE;
        d = hex_val(*q->p);
        if (d < 0) return JCS_E_PARSE;
        v = (v << 4) | (unsigned int)d;
        q->p++;
    }
    *out = v;
    return 0;
}

static int u8_put(char *buf, size_t cap, size_t *n, unsigned int cp)
{
    if (cp < 0x80) {
        if (*n + 1 > cap) return JCS_E_PARSE;
        buf[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*n + 2 > cap) return JCS_E_PARSE;
        buf[(*n)++] = (char)(0xC0 | (cp >> 6));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return JCS_E_PARSE;   /* 孤立代理项：拒绝 */
        if (*n + 3 > cap) return JCS_E_PARSE;
        buf[(*n)++] = (char)(0xE0 | (cp >> 12));
        buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (cp > 0x10FFFF) return JCS_E_PARSE;
        if (*n + 4 > cap) return JCS_E_PARSE;
        buf[(*n)++] = (char)(0xF0 | (cp >> 18));
        buf[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
    return 0;
}

static int p_string(pctx_t *q, char *buf, size_t cap, size_t *out_n)
{
    size_t n = 0;

    if (q->p >= q->end || *q->p != '"') return JCS_E_PARSE;
    q->p++;
    for (;;) {
        char ch;
        if (q->p >= q->end) return JCS_E_PARSE;
        ch = *q->p++;
        if (ch == '"') break;
        if (ch == '\\') {
            char e;
            if (q->p >= q->end) return JCS_E_PARSE;
            e = *q->p++;
            switch (e) {
            case '"': case '\\': case '/':
                if (n + 1 > cap) return JCS_E_PARSE;
                buf[n++] = e;
                break;
            case 'b': if (n + 1 > cap) return JCS_E_PARSE; buf[n++] = '\b'; break;
            case 'f': if (n + 1 > cap) return JCS_E_PARSE; buf[n++] = '\f'; break;
            case 'n': if (n + 1 > cap) return JCS_E_PARSE; buf[n++] = '\n'; break;
            case 'r': if (n + 1 > cap) return JCS_E_PARSE; buf[n++] = '\r'; break;
            case 't': if (n + 1 > cap) return JCS_E_PARSE; buf[n++] = '\t'; break;
            case 'u': {
                unsigned int cp, lo;
                int rc = u_hex4(q, &cp);
                if (rc != 0) return rc;
                if (cp >= 0xD800 && cp <= 0xDBFF) {          /* 高代理：找下一个 \u 低代理 */
                    unsigned int hi = cp;
                    if (q->p + 1 >= q->end || q->p[0] != '\\' || q->p[1] != 'u') {
                        return JCS_E_PARSE;
                    }
                    q->p += 2;
                    rc = u_hex4(q, &lo);
                    if (rc != 0) return rc;
                    if (lo < 0xDC00 || lo > 0xDFFF) return JCS_E_PARSE;
                    cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                }
                rc = u8_put(buf, cap, &n, cp);
                if (rc != 0) return rc;
                break;
            }
            default:
                return JCS_E_PARSE;
            }
        } else if ((unsigned char)ch < 0x20) {
            return JCS_E_PARSE;                             /* 未转义的控制字符 */
        } else {
            if (n + 1 > cap) return JCS_E_PARSE;
            buf[n++] = ch;                                  /* UTF-8 字节原样收 */
        }
    }
    *out_n = n;
    return 0;
}

static int p_number(pctx_t *q, long long *out)
{
    int neg = 0;
    unsigned long long mag = 0;
    int ndig = 0;

    if (q->p < q->end && *q->p == '-') { neg = 1; q->p++; }
    while (q->p < q->end && *q->p >= '0' && *q->p <= '9') {
        int d = *q->p - '0';
        if (mag > (0xFFFFFFFFFFFFFFFFULL - (unsigned long long)d) / 10ULL) {
            return JCS_E_PARSE;                             /* 整数溢出 */
        }
        mag = mag * 10ULL + (unsigned long long)d;
        ndig++;
        q->p++;
    }
    if (ndig == 0) return JCS_E_PARSE;
    if (ndig > 1 && *(q->p - ndig) == '0') return JCS_E_PARSE;      /* 前导零：Python json 也拒 */
    if (q->p < q->end && (*q->p == '.' || *q->p == 'e' || *q->p == 'E')) {
        return JCS_E_PARSE;                                     /* 浮点/指数：我们不用，报了才安全 */
    }
    if (!neg) {
        if (mag > 9223372036854775807ULL) return JCS_E_PARSE;
        *out = (long long)mag;
    } else {
        if (mag > 9223372036854775808ULL) return JCS_E_PARSE;
        *out = (mag == 9223372036854775808ULL) ? (-9223372036854775807LL - 1LL)
                                               : -(long long)mag;
    }
    return 0;
}

static int parse_value(pctx_t *q, jv_t **out);

static int p_object(pctx_t *q, jv_t **out)
{
    jv_t *o = jcs_obj(q->c);
    int rc;

    if (o == NULL) return JCS_E_NODES;
    q->p++;                                              /* '{' */
    p_ws(q);
    if (q->p < q->end && *q->p == '}') { q->p++; *out = o; return 0; }
    for (;;) {
        char key[PARSER_STR_MAX];
        size_t klen = 0;
        jv_t *val = NULL;

        p_ws(q);
        rc = p_string(q, key, sizeof(key) - 1, &klen);
        if (rc != 0) return rc;
        key[klen] = '\0';
        p_ws(q);
        if (q->p >= q->end || *q->p != ':') return JCS_E_PARSE;
        q->p++;
        p_ws(q);
        rc = parse_value(q, &val);
        if (rc != 0) return rc;
        rc = jcs_put(q->c, o, key, val);
        if (rc != 0) return rc;                          /* 含重复键 → 报错（见 README 边界）*/
        p_ws(q);
        if (q->p < q->end && *q->p == ',') { q->p++; continue; }
        if (q->p < q->end && *q->p == '}') { q->p++; *out = o; return 0; }
        return JCS_E_PARSE;
    }
}

static int p_array(pctx_t *q, jv_t **out)
{
    jv_t *a = jcs_arr(q->c);
    int rc;

    if (a == NULL) return JCS_E_NODES;
    q->p++;                                              /* '[' */
    p_ws(q);
    if (q->p < q->end && *q->p == ']') { q->p++; *out = a; return 0; }
    for (;;) {
        jv_t *val = NULL;
        p_ws(q);
        rc = parse_value(q, &val);
        if (rc != 0) return rc;
        rc = jcs_add(q->c, a, val);
        if (rc != 0) return rc;
        p_ws(q);
        if (q->p < q->end && *q->p == ',') { q->p++; continue; }
        if (q->p < q->end && *q->p == ']') { q->p++; *out = a; return 0; }
        return JCS_E_PARSE;
    }
}

static int parse_value(pctx_t *q, jv_t **out)
{
    char ch;
    int rc;

    if (q->depth >= PARSER_MAX_DEPTH) return JCS_E_DEPTH;
    p_ws(q);
    if (!p_peek(q, &ch)) return JCS_E_PARSE;

    if (ch == '{') {
        q->depth++;
        rc = p_object(q, out);
        q->depth--;
        return rc;
    }
    if (ch == '[') {
        q->depth++;
        rc = p_array(q, out);
        q->depth--;
        return rc;
    }
    if (ch == '"') {
        char buf[PARSER_STR_MAX];
        size_t n = 0;
        jv_t *v;
        rc = p_string(q, buf, sizeof(buf) - 1, &n);
        if (rc != 0) return rc;
        buf[n] = '\0';
        v = jcs_str(q->c, buf);
        if (v == NULL) return JCS_E_POOL;
        *out = v;
        return 0;
    }
    if (ch == 't') { if (p_lit(q, "true") != 0) return JCS_E_PARSE;  *out = jcs_bool(q->c, 1); return (*out ? 0 : JCS_E_NODES); }
    if (ch == 'f') { if (p_lit(q, "false") != 0) return JCS_E_PARSE; *out = jcs_bool(q->c, 0); return (*out ? 0 : JCS_E_NODES); }
    if (ch == 'n') { if (p_lit(q, "null") != 0) return JCS_E_PARSE;  *out = jcs_null(q->c);     return (*out ? 0 : JCS_E_NODES); }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        long long iv = 0;
        jv_t *v;
        rc = p_number(q, &iv);
        if (rc != 0) return rc;
        v = jcs_int(q->c, iv);
        if (v == NULL) return JCS_E_NODES;
        *out = v;
        return 0;
    }
    return JCS_E_PARSE;
}

int json_parse(jcs_ctx_t *c, const char *buf, size_t len, jv_t **out)
{
    pctx_t q;
    jv_t *root = NULL;
    int rc;

    if (c == NULL || buf == NULL || out == NULL) return JCS_E_PARSE;
    q.c = c;
    q.p = buf;
    q.end = buf + len;
    q.depth = 0;
    *out = NULL;

    rc = parse_value(&q, &root);
    if (rc != 0) return rc;
    p_ws(&q);
    if (q.p != q.end) return JCS_E_PARSE;                /* 尾随数据 */
    *out = root;
    return 0;
}
