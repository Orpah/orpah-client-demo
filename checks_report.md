# checks_report.md —— `tools/run_checks.py` 的输出（**入库**，同隔壁仓惯例）

跑的时间：2026-10-20 01:12:51

判定口径：**退出码 0 且输出里没有 `FAIL` / `Traceback`**（只看退出码会漏 —— 有些脚本自己吞异常）。

| 结果 | 闸门 | 命令 | rc |
|---|---|---|---|
| ✅ PASS | 协议内核交叉测试（C ↔ Python） | `C:\Python313\python.exe run_cross_test.py` | 0 |
| ✅ PASS | 接线图网表 vs 规格表（9 张） | `C:\Python313\python.exe fzz_nets.py` | 0 |
| ✅ PASS | 线色闸门 carrier-board.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py carrier-board.fzz` | 0 |
| ✅ PASS | 线色闸门 carrier-breadboard.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py carrier-breadboard.fzz` | 0 |
| ✅ PASS | 线色闸门 carrier-breadboard_byHand.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py carrier-breadboard_byHand.fzz` | 0 |
| ✅ PASS | 线色闸门 carrier-pcb.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py carrier-pcb.fzz` | 0 |
| ✅ PASS | 线色闸门 carrier-schematic.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py carrier-schematic.fzz` | 0 |
| ✅ PASS | 线色闸门 bstep-ch347f-2txah-evb.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py bstep-ch347f-2txah-evb.fzz` | 0 |
| ✅ PASS | 线色闸门 bstep-ch347f-txah-evb-thrj45.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py bstep-ch347f-txah-evb-thrj45.fzz` | 0 |
| ✅ PASS | 线色闸门 bstep-ch347f-txah-evb.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py bstep-ch347f-txah-evb.fzz` | 0 |
| ✅ PASS | 线色闸门 cstep-ch347f-atecc608b.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py cstep-ch347f-atecc608b.fzz` | 0 |
| ✅ PASS | 线色闸门 cstep-ch347f-nanoch32-atecc608b.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py cstep-ch347f-nanoch32-atecc608b.fzz` | 0 |
| ✅ PASS | 线色闸门 cstep-ch347f-nanoch32-i2ctest.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py cstep-ch347f-nanoch32-i2ctest.fzz` | 0 |
| ✅ PASS | 线色闸门 cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz` | 0 |
| ✅ PASS | 线色闸门 cstep-ch347f-txah-evb-nanoch32.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py cstep-ch347f-txah-evb-nanoch32.fzz` | 0 |
| ✅ PASS | 线色闸门 cstep-nanoch32-atecc608b.fzz | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\file_sanity.py cstep-nanoch32-atecc608b.fzz` | 0 |
| ✅ PASS | 面包板网表（bb_probe：<connect>/<buses>） | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\bb_probe.py carrier-breadboard.fzz --nets=F:\git\orpah-client-demo\hardware\carrier\carrier_nets.py` | 0 |
| ✅ PASS | 面包板摆位+接线（bb_precheck：几何；⑦ = 导线端点 vs 引脚接线点） | `C:\Python313\python.exe F:\git\fritzing-parts-langhua\tools\bb_precheck.py carrier-breadboard.fzz --nets=F:\git\orpah-client-demo\hardware\carrier\carrier_nets.py` | 0 |
| ✅ PASS | 标注扫描（量尺，不作闸门） | `C:\Python313\python.exe fzz_annot_scan.py --svg` | 0 |
| ✅ PASS | Python 语法（tools/ + proto/ + firmware/） | `C:\Python313\python.exe -m compileall -q tools proto firmware` | 0 |
| ✅ PASS | 固件编译-载板档（BOARD=carrier ⇒ 心跳灯 PB0 + SE I2C2，要求零 warning；✗ 未上机） | `make -C firmware rebuild SHELL=D:\Program Files\Git\bin\sh.exe BOARD=carrier` | 0 |
| ✅ PASS | 固件从零编译（riscv-none-embed-gcc，要求零 warning） | `make -C firmware rebuild SHELL=D:\Program Files\Git\bin\sh.exe` | 0 |
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
    ✓ MCU.VDDA  <->  电阻.connector0  <->  安全元件.VCC  （图上 = R1.connector0  U5.VCC  U6.VDDA）
    ✓ MCU.GND  <->  安全元件.GND                      （图上 = U5.GND  U6.GND）
    · 非连线（部件内部同网 / 空孔）：U5.NC  @E7
    · 非连线（部件内部同网 / 空孔）：U5.NC  @E6
    · 非连线（部件内部同网 / 空孔）：U5.NC  @F6
    · 非连线（部件内部同网 / 空孔）：U5.NC  @E5
  结论：全部对上 ✓
========================================================================================
一共 9 张图，0 张有问题
```

