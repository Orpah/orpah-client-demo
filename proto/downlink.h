/* downlink.h — 下行报文解码（设备侧）
 *
 * 单一源：`orpah-over-halow/orpah_proto.py` 的 `decode_msg` + 设备侧实际用到的字段
 * （`client.py` 的 `_rx_loop` / `client_sim.DeviceSim.on_down` 看的就是
 *  `type` / `tracked` / `status` / `code` 这几个）。
 *
 * ★ 一处**有意加严**（与 Python 不同，见 proto/README.md「已知边界」）：
 *   C 拒绝浮点/超 int64 整数/嵌套 > 8 层；Python 的 json 会照收。
 *   我们的下行报文不用这些形状 ⇒ 真收到了宁可**报错**（可能是有人改了规范），不要猜。
 */
#ifndef ORPAH_DOWNLINK_H
#define ORPAH_DOWNLINK_H

#include "jcs.h"

/* 报文类型（= orpah_proto 的 MSG_*） */
#define DL_T_REQ_CONNECT    "ORPAH-REQ-CONNECT"
#define DL_T_ACCESS_INFO    "ORPAH-ACCESS-INFO"
#define DL_T_REPORT         "ORPAH-REPORT"
#define DL_T_TRACKING       "ORPAH-TRACKING-STATUS"
#define DL_T_ERROR          "ORPAH-ERROR"
#define DL_T_LOST_TABLE     "ORPAH-LOST-TABLE"
#define DL_T_LOST_TABLE_REQ "ORPAH-LOST-TABLE-REQ"
#define DL_T_FOUND          "ORPAH-FOUND"
#define DL_T_ID_REPORT      "ORPAH-ID-REPORT"

/* 状态码（= orpah_proto 的 ST_*） */
#define DL_ST_NOT_TRACKED   "NOT-TRACKED"
#define DL_ST_TRACKED       "TRACKED"
#define DL_ST_LOG_OK        "LOG-OK"

/* 错误码（分开编号，便于定位“哪一步不像” ） */
#define DL_E_NOT_JSON       (-1)    /* 解析失败 */
#define DL_E_NOT_OBJECT     (-2)    /* 顶层不是对象（Python: not isinstance(msg, dict)）*/
#define DL_E_UNKNOWN_TYPE   (-3)    /* type 缺失或不在 MSG_TYPES 里 */

/* 解析 + 校验（语义同 decode_msg）：成功 0 且 *msg 为根对象；否则 DL_E_* */
int dl_decode(jcs_ctx_t *c, const char *buf, size_t len, jv_t **msg);

int dl_type_known(const char *type);

/* 报文字段（缺失/类型不符一律返回 NULL 或 0，不抛不猜） */
const char *dl_type(const jv_t *msg);
const char *dl_str(const jv_t *msg, const char *key);
int         dl_int(const jv_t *msg, const char *key, long long *out);

/* Python 真值语义（`if msg.get(k):`）：缺失 → 返回 0；
 * 有值 → 返回 1 并写 *out（0/1）。空串/空数组/空对象/0/false/null 都是假。*/
int         dl_truthy(const jv_t *msg, const char *key, int *out);

#endif /* ORPAH_DOWNLINK_H */
