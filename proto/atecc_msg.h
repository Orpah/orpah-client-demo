/* atecc_msg.h — ATECC608B 主机消息的**纯编解码**（不含任何硬件；可 PC 侧对拍）
 *
 * 与硬件无关的那一半放这里（`Periph/atecc.c` 只管 I²C 与唤醒脉冲）——
 * 这样一组黄金向量就能让 **C 实现与 Python 参考零偏差**（AGENTS §4）。
 *
 * 事实与出处（**外部权威**，取自 Microchip CryptoAuthLib；我们的摘要手册
 * `ATECC608B.pdf` 是 NDA 版摘要、没有命令集，见 `docs/atecc608b-se.md`）：
 *   · `Random` 命令码 **0x1B**、模式 `0x00`(自动更新种子)/`0x01`(不更新)、
 *     返回 **32 B** —— `lib/calib/calib_command.h`（`ATCA_RANDOM`、`RANDOM_SEED_UPDATE`…）
 *   · 无数据命令的包长 = `ATCA_CMD_SIZE_MIN`（**count(1)+opcode(1)+param1(1)+param2(2)
 *     +CRC(2) = 7**；不挂 CRC 时 5）
 *   · CRC = CRC-16/BUYPASS（见 `crc16.h` 的出处），**覆盖 count..数据、不含 CRC 自身**；
 *     挂在包尾是**小端**（`crc_le[0]` 是低字节）
 *
 * ★★ **响应形状已定案（2026-10-10 上机改正）**：I²C 上 **count 只占 1 字节**、数据从
 *   `resp[1]` 起。出处 = CryptoAuthLib 的 I²C HAL（`lib/hal/hal_sam_i2c_asf.c` /
 *   `lib/hal/hal_i2c_start.c` / `lib/hal/hal_uc3_i2c_asf.c`）里的 `hal_i2c_receive()`：
 *     `read_length = rxdata[0];`  ← **这就是 count（1 字节）**
 *   只有 **TA 器件**才把它当 2 字节（`read_length = rxdata[0]*256 + rxdata[1]`，
 *   见同一段里 `if (atcab_is_ta_device(cfg->devtype))`）；单线侧同样 1 字节
 *   （`lib/hal/hal_swi_gpio.h` 的 `ATCA_1WIRE_RESPONSE_LENGTH_SIZE 0x01`）。
 *   ⇒ `Random` 响应 = **33**（无 CRC）/ **35**（带 CRC）；`Info`/`Read` = **5** / **7**。
 *   ✗ 我们原来按"**4 字节大端 count**、数据在第 5 字节、36/38"写 —— 那是把单线侧
 *     "4 字节长度"的形状套到 I²C 上了。2026-10-10 实测把它踩出来：那一刻器件回的是
 *     **4 字节错误包** `04 FF 01 42`，我们却把它算成 `0x04FF0142` ⇒ 报"长度离谱"✗
 *     （真正的含义见下面那条）。
 *   ★ 强制件证（为什么必须是 1 字节）：唤醒应答 **总共只有 4 字节**、且库里是
 *     **整体 memcmp 4 字节**（`lib/hal/atca_hal.c::hal_check_wake()` 期望
 *     `{0x04,0x11,0x33,0x43}`、自检失败 `{0x04,0x07,0xC4,0x40}`）——
 *     count 若占 4 字节，那两包就该是 7 字节 ✗。
 *   （线上要不要挂 CRC 由芯片 Config 区 ChipMode 决定 ⇒ 见 `Periph/atecc.c` 的
 *     "两种模式各试一次、记住哪种通"的自动判定。）
 *
 * ★★ **4 字节错误包**（出处 `lib/calib/calib_command.c::isATCAError()`，原文注释
 *   "error packets are always **4 bytes long**"）：形状 = `count(=4)` + **状态码** + 2 字节，
 *   状态码在 `resp[1]`。这就是器件"说话"的最小形状 ⇒
 *   `atecc_msg_err_pkt_status()` 把状态读出来、`atecc_msg_status_name()` 翻成文字
 *   （**照库里的原话**，不自创说法）。实测样例：`04 11 33 43` = 0x11（刚被唤醒）、
 *   `04 FF 01 42` = 0xFF（CRC 不对/命令没被正确收到）。
 */
#ifndef ORPAH_ATECC_MSG_H
#define ORPAH_ATECC_MSG_H

#include <stddef.h>
#include <stdint.h>

#define ATECC_OP_RANDOM          0x1Bu
#define ATECC_OP_INFO            0x30u   /* 工装：读器件版本（响应 4 B）*/
#define ATECC_OP_READ            0x02u   /* 工装：读配置区某处（响应 4 B）*/
#define ATECC_MODE_SEED_UPDATE   0x00u
#define ATECC_MODE_NO_SEED_UPDATE 0x01u

/* 配置区 word 0x15（LockValue/LockConfig/LockData…）的**字节地址** = 0x15 × 4 = 0x54。
 * 依据：`Read` 的地址是**字节地址且必须是 4 的倍数**（0x15 不是 4 的倍数），
 * 而 CryptoAuthLib `atcab_is_locked()` 读的就是这一处：**response[0] == 0x55 ⇒ 配置区未锁**。
 * （我们读出来的是不是 `55 00 …`，本身就能反证这个地址推得对不对。）*/
#define ATECC_ZONE_CONFIG        0x00u
#define ATECC_CFG_ADDR_LOCK      0x54u

