/* hgic.h — 泰芯 AH 模组（TXW8301）**主机口 mac_bus** 的 HGIC 帧层
 *
 * 纯逻辑：**无 stdio / malloc / 浮点 / string.h ⇒ 可直接编进固件**（`-nostdlib`）。
 * 本层**只做帧**，不认识 ORPAH 报文（那是 msg./jcs./id_report. 的事）。
 *
 * 单一源（都是**模组 SDK 源码 + 真机实测**，不是猜的；本仓 PC 侧参考实现 = `tools/txah_hgic.py`）：
 *   · `sdk/include/lib/lmac/hgic.h`
 *       - `HGIC_HDR_TX_MAGIC 0x1A2B`（主机→模组）/ `HGIC_HDR_RX_MAGIC 0x2B1A`（模组→主机）
 *       - `struct hgic_hdr { u16 magic; u8 type; u8 ifidx:4|flags:4; u16 length; u16 cookie; }`
 *         （`__packed`，**8 字节**；UART 上小端 ⇒ 主机发出去的前两字节是 `2B 1A`）
 *       - `enum hgic_hdr_type { ACK=1, FRM=2, CMD=3, EVENT=4, … FRM2=9, CMD2=13, EVENT2=14 }`
 *       - `struct hgic_ctrl_hdr` = 8 B 头 + 4 B union（`cmd_id`/`status`/`event_id`…），
 *         `sizeof` = **12** ⇒ 参数从**偏移 12** 开始（`uart_bus.c` 的 `data = (uint8*)(ctrl+1)`）
 *       - `HGIC_TX_COOKIE_MASK` = 0x7FFF
 *   · `sdk/lib/bus/macbus/uart_bus.c`（UART 上的定帧规则）
 *       - 收满 2 字节比对 magic；**第 5、6 字节 = 整帧长度**（16bit 小端，**含 8 B 头**）
 *       - `UART_BUS_RX_BUF_SIZE` = 4096
 *   · 真机实测（2026-09-16 ~ 09-20；`tools/probe_txah_uart.py` / `hgic_bus.py`）
 *       - 数据面 = `FRM2`（8 B 头，**载荷紧跟 = 一整个以太网帧**，不含那 24 B frm_info）
 *         ⇒ 模组日志 `[mbus rx] <8+len> byte(s)`
 *       - 主机帧的 **cookie 逐帧 +1**：模组做顺序检查，真机日志出现过
 *         `cookie err: last:167, new:177`（以前每帧都用同一个 cookie）⇒ 见 `hgic_cookie_t`
 *       - 应答形如 `cmd_id(1) | status(1) | len(2,LE) | data(len)`，但也有**只回一个
 *         cmd_id** 的短应答（实测 `send-cmd 1` / `20`）⇒ 不编造 status
 *
 * ★ 与 Python 的**有意差异只有一个**：`hgic_parser_t.overrun`（见下）—— Python 的缓冲无上限，
 *   裸机必须有界；合法数据到不了那里（`length ≤ 4096` 保证每次都能切出帧）。
 */
#ifndef ORPAH_HGIC_H
#define ORPAH_HGIC_H

#include <stddef.h>
#include <stdint.h>

/* ---- 常量 ---- */
#define HGIC_MAGIC_HOST_TO_MODULE  0x1A2Bu   /* HGIC_HDR_TX_MAGIC */
#define HGIC_MAGIC_MODULE_TO_HOST  0x2B1Au   /* HGIC_HDR_RX_MAGIC */

#define HGIC_HDR_LEN       8u                /* sizeof(struct hgic_hdr)（packed） */
#define HGIC_CTRL_UNION    4u                /* hgic_ctrl_hdr 里 union 的大小 */
#define HGIC_MAX_FRAME     4096u             /* uart_bus.c: UART_BUS_RX_BUF_SIZE */
#define HGIC_COOKIE_MASK   0x7FFFu           /* hgic.h: HGIC_TX_COOKIE_MASK */

/* enum hgic_hdr_type（只列本项目用到的） */
#define HGIC_T_ACK     1u
#define HGIC_T_FRM     2u
#define HGIC_T_CMD     3u
#define HGIC_T_EVENT   4u
#define HGIC_T_FRM2    9u
#define HGIC_T_CMD2    13u
#define HGIC_T_EVENT2  14u

/* id 宽度分流（对照 hgic.h 的 `HDR_CMDID()`：id > 255 才走 "2" 系列） */
#define HGIC_USE_CMD2(id)    ((id) > 0xFFu ? HGIC_T_CMD2 : HGIC_T_CMD)
#define HGIC_USE_EVENT2(id)  ((id) > 0xFFu ? HGIC_T_EVENT2 : HGIC_T_EVENT)

/* enum hgic_cmd（只列本项目要用的） */
#define HGIC_CMD_SET_UART_FIXLEN 108u
#define HGIC_CMD_GET_UART_FIXLEN 109u

/* 错误码（全是负数；0 = 成功） */
#define HGIC_E_ARG   (-1)   /* 空指针 / 载荷为空（Python 侧对应 None） */
#define HGIC_E_SHORT (-2)   /* 输入不足 8 字节，给的不是完整头 */
#define HGIC_E_LEN   (-3)   /* 整帧长度不在 [8, 4096] */
#define HGIC_E_CAP   (-4)   /* 输出缓冲不够 */

/* ---- 帧头（8 B；`ifidx`/`flags` 是同一个字节的高低半字节，拆开给调用方） ---- */
typedef struct {
    uint16_t magic;
    uint16_t length;        /* **整帧**长度，含 8 B 头（= uart_bus.c 的定帧依据） */
    uint16_t cookie;
    uint8_t  type;
    uint8_t  ifidx;
    uint8_t  flags;
    uint8_t  from_module;   /* magic == MODULE_TO_HOST */
} hgic_hdr_t;

