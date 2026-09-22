/* crc16.h — 微芯 ATECC 系列用的 CRC-16（**MSB-first、poly 0x8005、初值 0**）
 *
 * 为什么不用常见那个"CRC-16/ARC"：ATECC 的 CRC 是**不反射**的算法。
 * 出处（**外部权威**，不是我们自己拍的）：Microchip CryptoAuthLib
 *   · `lib/calib/calib_command.c` 的 `atCRC()`：
 *       `uint16_t crc_register = 0; uint16_t polynom = 0x8005u;`
 *       `crc_bit = (uint8_t)(crc_register >> 15); crc_register <<= 1;`
 *       `if (data_bit != crc_bit) crc_register ^= polynom;`
 *       ⇒ 先取**最高位**、整体左移 ⇒ **MSB-first**（反射版会右移）。
 *   · 输出**小端**：`crc_le[0] = crc_register & 0xFF; crc_le[1] = crc_register >> 8;`
 * 套到 CRC 目录（CRC catalogue）里就是 **CRC-16/BUYPASS**（= CRC-16/UMTS）：
 *   width 16 / poly 0x8005 / init 0x0000 / refin false / refout false / xorout 0x0000
 *   ⇒ 校验值（"123456789"）= **0xFEE8**，用作本仓的**外部黄金向量**（`test_vectors_crc16.txt`）。
 *
 * 纯函数、无 stdio/malloc ⇒ 固件与 PC 侧交叉测试**同一份源码**（AGENTS §4）。
 */
#ifndef ORPAH_CRC16_H
#define ORPAH_CRC16_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/BUYPASS 对 "123456789" 的校验值（外部目录给出的常量，用于自检）*/
#define CRC16_BYPASS_CHECK  0xFEE8u

uint16_t crc16_bypass(const uint8_t *data, size_t len);

/* 同上，但按 ATECC 的线上字节序写**小端**两字节（低字节在前）。*/
void crc16_bypass_le(const uint8_t *data, size_t len, uint8_t out_le[2]);

#endif /* ORPAH_CRC16_H */
