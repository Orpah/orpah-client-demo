# checks_report.md —— `tools/run_checks.py` 的输出（**入库**，同隔壁仓惯例）

跑的时间：2026-10-12 00:56:05

判定口径：**退出码 0 且输出里没有 `FAIL` / `Traceback`**（只看退出码会漏 —— 有些脚本自己吞异常）。

| 结果 | 闸门 | 命令 | rc |
|---|---|---|---|
| ✅ PASS | 协议内核交叉测试（C ↔ Python） | `C:\Python313\python.exe run_cross_test.py` | 0 |
| ✅ PASS | 接线图网表 vs 规格表（9 张） | `C:\Python313\python.exe fzz_nets.py` | 0 |
| ✅ PASS | 标注扫描（量尺，不作闸门） | `C:\Python313\python.exe fzz_annot_scan.py --svg` | 0 |
| ✅ PASS | Python 语法（tools/ + proto/） | `C:\Python313\python.exe -m compileall -q tools proto` | 0 |
| ✅ PASS | 固件编译（riscv-none-embed-gcc，要求零 warning） | `make -C firmware SHELL=D:\Program Files\Git\bin\sh.exe` | 0 |
| ⏭ skip | 台架判据（check_c4g2_bench.py，要接板子 ⇒ 用 --bench） | — | — |

## 各闸门输出的尾部（便于「上次为什么红」一眼可查）

### 协议内核交叉测试（C ↔ Python）（rc=0，PASS）

```
C 侧已编译：P-256 曲线（cl (vcvars64, /std:c11)）
PASS C selfcheck（P-256 不变量）  ->  PASS selfcheck（不变量全过）
PASS C pubkey-selftest（公钥派生向量）  ->  PASS 14/14 行（F:\git\orpah-client-demo\proto\test_vectors_p256.txt）
C 侧已编译：ECDSA/RFC6979（cl (vcvars64, /std:c11)）
PASS C selfcheck（ECDSA/RFC6979 不变量）  ->  PASS selfcheck（ECDSA/RFC6979 不变量）
PASS C k-selftest（RFC 6979 官方 k 向量）  ->  PASS k-selftest 2/2 行（F:\git\orpah-client-demo\proto\test_vectors_ecdsa_rfc6979.txt）
PASS C sign-selftest（RFC 6979 官方 r||s 向量）  ->  PASS sign-selftest 2/2 行（F:\git\orpah-client-demo\proto\test_vectors_ecdsa_rfc6979.txt）
PASS C sign-selftest（本仓 ECDSA 向量）  ->  PASS sign-selftest 25/25 行（F:\git\orpah-client-demo\proto\test_vectors_ecdsa.txt）
C 侧已编译：ID 流水线（§8.2 选级 / 演示密钥 / nonce / 整帧）（cl (vcvars64, /std:c11)）
PASS C selfcheck（选级/nonce/流水线不变量）  ->  PASS id-cli selfcheck（选级/nonce/流水线/降级路径/签名器注入/缓冲不够报错）
PASS C keys-selftest（演示密钥向量）  ->  PASS keys-selftest 4/4 行（F:\git\orpah-client-demo\proto\test_vectors_id_keys.txt）
PASS C level-selftest（选级向量）  ->  PASS level-selftest 13/13 行（F:\git\orpah-client-demo\proto\test_vectors_id_level.txt）
PASS C nonce-selftest（nonce 向量）  ->  PASS nonce-selftest 5/5 行（F:\git\orpah-client-demo\proto\test_vectors_id_nonce.txt）
PASS C frame-selftest（整帧向量）  ->  PASS frame-selftest 5/5 行（F:\git\orpah-client-demo\proto\test_vectors_id_frame.txt）
C 侧已编译：ATECC608B 报文层（cl (vcvars64, /std:c11)）
PASS C selfcheck（CRC 外部锚点 + Random 包 + 响应解析）  ->  PASS ATECC608B 报文层自检（★ 原厂 atCRC 口径的 CRC + Random/Info/Read + Nonce/Sign 包 + **SHA/HMAC 三连** + 响应解析（含签名 r||s），共 11 类）
PASS C crc-selftest（CRC16 向量）  ->  PASS F:\git\orpah-client-demo\proto\test_vectors_crc16.txt：7 行全部一致
PASS C msg-selftest（Random 包/响应向量）  ->  PASS F:\git\orpah-client-demo\proto\test_vectors_atecc.txt：41 行全部一致
   [info] L3(alg=none) 服务端判定：accepted=True trust=none coverage_only=True
PASS 服务端验签 C 产出的报文（verify_report 接受 6 条 level=0/1/2）
PASS OpenSSL 验签通过 C 产出的 r||s（2 条 RFC 官方向量）
   [info] L3(alg=none) 整帧：accepted=True coverage_only=True
PASS 上游 verify_report 收下 C 产出的整帧（4 条 level=0/1/2）
------------------------------------------------------------------
PASS 协议内核：C 实现与 Python 参考零偏差（校验位 / SN 解析 / JCS / b64url / 报文信封 / 下行解码 / SHA-256 / HMAC / 已签报文 / HGIC 帧构造 / HGIC 流解析 / HGIC 控制面 / P-256 公钥派生 / ECDSA 签名（RFC 6979，含 RFC §A.2.5 官方向量）/ ID 流水线（§8.2 选级 + 演示密钥 + nonce + 整帧）/ ATECC608B 报文层（★ 原厂 atCRC 口径的 CRC + Random/Info/Read + Nonce/Sign 包 + 签名 r||s 解析）十六组）
```

