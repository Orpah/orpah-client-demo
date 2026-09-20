/* hgic.c — HGIC 帧层（纯逻辑：无 stdio / malloc / 浮点 / string.h ⇒ 可编进固件）。
 *
 * 实现说明放哪：
 *   · 单一源、常量来历、与 Python 的边界 ⇒ hgic.h 的文件头
 *   · 每条判据怎么测出来的 ⇒ proto/README.md（HGIC 一节）+ test_vectors_hgic*.txt
 */
#include "hgic.h"

/* ------------------------------------------------------------------ */
/* 小工具（不用 string.h / memcpy：固件是 -nostdlib，参考固件的先例也是自写） */
/* ------------------------------------------------------------------ */
static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* 只留缓冲尾部 keep 字节（= Python 的 `del buf[:-keep]` / `del buf[:pos]`） */
static void keep_tail(hgic_parser_t *p, size_t keep)
{
    size_t start, i;

    if (keep > p->len) {
        keep = p->len;
    }
    start = p->len - keep;
    for (i = 0; i < keep; i++) {
        p->buf[i] = p->buf[start + i];
    }
    p->len = keep;
}

/* ------------------------------------------------------------------ */
/* 帧头                                                               */
/* ------------------------------------------------------------------ */
int hgic_len_ok(uint16_t length)
{
    return length >= (uint16_t)HGIC_HDR_LEN && length <= (uint16_t)HGIC_MAX_FRAME;
}

size_t hgic_payload_len(const hgic_hdr_t *h)
{
    if (h == NULL || !hgic_len_ok(h->length)) {
        return 0;
    }
    return (size_t)h->length - (size_t)HGIC_HDR_LEN;
}