### 线色闸门 carrier-board.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\carrier\carrier-board.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 本图没有原理图导线 ⇒ **判不了** ✓ 不判 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: ['TXT90000013=#333333']
   ④ 面包板导线 1 段（0 根）| 颜色: ['#404040']

== 图例区（x>500 ✓ 按 y 排 ✓）
   y= 144.0  Wire90000012   color=#404040   长度 27.0

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）
   y=  12.0  TXT90000013    ''

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 carrier-breadboard.fzz（rc=0，PASS）

```
   y= 166.0  Wire90000330   color=#999999   长度 9.0
   y= 178.0  Wire90000332   color=#999999   长度 9.0
   y= 190.0  Wire90000334   color=#ef6100   长度 9.0
   y= 202.0  Wire90000336   color=#ef6100   长度 9.0

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）
   y=  12.0  TXT90000307    ''
   y=  31.0  TXT90000309    ''
   y=  43.0  TXT90000311    ''
   y=  55.0  TXT90000313    ''
   y=  67.0  TXT90000315    ''
   y=  79.0  TXT90000317    ''
   y=  91.0  TXT90000319    ''
   y= 103.0  TXT90000321    ''
   y= 115.0  TXT90000323    ''
   y= 127.0  TXT90000325    ''
   y= 139.0  TXT90000327    ''
   y= 151.0  TXT90000329    ''
   y= 163.0  TXT90000331    ''
   y= 175.0  TXT90000333    ''
   y= 187.0  TXT90000335    ''
   y= 199.0  TXT90000337    ''

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 carrier-breadboard_byHand.fzz（rc=0，PASS）

```
   y= 166.0  Wire90000330   color=#999999   长度 9.0
   y= 178.0  Wire90000332   color=#999999   长度 9.0
   y= 190.0  Wire90000334   color=#ef6100   长度 9.0
   y= 202.0  Wire90000336   color=#ef6100   长度 9.0

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）
   y=  12.0  TXT90000307    ''
   y=  31.0  TXT90000309    ''
   y=  43.0  TXT90000311    ''
   y=  55.0  TXT90000313    ''
   y=  67.0  TXT90000315    ''
   y=  79.0  TXT90000317    ''
   y=  91.0  TXT90000319    ''
   y= 103.0  TXT90000321    ''
   y= 115.0  TXT90000323    ''
   y= 127.0  TXT90000325    ''
   y= 139.0  TXT90000327    ''
   y= 151.0  TXT90000329    ''
   y= 163.0  TXT90000331    ''
   y= 175.0  TXT90000333    ''
   y= 187.0  TXT90000335    ''
   y= 199.0  TXT90000337    ''

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 carrier-pcb.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\carrier\carrier-pcb.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 238 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 238 根：(16 conn), (14 conn), (8 conn), (6 conn), (6 conn)
   ③ 实例 color 属性: ['TXT90000013=#333333']
   ④ 面包板导线 239 段（0 根）| 颜色: ['#404040']

