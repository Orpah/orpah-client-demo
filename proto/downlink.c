/* downlink.c — 下行解码实现（语义对齐 orpah_proto.decode_msg 与设备侧取值习惯） */
#include "downlink.h"

static const char *KNOWN_TYPES[] = {
    DL_T_REQ_CONNECT, DL_T_ACCESS_INFO, DL_T_REPORT, DL_T_TRACKING, DL_T_ERROR,
    DL_T_LOST_TABLE, DL_T_LOST_TABLE_REQ, DL_T_FOUND, DL_T_ID_REPORT,
};

int dl_type_known(const char *type)
{
    if (type == NULL) return 0;
    for (size_t i = 0; i < sizeof(KNOWN_TYPES) / sizeof(KNOWN_TYPES[0]); i++) {
        const char *a = KNOWN_TYPES[i];
        const char *b = type;
        while (*a != '\0' && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return 1;
    }
    return 0;
}

int dl_decode(jcs_ctx_t *c, const char *buf, size_t len, jv_t **msg)
{
    jv_t *root = NULL;
    int rc;

    if (msg == NULL) return DL_E_NOT_JSON;
    *msg = NULL;
    rc = json_parse(c, buf, len, &root);
    if (rc != 0) return DL_E_NOT_JSON;
    if (root == NULL || root->type != JV_OBJ) return DL_E_NOT_OBJECT;  /* Python: not dict */
    if (!dl_type_known(jv_get_str(root, "type"))) return DL_E_UNKNOWN_TYPE;
    *msg = root;
    return 0;
}

const char *dl_type(const jv_t *msg)
{
    return jv_get_str(msg, "type");
}

const char *dl_str(const jv_t *msg, const char *key)
{
    return jv_get_str(msg, key);
}

int dl_int(const jv_t *msg, const char *key, long long *out)
{
    return jv_get_int(msg, key, out);
}

int dl_truthy(const jv_t *msg, const char *key, int *out)
{
    jv_t *v = jv_get(msg, key);

    if (v == NULL) return 0;                       /* 键缺失：Python 是 None → 假，但要能区分 */
    switch (v->type) {
    case JV_NULL:  if (out) *out = 0; break;
    case JV_BOOL:  if (out) *out = v->u.b ? 1 : 0; break;
    case JV_INT:   if (out) *out = (v->u.i != 0) ? 1 : 0; break;
    case JV_STR:   if (out) *out = (v->u.s[0] != '\0') ? 1 : 0; break;
    case JV_ARR:   if (out) *out = (v->u.arr.n > 0) ? 1 : 0; break;
    case JV_OBJ:   if (out) *out = (v->u.obj.n > 0) ? 1 : 0; break;
    default:       if (out) *out = 0; break;
    }
    return 1;
}
