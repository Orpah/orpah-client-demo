/* crc16.h — 微芯 ATECC 系列用的 CRC-16（**照抄 CryptoAuthLib `atCRC()` 的口径**）
 *
 * ★★ 2026-10-10 定案（见 `docs/atecc608b-se.md` §8.2 第四段）：这个 CRC **不是**标准 CRC 变体 ✗
 *   —— 它是**混合口径**：**数据位低位先喂**、**寄存器左移**、poly 0x8005、初值 0。
 *   我们先前按 CRC-16/BUYPASS 写 ✗ ⇒ 器件把**每一条**命令都判成 "bad crc"（状态 0xFF）
 *   ⇒ 它从来**没有执行过一条命令** ✓（这就是 c4-γ-2 到 2026-10-10 全部怪现象的根因）。
 *
 * 出处（**外部权威**）：Microchip CryptoAuthLib `lib/calib/calib_command.c::atCRC()`：
 *       uint16_t crc_register = 0;  uint16_t polynom = 0x8005;
 *       for (counter = 0; counter < length; counter++)
 *           for (shift_register = 0x01; shift_register > 0x00; shift_register <<= 1) {
 *               data_bit = (data[counter] & shift_register) ? 1 : 0;  // ★ 低位先喂
 *               crc_bit  = crc_register >> 15;
 *               crc_register <<= 1;                                   // ★ 左移
 *               if (data_bit != crc_bit) { crc_register ^= polynom; }
 *           }
 *       crc_le[0] = crc_register & 0xFF;  crc_le[1] = crc_register >> 8;  // 输出小端
 *   覆盖范围 = 从 count 字节到**第二个 CRC 字节之前**（`atCalcCrc()`：`length = txsize - ATCA_CRC_SIZE`）。
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
