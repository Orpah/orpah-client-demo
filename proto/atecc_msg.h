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
 *   · CRC = 原厂 `atCRC()` 口径（**低位先喂数据 + 寄存器左移**，poly 0x8005，见 `crc16.h` 的出处），
 *     **覆盖 count..数据、不含 CRC 自身**；
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
#define ATECC_OP_NONCE           0x16u   /* ★ d 步：把 32 B 摘要装进器件缓冲 */
#define ATECC_OP_GENKEY          0x40u   /* ★ d 步：由已有私钥算公钥（工装/产线自检）*/
#define ATECC_OP_SIGN            0x41u   /* ★ d 步：用槽里的私钥签名 */
#define ATECC_MODE_SEED_UPDATE   0x00u
#define ATECC_MODE_NO_SEED_UPDATE 0x01u

/* 配置区 **word 0x15**（LockValue/LockConfig/LockData…）——
 * ★★ 2026-10-11 **实测纠正**：`Read` 的 param2 是**字地址**（word），**不是字节地址** ✗ ——
 *   原厂栈读配置区时实发 `07 02 80 00 00 09 AD` / `07 02 80 08 00 0A 4D` / …
 *   （四块 = 字地址 0x00 / 0x08 / 0x10 / 0x18 = 每块 8 个字 ✓）⇒ 锁字节在
 *   **字节 0x54 = 字 0x15** ⇒ param2 要写 **0x15** ✓（原来写 0x54 = 字 0x54 = 字节 0x150 ✗ 跑到区外了 ✗）。
 *   出处：`atcab_is_locked()` 读的就是这一处、`response[0] == 0x55 ⇒ 配置区未锁` ✓。*/
#define ATECC_ZONE_CONFIG        0x00u
#define ATECC_CFG_WORD_LOCK      0x15u   /* ★ 字地址（= 字节地址 0x54 / 4） */

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

/* ------------------------------------------------------------------ */
/* ★ d 步：Nonce(0x16) / Sign(0x41) —— **字节全部来自实测**（2026-10-11 台架）           */
/* ------------------------------------------------------------------ */
/* 台架：CH347F 当唯一主机，用**原厂库**驱动这颗已配好并双锁的 608B（Slot 0 = P-256 私钥），
 *   HAL 打出的 `send(0x03)` 就是下面这两条包（见 `docs/atecc608b-se.md` §8.10）：
 *     Nonce：`27 16 43 00 00 <32 B 摘要> 30 1A`   （count = 0x27 = 39 ✓）
 *     Sign ：`07 41 A0 00 00 7B 85`              （count = 0x07 ✓、param2 = 槽号 ✓）
 *     GenKey：`07 40 00 00 00 00 05`               （由已有私钥算公钥 ✓）
 *   ⇒ 数值以**实测**为准；名字只引原厂 API 说明里给得出的那些（`atcab_nonce_load()` 的
 *     docstring 列了目标 `TEMPKEY`/`MSGDIGBUF`/`ALTKEYBUF`）——**名↔值的对应是按字节反推的** ✓。*/
#define ATECC_NONCE_DIGEST_BYTES 32u
/* Nonce 的 param1 = 实测 `0x43`：原厂栈用它是「pass-through 模式 + 目标 = Message Digest Buffer」
 * （608 的这一档 = 把外部给的 32 B **当成摘要**直接装进消息摘要缓冲，不再自己哈希 ✓）*/
#define ATECC_NONCE_MODE_MSGDIGBUF  0x43u
/* Sign 的 param1 = 实测 `0xA0`（原厂栈的「外部消息」那一档）；param2 = **槽号**（Slot 0 ⇒ 0x0000）*/
#define ATECC_SIGN_MODE_EXTERNAL    0xA0u
#define ATECC_SIGN_BYTES         64u   /* r||s 各 32 B 大端（与原厂 `atcab_sign()` 的说明一致 ✓）*/
/* 包长：Nonce 多带 32 B 数据；响应多带 64 B 签名
 * （`27 16 …` = 39 = 1 count + 1 op + 1 p1 + 2 p2 + 32 数据 + 2 CRC ✓；
 *   签名响应 `43 …` = 67 = 1 count + 64 签名 + 2 CRC ✓）*/
