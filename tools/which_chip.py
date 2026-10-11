# -*- coding: utf-8 -*-
"""which_chip.py —— 用密钥库里登记的几把公钥**逐个**验一条签名，认出"桌上这颗 SE 是哪一颗"。

用法：
    # 先让固件签一条（控制台 `se`，或 PC 侧 `tools/se_provision.py read`）
    python tools\\which_chip.py <r-hex> <s-hex>

为什么要它（2026-10-11 上机踩出来的缺口 ✗）：
  固件里的 SN 原来是**演示 SN**（`CN-WH01-9AF3C1D2`），而入库的每一颗 SE 各有自己的 SN；
  上游 `verify_report` 是按**帧里的 SN**去查库的 ⇒ SN 对不上 ⇒ 只能拿演示派生钥去验
  SE 签的帧 ⇒ 必然 `signature_invalid` ✗ —— 那不是签错了，是**尺子对不上号** ✓。
  所以换一颗芯片时的第一步就是**认出它是哪一颗**，再把固件 SN 设成它
  （控制台 `idsn <SN>` ✓，不用重烧 ✓）。

口径（与 `docs/atecc608b-se.md` §8.23 那次临时脚本一致 ✓）：
  `se` 命令签的是 **SHA-256("ORPAH d-step probe v1") 这个摘要**（Prehashed）⇒
  验签必须用 `Prehashed(SHA256())`；库里 `pubkey_pem` 是 P-256 公钥 ✓。

★ 不硬编码任何本机路径：上游目录靠 `check_report_hex.find_upstream()`（同一套查找口径 ✓）。
"""
import os
import sqlite3
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import check_report_hex as crh                                    # noqa: E402

MSG = b"ORPAH d-step probe v1"


def which(r_hex, s_hex, upstream=None):
    """返回 (命中的 SN 列表, 逐行结果)；`--` 开头的参数不参与解析。

    ★ 读法 = **直读 `keys` 表的 `pubkey_pem`**（2026-10-11 真机上就是这么读通的 ✓）。
      不改成上游 `KeyStoreDB` 的读接口，是因为那些方法名/返回字段**我没核过** ✗ ——
      猜字段名的亏今晚已经吃过两次（`status` / `pubkey_b64` 都不存在 ✗）；这条表结构是
      `PRAGMA table_info(keys)` 当场看到的 ✓。
    """
    up = crh.find_upstream(upstream)
    if up is None:
        print("FAIL 找不到上游 orpah-over-halow（用 --upstream <dir> 或 ORPAH_UPSTREAM）")
        return None, None
    dbp = os.path.join(up, "orpah.db")
    if not os.path.isfile(dbp):
        print("FAIL 找不到 %s" % dbp)
        return None, None

    sig = utils.encode_dss_signature(int(r_hex, 16), int(s_hex, 16))
    h = hashes.Hash(hashes.SHA256())
    h.update(MSG)
    dg = h.finalize()
    print("密钥库：%s" % dbp)
    print("摘要 = SHA-256(%r) = %s" % (MSG, dg.hex()))
    print("-" * 74)
    con = sqlite3.connect(dbp)
    rows = list(con.execute("SELECT sn, gen, pubkey_pem, se_sn FROM keys"))
    con.close()
    out, hit = [], []
    for sn, gen, pem, se_sn in rows:
        pub = serialization.load_pem_public_key(pem.encode())
        try:
            pub.verify(sig, dg, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
            ok = True
        except Exception:                                          # noqa: BLE001
            ok = False
        out.append((sn, gen, se_sn, ok))
        print("%-30s gen=%-3s se_sn=%-44s %s"
              % (sn, gen, se_sn, "★ 验过了 ✓" if ok else "不对"))
        if ok:
            hit.append(sn)
    print("-" * 74)
    return hit, out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    up = None
    if "--upstream" in sys.argv:
        up = sys.argv[sys.argv.index("--upstream") + 1]
    if len(args) < 2:
        print(__doc__)
        return 1
    hit, _ = which(args[0], args[1], upstream=up)
    if hit is None:
        return 1
    if len(hit) == 1:
        print("结论：桌上这颗 = %s ✓（固件里 `idsn %s` 就能用库里的钥验签 ✓）"
              % (hit[0], hit[0]))
        return 0
    if hit:
        print("结论：%d 行都验过（库里公钥重复了？）%s" % (len(hit), hit))
        return 2
    print("结论：库里**都不对** ✗ ⇒ 这颗不是已入库的那几颗"
          "（或是新片/没跑过 `genkey` ⇒ 先看 `se` 的 Sign 是否 ok）")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