== 图例区（x>500 ✓ 按 y 排 ✓）
   y= 144.0  Wire90000012   color=#404040   长度 27.0

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）
   y=  12.0  TXT90000013    ''

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 carrier-schematic.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\carrier\carrier-schematic.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 80 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 3 根：Wire90000053(3 conn), Wire90000149(3 conn), Wire90000150(3 conn)
   ③ 实例 color 属性: 0 个 ✓（Fritzing 自己也不写 ✓）
   ④ 面包板导线 18 段（12 根）| 颜色: ['#418dd9']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 bstep-ch347f-2txah-evb.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\bstep-ch347f-2txah-evb.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 19 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: 0 个 ✓（Fritzing 自己也不写 ✓）
   ④ 面包板导线 19 段（19 根）| 颜色: ['#25cc35', '#418dd9', '#8c3b00', '#ab58a2', '#ef6100']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 bstep-ch347f-txah-evb-thrj45.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\bstep-ch347f-txah-evb-thrj45.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 8 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: 0 个 ✓（Fritzing 自己也不写 ✓）
   ④ 面包板导线 8 段（8 根）| 颜色: ['#25cc35', '#418dd9', '#8c3b00']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 bstep-ch347f-txah-evb.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\bstep-ch347f-txah-evb.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 16 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: 0 个 ✓（Fritzing 自己也不写 ✓）
   ④ 面包板导线 16 段（16 根）| 颜色: ['#25cc35', '#418dd9', '#8c3b00', '#ab58a2', '#ef6100']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 cstep-ch347f-atecc608b.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\cstep-ch347f-atecc608b.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 14 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: ['IMG1=#787878']
   ④ 面包板导线 14 段（11 根）| 颜色: ['#25cc35', '#418dd9', '#8c3b00', '#cc1414', '#fff800']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 cstep-ch347f-nanoch32-atecc608b.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\cstep-ch347f-nanoch32-atecc608b.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 21 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: ['IMG1=#787878']
   ④ 面包板导线 21 段（13 根）| 颜色: ['#25cc35', '#404040', '#418dd9', '#ab58a2', '#cc1414', '#ef6100']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 cstep-ch347f-nanoch32-i2ctest.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\cstep-ch347f-nanoch32-i2ctest.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 10 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: 0 个 ✓（Fritzing 自己也不写 ✓）
   ④ 面包板导线 10 段（3 根）| 颜色: ['#25cc35', '#404040', '#418dd9', '#ab58a2', '#cc1414', '#ef6100']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\cstep-ch347f-txah-evb-nanoch32-atecc608b.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 36 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: ['IMG1=#787878']
   ④ 面包板导线 36 段（34 根）| 颜色: ['#25cc35', '#418dd9', '#8c3b00', '#ab58a2', '#cc1414', '#ef6100']

== 图例区（x>500 ✓ 按 y 排 ✓）
   y=-144.1  Wire212        color=#8c3b00   长度 72.2
   y=-135.0  Wire83         color=#8c3b00   长度 9.2
   y= -63.0  Wire103        color=#cc1414   长度 108.0
   y= -54.0  Wire117        color=#cc1414   长度 54.0
   y= -45.0  Wire191        color=#cc1414   长度 54.0
   y= -36.0  Wire86         color=#418dd9   长度 9.1
   y= -36.0  Wire97         color=#25cc35   长度 81.0
   y= -27.1  Wire94         color=#418dd9   长度 98.6

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 cstep-ch347f-txah-evb-nanoch32.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\cstep-ch347f-txah-evb-nanoch32.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 25 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: 0 个 ✓（Fritzing 自己也不写 ✓）
   ④ 面包板导线 25 段（25 根）| 颜色: ['#25cc35', '#418dd9', '#8c3b00', '#ab58a2', '#ef6100']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 线色闸门 cstep-nanoch32-atecc608b.fzz（rc=0，PASS）

