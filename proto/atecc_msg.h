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
 * ⚠ **待上机确认的一条**（摘要手册没写、我们按公开实现推的形状）：响应头是
 *   **4 字节大端 count**，`count = 4 + 数据长 (+2 若有 CRC)` ⇒ `Random` 应得
 *   **36**（无 CRC）或 **38**（有 CRC）。真机第一次 `Random` 若报"count 不符合预期"，
 *   打出来的就是实测的原始字节 —— 按它改**这一处常量**即可，别去猜。
 *   （为什么要用 CRC 这件事本身也取决于芯片 Config 区的 ChipMode 位 ⇒ 见
 *   `Periph/atecc.c` 的"两种模式各试一次、记住哪种通"的自动判定。）
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
#define ATECC_RESP_LEN_NOCRC     (4u + ATECC_RANDOM_BYTES)          /* 36 */
#define ATECC_RESP_LEN_CRC       (4u + ATECC_RANDOM_BYTES + 2u)     /* 38 */
#define ATECC_RESP4_LEN_NOCRC    (4u + 4u)                          /* 8  = Info/Read 不带 CRC */
#define ATECC_RESP4_LEN_CRC      (4u + 4u + 2u)                     /* 10 = Info/Read 带 CRC */

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

/* 取出"4 字节数据"类响应（Info/Read）：`resp` = 完整响应（含 4 B count）。
 * `n` 必须 ∈ {8, 10}（10 时**真验** CRC）。返回 0 = OK。*/
int atecc_msg_resp_get4(const uint8_t *resp, size_t n, uint8_t out4[4]);

/* 响应前 4 字节 → count（**大端**）。*/
uint32_t atecc_msg_count(const uint8_t c4[4]);

/* 校验并取出 32 字节随机数。`resp` = 完整响应（含 4 字节 count）。
 * `n` 必须 ∈ {36, 38}：36 = 无 CRC、38 = 带 CRC（会**真验**那 2 字节）。
 * 返回 0 = OK；<0 = 错误（见下）。*/
#define ATECC_MSG_E_ARG   (-1)
#define ATECC_MSG_E_LEN   (-2)      /* 长度不是 36/38 */
#define ATECC_MSG_E_CRC   (-3)      /* 响应 CRC 对不上 */
int atecc_msg_resp_random(const uint8_t *resp, size_t n, uint8_t out32[ATECC_RANDOM_BYTES]);

#endif /* ORPAH_ATECC_MSG_H */
