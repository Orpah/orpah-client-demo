/* atecc.h — ATECC608B 安全元件驱动（c4-γ-2：**先用它的 RNG 当 nonce 后端**）
 *
 * 本文件管的范围（刻意做小）：**唤醒 + 发一条命令 + 收一条响应**，外加
 * `Random(0x1B)` 与"给 `id_build` 用的 nonce provider"。**不做**：
 *   · Slot0 私钥 / 签名（那是 d 步；现在 level=0 的签名仍是软件 P-256 替身）
 *   · Config 区读写 / 锁 / 产线烧录（d 步）
 * 报文编解码在 `proto/atecc_msg.c`（纯函数，可 PC 对拍）；I²C 在 `Periph/i2c.c`。
 *
 * ★ 唤醒时序（你自己的摘要手册 §2.3 给了三个下界，都是**最小宽度**）：
 *   · 上电后 `tPU ≥ 100 µs` 才能拉 SDA；
 *   · `tWLO ≥ 60 µs`（SDA 拉低）；
 *   · `tWHI ≥ 1500 µs`（SDA 拉高后到能发数据）；
 *   · **`tWHIST ≥ 20 ms`（使能自检时！）** —— 手册同一张表列的是**两个情形**：
 *     未使能自检 `tWHI ≥ 1500 µs`、**使能自检 `tWHIST ≥ 20 ms`**；两行的说明都写着
 *     "SDA should be stable high for this entire duration **unless polling is implemented**"
 *     （`tPU` 那行还补了"使能上电自检时上电延时会长得多"）
 *     ⇒ 所以 `atecc_wake()` **每轮重做整段唤醒序列、最多 `ATECC_WAKE_TRIES` 次**：
 *       只等 1.7 ms 且只发一次令牌时，**自检使能的正常芯片也会回 NACK**（2026-09-22 上机现象）。
 *   · `tWATCHDOG 0.7/1.3/1.7 s`：唤醒后这么久没收到**合法命令**就自己回睡。
 *   ⇒ 所以**每条命令前都要唤醒**（我们 60 s 才发一次 ID，中间它早睡了）。
 *   ⚠ 拉低 SDA 那一下必须把 **I2C 外设先关掉**（`i2c_disable`）：否则外设会把
 *     "SCL 高时 SDA 下降"当成一次 START，后面 `wait_flag(SB)` 会在错的时刻满足。
 *
 * ⚠ **如实**：唤醒脉冲的宽度靠 `i2c_delay_us()` 忙等（**近似**，见 i2c.h），
 *   两个约束都是下界 ⇒ 忙等宁可长；但**没有示波器核对过**真实宽度。
 */
#ifndef __ATECC_H__
#define __ATECC_H__

#include <stddef.h>
#include <stdint.h>

#include "atecc_msg.h"          /* 命令/响应常量（`ATECC_RANDOM_BYTES` 等）*/

/* nonce 只需要 16 字节（32 个十六进制字符，与软熵后端同长度 ⇒ 报文形状不变）*/
#define ATECC_NONCE_BYTES  16

typedef struct {
    uint32_t wake_ok;        /* 唤醒后收到 ACK 的次数 */
    uint32_t wake_fail;      /* 唤醒没 ACK 的次数（没接线/没供电/睡着了）*/
    uint32_t last_wake_tries;/* 最近一次唤醒**轮询了几次令牌**（1 = 第一次就 ACK）——
                              * ★ 这个数就是"这颗芯片到底要等多久"的**实测值**：
                              *   1 ⇒ 普通情形（tWHI ≥1.5 ms 就够）；远大于 1 ⇒ 它在自检
                              *   （手册 tWHIST ≥20 ms，见 `atecc.c` 的 `atecc_wake()`）。*/
    uint32_t cmd;            /* 发出去的命令条数 */
    uint32_t ok;             /* 成功取到 32 字节的次数 */
    uint32_t fail;           /* 失败次数 */
    uint32_t crc_mode;       /* 实测：0 = 线上不带 CRC、1 = 带 CRC（见 .c 的自动判定）*/
    uint32_t last_err;       /* 最近一次的错误码（负数转正存放）*/
    uint8_t  last_resp[8];   /* 最近一次「响应不符合预期」的前 8 字节（**给现场看**）*/
    uint32_t last_resp_len;  /* 那一次实际读到的长度 */
} atecc_stats_t;

/* 初始化 I2C 并按需探测（**探测失败不算致命**：没接 SE 也要能跑软熵后端）。
 * 返回 0 = SE 可用；<0 = 不可用（错误码见下），调用方据此决定 nonce 后端。*/
int atecc_init(void);

/* SE 可用吗（`atecc_init()` 的结论，含唤醒 + 一条 Random 自检）*/
int atecc_present(void);

/* 取 32 字节随机数（每次都**重新唤醒**）。0 = OK。*/
int atecc_random(uint8_t out32[ATECC_RANDOM_BYTES]);

