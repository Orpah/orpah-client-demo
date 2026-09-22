/* id_level.c — §8.2 选级（纯逻辑；无 stdio / 无 string.h，可直接编进 -nostdlib 固件）。*/
#include "id_level.h"

#include <stddef.h>        /* NULL（MSVC 不会间接给你，实测报 C2065）*/

static int _eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

int idl_pick(int se_ok, int sign_ok, int hmac_ok, const char **reason)
{
    if (!se_ok) {
        if (hmac_ok) {
            if (reason != NULL) { *reason = "se_unavailable"; }
            return IDL_L2;
        }
        if (reason != NULL) { *reason = "no_key"; }
        return IDL_L3;
    }
    if (!sign_ok) {
        if (reason != NULL) { *reason = "slot0_sign_failed"; }
        return IDL_L1;
    }
    if (reason != NULL) { *reason = "normal"; }
    return IDL_L0;
}

/* 查表（与上游 LEVEL_MODES 一一对应；写成表而不是 if 链，是为了让"模式名"只有一处）*/
struct idl_mode_row {
    const char *name;
    int se_ok;
    int sign_ok;
    int hmac_ok;
};

static const struct idl_mode_row IDL_MODES[IDL_MODE_COUNT] = {
    { "auto",      1, 1, 1 },      /* 全正常 → L0（ES256） */
    { "sign_fail", 1, 0, 1 },      /* Step2 Slot0 签名失败 → L1（HS256） */
    { "se_fail",   0, 1, 1 },      /* Step1 SE 不可用（有 HMAC）→ L2（HS256） */
    { "no_key",    0, 1, 0 }       /* Step3 无可用密钥 → L3（裸上报） */
};

int idl_mode_inputs(const char *mode, int *se_ok, int *sign_ok, int *hmac_ok)
{
    int i;

    if (mode == NULL) { return -1; }
    for (i = 0; i < IDL_MODE_COUNT; i++) {
        if (_eq(mode, IDL_MODES[i].name)) {
            if (se_ok != NULL)   { *se_ok = IDL_MODES[i].se_ok; }
            if (sign_ok != NULL) { *sign_ok = IDL_MODES[i].sign_ok; }
            if (hmac_ok != NULL) { *hmac_ok = IDL_MODES[i].hmac_ok; }
            return 0;
        }
    }
    return -1;
}

const char *idl_mode_name(int idx)
{
    if (idx < 0 || idx >= IDL_MODE_COUNT) { return NULL; }
    return IDL_MODES[idx].name;
}