int hgic_parse_hdr(const uint8_t *buf, size_t len, hgic_hdr_t *out)
{
    if (buf == NULL || out == NULL) {
        return HGIC_E_ARG;
    }
    if (len < (size_t)HGIC_HDR_LEN) {
        return HGIC_E_SHORT;
    }
    /* 小端：magic(2) type(1) ifidx:4|flags:4(1) length(2) cookie(2) */
    out->magic  = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    out->type   = buf[2];
    out->ifidx  = (uint8_t)((buf[3] >> 4) & 0x0Fu);
    out->flags  = (uint8_t)(buf[3] & 0x0Fu);
    out->length = (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
    out->cookie = (uint16_t)(buf[6] | ((uint16_t)buf[7] << 8));
    out->from_module = (uint8_t)(out->magic == HGIC_MAGIC_MODULE_TO_HOST ? 1u : 0u);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 组帧                                                               */
/* ------------------------------------------------------------------ */
int hgic_build(uint8_t *out, size_t cap, uint16_t magic, uint8_t type,
               const void *payload, size_t plen, uint16_t cookie,
               uint8_t ifidx, uint8_t flags)
{
    const uint8_t *body = (const uint8_t *)payload;
    size_t total = (size_t)HGIC_HDR_LEN + plen;

    if (out == NULL || (body == NULL && plen != 0)) {
        return HGIC_E_ARG;
    }
    if (total > (size_t)HGIC_MAX_FRAME) {
        return HGIC_E_LEN;
    }
    if (cap < total) {
        return HGIC_E_CAP;
    }

    out[0] = (uint8_t)(magic & 0xFFu);
    out[1] = (uint8_t)((magic >> 8) & 0xFFu);
    out[2] = type;
    out[3] = (uint8_t)(((ifidx & 0x0Fu) << 4) | (flags & 0x0Fu));
    out[4] = (uint8_t)(total & 0xFFu);
    out[5] = (uint8_t)((total >> 8) & 0xFFu);
    out[6] = (uint8_t)(cookie & 0xFFu);
    out[7] = (uint8_t)((cookie >> 8) & 0xFFu);
    copy_bytes(out + HGIC_HDR_LEN, body, plen);
    return (int)total;
}

int hgic_frame_frm2(uint8_t *out, size_t cap, const void *eth, size_t ethlen,
                    uint16_t cookie)
{
    return hgic_build(out, cap, HGIC_MAGIC_HOST_TO_MODULE, (uint8_t)HGIC_T_FRM2,
                      eth, ethlen, cookie, 0u, 0u);
}

int hgic_frame_cmd(uint8_t *out, size_t cap, uint16_t cmd_id,
                   const void *params, size_t plen, uint16_t cookie)
{
    const uint8_t *body = (const uint8_t *)params;
    size_t total = (size_t)HGIC_HDR_LEN + (size_t)HGIC_CTRL_UNION + plen;
    uint8_t type = (uint8_t)HGIC_USE_CMD2(cmd_id);

    if (out == NULL || (body == NULL && plen != 0)) {
        return HGIC_E_ARG;
    }
    if (total > (size_t)HGIC_MAX_FRAME) {
        return HGIC_E_LEN;
    }
    if (cap < total) {
        return HGIC_E_CAP;
    }

    out[0] = (uint8_t)(HGIC_MAGIC_HOST_TO_MODULE & 0xFFu);
    out[1] = (uint8_t)((HGIC_MAGIC_HOST_TO_MODULE >> 8) & 0xFFu);
    out[2] = type;
    out[3] = 0u;                                  /* ifidx = 0, flags = 0 */
    out[4] = (uint8_t)(total & 0xFFu);
    out[5] = (uint8_t)((total >> 8) & 0xFFu);
    out[6] = (uint8_t)(cookie & 0xFFu);
    out[7] = (uint8_t)((cookie >> 8) & 0xFFu);
    /* 4 B union：id ≤ 0xFF 用 1 B（后跟 3 个 0），否则 u16（后跟 2 个 0） */
    if (cmd_id <= 0xFFu) {
        out[8]  = (uint8_t)cmd_id;
        out[9]  = 0u;
        out[10] = 0u;
        out[11] = 0u;
    } else {
        out[8]  = (uint8_t)(cmd_id & 0xFFu);
        out[9]  = (uint8_t)((cmd_id >> 8) & 0xFFu);
        out[10] = 0u;
        out[11] = 0u;
    }
    copy_bytes(out + HGIC_HDR_LEN + HGIC_CTRL_UNION, body, plen);
    return (int)total;
}

/* ------------------------------------------------------------------ */
/* cookie                                                             */
/* ------------------------------------------------------------------ */
void hgic_cookie_init(hgic_cookie_t *c, uint16_t start)
{
    if (c == NULL) {
        return;
    }
    /* 与 Python `CookieCounter` 同式：下一次 next() 返回 start */
    c->n = (uint16_t)((uint16_t)(start - 1u) & HGIC_COOKIE_MASK);
}

uint16_t hgic_cookie_next(hgic_cookie_t *c)
{
    if (c == NULL) {
        return 0u;
    }
    c->n = (uint16_t)((uint16_t)(c->n + 1u) & HGIC_COOKIE_MASK);
    return c->n;
}

/* ------------------------------------------------------------------ */
/* 控制面解码（= txah_hgic.ctrl_info）                                 */
/* ------------------------------------------------------------------ */
int hgic_ctrl_parse(const hgic_hdr_t *h, const uint8_t *payload, size_t len,
                    hgic_ctrl_t *out)
{
    const uint8_t *body;
    size_t blen;
    int is_ctrl;

    if (h == NULL || payload == NULL || out == NULL) {
        return HGIC_E_ARG;
    }
    if (len == 0) {
        return HGIC_E_ARG;                     /* Python: `if not payload: return None` */
    }

    if (h->type == HGIC_T_CMD2 || h->type == HGIC_T_EVENT2) {
        if (len < 2) {
            return HGIC_E_ARG;
        }
        out->id = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
        body = payload + 2;
        blen = len - 2;
    } else {
        out->id = payload[0];
        body = payload + 1;
        blen = len - 1;
    }

    out->status = 0u;
    out->has_status = 0u;
    out->data = NULL;
    out->data_len = 0u;
    out->kind = (uint8_t)(h->from_module ? HGIC_CTRL_RESP : HGIC_CTRL_REQ);

    is_ctrl = (h->type == HGIC_T_CMD || h->type == HGIC_T_CMD2);
    if (is_ctrl && h->from_module) {
        if (blen >= 3u) {
            uint16_t ln = (uint16_t)(body[1] | ((uint16_t)body[2] << 8));
            if ((size_t)3u + (size_t)ln == blen) {
                out->has_status = 1u;
                out->status = body[0];
                out->data = body + 3;
                out->data_len = ln;
                return 0;
            }
        }
        /* 短应答：只回了 cmd_id（实测 send-cmd 1 / 20）—— 不编造 status */
        return 0;
    }

    /* 请求 / 事件：union(4) 之后才是数据；长度不足 3 时为空（同 Python） */
    if (blen >= 3u) {
        out->data = body + 3;
        out->data_len = blen - 3u;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 流式解析                                                           */
/* ------------------------------------------------------------------ */
void hgic_parser_init(hgic_parser_t *p, hgic_expect_t expect)
{
    if (p == NULL) {
        return;
    }
    p->len = 0u;
    p->expect = expect;
    p->garbage = 0u;
    p->bad_length = 0u;
    p->overrun = 0u;
}

size_t hgic_parser_pending(const hgic_parser_t *p)
{
    return (p == NULL) ? 0u : p->len;
}

static int magic_allowed(const hgic_parser_t *p, uint16_t v)
{
    if (p->expect == HGIC_EXPECT_MODULE_TO_HOST) {
        return v == (uint16_t)HGIC_MAGIC_MODULE_TO_HOST;
    }
    if (p->expect == HGIC_EXPECT_HOST_TO_MODULE) {
        return v == (uint16_t)HGIC_MAGIC_HOST_TO_MODULE;
    }
    return v == (uint16_t)HGIC_MAGIC_MODULE_TO_HOST || v == (uint16_t)HGIC_MAGIC_HOST_TO_MODULE;
}

/* 缓冲里**最靠前**的 magic 下标（= Python `_find_magic`：两种取最小位置）。找不到返回 -1。 */
static int find_magic(const hgic_parser_t *p)
{
    size_t i;

    for (i = 0; i + 1u < p->len; i++) {
        uint16_t v = (uint16_t)(p->buf[i] | ((uint16_t)p->buf[i + 1] << 8));
        if (magic_allowed(p, v)) {
            return (int)i;
        }
    }
    return -1;
}

/* 尽量切帧（逐步对应 Python `StreamParser.feed()` 的 while 循环） */
static void drain(hgic_parser_t *p, hgic_frame_cb cb, void *ctx)
{
    for (;;) {
        int pos = find_magic(p);
        hgic_hdr_t h;

        if (pos < 0) {
            /* 没找到 magic：留住最后 1 字节（可能是 magic 的前半） */
            if (p->len > 1u) {
                p->garbage += (uint32_t)(p->len - 1u);
                keep_tail(p, 1u);
            }
            return;
        }
        if (pos > 0) {
            p->garbage += (uint32_t)pos;
            keep_tail(p, p->len - (size_t)pos);
        }
        if (p->len < (size_t)HGIC_HDR_LEN) {
            return;                                     /* 头还没收齐 */
        }
        if (hgic_parse_hdr(p->buf, p->len, &h) != 0) {
            return;                                     /* 上面已保证 >= 8 ⇒ 到不了 */
        }
        if (!hgic_len_ok(h.length)) {
            p->bad_length++;
            keep_tail(p, p->len - 1u);                  /* del buf[:1]：这个 magic 是假的 */
            continue;
        }
        if (p->len < (size_t)h.length) {
            return;                                     /* 帧还没收齐 */
        }
        if (cb != NULL) {
            int stop = cb(&h, p->buf + HGIC_HDR_LEN,
                          (size_t)h.length - (size_t)HGIC_HDR_LEN, ctx);
            keep_tail(p, p->len - (size_t)h.length);    /* 先消费掉这一帧 */
            if (stop != 0) {
                return;                                 /* 调用方要求停下：剩余字节留着 */
            }
        } else {
            keep_tail(p, p->len - (size_t)h.length);
        }
    }
}

void hgic_parser_feed(hgic_parser_t *p, const uint8_t *data, size_t n,
                      hgic_frame_cb cb, void *ctx)
{
    if (p == NULL || (data == NULL && n != 0)) {
        return;
    }
    while (n > 0u) {
        size_t room = (size_t)HGIC_MAX_FRAME - p->len;
        size_t take;

        if (room == 0u) {
            /* ★ C 独有（Python 的缓冲无上限）：缓冲满却切不出帧。
             * 如实计数并丢掉最老的一半，避免死循环 —— 不静默吞。 */
            p->overrun++;
            keep_tail(p, (size_t)HGIC_MAX_FRAME / 2u);
            room = (size_t)HGIC_MAX_FRAME - p->len;
        }
        take = (n < room) ? n : room;
        copy_bytes(p->buf + p->len, data, take);
        p->len += take;
        data += take;
        n -= take;
        drain(p, cb, ctx);
    }
}
