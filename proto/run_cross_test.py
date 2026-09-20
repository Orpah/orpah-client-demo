#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_cross_test.py — c2：SN 内核「Python 参考 vs C 实现」零偏差对拍（纯 PC，不需要硬件）

做三件事：

  ① **C 侧自检**：用 host 编译器编译 `proto/sn.c` + `proto/sn_cli.c`，然后跑
       · `selfcheck`                          —— 内置黄金样本/边界（不依赖文件）
       · `selftest <test_vectors_sn.txt>`     —— ORG-UNIQUE → Luhn32 / Mod97
       · `sn-selftest <test_vectors_snparse.txt>` —— SN → ok / err / verify
  ② **快照与 Python 参考一致**（能 import 上游 `orpah_id` 时）：逐行用 Python 重算，
     与入库的两份向量文件比对；不一致 = FAIL（快照过期 或 两边已分叉）。
  ③ `--refresh`：用 Python 参考**重写**两份向量文件（生成后请核对 diff 再提交）。

判定口径（与上游 `run_checks.py` 一致）：**退出码 0 且输出无 FAIL**。
编译器探测顺序（沿用上游 `orpah-over-halow/c/run_cross_test.py`）：$CC → gcc/clang/cc → MSVC cl。

运行：
  python proto/run_cross_test.py
  python proto/run_cross_test.py --orpah-dir ../orpah-over-halow --refresh

HGIC 帧层（c3）另有 **第四组目标 `hgic_cli`**（`hgic.c` + `hgic_cli.c`）+ 三份向量
（`test_vectors_hgic*.txt`）：它的单一源在**本仓** `tools/txah_hgic.py`（模组 SDK 源码 + 真机实测），
**不依赖上游仓库** ⇒ 那三份快照与 C 自检在没有上游时照样会跑。
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_ORPAH = os.path.abspath(os.path.join(REPO, "..", "orpah-over-halow"))


