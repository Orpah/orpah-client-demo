# proto/ — 协议内核（C 实现）+ 与 Python 的交叉测试

**c2 状态（2026-09-20）：SN 内核 + JCS + b64url + 报文信封 + 下行解码 + **SHA-256** 全部完成，
交叉测试 7 组。
剩余：HGIC 帧层（8 字节头 / `FRM2`）⇒ 与 c3 的数据口驱动一起做；
HMAC-SHA256（降级 HS256）与 ECDSA（需先拍 P-256 来源 / k 方案）⇒ c4。**

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
| `sn.h` / `sn.c` | SN 内核：Crockford / `sn_ok` / `sn_err` / `sn_parse` / Luhn32 / Mod97 / `sn_verify_check`。**无 malloc、无 stdio ⇒ 可直接编进固件** |
| `jcs.h` / `jcs.c` | **RFC 8785 JCS 规范化**（我们用到的那部分）+ 迷你 JSON 构建器；`jcs_preimage()` = `orpah_id.preimage_of`。**不用 stdio/malloc/浮点 ⇒ 可编进固件** |
| `msg.h` / `msg.c` | 链路报文构造器：`msg_req_connect` / `msg_report` / `msg_id_report`（对齐 `orpah_proto._base`+`build_*` 的**插入序**）。`id-report` 的 `sn` 取**内层 payload.sn** |
| `downlink.h` / `downlink.c` | **下行解码**：`dl_decode()`（同 `decode_msg`：必为对象 + `type` 已知）+ `dl_type/dl_str/dl_int/dl_truthy`（Python 真值语义） |
| `sha256.h` / `sha256.c` | **SHA-256**（FIPS 180-4）：`sha256_init/update/final` + 一次性 `sha256()`。§5.1 的 `SHA-256(preimage)` 用；也是将来 RFC 6979 确定性 k 的前置 |
| `b64url.h` / `b64url.c` | base64url（无填充），对应 `orpah_id.b64url_encode` |
| `sn_cli.c` / `jcs_cli.c` / `sha_cli.c` | host 侧 CLI（对拍/调试用；**不编进固件**）。`jcs_cli` 同时管 JCS/b64url/报文/下行四组 |
| `run_cross_test.py` | 一键对拍：生成/校验向量 + 编译 C + 三方比对 |
| `test_vectors_sn.txt` | `ORG-UNIQUE<TAB>LUHN32<TAB>MOD97`（63 行） |
| `test_vectors_snparse.txt` | `SN<TAB>ok<TAB>err<TAB>verify`（19 行） |
| `test_vectors_jcs.txt` | `<case-name><TAB><jcs-hex>`（12 行；case 名与 `jcs_cli.c` 的 `build_case()` 一一对应） |
| `test_vectors_b64url.txt` | `<raw-hex><TAB><b64url>`（17 行，含 0/1/2 字节三种余数与字母表里的 `-` `_`） |
| `test_vectors_msg.txt` | `<kind><TAB>a1..a5<TAB><envelope-hex>`（8 行：req-connect / report / id-report） |
| `test_vectors_downlink.txt` | `<json-hex><TAB>valid/type/sn/ts/tracked/status/code`（11 行：3 种下行 + 5 种畸形 + 1 个真值语义） |
| `test_vectors_sha256.txt` | `<input-hex><TAB><digest-hex>`（16 行：空 / 块边界 55~129 / 1000B / **真实签名预像**） |

> 四份向量文件全部**由 Python 参考实现生成（勿手改）**，列在脚本里统一用 `--refresh` 重生。

## 跑

```bash
python proto/run_cross_test.py                 # 判定：exit 0 且输出无 FAIL
python proto/run_cross_test.py --refresh       # 用 Python 重写向量（核对 diff 再提交）
python proto/run_cross_test.py --orpah-dir ../orpah-over-halow
```

它对三件事下结论：

1. **C 侧自检**（三个可执行目标）：
   · **SN 内核**：`selfcheck`（11 项）+ `selftest`（63 行校验位）+ `sn-selftest`（19 行 SN 解析）
   · **JCS/b64url/msg/dl**：`selfcheck`（4 项）+ `jcs-selftest`（12 行）+ `b64url-selftest`（17 行）
     + `msg-selftest`（8 行报文信封）+ `dl-selftest`（11 行下行解码）
   · **SHA-256**：`selfcheck`（**分块自洽 3343 项**：同一条输入“一次算”与“分多段喂”必须逐位相同）
     + `sha256-selftest`（16 行 hashlib 向量）
2. **快照没过期**：七份向量文件与 Python 参考**逐行一致**；
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

## JCS 实现的范围与边界（写代码前先看这段）

对齐的是 **Python 的 `json.dumps(ensure_ascii=False, sort_keys=True, separators=(",",":"))`**，
不是“纯理论上的 RFC 8785”：