/* 整帧长度是否合法（**独立于解析**，与 Python 一致：`StreamParser` 自己判，不是 parse_header 判） */
int hgic_len_ok(uint16_t length);

/* 载荷长度 = 整帧长度 − 8（头长度非法时为 0） */
size_t hgic_payload_len(const hgic_hdr_t *h);

/* 解析前 8 字节。**只做拆分**（不做长度合法性判断，同 Python 的 `parse_header`）：
 * 成功 0 并写 *out；给的不是完整头 ⇒ HGIC_E_SHORT。 */
int hgic_parse_hdr(const uint8_t *buf, size_t len, hgic_hdr_t *out);

/* 组帧：out 收 8 B 头 + payload。成功返回**整帧长度**（>0），否则 HGIC_E_*。
 * `length` 由本函数算（8 + plen）—— 调用方不用也不能指定（Python 侧同理）。 */
int hgic_build(uint8_t *out, size_t cap, uint16_t magic, uint8_t type,
               const void *payload, size_t plen, uint16_t cookie,
               uint8_t ifidx, uint8_t flags);

/* 数据帧（主机→模组）：`FRM2` + 载荷紧跟。载荷 = **一整个以太网帧**（≥14 B 以太头）。 */
int hgic_frame_frm2(uint8_t *out, size_t cap, const void *eth, size_t ethlen,
                    uint16_t cookie);

/* 命令帧（主机→模组）：`CMD`(3) 或 `CMD2`(13)（id > 255）。
 * ★ 那 4 字节 union **必须补满**（id ≤ 0xFF 时后跟 3 个 0，否则 u16 id 后跟 2 个 0）——
 *   模组的参数是从偏移 12 取的，补不满参数就落到错位置上。 */
int hgic_frame_cmd(uint8_t *out, size_t cap, uint16_t cmd_id,
                   const void *params, size_t plen, uint16_t cookie);

/* ---- cookie 计数器：主机帧逐帧 +1（模组做顺序检查），按 15 位回绕 ---- */
typedef struct { uint16_t n; } hgic_cookie_t;

void     hgic_cookie_init(hgic_cookie_t *c, uint16_t start);
uint16_t hgic_cookie_next(hgic_cookie_t *c);

/* ---- 控制面解码（`CMD`/`CMD2`/`EVENT`/`EVENT2` 的 body） ---- */
#define HGIC_CTRL_REQ   0   /* 主机→模组（回环自测才会看到） */
#define HGIC_CTRL_RESP  1   /* 模组→主机（应答或事件上报；沿用 Python 的叫法） */

typedef struct {
    uint8_t        kind;        /* HGIC_CTRL_REQ / HGIC_CTRL_RESP */
    uint16_t       id;          /* cmd_id / event_id */
    uint8_t        has_status;  /* 1 = 真有 status/len 字段；0 = 短应答或请求/事件 */
    uint8_t        status;
    const uint8_t *data;        /* **指向入参 payload 内部**（不拷贝，生命周期同入参） */
    size_t         data_len;
} hgic_ctrl_t;

/* 语义同 `tools/txah_hgic.py` 的 `ctrl_info()`（三种形状分开，不编造）：
 *   · 模块应答且 `3 + len == body 长度` ⇒ 带 status/len/data
 *   · 模块应答但不够/对不上 ⇒ **短应答**（只有 id，has_status=0）
 *   · 请求 / 事件 ⇒ `data` = union 之后的全部（长度 < 3 时为空）
 * 成功 0；载荷为空或长度不足以取 id ⇒ HGIC_E_ARG（Python 侧返回 None）。 */
int hgic_ctrl_parse(const hgic_hdr_t *h, const uint8_t *payload, size_t len,
                    hgic_ctrl_t *out);

/* ---- 流式解析（定帧 + 重同步） ---- */
typedef enum {
    HGIC_EXPECT_ANY = 0,                  /* 两种 magic 都收（= Python 的 expect_from_module=None） */
    HGIC_EXPECT_MODULE_TO_HOST = 1,       /* 只收模组→主机（**固件用这个**） */
    HGIC_EXPECT_HOST_TO_MODULE = 2        /* 只收主机→模组（回环/自测用） */
} hgic_expect_t;

/* 每识别出一帧调一次。返回非 0 = 「这一帧已交付，但**先别继续**」：
 * 本次解析到此为止，**剩余字节留在缓冲里**，下次 feed 接着切（不会重复交付这一帧）。 */
typedef int (*hgic_frame_cb)(const hgic_hdr_t *hdr, const uint8_t *payload,
                             size_t plen, void *ctx);

typedef struct {
    uint8_t       buf[HGIC_MAX_FRAME];
    size_t        len;
    hgic_expect_t expect;
    uint32_t      garbage;      /* 丢掉的杂字节数（**可见**，不静默丢） */
    uint32_t      bad_length;   /* 长度字段不合法而跳过的"假 magic"个数 */
    uint32_t      overrun;      /* ★ C 独有：单次 feed 超出内部缓冲（Python 无上限） */
} hgic_parser_t;

void hgic_parser_init(hgic_parser_t *p, hgic_expect_t expect);

/* 喂入新字节并尽量切帧。行为**逐步对齐** `txah_hgic.StreamParser.feed()`
 * （含「找不到 magic 时留住最后 1 字节」的重同步规则）。`cb` 可为 NULL（只丢不交付）。 */
void hgic_parser_feed(hgic_parser_t *p, const uint8_t *data, size_t n,
                      hgic_frame_cb cb, void *ctx);

/* 缓冲里还有多少字节没被消费（观测用）。 */
size_t hgic_parser_pending(const hgic_parser_t *p);

#endif /* ORPAH_HGIC_H */
