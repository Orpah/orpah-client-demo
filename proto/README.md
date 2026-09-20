# proto/ — 协议内核（C 实现）+ 与 Python 的交叉测试

**c2 进度（2026-09-20）：SN 内核已完成并通过交叉测试；报文编解码 / JCS / `b64url` 未做。**

## 为什么要这一层

同一套协议内核要跑在两个地方：PC 上的**上游 Python 参考**，和固件里的 **C**。
两边只要有一处字节不一致，签名就验不过（`signature_invalid`）—— 所以判定**不靠人眼对文档**，
靠**同一批黄金向量对拍**（`../AGENTS.md` §4 定的口径：退出码 0 且输出无 `FAIL`）。

## 本仓不定义协议

单一源永远在上游：`orpah-over-halow/orpah_id.py`（SN/校验位）与
`Protocol/docs/OrpahIDProtocol.md`（规范）。本目录的 C 只是**移植**；有出入以 Python 为准。

## 文件

| 文件 | 作用 |
|---|---|
| `sn.h` / `sn.c` | SN 内核：Crockford 表 / `sn_ok` / `sn_err` / `sn_parse` / Luhn32 / Mod97 / `sn_verify_check`。**无 malloc、无 stdio ⇒ 可直接编进固件** |
| `sn_cli.c` | host 侧 CLI（对拍/调试用；**不编进固件**） |
| `run_cross_test.py` | 一键对拍：生成/校验向量 + 编译 C + 三方比对 |
| `test_vectors_sn.txt` | `ORG-UNIQUE<TAB>LUHN32<TAB>MOD97`（63 行，**Python 生成，勿手改**） |
| `test_vectors_snparse.txt` | `SN<TAB>ok<TAB>err<TAB>verify`（19 行，同上） |

## 跑

```bash
python proto/run_cross_test.py                 # 判定：exit 0 且输出无 FAIL
python proto/run_cross_test.py --refresh       # 用 Python 重写向量（核对 diff 再提交）
python proto/run_cross_test.py --orpah-dir ../orpah-over-halow
```

它对三件事下结论：

1. **C 侧自检**：`selfcheck`（内置黄金/边界 11 项）+ `selftest`（63 行校验位）+ `sn-selftest`（19 行 SN 解析）；
2. **快照没过期**：入库的两份向量文件与 Python 参考**逐行一致**；
3. 合起来 ⇒ **C ↔ 向量文件 ↔ Python 三方零偏差**。

⚠ 找不到上游参考实现时，脚本会**明确打印"未与 Python 交叉验证"**（但仍退出 0，因为 C 侧自检确实过了）——
别把那种 PASS 当成已交叉验证 ✓。

## ★ 一条最要紧的事实：校验位算法是 **Luhn32 / Mod97**，不是 Damm32

`orpah_id.verify_check()` 按**校验位长度**分流：

| 校验位长度 | 算法 | 本仓 C |
|---|---|---|
| 0 位 | 无校验，直接通过 | `sn_verify_check` |
| **1 位** | **Luhn32**（Luhn mod 32，Crockford） | `sn_verify_check_luhn32` |
| **2 位** | **Mod97**（IBAN 思路，输出 02–98） | `sn_verify_check_mod97` |

Damm32（`orpah-over-halow/damm32.py`）是**Phase 2 的替代算法**，`verify_check` 当前**不启用**它
（其自身注释也写着 32×32 表"待定稿"）。固件应当照 `verify_check` 实现 ✓。

## 三条踩过的坑（都写进代码注释了，别再踩）

1. **Luhn32 ≠ Damm32，期望值必须来自 Python 输出，不能凭记忆**：
   本仓 AGENTS 里那句"黄金样本 `WH01-9AF3C1D2 → B`"说的是 **Damm32**；
   **Luhn32 真值是 `E`、Mod97 是 `21`**（而且恰好有另一个输入 `T` 的 Luhn32 是 `B`，更藏得住混淆）。
   我第一次在 `sn_cli.c` 里就写成了 `B` ⇒ 自检当场失败（这正是"别凭记忆写常量"的活教材）。
2. **cmd 里调 `vcvars64.bat` 必须写 `call`**：不带 `call` 时 cmd 把控制权交给批处理就不回来了，
   后面的 `&& cl ...` **永远不执行**，现象是"退出码非 0 且**没有任何输出**"，极难查。
   （上游 `orpah-over-halow/c/run_cross_test.py` 少写了 `call`，那条 MSVC 兜底路径实际跑不通。）
3. **`subprocess` 必须 `encoding="utf-8", errors="replace"`**：Windows 上默认按 GBK 解码子进程输出 ⇒
   读线程抛 `UnicodeDecodeError` 且 **`stdout` 变成空** ⇒ 报错看起来"什么都没打印"。

## 下一步（c2 未完）

报文编解码 + **JCS（RFC 8785）规范化** + `b64url` + **签名预像**。
JCS 是要害（键序按 UTF-16 码元、数字最短表示，差一个字节整条链就验不过），
同样按"**黄金向量 + 对拍**"做，**不与本目录的 SN 内核混在一起**（一次只动一件事）。
