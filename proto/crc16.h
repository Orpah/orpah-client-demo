/* crc16.h — 微芯 ATECC 系列用的 CRC-16（**照抄 CryptoAuthLib `atCRC()` 的口径**）
 *
 * ★★ 2026-10-10 定案（见 `docs/atecc608b-se.md` §8.2 第四段）：这个 CRC **不是**标准 CRC 变体 ✗
 *   —— 它是**混合口径**：**数据位低位先喂**、**寄存器左移**、poly 0x8005、初值 0。
 *   我们先前按 CRC-16/BUYPASS 写 ✗ ⇒ 器件把**每一条**命令都判成 "bad crc"（状态 0xFF）
 *   ⇒ 它从来**没有执行过一条命令** ✓（这就是 c4-γ-2 到 2026-10-10 全部怪现象的根因）。
 *
 * 出处（**外部权威**，只引“口径/行为”：Microchip CryptoAuthLib `lib/calib/calib_command.c::atCRC()`）：
 *   · 数据位的喂入顺序是**低位先**（`data_bit = (data[counter] & shift_register)`，`shift_register` 从 0x01 递增）
 *   · 寄存器是**左移**（`crc_bit = crc_register >> 15; crc_register <<= 1;`），失配则异或 poly `0x8005`
 *   · 初值 0；输出**小端**（低字节在前）；覆盖 count..最后一个数据字节（不含 CRC 自身）
 *
 * ★ **本仓的实现不搬它的代码** ✓：上面这个口径等价于
 *     「先把每个字节**按位倒过来**，再喂进**教科书式 MSB-first 的 CRC-16**（poly 0x8005、初值 0）」
 *   —— 因为“低位先喂数据 + 寄存器左移”与“先倒位 + 按高位移出”**逐位等价**
 *   （喂入的位序列相同、寄存器状态更新规则相同 ⇒ 同一个 16 位余数）。
 *   所以 `crc16.c` 用我们自己那份 MSB-first 写法 + 一个 `rev8()`，**只看行为、不拷代码** ✓。
 *
 * ★ 外部锚点：这个算法**在 CRC 目录里没有** ✗（所以不能拿 "123456789" 的目录校验值当锚点）
 *   ⇒ 锚点用**另一方产出的真实字节**（器件实回 / 原厂库实发，四个都记在 docs 里）：
 *     · 器件实回 `04 11`            → CRC `33 43`（= 0x4333，唤醒成功）
 *     · 器件实回 `04 FF`            → CRC `01 42`（= 0x4201，状态 0xFF = bad crc）
 *     · 原厂实发 `07 1B 00 00 00`  → CRC `24 CD`（= 0xCD24，atcab_random）
 *     · 原厂实发 `07 30 00 00 00`  → CRC `03 5D`（= 0x5D03，atcab_info）
 *   `proto/atecc_cli.c selfcheck` 与 `proto/run_cross_test.py` 都拿它们当判据 ✓。
 *
 * 纯函数、无 stdio/malloc ⇒ 固件与 PC 侧交叉测试**同一份源码**（AGENTS §4）。
 */
#ifndef ORPAH_CRC16_H
#define ORPAH_CRC16_H

#include <stddef.h>
#include <stdint.h>

/* 外部锚点（取自器件实回 / 原厂库实发的字节，详见文件头注释与 docs）*/
#define CRC16_ATCA_WAKE_CRC   0x4333u   /* crc16_atca({04,11})           唤醒成功 */
#define CRC16_ATCA_ERR_CRC    0x4201u   /* crc16_atca({04,FF})           状态 0xFF */
#define CRC16_ATCA_RAND_CRC   0xCD24u   /* crc16_atca({07,1B,00,00,00})  atcab_random */
#define CRC16_ATCA_INFO_CRC   0x5D03u   /* crc16_atca({07,30,00,00,00})  atcab_info */

uint16_t crc16_atca(const uint8_t *data, size_t len);

/* 同上，但按 ATECC 的线上字节序写**小端**两字节（低字节在前）。*/
void crc16_atca_le(const uint8_t *data, size_t len, uint8_t out_le[2]);

#endif /* ORPAH_CRC16_H */