/* 唤醒（SDA 低 ≥60 µs → 高 ≥1500 µs → 写唤醒令牌地址 0x00 等 ACK）。0 = OK。*/
int atecc_wake(void);

/* 上机自检：唤醒 + 一条 Random，把 count/CRC 模式/前 8 字节打出来（**现场判据**）。
 * 返回 0 = OK。*/
int atecc_selftest(void);

/* ★ 工装（2026-09-23）：把 **SCL/SDA 当普通 GPIO 开漏翻转** `seconds` 秒，
 * 供"沿整条路逐点量"**定位断线 / 虚焊 / 孔位不对 / 探点不对**。
 * 两条线用**不同频率**（SCL 2 Hz、SDA 1 Hz）—— 逐只点芯片的脚时，
 * **快翻转的那只 = SCL 网、慢翻转的那只 = SDA 网**（恒定高 = VCC、恒定低 = GND、
 * 不动 = NC）⇒ 不需要知道脚号也能得到一张"脚位真值表"。
 * 高电平仍靠外部 4.7 k 上拉（**开漏**）⇒ 看到的电平与真跑 I²C 时同一套，才有可比性。
 * 结束时调 `i2c_init()` 恢复复用开漏（单一源）。返回 0。*/
int atecc_line_test(uint32_t seconds);
/* ★ 工装（2026-09-23）：**不信"唤醒令牌没 ACK"就等于器件不在**——只用脉冲 + tWHI，
 * 然后**扫 0x01~0x7F**（地址可编程，不能假定 0x60；先打两线空闲电平），
 * 命中要求**探 3 次里 ≥2 次 ACK**（单次 ACK 当偶然，不当器件 —— 2026-09-23 实测出现过），
 * 命中后接着逐项验：
 * `Info(0x30)` 读版本 → `Read` 配置区 word 0x15（= 字节 0x54）判锁 → `Random(0x1B)`，
 * 最后打一行**结论**（addr / rev / cfg=locked|unlocked / random=OK|失败）。
 * 依据：手册表 2-2 说的是"脉冲 + tWHI → **Data Comm**"，
 * 而"地址 0x00 的唤醒令牌"是**器件在 Sleep 态**才应答的东西 —— 器件若已在 Idle，
 * 它对 0x00 会回 NACK，而旧固件在这种情形下**从来没试过后面的命令**。
 * 扫不到任何地址时**照样**在旧地址上写一条 `Random`（万一扫描本身有毛病）再下结论 ——
 * **两种情形都会打那一行结论**（一颗芯片一行判据）。
 * 返回 0 = 命令通了。*/
int atecc_diag_probe(void);

/* ★ 工装（2026-09-23）：**重复扫描统计** —— 一轮 = 完整唤醒序列 + 扫 0x01~0x7F。
 * 一次 ACK 说明不了问题，但"答中几轮 / 共几轮"一下就把三种情形分开：
 *   真实器件（每轮都答）/ 虚焊接触不良（时有时无）/ 纯毛刺（偶发且地址还会变）。
 * 也是"不靠示波器"的做法：那一下太短又不常出现，重复采样交给固件做。
 * `rounds` = 0 ⇒ 默认 10；上限 `ATECC_SCAN_MAX_ROUNDS`。
 * 返回 0 = 找到"每轮都答"的稳定器件（可接着 `sewake` 逐项验）。*/
int atecc_diag_scan_stats(uint32_t rounds);

/* ★ 命令用的 7 位器件地址（缺省 = `i2c.h` 的 `I2C_ADDR_ATECC`）。
 * 依据：手册第 13 页 "Programmable I2C address after data (secret) zone lock"
 * ⇒ 608B 的地址**可编程**，不一定是 0x60；公开实测里有 0x35 这类例子。*/
void    atecc_set_addr(uint8_t addr7);
uint8_t atecc_get_addr(void);
void atecc_stats(atecc_stats_t *out);

/* 给 `proto/id_build.c` 用的 nonce provider（`idb_nonce_fn` 的形状）：
 * 取 32 字节随机数的**前 16 字节** → 32 个大写十六进制字符 + NUL。
 * ⚠ 为什么只用 16 字节：与软熵后端同长度 ⇒ 报文（和已入库的黄金向量）形状不变。
 *   规范只要求"nonce 来自 SE 的 RNG"，没规定长度。*/
int atecc_nonce_hex(char out[ATECC_NONCE_BYTES * 2 + 1], void *ctx);

#define ATECC_E_ARG     (-1)
#define ATECC_E_WAKE    (-2)     /* 唤醒没 ACK */
#define ATECC_E_IO      (-3)     /* I²C 读写失败（NACK/超时）*/
#define ATECC_E_RESP    (-4)     /* 响应不符合预期（长度/count/CRC）*/

#endif /* __ATECC_H__ */