#define ATECC_CMD_LEN_NONCE_NOCRC  (ATECC_CMD_LEN_NOCRC + ATECC_NONCE_DIGEST_BYTES)
#define ATECC_CMD_LEN_NONCE_CRC    (ATECC_CMD_LEN_CRC + ATECC_NONCE_DIGEST_BYTES)
/* ------------------------------------------------------------------ */
/* ★★ d 步（2026-10-11 上机）：`SHA(0x47)` —— **HMAC 也走这一条**               */
/* ------------------------------------------------------------------ */
/* 608 上 HMAC 的“开始/结束”就是 `SHA` 的两种模式 ✓（原厂 `calib_sha_hmac_init/update/finish()`）。
 * 形状照我们 PC 侧那份**已上机验证**的实现（`tools/se_provision.py` 的 `sha_hmac()`，
 * 出处 `docs/atecc608b-se.md` §8.24 ✓）：
 *   ① `SHA(HMAC_START=0x04, param2=**key slot**)`                    ⇒ 4 B 状态包（状态 0x00 = 成功）
 *   ② `SHA(HMAC_UPDATE=0x01, param2=**本块字节数**, + 该块)`          ⇒ 同丠（0 次或多次）
 *   ③ `SHA(608_HMAC_END=0x02 | TARGET_TEMPKEY=0x00, param2=**尾块字节数**, + 尾块)`
 *                                                          ⇒ 33/35 B（32 B 摘要）
 * ★★ **外部锚点**（§8.24 那次实测的**实发字节**）：`0747040400ae47` = 恰好 **7 B**
 *   ⇒ `count=07 | op=47 | p1=04 | param2=0400`（**小端** = 4 ✓ 就是 HMAC 槽）`| CRC=ae47` ✓
 *   同一处还记了“**UPDATE 长度 `0x47`** = 71 = 1+1+1+2+64+2 ✓、块长 `0x40` = 64 ✓”
 *   ⇒ 本文件的包长/块长与那次真机包**逐字节对得上** ✓（不是我自己推的 ✗）。
 * ★ 用 START 的 slot = 4：原厂自己的测试夹具就是“HMAC 用 slot 4” ✓（见 §8.5 注 ✓）。*/
#define ATECC_OP_SHA               0x47u
#define ATECC_SHA_MODE_HMAC_START  0x04u
#define ATECC_SHA_MODE_HMAC_UPDATE 0x01u
#define ATECC_SHA_MODE_608_HMAC_END 0x02u   /* 608 的结束码（库里按器件型号选 ✓）*/
#define ATECC_SHA_TARGET_TEMPKEY   0x00u    /* 结果落点 = TempKey；响应里总回 32 B 摘要 ✓ */
#define ATECC_SHA_BLOCK            64u      /* 一次喂一块（原厂 `SHA_DATA_MAX` ✓）*/
#define ATECC_HMAC_SLOT            4u       /* 降级 HMAC 钥的槽 */
#define ATECC_SHA_DIGEST_BYTES     32u
/* UPDATE/END 带数据 ⇒ **最长包就在这里**（= 1 count + 1 op + 1 p1 + 2 p2 + 64 块 + 2 CRC = 71）*/
#define ATECC_CMD_LEN_SHA_DATA_NOCRC (ATECC_CMD_LEN_NOCRC + ATECC_SHA_BLOCK)      /* 69 */
#define ATECC_CMD_LEN_SHA_DATA_CRC   (ATECC_CMD_LEN_CRC + ATECC_SHA_BLOCK)        /* 71 */
#define ATECC_RESP_LEN_SHA_NOCRC   (ATECC_RESP_DATA_IDX + ATECC_SHA_DIGEST_BYTES)      /* 33 */
#define ATECC_RESP_LEN_SHA_CRC     (ATECC_RESP_DATA_IDX + ATECC_SHA_DIGEST_BYTES + 2u) /* 35 */
/* ★ **命令包的最长长度**（★ 2026-10-11 更新）：现在最长的是 **SHA UPDATE**（71 B）✗
 *   —— 不再是 Nonce 的 39 ✗。传输层（`Periph/atecc.c`）的发包缓冲与长度上限都引它，
 *   **别各自写数** ✗：那里曾按 7 B 写，Nonce（39 B）一发包就被守卫拦下（还差一点写出栈溢出 ✗），
 *   而现场日志只看到“装摘要失败”、看不到原因 ✗。
 *   ⚠ 若新增更长的包，**必须同步改这里**（一处定义，多处引用 ✓）。*/
