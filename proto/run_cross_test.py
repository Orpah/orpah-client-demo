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

VEC_SN = os.path.join(HERE, "test_vectors_sn.txt")
VEC_PARSE = os.path.join(HERE, "test_vectors_snparse.txt")
VEC_JCS = os.path.join(HERE, "test_vectors_jcs.txt")
VEC_B64 = os.path.join(HERE, "test_vectors_b64url.txt")

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
    if os.path.isdir(args.orpah_dir):
        sys.path.insert(0, args.orpah_dir)
        try:
            import orpah_id as o                      # noqa: F401
        except Exception as e:                        # noqa: BLE001
            print("!! 找到 %s 但 import orpah_id 失败：%r" % (args.orpah_dir, e))
            o = None
    else:
        print("!! 未找到上游参考实现目录 %s（--orpah-dir 指定）" % args.orpah_dir)

    # ---- ③ --refresh：先重写向量（需要上游） -------------------------------
    if args.refresh:
        if o is None:
            print("!! --refresh 需要上游 Python 参考实现；已退出")
            return 2
        rows_sn, rows_parse = build_rows(o)
        rows_jcs = gen_jcs_rows(o)
        rows_b64 = gen_b64_rows(o)
        write_rows(VEC_SN, HEADER_SN, rows_sn)
        write_rows(VEC_PARSE, HEADER_PARSE, rows_parse)
        write_rows(VEC_JCS, HEADER_JCS, rows_jcs)
        write_rows(VEC_B64, HEADER_B64, rows_b64)
        print("--refresh 已重写：%s(%d) / %s(%d) / %s(%d) / %s(%d)"
              % (os.path.basename(VEC_SN), len(rows_sn),
                 os.path.basename(VEC_PARSE), len(rows_parse),
                 os.path.basename(VEC_JCS), len(rows_jcs),
                 os.path.basename(VEC_B64), len(rows_b64)))

    # ---- ② 快照 vs Python 参考 -------------------------------------------
    if o is not None:
        exp_sn, exp_parse = build_rows(o)
        groups = [
            ("test_vectors_sn.txt", exp_sn, read_rows(VEC_SN)),
            ("test_vectors_snparse.txt", exp_parse, read_rows(VEC_PARSE)),
            ("test_vectors_jcs.txt", gen_jcs_rows(o), read_rows(VEC_JCS)),
            ("test_vectors_b64url.txt", gen_b64_rows(o), read_rows(VEC_B64)),
        ]
        for name, exp, got in groups:
            if [tuple(str(x) for x in r) for r in exp] != [tuple(r) for r in got]:
                fails.append("快照 %s 与 Python 参考不一致（用 --refresh 重生成并核对 diff）" % name)
                print("FAIL 快照 %s 与 Python 参考不一致" % name)
                for i, (a, b) in enumerate(zip(exp, got)):
                    if tuple(str(x) for x in a) != tuple(b):
                        print("   line %d: PY=%s  FILE=%s" % (i + 1, a, b))
                        break
                else:
                    print("   行数不同：PY=%d FILE=%d" % (len(exp), len(got)))
            else:
                print("PASS 快照 %s 与 Python 参考逐行一致（%d 行）" % (name, len(got)))
    else:
        print("跳过 ②：拿不到 Python 参考 ⇒ 只跑 C 侧自检（**不等于**已交叉验证）")

    # ---- ① C 侧自检（每个内核一个可执行目标） ------------------------------
    targets = [
        ("SN 内核", "sn_cli", ["sn.c", "sn_cli.c"],
         [("C selfcheck（SN 内置黄金样本）", ["selfcheck"]),
          ("C selftest（校验位向量）", ["selftest", VEC_SN]),
          ("C sn-selftest（SN 解析向量）", ["sn-selftest", VEC_PARSE])]),
        ("JCS/b64url", "jcs_cli", ["jcs.c", "b64url.c", "jcs_cli.c"],
         [("C selfcheck（JCS/b64url 冒烟）", ["selfcheck"]),
          ("C jcs-selftest（JCS 向量）", ["jcs-selftest", VEC_JCS]),
          ("C b64url-selftest（b64url 向量）", ["b64url-selftest", VEC_B64])]),
    ]
    with tempfile.TemporaryDirectory() as td:
        for tname, tbin, srcs, checks in targets:
            exe = os.path.join(td, tbin + (".exe" if os.name == "nt" else ""))
            how, err = compile_c(exe, [os.path.join(HERE, s) for s in srcs])
            if err is not None:
                print("FAIL 编译 %s 失败（%s）：" % (tname, how or "找不到编译器"))
                print(err.strip() or "(无输出)")
                fails.append("编译 " + tname)
                continue
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

    print("-" * 66)
    if fails:
        print("FAIL %d 项未过：%s" % (len(fails), fails))
        return 2
    if o is None:
        print("PASS（C 侧自检全过；**未与 Python 交叉验证** —— 没找到上游参考实现）")
        return 0
    print("PASS 协议内核：C 实现与 Python 参考零偏差（校验位 / SN 解析 / JCS / b64url 四组）")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