```
== 文件自检: F:\git\orpah-client-demo\hardware\wiring\cstep-nanoch32-atecc608b.fzz
   ① 非标准色的导线（面包板 ✓ 13 色表）: 0 个 ✓
   ①b 原理图导线 16 段｜颜色 ⊆ 原理图那一节（13 色 ✓）: 0 根 ✓
   ①c 原理图**不许用白/黄** ✗: 0 根 ✓
   ② 零长**真退化**线: 0 个 ✓
   ②' 零长**占位**线 : 0 个 ✓（非本视图的真实走线 ✓ 三视图占位 ✓ 见 gen_routes.wire_block）
   ②'' 零长**接头**线（任一视图带 `<connect>` ⇒ 有意画的连接点 ✓ 不算退化 ✓）: 0 个 ✓
   ③ 实例 color 属性: ['IMG1=#787878']
   ④ 面包板导线 16 段（13 根）| 颜色: ['#25cc35', '#404040', '#418dd9', '#cc1414', '#fff800']

== 图例区（x>500 ✓ 按 y 排 ✓）

== 图例文字（从零件 svg 里读 ✓ 按 y 排 ✓）

⇒ ✓ 全部合格
   （退出码 0 ✓；口径 = `fz_colors` 两张表 ✓ —— 它自己已拿 Fritzing 的 XML 自校过 ✓）
```

### 面包板网表（bb_probe：<connect>/<buses>）（rc=0，PASS）

```
== 面包板网表核对（`bb_probe` ✓ 只看 `<connect>` 与 `<buses>` ✓ 不看几何 ✗）==
   F:\git\orpah-client-demo\hardware\carrier\carrier-breadboard.fzz
   板孔 830 颗 ✓｜bus 130 条 ✓（126×5 + 4×50 = 830 ✓）
   声明（`<connect>`）272 条 ✓｜指向不存在目标的 0 条 ✓
   网 15 张 ✓｜图上出现的脚 62 只 ✓｜`EXPECT` 里点名的脚 **量到 48 / 48** 只 ✓
   3V3       ✓  应有  9 只（图上 9 只 ✓）
   3V3_IN    ✓  应有  4 只（图上 4 只 ✓）
   CON_RX    ✓  应有  2 只（图上 2 只 ✓）
   CON_TX    ✓  应有  2 只（图上 2 只 ✓）
   GND       ✓  应有  7 只（图上 7 只 ✓）
   LED_HB    ✓  应有  2 只（图上 2 只 ✓）
   LED_R     ✓  应有  2 只（图上 2 只 ✓）
   MOD_RX    ✓  应有  2 只（图上 2 只 ✓）
   MOD_TX    ✓  应有  2 只（图上 2 只 ✓）
   SCL       ✓  应有  4 只（图上 4 只 ✓）
   SDA       ✓  应有  4 只（图上 4 只 ✓）
   SWCLK     ✓  应有  2 只（图上 2 只 ✓）
   SWDIO     ✓  应有  2 只（图上 2 只 ✓）
   VBAT      ✓  应有  2 只（图上 2 只 ✓）
   VBAT_OK   ✓  应有  2 只（图上 2 只 ✓）
   · 图上还有 `EXPECT` 没写的脚 17 只（仅报告 ✓）：P:U1.connector5, P:U1.connector11, P:U1.connector4, P:U1.connector13, P:U1.connector1, P:U1.connector12, P:U1.connector6, P:U1.connector19
判定: ✓ 与项目数据一致（15/15 张网连通成一片 ✓）
F:\git\fritzing-parts-langhua\tools\bb_probe.py:121: DeprecationWarning: Testing an element's truth value will always return True in future versions.  Use specific 'len(elem)' or 'elem is not None' test instead.
  host = child(root, "instances") or root
```

### 面包板摆位+接线（bb_precheck：几何；⑦ = 导线端点 vs 引脚接线点）（rc=0，PASS）