#define ATECC_CMD_LEN_MAX          ATECC_CMD_LEN_SHA_DATA_CRC
#define ATECC_RESP_LEN_SIG_NOCRC   (ATECC_RESP_DATA_IDX + ATECC_SIGN_BYTES)
#define ATECC_RESP_LEN_SIG_CRC     (ATECC_RESP_DATA_IDX + ATECC_SIGN_BYTES + 2u)
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

/* ★ d 步：`Nonce(0x16)` 的「pass-through 装入 32 B 摘要」那一档。
 * 形状 = count + op + param1(`0x43`) + param2(2 B) + **32 B 数据** + [CRC]。
 * 返回包长（`ATECC_CMD_LEN_NONCE_*`，37/39），0 = 容量不够。*/
size_t atecc_msg_nonce_load(uint8_t *out, size_t cap,
                            const uint8_t digest32[ATECC_NONCE_DIGEST_BYTES], int with_crc);

/* ★ d 步：`Sign(0x41)` 的「对外部消息（= 上面那条 Nonce 装进去的摘要）签名」。
 * 形状 = count + op + param1(`0xA0`) + **槽号**(2 B) + [CRC]。返回包长（5/7），0 = 容量不够。*/
size_t atecc_msg_sign(uint8_t *out, size_t cap, uint16_t slot, int with_crc);

/* ★ d 步：校验并取出**签名**（`r||s` 64 B）。`n` 必须 ∈ {65, 67}（67 时**真验** CRC）。
 * 返回 0 = OK；<0 同下（`ATECC_MSG_E_*`）。*/
int atecc_msg_resp_sign(const uint8_t *resp, size_t n, uint8_t out64[ATECC_SIGN_BYTES]);

/* ★★ d 步：`SHA(0x47)` 三连（HMAC）—— 钥在 `key_slot` 里，**不出芯片** ✓。
 * 形状/锚点见上面那一段。三者都回**包长**（0 = 容量不够）：
 *   · `atecc_msg_sha_start(slot)`            = 5/7（无数据命令）
 *   · `atecc_msg_sha_update(blk, n)`         = 69/71（`n` 应为 `ATECC_SHA_BLOCK` = 64）
 *   · `atecc_msg_sha_hmac_end(tail, n)`      = `n + 5`/`n + 7`（`n` < 64）
 * ⚠ `n` 必须 ≤ `ATECC_SHA_BLOCK`（超过就该分块 ✗ —— 器件一次只吃 64 B）。
 * ★★ **`param2` 的填法不统一，是实测出来的**（2026-10-11 ✗）：`START` = key slot ✓、
 *   **`UPDATE` = 0** ✓（✗ 填 64 会让器件把后面任一条命令判错 —— 长度已经在 `count` 里 ✓）、
 *   `END` = 尾块长度 ✓。三者都对得上“器件内 HMAC = 软件 HMAC”那条判据 ✓。*/
size_t atecc_msg_sha_start(uint8_t *out, size_t cap, uint16_t key_slot, int with_crc);
size_t atecc_msg_sha_update(uint8_t *out, size_t cap,
                            const uint8_t *blk, size_t n, int with_crc);
size_t atecc_msg_sha_hmac_end(uint8_t *out, size_t cap,
                              const uint8_t *tail, size_t n, int with_crc);

/* ★ d 步：取出 **32 B HMAC 摘要**（`n` 必须 ∈ {33, 35}；35 时**真验** CRC）。
 * 返回 0 = OK；<0 同 `ATECC_MSG_E_*`。*/
int atecc_msg_resp_sha(const uint8_t *resp, size_t n, uint8_t out32[ATECC_SHA_DIGEST_BYTES]);

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