| 项 | 本实现 | 说明 |
|---|---|---|
| 键排序 | 逐字节字典序 | = Python 的码点序（UTF-8 字节序 == 码点序）✓。⚠ 真 JCS 按 **UTF-16 码元**排，遇**非 BMP**字符（>U+FFFF）会与 Python 不同 —— 我们报文的键全是 ASCII，用不到，但这道差异要知道 |
| 数字 | **只有整数** | 不提供浮点：从根上避开“Python 的最短往返表示”那种跨语言不一致（我们的报文也确实只用整数） |
| 字符串 | 非 ASCII 原样 UTF-8；只转义 `"` `\` 与控制字符（`\u00XX` 小写十六进制） | 对应 `ensure_ascii=False` |
| 内存 | 调用方给的 arena（固定数组），**无 malloc** | 裸机 `-nostdlib` 可用；arena 单次使用，不做回收 |
| 错误 | 返回负的错误码（节点/池/容量/重复键/缓冲不够），**绝不静默截断** | 重复键在 Python 里不可能出现 ⇒ C 侧报了才安全 |

## ★ 两个编码器别用错：**签名预像用 JCS，链路信封用插入序**

| 用途 | Python | C |
|---|---|---|
| **签名预像** `preimage_of` | `json.dumps(..., sort_keys=True, separators=(",",":"))` | `jcs_encode()` / `jcs_preimage()` —— **排序** |
| **链路报文** `encode_msg` | `json.dumps(..., separators=(",",":"))` —— **不带** `sort_keys` | `jcs_encode_raw()` —— **保持插入序** |

所以：报文 JSON 里的键序要求**逐字节复现 Python 源码里的插入顺序**（`msg_report` 是
`v,type,ts,sn,seq,cap,rssi` ✓）；而签名预像才走 JCS 排序。两者混了就是“能发出去但服务端验不过”。

## JSON 解析器的**已知边界**（解析器在 `jcs.c`，下行用）

解析器**有意比 Python 的 `json` 严**（我们的报文不会长成这些形状 ⇒ 真收到了宁可报错，不要猜）：

| 输入 | Python `json.loads` | 本 C |
|---|---|---|
| `1.5` / `1e3`（浮点/指数） | 接受 | **拒**（我们报文只用整数） |
| 超出 int64 的整数 | 接受（任意精度） | **拒** |
| 嵌套 > 8 层 | 接受 | **拒** |
| 重复键 | 后者覆盖前者 | **拒**（Python dict 不会重复，报了才安全） |
| 孤立代理项 `"\ud800"` | 接受（得到一个孤立代理） | **拒**（无法编成合法 UTF-8） |
| `NaN` / `Infinity` | 默认接受 | **拒** |

⇒ 所以 `test_vectors_downlink.txt` **只收录“Python 也说无效”的无效用例**（坏 JSON / 顶层非对象 /
未知 type / 截断），上面这几条**不入向量**（否则会变成用一个已知差异去“证明”零偏差）。

## 五条踩过的坑（都写进代码注释了，别再踩）

1. **Luhn32 ≠ Damm32，期望值必须来自 Python 输出，不能凭记忆**：
   本仓 AGENTS 里那句"黄金样本 `WH01-9AF3C1D2 → B`"说的是 **Damm32**；
   **Luhn32 真值是 `E`、Mod97 是 `21`**（而且恰好有另一个输入 `T` 的 Luhn32 是 `B`，更藏得住混淆）。
   我第一次在 `sn_cli.c` 里就写成了 `B` ⇒ 自检当场失败（这正是"别凭记忆写常量"的活教材）。
2. **cmd 里调 `vcvars64.bat` 必须写 `call`**：不带 `call` 时 cmd 把控制权交给批处理就不回来了，
   后面的 `&& cl ...` **永远不执行**，现象是"退出码非 0 且**没有任何输出**"，极难查。
   （上游 `orpah-over-halow/c/run_cross_test.py` 少写了 `call`，那条 MSVC 兜底路径实际跑不通。）
3. **`subprocess` 必须 `encoding="utf-8", errors="replace"`**：Windows 上默认按 GBK 解码子进程输出 ⇒
   读线程抛 `UnicodeDecodeError` 且 **`stdout` 变成空** ⇒ 报错看起来"什么都没打印"。4. **大块代码替换后必须回读确认**：一次多替换里报了“成功”，实际上那块 `arr_mixed` 仍是旧版
   （现象是 C 输出 `{}`、只差 1/12），查了几轮才定位。⇒ 改完**大块/关键逻辑**后，
   用 `read_file`/`grep` 回读一眼，别只看工具回执 ✓。
5. **用字符串当“缺省”哨兵时，小心它和真数据撞车**：报文用例里我用 `-` 表示“字段不给”，
   但 **RSSI 是负数**（`-55` 开头就是 `-`）⇒ 一律被当成“不给”，`msg-selftest` 当场挂 4/8。
   正确做法：只有**整字段**正好等于 `-`（`strcmp(s, "-") == 0`）才算缺省 ✓。
## 下一步（c2 未完）

报文编解码 + **JCS（RFC 8785）规范化** + `b64url` + **签名预像**。
JCS 是要害（键序按 UTF-16 码元、数字最短表示，差一个字节整条链就验不过），
同样按"**黄金向量 + 对拍**"做，**不与本目录的 SN 内核混在一起**（一次只动一件事）。
