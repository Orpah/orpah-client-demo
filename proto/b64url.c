/* b64url.c — base64url 编码（无填充）。无 stdio / malloc。 */
#include "b64url.h"

static const char B64URL_ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url_len(size_t n)
{
    size_t full = n / 3;
    size_t rem = n % 3;
    return full * 4 + (rem ? rem + 1 : 0);      /* 无 '=' 填充 */
}

int b64url_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
    size_t need = b64url_len(n);
    size_t i = 0, o = 0;

    if (out == NULL) return -1;
    if (cap < need) return -1;
    if (in == NULL && n != 0) return -1;

    while (i + 3 <= n) {
        unsigned int v = ((unsigned int)in[i] << 16) |
                         ((unsigned int)in[i + 1] << 8) |
                         (unsigned int)in[i + 2];
        out[o++] = B64URL_ALPHA[(v >> 18) & 0x3F];
        out[o++] = B64URL_ALPHA[(v >> 12) & 0x3F];
        out[o++] = B64URL_ALPHA[(v >> 6) & 0x3F];
        out[o++] = B64URL_ALPHA[v & 0x3F];
        i += 3;
    }
    if (n - i == 1) {
        unsigned int v = (unsigned int)in[i] << 16;
        out[o++] = B64URL_ALPHA[(v >> 18) & 0x3F];
        out[o++] = B64URL_ALPHA[(v >> 12) & 0x3F];
    } else if (n - i == 2) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        out[o++] = B64URL_ALPHA[(v >> 18) & 0x3F];
        out[o++] = B64URL_ALPHA[(v >> 12) & 0x3F];
        out[o++] = B64URL_ALPHA[(v >> 6) & 0x3F];
    }
    return (int)o;
}
