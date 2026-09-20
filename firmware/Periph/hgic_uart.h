/* hgic_uart.h — 模组数据口驱动（HGIC over UART）：**只做硬件那一半**
 *
 * 分工（别把帧格式抄进来）：
 *   · 帧格式（8 B 头 / `FRM2` / `CMD` / cookie / 重同步）已在 **`proto/hgic.c`** —— 那是
 *     **与本仓 `tools/txah_hgic.py` 逐字节对拍过**的一份（c3-1：14 + 12 + 13 条向量）。
 *   · 本驱动只做：开 USART 与中断、把收到的字节**入环**、主循环把环里的字节喂给
 *     `hgic_parser_feed()`、把 `hgic_frame_*` 组好的帧写出去。**不重新实现任何帧逻辑**。
 *
 * ★ 为什么"中断只入环、解析留给主循环"（不是图省事）：
 *   1) 中断里**不能打印** —— `uart_printf()` 是阻塞 TX，占住中断时模组还在送字节 ⇒ 丢数据；
 *   2) 环满时**看得见**（`ring_drop` 计数，不静默丢）；解析器在主循环里照常重同步。
 *   代价：主循环必须**勤调** `hgic_uart_poll()`（本仓 main 是空转循环，够快）。
 *
 * 实测依据（不是猜的）：`docs/txah-uart-macbus.md`（模组 UART0 = IOA10(TX)/IOA11(RX)、
 *   115200 8N1、数据面 = `FRM2` 内嵌一整个以太帧）+ `proto/hgic.h` 文件头列的 SDK 源码。
 * ★ 引脚/端口口径**仍是待定项**：全部从 `Core/board.h` 的 `MODULE_*` 宏取，本文件不写死。
 */
#ifndef __HGIC_UART_H__
#define __HGIC_UART_H__

#include <stddef.h>
#include <stdint.h>

#include "hgic.h"          /* proto/ 的帧层（Makefile 里 -I../proto） */

/* 收到的帧的回调。**在主循环上下文**被调用（不是中断）⇒ 里面可以打印、可以做慢活。
 * ⚠ `payload` 指向**解析器内部缓冲**，下一次 `hgic_uart_poll()` 就失效 —— 要留下就自己拷。 */
typedef void (*hgic_rx_fn)(const hgic_hdr_t *h, const uint8_t *payload, size_t plen);

typedef struct {
    uint32_t rx_bytes;      /* 中断里收到的总字节数 */
    uint32_t rx_frames;     /* 切出来的完整帧（数据帧 + 控制面） */
    uint32_t rx_frm2;       /* 其中数据帧 `FRM2`/`FRM`（= 一整个以太帧） */
    uint32_t rx_ctrl;       /* 其中控制面 `CMD`/`CMD2`/`EVENT`/`EVENT2` */
    uint32_t tx_frames;     /* 主机发出去的帧 */
    uint32_t ring_drop;     /* ★ 环满而丢掉的字节（**可见**，不静默） */
    uint32_t garbage;       /* 解析器丢的杂字节（对应 PC 侧 `stats().bad_bytes` 的一半） */
    uint32_t bad_length;    /* 假 magic / 长度字段非法而跳过的个数 */
    uint32_t overrun;       /* 单次 feed 超出解析器内部缓冲（**C 独有**，Python 无上限） */
} hgic_uart_stats_t;

/* 初始化：USART + RX 中断 + 环形缓冲 + 帧解析器（只收"模组→主机"方向）。
 * ⚠ 调用前 GPIO 时钟要已开（本仓 main 里开 GPIOA（心跳灯 PA15 同属 GPIOA）+ AFIO）。*/
void hgic_uart_init(hgic_rx_fn cb);

/* 主循环调（越勤越好）：排空环 → 喂解析器 → 切出的帧回调 `cb`。 */
void hgic_uart_poll(void);

/* 发送：`frame` 必须是**已由 `hgic_frame_*` 组好的整帧**（返回写出的字节数 / HGIC_E_*）。*/
int hgic_uart_send(const uint8_t *frame, size_t len);

/* 上行一条以太帧：内部组 `FRM2` 再发（`ethlen` 含 14 B 以太头）。*/
int hgic_uart_send_frm2(const uint8_t *eth, size_t ethlen);

/* 发一条命令帧（只读命令如 `GET_UART_FIXLEN` 用它）。*/
int hgic_uart_send_cmd(uint16_t cmd_id, const void *params, size_t plen);

void    hgic_uart_stats(hgic_uart_stats_t *out);
uint8_t hgic_uart_last_cookie(void);      /* 最近一次发出的 cookie（对拍/日志用） */

#endif /* __HGIC_UART_H__ */