def load_hgic_ref():
    """HGIC 帧层的单一源 = **本仓** `tools/txah_hgic.py`（模组 SDK 源码 + 真机实测）。

    它**不依赖上游仓库**（与 SN/JCS/报文那几组不同）⇒ HGIC 三组快照与 C 自检
    在没有 `orpah-over-halow` 时应照跑不误。
    """
    tools = os.path.join(REPO, "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import txah_hgic
    return txah_hgic

VEC_SN = os.path.join(HERE, "test_vectors_sn.txt")
VEC_PARSE = os.path.join(HERE, "test_vectors_snparse.txt")
VEC_JCS = os.path.join(HERE, "test_vectors_jcs.txt")
VEC_B64 = os.path.join(HERE, "test_vectors_b64url.txt")
VEC_MSG = os.path.join(HERE, "test_vectors_msg.txt")
VEC_DL = os.path.join(HERE, "test_vectors_downlink.txt")
VEC_SHA = os.path.join(HERE, "test_vectors_sha256.txt")
VEC_HMAC = os.path.join(HERE, "test_vectors_hmac.txt")
VEC_IDR = os.path.join(HERE, "test_vectors_id_report.txt")
VEC_HGIC = os.path.join(HERE, "test_vectors_hgic.txt")
VEC_HGIC_PARSE = os.path.join(HERE, "test_vectors_hgic_parse.txt")
VEC_HGIC_CTRL = os.path.join(HERE, "test_vectors_hgic_ctrl.txt")

HEADER_SN = (
    "# proto/test_vectors_sn.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：Python 参考实现 orpah-over-halow/orpah_id.py\n"
    "# 列：ORG-UNIQUE<TAB>LUHN32<TAB>MOD97\n"
    "# 说明：校验位只算 ORG-UNIQUE（不含 CC）—— 规范 §2 硬规则\n"
)
HEADER_PARSE = (
    "# proto/test_vectors_snparse.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：Python 参考实现 orpah-over-halow/orpah_id.py 的 sn_ok/sn_err/verify_check\n"
    "# 列：SN<TAB>ok<TAB>err<TAB>verify   （err 的 None 记作 \"-\"）\n"
)
HEADER_JCS = (
    "# proto/test_vectors_jcs.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：orpah_id.jcs()（= json.dumps(ensure_ascii=False, sort_keys=True,\n"
    "#                                  separators=(',',':')) 的 UTF-8 字节）\n"
    "# 列：<case-name><TAB><jcs-hex>\n"
    "# ⚠ case-name 必须与 proto/jcs_cli.c 里 build_case() 的名字一一对应\n"
)
HEADER_B64 = (
    "# proto/test_vectors_b64url.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：orpah_id.b64url_encode()（= urlsafe_b64encode().rstrip('=')）\n"
    "# 列：<raw-hex><TAB><b64url>\n"
)
HEADER_MSG = (
    "# proto/test_vectors_msg.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：orpah_proto.build_*() + encode_msg()（**插入序**，不带 sort_keys，\n"
    "#                  separators=(',',':')）—— 信封不是 JCS！\n"
    "# 列：<kind><TAB><a1><TAB><a2><TAB><a3><TAB><a4><TAB><a5><TAB><envelope-hex>\n"
    "#   req-connect : a1=sn a2=ts a3=mac|- a4=hw|- a5=-\n"
    "#   report      : a1=sn a2=ts a3=seq a4=cap(-|0|1) a5=rssi|-\n"
    "#   id-report   : a1=sn a2=ts a3..a5=-（内层用固定 stub，两侧一致）\n"
)
HEADER_DL = (
    "# proto/test_vectors_downlink.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：orpah_proto.decode_msg() + 设备侧取值习惯（client.py / DeviceSim.on_down）\n"
    "# 列：<json-hex><TAB>valid<TAB>type<TAB>sn<TAB>ts<TAB>tracked<TAB>status<TAB>code\n"
    "#   ⚠ 只收录“Python 也说无效”的无效用例（浮点/超 int64/嵌套过深 是 C 有意加严，\n"
    "#     不入向量，边界见 proto/README.md）\n"
)
HEADER_SHA = (
    "# proto/test_vectors_sha256.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：Python hashlib.sha256()\n"
    "# 列：<input-hex><TAB><digest-hex>   （空输入 = 行首就是 TAB）\n"
    "# 覆盖：空 / 一个块边界 55/56/57/63/64/65/127/128 / 1000 字节 / 全零 128B,\n"
    "#       以及**真实签名预像**（jcs({\"hdr\",\"payload\"})，即 c4 要签的那串字节）\n"
)
HEADER_HMAC = (
    "# proto/test_vectors_hmac.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：Python hmac.new(key, msg, hashlib.sha256).digest()\n"
    "# 列：<key-hex><TAB><msg-hex><TAB><mac-hex>\n"
    "# 覆盖：空键/空消息 / 键 32~(64)→ 超分组 65/128（**必须先哈希**）/ 消息跨块,\n"
    "#       以及★**真实降级路径**：32B 演示 HMAC 密钥 × 真实签名预像（§5.1 的 HS256）\n"
)
HEADER_IDR = (
    "# proto/test_vectors_id_report.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：orpah_id.Device.report() + orpah_proto.encode_msg()/build_id_report()\n"
    "# 列：<sn><TAB><ts><TAB><nonce><TAB><level><TAB><key-hex><TAB><battery|->\n"
    "#     <TAB><caprtc|-><TAB><firmware|-><TAB><report-hex><TAB><envelope-hex>\n"
    "# 覆盖：level 1/2（HS256，降级链）/ level 3（none，不签名）/ 无 cap 与带 cap+battery+firmware\n"
    "#   ⚠ level=0（ES256）**不在此**：C 侧明确报未实现（IDR_E_ES256），等 P-256 拍板\n"
)
HEADER_HGIC = (
    "# proto/test_vectors_hgic.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：**本仓** tools/txah_hgic.py（= 模组 SDK hgic.h/uart_bus.c + 真机实测）\n"
    "# 列：<kind><TAB><a1><TAB><a2><TAB><a3><TAB><a4><TAB><a5><TAB><a6><TAB><frame-hex>\n"
    "#   hdr : a1=magic(hex) a2=type a3=ifidx a4=flags a5=cookie a6=载荷-hex|-\n"
    "#   frm2: a1=以太帧-hex|- a2=cookie a3..a6=-          （lean=True ⇒ FRM2）\n"
    "#   cmd : a1=cmd_id a2=参数-hex|- a3=cookie a4..a6=-     （id>255 走 CMD2）\n"
    "# 覆盖：长度上下界 8/4096、ifidx:4|flags:4 打包、cookie 0/32767、256 附近的 CMD↔CMD2 分界\n"
)
HEADER_HGIC_PARSE = (
    "# proto/test_vectors_hgic_parse.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：**本仓** tools/txah_hgic.py 的 StreamParser.feed()\n"
    "# 列：<expect><TAB><流-hex><TAB><chunk><TAB><frames><TAB><garbage><TAB><bad_length><TAB><overrun>\n"
    "#   expect ∈ any|rx|tx（rx = 只收模组→主机，= Python 的 expect_from_module=True）\n"
    "#   chunk  = 每次喂多少字节（0 = 一次喂完；1 = 逐字节）\n"
    "#   frames = ';' 分隔的 <from>:<type>:<len>:<cookie>:<ifidx>:<flags>:<载荷hex|->（无帧 = -）\n"
    "#   覆盖：逐字节喂、前置杂音、假 magic（长度非法）、截断、两种 magic 混流、方向过滤\n"
    "#   ⚠ overrun 是 **C 独有**（Python 缓冲无上限），合法数据恒为 0 —— 收录它就是钉住“永不为 0 以外”\n"
)
HEADER_HGIC_CTRL = (
    "# proto/test_vectors_hgic_ctrl.txt —— 由 proto/run_cross_test.py --refresh 生成，**勿手改**\n"
    "# 来源（单一源）：**本仓** tools/txah_hgic.py 的 ctrl_info()\n"
    "# 列：<type><TAB><from 0|1><TAB><载荷-hex><TAB><rc><TAB><kind><TAB><id><TAB><status><TAB><data-hex>\n"
    "#   kind ∈ req|resp|'-'（rc≠0 时全为 -）；status = 十进制或 '-'（短应答无 status）\n"
    "#   覆盖：带 status/len 的应答、短应答（只回 id）、请求、事件、CMD2/EVENT2、空载荷\n"
)


# ---------------------------------------------------------------------------
# 向量（确定性；与上游 c/run_cross_test.py 同风格：黄金样本 + 边界 + 定种子随机）
# ---------------------------------------------------------------------------
def sn_cores():
    """ORG-UNIQUE 样本集（含黄金样本、边界、小写、定种子随机）。"""
    cores = [
        "WH01-9AF3C1D2",        # 黄金样本
        "0", "Z", "2", "7", "T", "V",
        "000000000000", "ZZZZZZZZ",
        "wh01-9af3c1d2",        # 小写 → 与黄金样本同值（Crockford 收大小写）
        "WH01", "CA-0001", "AA-000-0000",
    ]
    rnd = random.Random(20260920)
    alpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
    for _ in range(50):
        cores.append("".join(rnd.choice(alpha) for _ in range(rnd.randint(1, 20))))

    seen, out = set(), []
    for c in cores:
        if c in seen:            # ★ 按**原串**去重（不按 upper）—— 否则小写用例被吃掉，
            continue             #   而「小写字母在 Mod97 里被静默跳过」正是要锁住的语义
        seen.add(c)
        out.append(c)
    return out


def sn_cases(o):
    """SN 样本集：合法/非法/带两种校验位/篡改校验位/大小写/超长 …（期望值全由 Python 现算）。"""
    golden = "WH01-9AF3C1D2"
    luhn = o.compute_check(golden, "luhn32")
    mod = o.compute_check(golden, "mod97")
    cases = [
        f"CN-{golden}",                 # 无校验位
        f"CN-{golden}-{luhn}",          # 1 位 → Luhn32
        f"CN-{golden}-{mod}",           # 2 位 → Mod97
        f"CN-{golden}-Z",               # 篡改校验位
        f"cn-{golden.lower()}",         # 小写 → bad-format（正则只收大写）
        f"CN-{golden}-",                # 空校验段
        f"CN-{golden}-{luhn}{luhn}",    # 2 位但不是 Mod97（走 Mod97 校验）
        f"CN-{golden}-{luhn}XY",        # 3 位校验段 → 正则不接受（bad-format）
        "CN-WH1-9AF3C1D2",              # ORG 太短
        "CN-WH01-9AF3C1D",              # UNIQUE 太短
        "CN-WH01-9AF3C1D2X",            # UNIQUE 太长（17）
        "CN-WH01-9AF3C1I2",             # 含 I（Crockford 排除）
        "CN-WH01-9AF3C1O2",             # 含 O
        "C1-WH01-9AF3C1D2",             # CC 含数字
        "WH01-9AF3C1D2",                # 缺 CC
        f"CN-{golden}-I",               # 1 位校验段含 I（Crockford 排除）→ Luhn32 判不通过
        "CN-WH01-9AF3C1D2-" + "Z" * 33, # 超长
        "CN-ABCDEFG-9AF3C1D2",          # ORG 7 位（超 6）
        "",                             # 空串 → "empty"
    ]
    return cases


def build_rows(o):
    rows_sn = []
    for c in sn_cores():
        rows_sn.append((c, o.compute_check(c, "luhn32"), o.compute_check(c, "mod97")))

    rows_parse = []
    for sn in sn_cases(o):
        err = o.sn_err(sn)
        rows_parse.append((sn, 1 if o.sn_ok(sn) else 0,
                           "-" if err is None else err,
                           1 if o.verify_check(sn) else 0))
    return rows_sn, rows_parse


# ★ 名字必须与 proto/jcs_cli.c 里 build_case() 的完全一致（不一致会 FAIL 并指出）
JCS_CASES = {
    "minimal": {},
    "empty_arr": [],
    "flat_int": {"a": 1, "b": -2, "c": 0},
    "big_int": {"ts": 1789879939, "neg": -9223372036854775808},
    "strings_basic": {"b": "hello", "a": "world"},
    "strings_escape": {"q": 'a"b', "bs": "c\\d", "nl": "e\nf",
                       "tab": "g\th", "ctl": "\u0001x"},
    "unicode_utf8": {"s": "\u4e2d\u6587", "e": "\u00e9"},
    "nested": {"o": {"z": 1, "a": {"y": 2, "x": 3}}},
    "arr_mixed": {"a": [1, "x", True, None, {"k": 2}]},
    "bool_null": {"t": True, "f": False, "n": None},
    "key_order_case": {"Z": 1, "a": 2, "A": 3, "1": 4, "_": 5},
    "report_shape": {
        "hdr": {"typ": "orpah-id-report", "ver": 1, "alg": "ES256", "level": 0},
        "payload": {
            "sn": "CN-WH01-9AF3C1D2", "ts": 0,
            "nonce": "3F9A8B2C1D4E5F6A7B8C9D0E1F2A3B4C",
            "seen_routers": [
                {"bssid": "AA:BB:CC:DD:EE:FF", "ssid": "ORPAHID_ZONE_A", "rssi": -42},
                {"bssid": "11:22:33:44:55:66", "ssid": "ORPAHID_ZONE_B", "rssi": -71},
            ],
            "cap": {"rtc": False}, "battery_mv": 3900, "firmware": "c1-bench",
        },
    },
}

# b64url 用例：覆盖三种余数（0/1/2 字节）+ 字母表里的 - 与 _
B64_CASES = [
    "", "66", "666f", "666f6f", "666f6f62", "666f6f6261",
    "fb", "ff", "ffff", "ffffff", "ffffffff",
    "00", "0000", "000000",
    "3f9a8b2c1d4e5f6a7b8c9d0e1f2a3b4c",                    # 16B
    "00" * 32,                                              # 32B
    "ab" * 64,                                              # 64B（ES256 raw r||s 的长度）
]


def gen_jcs_rows(o):
    return [(name, o.jcs(obj).hex()) for name, obj in JCS_CASES.items()]


def gen_b64_rows(o):
    return [(h, o.b64url_encode(bytes.fromhex(h))) for h in B64_CASES]


# ★ 报文用例：与 proto/jcs_cli.c 的 msg_case_hex() 共用同一套 7 列规格
MSG_CASES = [
    ("req-connect", "CN-WH01-9AF3C1D2", "0", "-", "-", "-"),
    ("req-connect", "CN-WH01-9AF3C1D2", "1789879939", "4A:06:59:00:00:01", "CH32V203+TX-AH", "-"),
    ("report", "CN-WH01-9AF3C1D2", "0", "1", "-", "-"),
    ("report", "CN-WH01-9AF3C1D2", "0", "1", "0", "-55"),
    ("report", "CN-WH01-9AF3C1D2", "1789879939", "7", "1", "-42"),
    ("report", "CN-WH01-9AF3C1D2", "0", "2", "-", "-20"),
    ("id-report", "CN-WH01-9AF3C1D2", "0", "-", "-", "-"),
    ("id-report", "CN-WH01-9AF3C1D2", "1789879939", "-", "-", "-"),
]

ID_STUB_NONCE = "AA" * 16
ID_STUB_SIG = "STUB"


def _id_stub(sn):
    """id-report 外壳用例的内层报文 stub（**必须与 jcs_cli.c 的 id_stub() 一致**）。"""
    return {"hdr": {"typ": "orpah-id-report", "ver": 1, "alg": "ES256", "level": 0},
            "payload": {"sn": sn, "ts": 0, "nonce": ID_STUB_NONCE, "seen_routers": []},
            "sig": ID_STUB_SIG}


def build_msg_case(proto, kind, a1, a2, a3, a4, a5):
    if kind == "req-connect":
        msg = proto.build_req_connect(a1, mac=None if a3 == "-" else a3,
                                      hw=None if a4 == "-" else a4, ts=int(a2))
    elif kind == "report":
        cap = None if a4 == "-" else {"rtc": a4 == "1"}
        msg = proto.build_report(a1, ts=int(a2), seq=int(a3), cap=cap,
                                 rssi=None if a5 == "-" else int(a5))
    elif kind == "id-report":
        msg = proto.build_id_report(_id_stub(a1), ts=int(a2))
    else:
        raise ValueError("unknown kind: %s" % kind)
    return proto.encode_msg(msg).hex()


def gen_msg_rows(o, proto):
    rows = []
    for kind, a1, a2, a3, a4, a5 in MSG_CASES:
        rows.append((kind, a1, a2, a3, a4, a5, build_msg_case(proto, kind, a1, a2, a3, a4, a5)))
    return rows


# ★ 下行解码用例：与 proto/jcs_cli.c 的 dl_report_line() 共用同一套 8 列规格
DL_CASES = [
    ("access-info-tracked", lambda p: p.encode_msg(
        p.build_access_info("CN-WH01-9AF3C1D2", True, status=p.ST_TRACKED, ts=1789879939))),
    ("access-info-untracked", lambda p: p.encode_msg(
        p.build_access_info("CN-WH01-9AF3C1D2", False, ts=0))),
    ("tracking-status", lambda p: p.encode_msg(
        p.build_tracking_status("CN-WH01-9AF3C1D2", p.ST_NOT_TRACKED, msg_text="ok", ts=5))),
    ("error", lambda p: p.encode_msg(
        p.build_error("RATELIMIT", sn="CN-WH01-9AF3C1D2", msg_text="slow", ts=7))),
    ("found", lambda p: p.encode_msg({"v": 1, "type": p.MSG_FOUND, "ts": 0})),
    ("bad-json", lambda p: b"not json"),
    ("top-array", lambda p: b"[]"),
    ("empty-object", lambda p: b"{}"),
    ("unknown-type", lambda p: b'{"v":1,"type":"NOPE","ts":0}'),
    ("truncated", lambda p: b'{"v":1,'),
    ("tracked-as-string", lambda p: b'{"v":1,"type":"ORPAH-ACCESS-INFO","ts":1,'
                                    b'"sn":"X","tracked":"no"}'),
]


def dl_expect(proto, raw):
    """与 C 侧 dl_report_line() 同语义：只认 str / int（bool 不算 int）。"""
    msg = proto.decode_msg(raw)
    if msg is None:
        return ["0", "-", "-", "-", "-", "-", "-"]

    def s(v):
        return v if isinstance(v, str) else "-"

    def i(v):
        return str(v) if (isinstance(v, int) and not isinstance(v, bool)) else "-"

    tracked = "-" if "tracked" not in msg else ("1" if msg["tracked"] else "0")
    return ["1", s(msg.get("type")), s(msg.get("sn")), i(msg.get("ts")),
            tracked, s(msg.get("status")), s(msg.get("code"))]


def gen_dl_rows(o, proto):
    rows = []
    for label, mk in DL_CASES:
        raw = mk(proto)
        rows.append((raw.hex(),) + tuple(dl_expect(proto, raw)))
    return rows


# 真实签名预像（= c4 要签的那串字节）。SHA-256 与 HMAC 两组共用同一份定义。
def real_preimage(o):
    return o.jcs({
        "hdr": {"typ": "orpah-id-report", "ver": 1, "alg": "ES256", "level": 0},
        "payload": {
            "sn": "CN-WH01-9AF3C1D2", "ts": 0,
            "nonce": "3F9A8B2C1D4E5F6A7B8C9D0E1F2A3B4C",
            "seen_routers": [{"bssid": "AA:BB:CC:DD:EE:FF", "ssid": "ORPAHID_ZONE_A",
                              "rssi": -42}],
            "cap": {"rtc": False}, "battery_mv": 3900, "firmware": "c4",
        },
    })


# SHA-256 输入：块边界是重点（55/56/57 决定填充走一块还是两块）
def sha_inputs(o):
    preimage = real_preimage(o)
    fill = lambda n: bytes([(i * 7 + 3) % 251 for i in range(n)])
    return [
        ("empty", b""),
        ("abc", b"abc"),
        ("nist-two-block", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        ("range-256", bytes(range(256))),
        ("55", fill(55)), ("56", fill(56)), ("57", fill(57)),
        ("63", fill(63)), ("64", fill(64)), ("65", fill(65)),
        ("127", fill(127)), ("128", fill(128)), ("129", fill(129)),
        ("1000", fill(1000)), ("zeros-128", b"\x00" * 128),
        ("real-preimage", preimage),
    ]


def gen_sha_rows(o):
    import hashlib
    return [(data.hex(), hashlib.sha256(data).hexdigest()) for _label, data in sha_inputs(o)]


def _idr_cases(o):
    """(sn, ts, nonce, level, battery, caprtc, firmware) —— key 由 derive_demo_hmac 填。"""
    sn = "CN-WH01-9AF3C1D2"
    n1 = "3F9A8B2C1D4E5F6A7B8C9D0E1F2A3B4C"
    n2 = "00112233445566778899AABBCCDDEEFF"
    return [
        (sn, "0", n1, "1", "-", "-", "-"),                       # 最小降级报
        (sn, "1789879939", n2, "2", "3900", "0", "c4a"),          # 带 cap/battery/firmware
        (sn, "1789879939", n1, "1", "-", "1", "-"),                # cap.rtc=true
        (sn, "0", n2, "3", "-", "-", "-"),                       # L3：不签名（alg=none）
        (sn, "1789879939", n1, "1", "3000", "0", "-"),           # 低电量（告警输入端）
    ]


def idr_row(o, proto, case):
    sn, ts, nonce, level, batt, caprtc, fw = case
    key_hex = o.derive_demo_hmac(sn, 1).hex()
    dev = o.Device(sn=sn)
    dev.hmac_key = bytes.fromhex(key_hex)
    rep = dev.report(level=int(level), ts=int(ts), nonce=nonce, seen_routers=[],
                     battery_mv=None if batt == "-" else int(batt),
                     firmware=None if fw == "-" else fw,
                     cap=None if caprtc == "-" else {"rtc": caprtc == "1"})
    return (sn, ts, nonce, level, key_hex, batt, caprtc, fw,
            proto.encode_msg(rep).hex(),
            proto.encode_msg(proto.build_id_report(rep, ts=int(ts))).hex())


def gen_idr_rows(o, proto):
    return [idr_row(o, proto, c) for c in _idr_cases(o)]


def gen_hmac_rows(o):
    """★ 最后一条是**真实降级路径**：32B 演示 HMAC 密钥 × 真实签名预像（§5.1 的 HS256）。"""
    import hashlib
    import hmac as py_hmac

    fill = lambda n, s=1: bytes([(i * s + 7) % 251 for i in range(n)])
    rk = o.derive_demo_hmac("CN-WH01-9AF3C1D2", 1)
    real_key = bytes.fromhex(rk) if isinstance(rk, str) else bytes(rk)

    cases = [
        (b"", b""),
        (b"key", b"The quick brown fox jumps over the lazy dog"),
        (fill(32), b""),
        (fill(32), fill(1)),
        (fill(32), fill(55)), (fill(32), fill(56)), (fill(32), fill(57)),
        (fill(32), fill(64)), (fill(32), fill(65)),
        (fill(64), fill(100)),          # 键正好一个分组
        (fill(65), fill(100)),          # 键超分组 ⇒ 实现必须先哈希
        (fill(128), fill(1000)),
        (real_key, real_preimage(o)),
    ]
    return [(k.hex(), m.hex(), py_hmac.new(k, m, hashlib.sha256).digest().hex())
            for k, m in cases]


# ---------------------------------------------------------------------------
# HGIC 帧层向量（c3）—— 单一源 = 本仓 tools/txah_hgic.py
# ---------------------------------------------------------------------------
# kind, a1..a6（列的含义见 HEADER_HGIC）
HGIC_FRAME_CASES = [
    ("hdr", "1A2B", "9", "0", "0", "0", "-"),                   # 空载荷：整帧 = 8
    ("hdr", "1A2B", "9", "5", "10", "4660", "-"),               # ifidx:4|flags:4 + cookie
    ("hdr", "2B1A", "4", "15", "15", "32767", "aa55"),          # 另一半字节都置 1
    ("hdr", "0000", "0", "1", "2", "1", "00ff"),              # magic/type 不校验（原样发）
    ("hdr", "1A2B", "9", "0", "0", "0", "aa" * 4088),          # 整帧恰好 4096（上界）
    ("frm2", "aabbccddeeff11223344556688b50102", "1", "-", "-", "-", "-"),
    ("frm2", "-", "0", "-", "-", "-", "-"),                     # 空载荷（边界）
    ("frm2", "88b5", "32767", "-", "-", "-", "-"),
    ("cmd", "108", "-", "0", "-", "-", "-"),                    # GET_UART_FIXLEN
    ("cmd", "43", "-", "7", "-", "-", "-"),                     # GET_FW_INFO
    ("cmd", "108", "0102", "9", "-", "-", "-"),               # 带参：参数在偏移 12
    ("cmd", "255", "-", "11", "-", "-", "-"),                   # CMD 上界
    ("cmd", "256", "-", "12", "-", "-", "-"),                   # CMD2 下界
    ("cmd", "65535", "aabbccdd", "13", "-", "-", "-"),         # CMD2 上界 + 4B 参数
]


def hgic_frame_build(hg, kind, a1, a2, a3, a4, a5, a6):
    if kind == "hdr":
        pl = b"" if a6 == "-" else bytes.fromhex(a6)
        return hg.build(int(a1, 16), int(a2), pl, cookie=int(a5),
                        ifidx=int(a3), flags=int(a4))
    if kind == "frm2":
        pl = b"" if a1 == "-" else bytes.fromhex(a1)
        return hg.data_frame(pl, cookie=int(a2), lean=True)
    if kind == "cmd":
        pl = b"" if a2 == "-" else bytes.fromhex(a2)
        return hg.cmd_frame(int(a1), pl, cookie=int(a3))
    raise ValueError("unknown hgic kind: %s" % kind)


def gen_hgic_frame_rows(hg):
    return [tuple(list(c) + [hgic_frame_build(hg, *c).hex()]) for c in HGIC_FRAME_CASES]


def hgic_streams(hg):
    """(expect, 字节流, chunk) —— 流由帧拼出来，不手写十六进制（免得两边看错同一串）。"""
    f_tx = hg.data_frame(bytes(range(1, 21)), cookie=1, lean=True)      # 主机→模组
    f_tx2 = hg.cmd_frame(109, b"", cookie=2)
    f_rx = hg.build(hg.MAGIC_MODULE_TO_HOST, hg.TYPE_NAMES_REV["FRM2"],
                    b"\x88\xb5\x01\x02", cookie=5, ifidx=3, flags=4)   # 模组→主机
    fake = bytes([0x2B, 0x1A, 9, 0, 7, 0, 0, 0])                        # 长度 7 ⇒ 假 magic
    return [
        ("rx", f_rx, 0),
        ("rx", f_rx, 1),                      # 逐字节喂
        ("rx", f_rx, 3),
        ("any", f_rx + f_tx, 0),              # 两种 magic 混流
        ("rx", f_rx + f_tx, 0),               # 方向过滤：主机→那帧变杂音
        ("tx", f_rx + f_tx, 0),
        ("any", b"\x01\x02\x03" + f_rx, 0),   # 前置杂音
        ("any", fake + f_rx, 0),              # 假 magic：计 bad_length 后继续
        ("any", f_rx[:4], 0),                 # 截断（只有半个头）
        ("any", f_tx2 + f_rx + b"\x00\xff" + f_rx, 2),
        ("any", b"\x2b", 1),                  # 只有 magic 的前半
        ("any", b"", 0),
    ]


def hgic_parse_expect(hg, expect):
    return {"any": None, "rx": True, "tx": False}[expect]


def hgic_parse_row(hg, expect, stream, chunk):
    sp = hg.StreamParser(expect_from_module=hgic_parse_expect(hg, expect))
    parts = [stream] if chunk <= 0 else [stream[i:i + chunk] for i in range(0, len(stream), chunk)]
    frames = []
    for part in parts:
        for hdr, payload in sp.feed(part):
            frames.append("%d:%d:%d:%d:%d:%d:%s" % (
                1 if hdr["from_module"] else 0, hdr["type"], hdr["length"],
                hdr["cookie"], hdr["ifidx"], hdr["flags"], payload.hex() or "-"))
    return (expect, stream.hex(), str(chunk), (";".join(frames) or "-"),
            str(sp.garbage), str(sp.bad_length), "0")


def gen_hgic_parse_rows(hg):
    return [hgic_parse_row(hg, *c) for c in hgic_streams(hg)]


# type, from_module, 载荷-hex
HGIC_CTRL_CASES = [
    (3, 1, "6c000300aabbcc"),      # CMD 108 带 status=0 / len=3 / 3B 数据
    (3, 1, "6c000300aabb"),        # len 与 body 对不上 ⇒ 短应答
    (3, 1, "6c"),                  # 只回 id（真机 send-cmd 1/20 就是这样）
    (3, 1, ""),                    # 空载荷 ⇒ None
    (3, 0, "6c01020304"),          # 请求（union 之后才是数据）
    (3, 0, "6c0102"),
    (4, 1, "0e01020304"),          # 事件（EVENT）
    (13, 1, "2c01000100bb"),       # CMD2 id=300 + status/len + 1B 数据
    (13, 1, "2c010300aa"),         # CMD2 但 len 对不上 ⇒ 短应答
    (13, 1, "6c"),                 # CMD2 但载荷不足以取 u16 id
    (14, 1, "2c01ff"),             # EVENT2
    (9, 1, "aabb"),                # 数据帧（不是控制帧）
    (2, 1, "01"),
]


def gen_hgic_ctrl_rows(hg):
    out = []
    for type_, frm, payload_hex in HGIC_CTRL_CASES:
        payload = b"" if payload_hex == "" else bytes.fromhex(payload_hex)
        hdr = {"type": type_, "from_module": bool(frm), "magic": 0, "length": 0,
               "cookie": 0, "ifidx": 0, "flags": 0}
        info = hg.ctrl_info(hdr, payload)
        if info is None:
            out.append((type_, frm, payload_hex, "-1", "-", "-", "-", "-"))
            continue
        data_hex = info["data"].hex() if info["data"] else "-"
        status = "-" if info["status"] is None else str(info["status"])
        out.append((type_, frm, payload_hex, "0", info["kind"], str(info["id"]),
                    status, data_hex))
    return out


def read_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            rows.append(tuple(line.split("\t")))
    return rows

def write_rows(path, header, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")


def _short(v):
    """超长列（HGIC 有 4 KB 载荷）不刷屏。"""
    s = str(v)
    return s if len(s) <= 200 else s[:200] + "…(%d)" % len(s)


def cmp_snapshot(name, exp, got, fails):
    """逐行比对快照与 Python 参考；不一致就记 FAIL 并打出首处差异。"""
    if [tuple(str(x) for x in r) for r in exp] != [tuple(r) for r in got]:
        fails.append("快照 %s 与 Python 参考不一致（用 --refresh 重生成并核对 diff）" % name)
        print("FAIL 快照 %s 与 Python 参考不一致" % name)
        for i, (a, b) in enumerate(zip(exp, got)):
            if tuple(str(x) for x in a) != tuple(b):
                print("   line %d:\n     PY  =%s\n     FILE=%s" % (i + 1, _short(a), _short(b)))
                break
        else:
            print("   行数不同：PY=%d FILE=%d" % (len(exp), len(got)))
    else:
        print("PASS 快照 %s 与 Python 参考逐行一致（%d 行）" % (name, len(got)))


# ---------------------------------------------------------------------------
# host 编译器（沿用上游套路）
# ---------------------------------------------------------------------------
def _vs_env():
    for base in (os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                 os.environ.get("ProgramFiles", r"C:\Program Files")):
        vsw = os.path.join(base, "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if os.path.isfile(vsw):
            try:
                out = subprocess.run([vsw, "-latest", "-property", "installationPath"],
                                     capture_output=True, text=True, timeout=10).stdout.strip()
                vc = os.path.join(out, "VC", "Auxiliary", "Build", "vcvars64.bat")
                if out and os.path.isfile(vc):
                    return vc
            except Exception:
                pass
    for base in (r"C:\Program Files\Microsoft Visual Studio\2022",
                 r"C:\Program Files (x86)\Microsoft Visual Studio\2022"):
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            vc = os.path.join(base, edition, "VC", "Auxiliary", "Build", "vcvars64.bat")
            if os.path.isfile(vc):
                return vc
    return None


def _run(cmd, **kw):
    """subprocess 包装：**必须显式指定 utf-8**。

    ⚠ Windows 上 subprocess 默认按 GBK/936 解码输出 ⇒ 读线程里抛
      UnicodeDecodeError，而且 stdout 会变成空的 —— 于是“报错看起来什么都没有”。
      这条坑在各仓 AGENTS 里都记过（`encoding="utf-8", errors="replace"`）。
    """
    kw.setdefault("capture_output", True)
    kw.setdefault("encoding", "utf-8")
    kw.setdefault("errors", "replace")
    return subprocess.run(cmd, **kw)


def compile_c(exe, srcs):
    """编译 host 侧对拍程序。返回 (说明, 错误输出或 None)。**不吞报错**。"""
    cc = os.environ.get("CC")
    if cc:
        cmd = cc.split() + ["-std=c99", "-O2", "-o", exe] + srcs
        p = _run(cmd)
        return ("CC=" + cc), (None if p.returncode == 0 else (p.stdout or "") + (p.stderr or ""))
    for cand in ("gcc", "clang", "cc"):
        if shutil.which(cand):
            p = _run([cand, "-std=c99", "-O2", "-Wall", "-Wextra", "-o", exe] + srcs)
            return cand, (None if p.returncode == 0 else (p.stdout or "") + (p.stderr or ""))
    vc = _vs_env()
    if vc and shutil.which("cl"):
        # cl 默认把 .obj 写到 cwd ⇒ 在（临时）目录里编译，别污染仓库。
        # /std:c11：本代码用 C99 的"for 内声明"等写法，MSVC 默认（C89 模式）编不过。
        # /std:c11：本代码用 C99 的"for 内声明"等写法，MSVC 默认（C89 模式）编不过。
        # ⚠ 调批处理**必须写 `call`**：否则 cmd 把控制权交给 .bat 就不回来了，
        #   后面的 `&& cl ...` 根本不执行，现象是"退出码非 0 且没有任何输出"
        #   （上游 c/run_cross_test.py 少写了 call，那条 MSVC 兜底路径实际跑不通）。
        srclist = " ".join('"%s"' % os.path.abspath(s) for s in srcs)
        td = os.path.dirname(exe)
        cmd = 'call "%s" >nul 2>&1 && cl /nologo /std:c11 /utf-8 %s /Fo:"%s\\\\" /Fe:"%s"' % (
            vc, srclist, td, exe)
        p = _run(cmd, shell=True, cwd=td)
        err = None if p.returncode == 0 else (p.stdout or "") + (p.stderr or "")
        return "cl (vcvars64, /std:c11)", err
    return None, "未找到可用 C 编译器；可设 CC 环境变量指定"


def run(exe, args, stdin=None):
    return _run([exe] + args, input=stdin)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--orpah-dir", default=DEFAULT_ORPAH,
                    help="上游参考实现所在目录（默认 ../orpah-over-halow）")
    ap.add_argument("--refresh", action="store_true",
                    help="用 Python 参考重写两份向量文件（生成后请核对 diff 再提交）")
    args = ap.parse_args()

    fails = []
    o = None
    proto = None
    hg = None
    try:
        hg = load_hgic_ref()
    except Exception as e:                            # noqa: BLE001
        print("!! 拿不到 HGIC 单一源 tools/txah_hgic.py：%r" % (e,))
    if os.path.isdir(args.orpah_dir):
        sys.path.insert(0, args.orpah_dir)
        try:
            import orpah_id as o                      # noqa: F401
            import orpah_proto as proto               # noqa: F401
        except Exception as e:                        # noqa: BLE001
            print("!! 找到 %s 但 import orpah_id/orpah_proto 失败：%r" % (args.orpah_dir, e))
            o = None
            proto = None
    else:
        print("!! 未找到上游参考实现目录 %s（--orpah-dir 指定）" % args.orpah_dir)

    # ---- ③ --refresh：先重写向量（HGIC 三份只依赖本仓 tools/；其余需要上游） ----
    if args.refresh:
        if hg is not None:
            rows_hf = gen_hgic_frame_rows(hg)
            rows_hp = gen_hgic_parse_rows(hg)
            rows_hc = gen_hgic_ctrl_rows(hg)
            write_rows(VEC_HGIC, HEADER_HGIC, rows_hf)
            write_rows(VEC_HGIC_PARSE, HEADER_HGIC_PARSE, rows_hp)
            write_rows(VEC_HGIC_CTRL, HEADER_HGIC_CTRL, rows_hc)
            print("--refresh 已重写 HGIC 三份：%s(%d) / %s(%d) / %s(%d)"
                  % (os.path.basename(VEC_HGIC), len(rows_hf),
                     os.path.basename(VEC_HGIC_PARSE), len(rows_hp),
                     os.path.basename(VEC_HGIC_CTRL), len(rows_hc)))
        else:
            print("!! 拿不到 tools/txah_hgic.py（HGIC 单一源）⇒ 跳过 HGIC 三份向量")
        if o is None or proto is None:
            print("!! --refresh 需要上游 Python 参考实现（orpah_id + orpah_proto）；已退出")
            return 2
        rows_sn, rows_parse = build_rows(o)
        rows_jcs = gen_jcs_rows(o)
        rows_b64 = gen_b64_rows(o)
        rows_msg = gen_msg_rows(o, proto)
        rows_dl = gen_dl_rows(o, proto)
        rows_sha = gen_sha_rows(o)
        rows_hmac = gen_hmac_rows(o)
        write_rows(VEC_SN, HEADER_SN, rows_sn)
        write_rows(VEC_PARSE, HEADER_PARSE, rows_parse)
        write_rows(VEC_JCS, HEADER_JCS, rows_jcs)
        write_rows(VEC_B64, HEADER_B64, rows_b64)
        write_rows(VEC_MSG, HEADER_MSG, rows_msg)
        write_rows(VEC_DL, HEADER_DL, rows_dl)
        write_rows(VEC_SHA, HEADER_SHA, rows_sha)
        write_rows(VEC_HMAC, HEADER_HMAC, rows_hmac)
        if proto is not None:
            write_rows(VEC_IDR, HEADER_IDR, gen_idr_rows(o, proto))
        print("--refresh 已重写：%s(%d) / %s(%d) / %s(%d) / %s(%d) / %s(%d) / %s(%d) / %s(%d) / %s(%d)%s"
              % (os.path.basename(VEC_SN), len(rows_sn),
                 os.path.basename(VEC_PARSE), len(rows_parse),
                 os.path.basename(VEC_JCS), len(rows_jcs),
                 os.path.basename(VEC_B64), len(rows_b64),
                 os.path.basename(VEC_MSG), len(rows_msg),
                 os.path.basename(VEC_DL), len(rows_dl),
                 os.path.basename(VEC_SHA), len(rows_sha),
                 os.path.basename(VEC_HMAC), len(rows_hmac),
                 " / %s(%d)" % (os.path.basename(VEC_IDR), len(gen_idr_rows(o, proto))) if proto else ""))

    # ---- ② 快照 vs Python 参考 -------------------------------------------
    if o is not None:
        exp_sn, exp_parse = build_rows(o)
        groups = [
            ("test_vectors_sn.txt", exp_sn, read_rows(VEC_SN)),
            ("test_vectors_snparse.txt", exp_parse, read_rows(VEC_PARSE)),
            ("test_vectors_jcs.txt", gen_jcs_rows(o), read_rows(VEC_JCS)),
            ("test_vectors_b64url.txt", gen_b64_rows(o), read_rows(VEC_B64)),
        ]
        if proto is not None:
            groups.append(("test_vectors_msg.txt", gen_msg_rows(o, proto), read_rows(VEC_MSG)))
            groups.append(("test_vectors_downlink.txt", gen_dl_rows(o, proto), read_rows(VEC_DL)))
            groups.append(("test_vectors_id_report.txt", gen_idr_rows(o, proto), read_rows(VEC_IDR)))
        else:
            print("跳过 test_vectors_msg/downlink.txt：拿不到 orpah_proto（报文构造/解码的单一源）")
        groups.append(("test_vectors_sha256.txt", gen_sha_rows(o), read_rows(VEC_SHA)))
        groups.append(("test_vectors_hmac.txt", gen_hmac_rows(o), read_rows(VEC_HMAC)))
        for name, exp, got in groups:
            cmp_snapshot(name, exp, got, fails)
    else:
        print("跳过 ②：拿不到 Python 参考 ⇒ 只跑 C 侧自检（**不等于**已交叉验证）")

    # ---- ②b 快照：HGIC 帧层（单一源 = 本仓 tools/txah_hgic.py，**不依赖上游**） ----
    if hg is not None:
        for sname, spath, gen in (("test_vectors_hgic.txt", VEC_HGIC, gen_hgic_frame_rows),
                                  ("test_vectors_hgic_parse.txt", VEC_HGIC_PARSE, gen_hgic_parse_rows),
                                  ("test_vectors_hgic_ctrl.txt", VEC_HGIC_CTRL, gen_hgic_ctrl_rows)):
            if not os.path.isfile(spath):
                print("FAIL 缺向量文件 %s（先跑 --refresh）" % sname)
                fails.append("缺 " + sname)
                continue
            cmp_snapshot(sname, gen(hg), read_rows(spath), fails)
    else:
        print("跳过 ②b：拿不到 tools/txah_hgic.py（HGIC 的单一源）")

    # ---- ① C 侧自检（每个内核一个可执行目标） ------------------------------
    targets = [
        ("SN 内核", "sn_cli", ["sn.c", "sn_cli.c"],
         [("C selfcheck（SN 内置黄金样本）", ["selfcheck"]),
          ("C selftest（校验位向量）", ["selftest", VEC_SN]),
          ("C sn-selftest（SN 解析向量）", ["sn-selftest", VEC_PARSE])]),
        ("JCS/b64url/msg/dl/idr", "jcs_cli",
         ["jcs.c", "b64url.c", "msg.c", "downlink.c", "sha256.c", "hmac.c",
          "id_report.c", "jcs_cli.c"],
         [("C selfcheck（JCS/b64url 冒烟）", ["selfcheck"]),
          ("C jcs-selftest（JCS 向量）", ["jcs-selftest", VEC_JCS]),
          ("C b64url-selftest（b64url 向量）", ["b64url-selftest", VEC_B64]),
          ("C msg-selftest（报文信封向量）", ["msg-selftest", VEC_MSG]),
          ("C dl-selftest（下行解码向量）", ["dl-selftest", VEC_DL]),
          ("C id-report-selftest（已签报文 + 信封）", ["id-report-selftest", VEC_IDR])]),
        ("SHA-256/HMAC", "sha_cli", ["sha256.c", "hmac.c", "sha_cli.c"],
         [("C selfcheck（分块自洽 + HMAC 不变量）", ["selfcheck"]),
          ("C sha256-selftest（hashlib 向量）", ["sha256-selftest", VEC_SHA]),
          ("C hmac-selftest（hmac 向量）", ["hmac-selftest", VEC_HMAC])]),
        ("HGIC 帧层", "hgic_cli", ["hgic.c", "hgic_cli.c"],
         [("C selfcheck（HGIC 不变量）", ["selfcheck"]),
          ("C frame-selftest（帧构造向量）", ["frame-selftest", VEC_HGIC]),
          ("C parse-selftest（流解析/重同步向量）", ["parse-selftest", VEC_HGIC_PARSE]),
          ("C ctrl-selftest（控制面解码向量）", ["ctrl-selftest", VEC_HGIC_CTRL])]),
    ]
    exes = {}
    with tempfile.TemporaryDirectory() as td:
        for tname, tbin, srcs, checks in targets:
            exe = os.path.join(td, tbin + (".exe" if os.name == "nt" else ""))
            how, err = compile_c(exe, [os.path.join(HERE, s) for s in srcs])
            if err is not None:
                print("FAIL 编译 %s 失败（%s）：" % (tname, how or "找不到编译器"))
                print(err.strip() or "(无输出)")
                fails.append("编译 " + tname)
                continue
            exes[tbin] = exe
            print("C 侧已编译：%s（%s）" % (tname, how))
            for title, argv in checks:
                p = run(exe, argv)
                out = (p.stdout or "").strip()
                tail = out.splitlines()[-1] if out else ""
                ok = (p.returncode == 0) and ("FAIL" not in out) and tail.startswith("PASS")
                print("%s %s  ->  %s" % ("PASS" if ok else "FAIL", title, tail or "(无输出)"))
                if not ok:
                    fails.append(title)
                    print(out)

        # ---- ★ 最重要的一步：让 Python 的 verify_report 真去验 **C 产出的报文** ----
        #     字节一致只证明“两边一样”；这一步证明“**服务端真的收得下**”（离线版的服务端判据）。
        if proto is not None and "jcs_cli" in exes:
            import json as _json
            ks = o.KeyStore()
            ks.register(o.Device(sn="CN-WH01-9AF3C1D2"), model="bench-c4a", firmware="c4a")
            n_ok = 0
            for case in _idr_cases(o):
                sn, ts, nonce, level, batt, caprtc, fw = case
                key_hex = o.derive_demo_hmac(sn, 1).hex()
                p = _run([exes["jcs_cli"], "id-report", sn, ts, nonce, level,
                          key_hex, batt, caprtc, fw])
                out = (p.stdout or "").strip()
                if p.returncode != 0 or "\t" not in out:
                    print("FAIL id-report CLI 未产出（rc=%s out=%r）" % (p.returncode, out))
                    fails.append("id-report CLI")
                    break
                try:
                    rep = _json.loads(bytes.fromhex(out.split("\t")[0]).decode("utf-8"))
                except Exception as e:                    # noqa: BLE001
                    print("FAIL 解析 C 产出的报文失败：%r" % (e,))
                    fails.append("解析 C 产出")
                    break
                # ⚠ 时间窗是**另一个机制**（上游有专门测试），这里只判签名：
                #   向量里的 ts 是**固定值**（提交的文件必须确定性），所以把 now 钉到它上；
                #   ts=0（设备无 RTC 的正常路径）则不传 now（协议本来就会跳过窗口）。
                ts_int = int(ts)
                v = o.verify_report(rep, ks, now=(None if ts_int == 0 else ts_int),
                                    used_nonces=o.NonceCache())
                if int(level) in (1, 2):
                    if not v.get("accepted"):
                        print("FAIL 服务端**不接受** C 产出的降级报文：level=%s error=%s"
                              % (level, v.get("error")))
                        fails.append("verify_report 拒绝 C 产出（level=%s）" % level)
                    elif int(v.get("level", -1)) != int(level):
                        print("FAIL 验签通过但级别不符：C=%s 服务端=%s"
                              % (level, v.get("level")))
                        fails.append("级别不符（level=%s）" % level)
                    else:
                        n_ok += 1
                else:
                    # L3（alg=none）：规范 §8.3 就是“只做覆盖” ⇒ 只陈述，不当判据
                    print("   [info] L3(alg=none) 服务端判定：accepted=%s trust=%s coverage_only=%s"
                          % (v.get("accepted"), v.get("trust"), v.get("coverage_only")))
            if n_ok:
                print("PASS 服务端验签 C 产出的降级报文（verify_report 接受 %d 条 level=1/2）" % n_ok)

    print("-" * 66)
    if fails:
        print("FAIL %d 项未过：%s" % (len(fails), fails))
        return 2
    if o is None:
        print("PASS（C 侧自检全过；**未与 Python 交叉验证** —— 没找到上游参考实现）")
        return 0
    print("PASS 协议内核：C 实现与 Python 参考零偏差（校验位 / SN 解析 / JCS / b64url / 报文信封 / "
          "下行解码 / SHA-256 / HMAC / 已签报文 / HGIC 帧构造 / HGIC 流解析 / HGIC 控制面 十二组）")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