### 接线图网表 vs 规格表（9 张）（rc=0，PASS）

```
========================================================================================
cstep-nanoch32-atecc608b.fzz -> F:\git\orpah-client-demo\hardware\wiring\cstep-nanoch32-atecc608b.fzz
  实例： PCB1=5783, U5=86334118, Breadboard1=86334129, R1=86334204, R2=86334316, IMG1=86334537, U6=86334794
  实际网表（9 个网）：
    Breadboard1.A4  <->  Breadboard1.E4  <->  U5.GND  <->  U6.GND  <->  U6.VSSA  <->  U6.VSS_1  <->  U6.VSS_2  <->  U6.VSS_3
    Breadboard1.E5  <->  U5.NC
    Breadboard1.E6  <->  U5.NC
    Breadboard1.E7  <->  U5.NC
    Breadboard1.F10  <->  Breadboard1.F4  <->  Breadboard1.H10  <->  Breadboard1.H4  <->  Breadboard1.J4  <->  R1.connector1  <->  U5.SDA  <->  U6.PB7/USB2DM
    Breadboard1.F5  <->  Breadboard1.I11  <->  Breadboard1.I5  <->  Breadboard1.J11  <->  Breadboard1.J5  <->  R2.connector1  <->  U5.SCL  <->  U6.PB6/USB2DP
    Breadboard1.F6  <->  U5.NC
    Breadboard1.F7  <->  Breadboard1.G7  <->  Breadboard1.I7  <->  Breadboard1.J7  <->  R1.connector0  <->  R2.connector0  <->  U5.VCC  <->  U6.VDDA  <->  U6.VDD_2  <->  U6.VDD_VIO_1  <->  U6.VDD_VIO_3
    U6.5V
  --- 逐网核对（模块侧不限实例编号）---
    ✓ MCU.PB7/USB2DM  <->  电阻.connector1  <->  安全元件.SDA  （图上 = R1.connector1  U5.SDA  U6.PB7/USB2DM）
    ✓ MCU.PB6/USB2DP  <->  电阻.connector1  <->  安全元件.SCL  （图上 = R2.connector1  U5.SCL  U6.PB6/USB2DP）
    ✓ MCU.VDDA  <->  电阻.connector0  <->  安全元件.VCC  （图上 = R2.connector0  U5.VCC  U6.VDDA）
    ✓ MCU.GND  <->  安全元件.GND                      （图上 = U5.GND  U6.GND）
    · 非连线（部件内部同网 / 空孔）：U5.NC  @E7
    · 非连线（部件内部同网 / 空孔）：U5.NC  @E6
    · 非连线（部件内部同网 / 空孔）：U5.NC  @F6
    · 非连线（部件内部同网 / 空孔）：U5.NC  @E5
  结论：全部对上 ✓
========================================================================================
一共 9 张图，0 张有问题
```

### 标注扫描（量尺，不作闸门）（rc=0，PASS）

```
扫 9 个 .fzz，找：打印日志 / 供电 / I2C通讯
  bstep-ch347f-2txah-evb.fzz: （都没有）
  bstep-ch347f-txah-evb-thrj45.fzz: （都没有）
  bstep-ch347f-txah-evb.fzz: （都没有）
  cstep-ch347f-atecc608b.fzz: （都没有）
  cstep-ch347f-nanoch32-atecc608b.fzz: （都没有）
  cstep-ch347f-nanoch32-i2ctest.fzz: （都没有）
  cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz: （都没有）
  cstep-ch347f-txah-evb-nanoch32.fzz: （都没有）
  cstep-nanoch32-atecc608b.fzz: （都没有）
--- 同名 .svg 对照 ---
  bstep-ch347f-2txah-evb.svg: 打印日志 / 供电
  bstep-ch347f-txah-evb-thrj45.svg: 打印日志 / 供电
  bstep-ch347f-txah-evb.svg: 打印日志 / 供电
  cstep-ch347f-atecc608b.svg: 供电 / I2C通讯
  cstep-ch347f-nanoch32-atecc608b.svg: 打印日志 / 供电 / I2C通讯
  cstep-ch347f-nanoch32-i2ctest.svg: 打印日志 / 供电 / I2C通讯
  cstep-ch347f-txah-evb-nanoch32-atecc608b.svg: 打印日志 / 供电
  cstep-ch347f-txah-evb-nanoch32.svg: 打印日志 / 供电
  cstep-nanoch32-atecc608b.svg: 供电 / I2C通讯
合计命中 0 处（.fzz 内）。0 = 这些字都只在导出的 .svg 上。
```

### Python 语法（tools/ + proto/）（rc=0，PASS）

```

```

### 固件编译（riscv-none-embed-gcc，要求零 warning）（rc=0，PASS）

```
make: Entering directory 'F:/git/orpah-client-demo/firmware'
make: Nothing to be done for 'all'.
make: Leaving directory 'F:/git/orpah-client-demo/firmware'
```

## 需要台架的（不在默认里）

`--bench` 会另外跑 `tools/check_c4g2_bench.py`（SE 唤醒/Random 真随机数/**Slot 0 签名 + 上位机用 Slot 0 公钥独立验签**/`nonce_src`/`sign_src`/上游 `verify_report` 收帧）。跑之前先确认台架与 `hardware/wiring/README.md` 一致。