```
   [板位] 板子实际在 (4.33364, -9.00437) ✓ ⇒ 孔位整体偏移 (+8.982, -9.004) 单位 = (+2.535, -2.541) mm
== 摆位闸门（§A.4 摆位先于入网 ＋ §A.5 物理在前）==  F:\git\orpah-client-demo\hardware\carrier\carrier-breadboard.fzz
   件 12 个 ✓｜板孔 830 个 ✓｜容差 0.60 mm ✓｜用孔 57 个 ✓
   ① ✓ 每个脚都声明了一颗真实的孔
   ② ✓ 腿端（或盘心）落在声明的那颗孔上 ≤ 0.60 mm
   ④ ✓ 一个孔只被一只脚占用
   ⑤ ✓ 一 bus 一网（没把两张网并到一起）
   ⑥ ✓ 同件内脚与脚的间距 = 元件自称的间距（容差 2.0 mm ✓）
   ⑦ ✓ 接线的脚：导线端点落在它的接线点上 ≤ 0.10 mm
      —— ⑦ 量到 **5 处**接线 ✓（最大 0.000 mm ✓）
      —— ⑥ 量到 **8 只脚**参与比对 ✓（另有 57 只 `geometry=(0,0)`＝没改写 ⇒ 跳过 ✓）
   ③ ✓ 盘心落孔（仅报告 ✓）：2/57 只脚 > 0.60 mm；最大 5.01 mm
      —— 有 `legId` 的件（R1, R2）接点就是**腿端** ✓，盘心远是正常的 ✓
   ✓ 豁免（接导线、不插孔）5 只：U1.connector20, BT1.connector0, BT1.connector1, SW1.connector0, SW1.connector1
   · 悬空 3 只（**网表里没有它** ⇒ 设计如此 ✓ 不算错）：SW1.connector2, SW1.connector3, SW1.connector4
⇒ ✓ **允许布线（摆位到位 ✓）**
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

### Python 语法（tools/ + proto/ + firmware/）（rc=0，PASS）

```

```

### 固件编译-载板档（BOARD=carrier ⇒ 心跳灯 PB0 + SE I2C2，要求零 warning；✗ 未上机）（rc=0，PASS）

```
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/id_report.c -o build/proto/id_report.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/hgic.c -o build/proto/hgic.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/id_build.c -o build/proto/id_build.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/id_level.c -o build/proto/id_level.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/id_keys.c -o build/proto/id_keys.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/id_nonce.c -o build/proto/id_nonce.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/p256.c -o build/proto/p256.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/rfc6979.c -o build/proto/rfc6979.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/ecdsa.c -o build/proto/ecdsa.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/atecc_msg.c -o build/proto/atecc_msg.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=1 -DBOARD_CARRIER=1 -ICore -IPeriph -I../proto -c ../proto/crc16.c -o build/proto/crc16.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -ICore -IPeriph -c startup/startup_ch32v203.S -o build/startup.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -nostartfiles -nostdlib -T ld/link.ld -Wl,--gc-sections -Wl,-Map=build/orpah-client.map -o build/orpah-client.elf build/Core/main.o build/Core/id_core.o build/Periph/gpio.o build/Periph/uart.o build/Periph/i2c.o build/Periph/atecc.o build/Periph/hgic_uart.o build/proto/sn.o build/proto/jcs.o build/proto/b64url.o build/proto/msg.o build/proto/downlink.o build/proto/sha256.o build/proto/hmac.o build/proto/id_report.o build/proto/hgic.o build/proto/id_build.o build/proto/id_level.o build/proto/id_keys.o build/proto/id_nonce.o build/proto/p256.o build/proto/rfc6979.o build/proto/ecdsa.o build/proto/atecc_msg.o build/proto/crc16.o build/startup.o -lgcc
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-size build/orpah-client.elf
   text	   data	    bss	    dec	    hex	filename
  48356	     46	  20176	  68578	  10be2	build/orpah-client.elf
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-objcopy -O binary build/orpah-client.elf build/orpah-client.bin
  built: build/orpah-client.bin  (raw binary; bytes = size text + data above)
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-objcopy -O ihex build/orpah-client.elf build/orpah-client.hex
  built: build/orpah-client.hex  (intel hex; for WCHISPTool / MounRiver download)
  BOARD=carrier => carrier: LED PB0, SE I2C2 -- NOT bench-verified
  NOTE: SE_I2C=1 => ATECC608B on I2C2 (PB10/PB11, carrier tier).
        NOT bench-verified (SE_I2C=0 / I2C1 PB6/PB7 is the verified one).
