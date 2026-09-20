/* msg.h — ORPAH 链路报文构造器（设备侧要发的三种）
 *
 * 单一源：`orpah-over-halow/orpah_proto.py` 的 `_base` 与 `build_*`。
 *
 * ★ 三个容易踩的点：
 *   1. **信封不是 JCS**：`encode_msg` 用 `json.dumps(..., separators=(",",":"))`
 *      **不带 sort_keys** ⇒ 键按**插入序**（本文件的构造顺序 = Python 源码里的字面量顺序）。
 *      JCS 只用于**签名预像**。编码时用 `jcs_encode_raw()`，**别用** `jcs_encode()`。
 *   2. `_base(..., **kw)` 里 kw 的插入顺序 = 调用处的书写顺序（`build_report` 是
 *      **seq → cap → rssi**，不是 seq → rssi → cap）。
 *   3. Python 的 `if mac:` / `if cap:` 是**真值判断**（空串/None 都算不给），
 *      而 `if rssi is not None` 只看 None ⇒ 本文件区分 `""` 与 `NULL`、并用显式的
 *      `rssi_set` 表达"给不给 rssi"。
 *
 * ⚠ 本文件只做**设备要发**的三种（REQ-CONNECT / REPORT / ID-REPORT）；
 *   Router/Server 侧的那些构造器（ACCESS-INFO / LOST-TABLE / FOUND …）不在 c 步范围。
 */
#ifndef ORPAH_MSG_H
#define ORPAH_MSG_H

#include "jcs.h"

#define PROTO_VERSION     1

#define MSG_REQ_CONNECT   "ORPAH-REQ-CONNECT"
#define MSG_REPORT        "ORPAH-REPORT"
#define MSG_ID_REPORT     "ORPAH-ID-REPORT"

/* cap.rtc 三态（对应 Python：不给 cap / {"rtc": False} / {"rtc": True}） */
#define MSG_CAP_NONE      (-1)
#define MSG_CAP_FALSE     0
#define MSG_CAP_TRUE      1

/* C→R ORPAH-REQ-CONNECT。mac/hw 传 NULL 或空串 = 不写该字段（对应 `if mac:`） */
jv_t *msg_req_connect(jcs_ctx_t *c, const char *sn, long long ts,
                      const char *mac, const char *hw);

/* C→R/S ORPAH-REPORT。cap_rtc 取 MSG_CAP_*；rssi_set=0 表示不写 rssi */
jv_t *msg_report(jcs_ctx_t *c, const char *sn, long long ts, int seq,
                 int cap_rtc, int rssi_set, int rssi);

/* C→R→S ORPAH-ID-REPORT：把已签的 orpah-id-report 包一层。
 * `sn` = **内层 payload.sn**（Python 从 signed_report 里取，调用方负责传同一个值）；
 * `signed_report` = 内层 {hdr,payload[,sig]} 节点（可含 sig）。*/
jv_t *msg_id_report(jcs_ctx_t *c, const jv_t *signed_report,
                    const char *sn, long long ts);

#endif /* ORPAH_MSG_H */