#define ATECC_RANDOM_BYTES       32u
#define ATECC_CMD_LEN_NOCRC      5u    /* count+opcode+param1+param2 */
#define ATECC_CMD_LEN_CRC        7u
/* ★ 响应里**数据从第 2 个字节起**（count 只占 1 字节，见文件头的出处）*/
#define ATECC_RESP_DATA_IDX      1u
#define ATECC_RESP_LEN_NOCRC     (ATECC_RESP_DATA_IDX + ATECC_RANDOM_BYTES)       /* 33 */
#define ATECC_RESP_LEN_CRC       (ATECC_RESP_DATA_IDX + ATECC_RANDOM_BYTES + 2u)  /* 35 */
#define ATECC_RESP4_LEN_NOCRC    (ATECC_RESP_DATA_IDX + 4u)                       /* 5 = Info/Read 不带 CRC */
#define ATECC_RESP4_LEN_CRC      (ATECC_RESP_DATA_IDX + 4u + 2u)                  /* 7 = Info/Read 带 CRC */
/* 4 字节错误包的长度与"响应最小长度"（出处 `lib/calib/calib_command.h`：
 * `ATCA_RSP_SIZE_MIN` = 4；`calib_execution.c` 里 `if (rxsize < 4u) ATCA_RX_FAIL`）*/
#define ATECC_ERR_PKT_LEN        4u
#define ATECC_RSP_SIZE_MIN       4u
/* 状态码：出处 `lib/calib/calib_command.h` 的 `CMD_STATUS_*` 与
 * `lib/calib/calib_command.c::isATCAError()` 的 switch（**照它的语义，不改写**）*/
#define ATECC_STATUS_OK          0x00u  /* 没错误（安全器件在 isATCAError 里也是这条）*/
#define ATECC_STATUS_CHECKMAC    0x01u  /* CheckMac/Verify 不匹配 */
#define ATECC_STATUS_PARSE       0x03u  /* 收到的长度/opcode/参数非法 */
#define ATECC_STATUS_ECC         0x05u  /* ECC 计算错误 */
#define ATECC_STATUS_SELFTEST    0x07u  /* 芯片处于自检失败态 */
#define ATECC_STATUS_RNG_HEALTH  0x08u  /* 随机数健康检查失败 */
#define ATECC_STATUS_EXEC        0x0Fu  /* 芯片当前状态执行不了这条命令 */
#define ATECC_STATUS_WAKEUP      0x11u  /* 刚被成功唤醒（这就是唤醒应答里的状态）*/
#define ATECC_STATUS_COMM        0xFFu  /* CRC 不对（命令没被正确收到）或其它通讯错 */

/* 组一条 Random 命令包（**不含** I²C 的字地址 0x03，那属于传输层）。
 * `with_crc` != 0 时包尾附小端 CRC。返回写入字节数（5 或 7），0 = 容量不够。*/
size_t atecc_msg_random(uint8_t *out, size_t cap, uint8_t mode, int with_crc);

/* ★ 工装（2026-09-23）：`Info`（读器件版本）/ `Read`（读区里 4 字节）的包构造。
 * 形状与 `atecc_msg_random()` **同一套规则**（count + opcode + param1 + param2 + [CRC]）⇒
 * 本文件是包形状的**单一源**，`atecc.c` 里不再自己拼字节。
 * ⚠ 形状照 CryptoAuthLib（`ATCA_INFO`/`ATCA_READ`），但**尚未做 PC 对拍**（工装级诊断；
 *   d 步做正式命令层时会连同向量一起补）。返回包长（5/7），0 = 容量不够。*/
size_t atecc_msg_info(uint8_t *out, size_t cap, uint8_t mode, int with_crc);
size_t atecc_msg_read(uint8_t *out, size_t cap, uint8_t zone, uint16_t addr, int with_crc);

/* 取出"4 字节数据"类响应（Info/Read）：`resp` = 完整响应（含 1 B count）。
 * `n` 必须 ∈ {5, 7}（7 时**真验** CRC）。返回 0 = OK。*/
int atecc_msg_resp_get4(const uint8_t *resp, size_t n, uint8_t out4[4]);

/* 响应第 1 字节 → count（**1 字节**，不是 4 字节大端；见文件头）。*/
uint32_t atecc_msg_count(const uint8_t *resp);

/* 4 字节错误包 → 状态码（取自 `resp[1]`）。
 * 返回 0 = 确实是错误包（`*status` 有效）；<0 = 长度不对（不是 4 字节 / count 字段不是 4）。*/
int atecc_msg_err_pkt_status(const uint8_t *resp, size_t n, uint8_t *status);

/* 状态码 → 文字（英文，**照 CryptoAuthLib 的说法**；未知码回 "unknown"）。*/
const char *atecc_msg_status_name(uint8_t status);

/* 校验并取出 32 字节随机数。`resp` = 完整响应（含 1 字节 count）。
 * `n` 必须 ∈ {33, 35}：33 = 无 CRC、35 = 带 CRC（会**真验**那 2 字节）。
 * 返回 0 = OK；<0 = 错误（见下）。*/
#define ATECC_MSG_E_ARG   (-1)
#define ATECC_MSG_E_LEN   (-2)      /* 长度不是 33/35 */
#define ATECC_MSG_E_CRC   (-3)      /* 响应 CRC 对不上 */
int atecc_msg_resp_random(const uint8_t *resp, size_t n, uint8_t out32[ATECC_RANDOM_BYTES]);

#endif /* ORPAH_ATECC_MSG_H */