make[1]: Leaving directory 'F:/git/orpah-client-demo/firmware'
make: Leaving directory 'F:/git/orpah-client-demo/firmware'
```

### 固件从零编译（riscv-none-embed-gcc，要求零 warning）（rc=0，PASS）

```
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/sha256.c -o build/proto/sha256.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/hmac.c -o build/proto/hmac.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/id_report.c -o build/proto/id_report.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/hgic.c -o build/proto/hgic.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/id_build.c -o build/proto/id_build.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/id_level.c -o build/proto/id_level.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/id_keys.c -o build/proto/id_keys.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/id_nonce.c -o build/proto/id_nonce.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/p256.c -o build/proto/p256.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/rfc6979.c -o build/proto/rfc6979.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/ecdsa.c -o build/proto/ecdsa.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/atecc_msg.c -o build/proto/atecc_msg.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os -ffunction-sections -fdata-sections -fmessage-length=0 -fsigned-char -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -DWCH_INTERRUPT_FAST -DSE_I2C_USE_2=0 -DBOARD_CARRIER=0 -ICore -IPeriph -I../proto -c ../proto/crc16.c -o build/proto/crc16.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -ICore -IPeriph -c startup/startup_ch32v203.S -o build/startup.o
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -nostartfiles -nostdlib -T ld/link.ld -Wl,--gc-sections -Wl,-Map=build/orpah-client.map -o build/orpah-client.elf build/Core/main.o build/Core/id_core.o build/Periph/gpio.o build/Periph/uart.o build/Periph/i2c.o build/Periph/atecc.o build/Periph/hgic_uart.o build/proto/sn.o build/proto/jcs.o build/proto/b64url.o build/proto/msg.o build/proto/downlink.o build/proto/sha256.o build/proto/hmac.o build/proto/id_report.o build/proto/hgic.o build/proto/id_build.o build/proto/id_level.o build/proto/id_keys.o build/proto/id_nonce.o build/proto/p256.o build/proto/rfc6979.o build/proto/ecdsa.o build/proto/atecc_msg.o build/proto/crc16.o build/startup.o -lgcc
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-size build/orpah-client.elf
   text	   data	    bss	    dec	    hex	filename
  48340	     46	  20176	  68562	  10bd2	build/orpah-client.elf
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-objcopy -O binary build/orpah-client.elf build/orpah-client.bin
  built: build/orpah-client.bin  (raw binary; bytes = size text + data above)
F:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-objcopy -O ihex build/orpah-client.elf build/orpah-client.hex
  built: build/orpah-client.hex  (intel hex; for WCHISPTool / MounRiver download)
  BOARD=bench => bench: LED PA15, SE I2C1 -- bench-verified
make[1]: Leaving directory 'F:/git/orpah-client-demo/firmware'
make: Leaving directory 'F:/git/orpah-client-demo/firmware'
```

## 需要台架的（不在默认里）

`--bench` 会另外跑 `tools/check_c4g2_bench.py`（SE 唤醒/Random 真随机数/**Slot 0 签名 + 上位机用 Slot 0 公钥独立验签**/`nonce_src`/`sign_src`/上游 `verify_report` 收帧）。跑之前先确认台架与 `hardware/wiring/README.md` 一致。
