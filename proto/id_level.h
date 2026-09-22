/* id_level.h — §8.2 降级选级（设备侧策略）**唯一源**
 *
 * 单一源：`orpah-over-halow/orpah_id.py` 的 `pick_level(se_ok, sign_ok, hmac_ok)` + `LEVEL_MODES`。
 * 规范：`OrpahIDProtocol.md` §8.1（四级表）/ §8.2（降级流程，照字面实现）。
 *
 * 为什么单独一个文件：**选级是策略、不是加密** —— 页面（演示下拉）、PC 仿真器、固件三处都要用它。
 * 各写一份的坏法是"某一处多一个模式/少一条分支"，而现象只是**级别悄悄不对**（没有报错）。
 *
 * §8.2 的四个条件（与上游逐字对应）：
 *   Step1 SE(ATECC608B) I2C 通信失败 → 看 CH32 有没有 HMAC 密钥：有 → L2 / 无 → L3
 *   Step2 SE 通、`atcab_sign(Slot0)` 失败 → L1（改用 Slot 5 的 HMAC）
 *   Step2 SE 通、签名成功 → L0（正常 ECDSA）
 */
#ifndef ORPAH_ID_LEVEL_H
#define ORPAH_ID_LEVEL_H

/* 级别（§8.1）。alg 字符串由 `idr_alg_of()` 给（别在这里再抄一份）。*/
#define IDL_L0 0        /* ECDSA P-256（正常）*/
#define IDL_L1 1        /* Slot 0 签名失败 → HMAC-SHA256 */
#define IDL_L2 2        /* SE 完全不可用 → HMAC-SHA256 */
#define IDL_L3 3        /* 无可用密钥 → 不签名 */

/* 选级。`reason` 可传 NULL；非 NULL 时写入的字符串与上游 `pick_level` **逐字相同**
 * （"normal" / "slot0_sign_failed" / "se_unavailable" / "no_key"）——
 * 页面/日志里显示的就是它，对拍也靠它。*/
int idl_pick(int se_ok, int sign_ok, int hmac_ok, const char **reason);

/* §8.2 的**故障注入**：模式名 → 喂给 `idl_pick()` 的"哪个环节坏了"。
 * 单一源 = 上游 `LEVEL_MODES`（顺序也照它：auto / sign_fail / se_fail / no_key）。
 * ★ 传的是**故障**、不是 level —— 级别必须由 `idl_pick()` 算出来，
 *   直接写死 level 会把演示变成假演（上游写这句时就是踩过这个）。
 * 成功返回 0 并写三个出参；模式名不认识返回 -1。*/
int idl_mode_inputs(const char *mode, int *se_ok, int *sign_ok, int *hmac_ok);

/* 模式表（给控制台 help / 页面下拉用，顺序 = 上游 LEVEL_MODES 的插入序）*/
#define IDL_MODE_COUNT 4
const char *idl_mode_name(int idx);        /* 越界返回 NULL */

#endif /* ORPAH_ID_LEVEL_H */
