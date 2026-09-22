# -*- coding: utf-8 -*-
"""check_report_hex.py — 把**固件控制台 `idhex` 打出来的十六进制以太帧**交给**上游服务端
验签逻辑**判一次（= c4-γ 的上机判据里的"服务端验签通过"那一条，离线版）。

为什么要它：上机时我们能拿到的最硬证据是"模组/控制台里那条真实字节"。把它贴进来判一次，
就能回答"**服务端收得下吗**"——而不是只看固件自己打的一句 `sent`（那是自报）。

用法：
    python tools\\check_report_hex.py 7b2276223a312c...      # 直接给 hex（可跨行/带空白）
    python tools\\check_report_hex.py --file frame.hex
    python tools\\check_report_hex.py --selftest             # 用 proto/test_vectors_id_frame.txt 自检
    支持从 stdin 读（echo … | python tools\\check_report_hex.py -）

退出码：0 = 上游接受（level 0/1/2；level 3 是 §8.3 的"仅覆盖"，只陈述不算判据）；
        2 = 被拒（打印 error 码）；1 = 用法/环境问题（例如找不到上游参考实现）。

⚠ 单一源：帧解析与验签**都用上游**（`orpah_proto.parse_eth_frame` + `orpah_id.verify_report`），
  本文件不重写协议。所以要能 import 到 `orpah-over-halow/`（同级目录 / 环境变量 ORPAH_UPSTREAM）。
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEMO_SN = "CN-WH01-9AF3C1D2"
DEMO_GEN = 1


def find_upstream(explicit=None):
    cands = []
    if explicit:
        cands.append(explicit)
    if os.environ.get("ORPAH_UPSTREAM"):
        cands.append(os.environ["ORPAH_UPSTREAM"])
    cands += [os.path.join(os.path.dirname(REPO), "orpah-over-halow"),
              r"F:\git\orpah-over-halow",
              os.path.join(os.path.dirname(os.path.dirname(REPO)), "orpah-over-halow")]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "orpah_id.py")):
            return c
    return None


def load_upstream(explicit=None):
    up = find_upstream(explicit)
    if up is None:
        print("FAIL 找不到上游参考实现 orpah-over-halow（用 --upstream <dir> 或 ORPAH_UPSTREAM）")
        return None, None
    sys.path.insert(0, up)
    import orpah_id as o          # noqa: PLC0415
    import orpah_proto as proto   # noqa: PLC0415
    return o, proto


def strip_hex(text):
    """把 `idhex` 打出来的东西（可能带 [id] 行、空格、换行）里的十六进制抠出来。"""
    out = []
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("[") or s.startswith("#"):
            continue
        for ch in s:
            if ch in "0123456789abcdefABCDEF":
                out.append(ch)
            elif ch in " \t\r":
                continue
            else:                      # 非 hex 字符：这一行到此结束（例如句尾说明）
                break
    return "".join(out)


def judge(o, proto, frame, nonce_cache=None):
    got = proto.parse_eth_frame(frame)
    if got is None:
        print("FAIL 不是一条合法的 ORPAH 以太帧（长度要 ≥14 且 ethertype=0x88B5）")
        return 2
    try:
        env = json.loads(got[1].decode("utf-8"))
    except Exception as e:                                  # noqa: BLE001
        print("FAIL 内层 JSON 解不出：%r" % (e,))
        return 2
    rep = env.get("report")
    if not isinstance(rep, dict):
        print("FAIL 信封里没有 report 节点（键=%s）" % sorted(env.keys()))
        return 2

    hdr = rep.get("hdr") or {}
    pay = rep.get("payload") or {}
    print("帧长 %d B  type=%s  sn=%s  ts=%s  level=%s  alg=%s  nonce=%s"
          % (len(frame), env.get("type"), pay.get("sn"), pay.get("ts"),
             hdr.get("level"), hdr.get("alg"), pay.get("nonce")))
    print("cap=%s  battery_mv=%s  firmware=%s  seen_routers=%s"
          % (pay.get("cap"), pay.get("battery_mv"), pay.get("firmware"),
             pay.get("seen_routers")))

    ks = o.KeyStore()
    ks.register(o.Device(sn=pay.get("sn") or DEMO_SN, gen=DEMO_GEN),
                model="bench-c4g1", firmware="c4g1")
    v = o.verify_report(rep, ks, used_nonces=(nonce_cache or o.NonceCache()))
    print("服务端判定：accepted=%s level=%s trust=%s coverage_only=%s error=%s"
          % (v.get("accepted"), v.get("level"), v.get("trust"), v.get("coverage_only"),
             v.get("error")))
    lvl = hdr.get("level")
    if lvl in (0, 1, 2):
        if v.get("accepted") and int(v.get("level", -1)) == int(lvl):
            print("PASS 上游 verify_report 接受这条报文（level=%s）" % lvl)
            return 0
        print("FAIL 上游不接受（error=%s）" % v.get("error"))
        return 2
    print("info level=3（alg=none）按 §8.3 只做覆盖，不作为“验签通过”的判据")
    return 0


def selftest(o, proto):
    path = os.path.join(REPO, "proto", "test_vectors_id_frame.txt")
    rows = [l.rstrip("\n").split("\t") for l in open(path, encoding="utf-8")
            if l.strip() and not l.startswith("#")]
    nbad = 0
    for r in rows:
        print("-" * 60)
        print("用例 mode=%s level=%s" % (r[3], r[6]))
        rc = judge(o, proto, bytes.fromhex(r[8]))
        want = 0 if int(r[6]) in (0, 1, 2) else 0
        if rc != want:
            nbad += 1
    print("-" * 60)
    if nbad:
        print("FAIL --selftest：%d/%d 条不符" % (nbad, len(rows)))
        return 2
    print("PASS --selftest：%d 条向量全部按预期被上游接受（level 3 只陈述）" % len(rows))
    return 0


def main():
    ap = argparse.ArgumentParser(description="把固件产出的十六进制帧交给上游验签逻辑判一次")
    ap.add_argument("hex", nargs="?", help="十六进制帧（或用 - 从 stdin 读）")
    ap.add_argument("--file", help="从文件读 hex")
    ap.add_argument("--selftest", action="store_true", help="用 proto/test_vectors_id_frame.txt 自检")
    ap.add_argument("--upstream", help="orpah-over-halow 目录（默认自动找）")
    args = ap.parse_args()

    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                   # noqa: BLE001
            pass

    o, proto = load_upstream(args.upstream)
    if o is None:
        return 1
    if args.selftest:
        return selftest(o, proto)

    if args.file:
        text = open(args.file, encoding="utf-8", errors="replace").read()
    elif args.hex and args.hex != "-":
        text = args.hex
    else:
        text = sys.stdin.read()
    h = strip_hex(text)
    if len(h) < 28:
        print("FAIL hex 太短（%d 个字符）—— 至少要有一条以太帧" % len(h))
        return 1
    if len(h) % 2:
        h = h[:-1]
    try:
        frame = bytes.fromhex(h)
    except ValueError as e:
        print("FAIL hex 解不开：%r" % (e,))
        return 1
    return judge(o, proto, frame)


if __name__ == "__main__":
    sys.exit(main())
