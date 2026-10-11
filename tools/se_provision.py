# -*- coding: utf-8 -*-
"""se_provision.py — **ATECC608B 产线流程**（一颗一行判据；读配置 → 锁 Config → GenKey → 锁 Data → 复验）。

为什么要它：d 步那套流程原来只活在**临时工装**里（`%TEMP%\\lock_and_verify.py` 之类 ✗），
明天就找不到了 ✗；而手上还有 **7 颗**要按同一套走。⇒ 入库、可复跑、每步都有判据 ✓。

★ **不依赖原厂库** ✓：组包/CRC 用 `tools/ch347_i2c.py`（它的 CRC 又是
`proto/run_cross_test.py` 的**单一源** ✓），I²C 用 CH347F ✓。原厂包只在本机当过参照物
（见 `docs/atecc608b-se.md` §8.3），**不是产品依赖** ✗。

★ 台架规矩（同一时刻只让一个 I²C 主机干活）：跑这个工具时 **nano 必须断电 / 按住 RST**；
SE 与上拉的 3V3 从 CH347F 出 ✓（接线见 `hardware/wiring/README.md` 的
`cstep-ch347f-nanoch32-atecc608b`）。

步骤（顺序照 `docs/atecc608b-se.md` §8.7；**标 ✗ 的两步不可逆，都必须显式 `--yes`**）：

    read          只读：读配置 128 B + 判 Config/Data 锁没锁 + 与**参照配置**逐字节比对
    lock-config   ✗ 锁配置区（之后 SE 的槽位/地址/I²C 参数就定死了）
    genkey        ✗ 生成 Slot 0 的 P-256 私钥并**只**导出公钥（私钥物理不出芯片 ✓）
    lock-data     ✗ 锁数据区（之后私钥再也改不了 —— 这正是规范 §6 要的“一代终身”✓）
    verify        复验：真随机数 + `Nonce`+`Sign` 签一条固定摘要 ⇒ **用 Slot 0 公钥独立验签**
    all           一条龙：read → lock-config → genkey → lock-data → verify（每步都先核对再动）

用法（一次只跑一步最稳；`all` 用于新片、也要带 `--yes`）：

    python tools\\se_provision.py read                     # 只读，先跑它
    python tools\\se_provision.py all --yes --record se_provisioned.txt
    python tools\\se_provision.py verify                   # 已经配好的片子单独复验

退出码：0 = 这一步的判据全过；2 = 判据不过/器件状态不符（**不往下走** ✗）；1 = 用法/环境问题。

⚠ **判据不该自己说 "锁好了"**：锁完都**读回**（`LockValue/LockConfig` 应变 `00 00`），
签名则交给**独立实现**（python `cryptography`）用公钥验 —— 不是我们自报 ✓。
"""
import argparse
import datetime
import hashlib
import os
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "proto"))

import ch347_i2c as ci                      # noqa: E402  （I²C 主机 + 组包 + CRC 单一源）
try:
    import run_cross_test as rct            # noqa: E402  （状态码 → 文字，与 C 侧逐字相同）
    HAVE_RCT = True
except Exception:                           # noqa: BLE001
    HAVE_RCT = False

# ---- 常量（出处逐条标；数值以**实测**为准）----------------------------------
ADDR_DEFAULT = 0x60
WADDR_CMD = 0x03        # 命令（写）—— 实测：原厂栈读写用 0x03/0x00，见 docs/atecc608b-se.md §8.2
WADDR_RESP = 0x00       # 响应（读）
OP_READ = 0x02
OP_CMD = 0x17           # Lock
OP_GENKEY = 0x40
OP_NONCE = 0x16
OP_SIGN = 0x41
MODE_READ_32 = 0x80     # `Read` 的 32 B 模式（§8.4 记的就是这样读的四块）
LOCK_ZONE_CONFIG = 0x80 # `Lock` 的 zone：Config（实测包 `07 17 80 00 00 39 8D` ✓）
LOCK_ZONE_DATA = 0x00   # `Lock` 的 zone：Data（p1=0x00；★ 首次上机时与参照对拍记下 ✓）
GENKEY_MODE_NEW = 0x00  # GenKey：生成（槽已锁时它**返回已有公钥**、不会重新生成 ✓ 实测过）
NONCE_MODE_MSGDIGBUF = 0x43
SIGN_MODE_EXTERNAL = 0xA0
SIGN_SLOT = 0x0000
WAKE_REPLY = b"\x04\x11\x33\x43"
SELFTEST_FAIL = b"\x04\x07\xC4\x40"
SELFTEST_MSG = b"ORPAH d-step probe v1"     # 与固件 `se` 自检签的同一条摘要 ✓

# ---- 参照：2026-10-11 那颗的**出厂配置** 128 B（`docs/atecc608b-se.md` §8.4 实测记录）----
# 用途：新片先跟它逐字节比 —— 一致才敢往下锁 ✗（不一致 = 这批芯片被动过/不是同型号 ⇒ 停下问人 ✓）。
REF_CONFIG_HEX = (
    "0123EDD500006003277C8CE3EE615900"      # [  0] SN 前 8 B + Rev + AES 使能…
    "C0000000832087208F20C48F8F8F8F8F"      # [ 16] I2C_Enable/地址(0xC0=0x60) + Slot0..4 配置
    "9F8FAF8F000000000000000000000000"      # [ 32] Slot5 起 + 其余
    "0000AF8FFFFFFFFF00000000FFFFFFFF"      # [ 48]
    "00000000000000000000000000000000"      # [ 64]
    "0000000000005555FFFF000000000000"      # [ 80] ★ 86/87 = LockValue/LockConfig（此为**未锁**的 55 55 ✓）
    "3300330033001C001C001C001C001C00"      # [ 96] 各槽的 KeyConfig（1C = P-256 / 33 = HMAC）
    "3C003C003C003C003C003C003C001C00")     # [112]


def hexs(b):
    return " ".join("%02X" % x for x in b) if b else "-"


class Se:
    """一颗 SE 的会话（CH347F 当 I²C 主机）。"""

    def __init__(self, index, clk, addr7, with_crc=True, verbose=True):
        self.addr = addr7
        self.crc = with_crc
        self.verbose = verbose
        self.dev = ci.Ch347I2c(index=index)
        self.dev.__enter__()
        if not self.dev.i2c_usable(clk):
            raise ci.cs.Ch347Error("CH347 I²C 初始化失败（设备没插/iIndex 不对/驱动不对）")

    def close(self):
        try:
            self.dev.__exit__(None, None, None)
        except Exception:                    # noqa: BLE001
            pass

    # ---- 传输层 ----------------------------------------------------------
    def wake(self, tries=6):
        """唤醒 + **把 4 字节唤醒应答读掉**（不读掉它，后面读响应会把旧包读回来 ✗，见 §8.2）。

        返回 (ok, 读到的 4 字节)。⚠ 器件**本来就醒着**时这里拿不到 `04 11 33 43`（读到的是
        输出缓冲里的旧内容）⇒ **不当失败** ✓（同 `docs/atecc608b-se.md` §8.8 那条教训）。"""
        last = b""
        for _ in range(tries):
            self.dev.xfer([0x00], 0)         # 唤醒令牌 = 对地址 0x00 写 0 字节
            time.sleep(0.002)
            last = self.dev.read_from(self.addr, WADDR_RESP, 4)
            if last[:4] == WAKE_REPLY:
                return True, last
            if last[:4] == SELFTEST_FAIL:
                return False, last           # 器件自检失败 —— 真问题 ✗
            time.sleep(0.002)
        return False, last

    def _ready(self, ms=250):
        """等器件备好：**轮询地址直到 ACK**（它算完之前根本不答地址 ✓ —— 原厂 HAL 也这样）。"""
        end = time.monotonic() + ms / 1000.0
        while time.monotonic() < end:
            if self.dev.probe(self.addr):
                return True
            time.sleep(0.001)
        return False

    def cmd(self, pkt, want_len, tries=3):
        """发一条命令、读一条响应。返回 (rc, resp, note)；rc==0 且 len(resp)==want_len 才算好。

        `want_len` = **期望的完整响应长度**（含 1 字节 count）。4 字节错误包会被翻成状态文字 ✓。

        ★★ 响应必须**一次读完**（`want_len` 个字节），再按 count 切片 ✓ ——
        不能“先读 1 字节 count、再去读剩下的” ✗：那第二次会**重发字地址 0x00** ⇒
        器件以为“从头读” ⇒ 读到的是 `count` 开头的一整份新流（错位）⇒ CRC 必错 ✗
        （2026-10-11 实测踩到：`Read` 命令被完整 ACK、回 35 B、但 CRC 不对 ✓）。
        多读的字节（器件回短响应时）是 don't-care ✓，只按 `count` 取前面那一段用 ✓。"""
        for k in range(tries):
            self.wake()
            ack = self.dev.write_only(self.addr, pkt, WADDR_CMD, ret_ack=True)
            if self.verbose:
                print("      · 写命令 %s（ACK=%s，期望 %d）"
                      % (hexs(pkt), ack, 2 + len(pkt)))
            if ack is not None and ack < 2 + len(pkt):
                continue                     # 中途被 NACK（通常是包长/CRC 不对）
            if not self._ready():
                if self.verbose:
                    print("      · 器件 250 ms 内不答地址（还没算完/没接上）⇒ 重试")
                continue
            raw = self.dev.read_from(self.addr, WADDR_RESP, max(4, want_len))
            if not raw:
                continue
            cnt = raw[0]                     # ★ count 只占 1 字节
            if cnt == 4:
                st = raw[1]
                nm = rct.py_atecc_status_name(st) if HAVE_RCT else "?"
                return 2, raw[:4], "4 字节错误包：状态 0x%02X（%s）" % (st, nm)
            if len(raw) < cnt:
                return 2, raw, "读回 %d 字节 < count %d" % (len(raw), cnt)
            resp = raw[:cnt]
            if cnt != want_len:
                return 2, resp, "count=%d（期望 %d）" % (cnt, want_len)
            ck = ci.crc_ok(resp)
            if ck is False:
                return 2, resp, "响应 CRC 不对（raw=%s）" % hexs(resp)
            return 0, resp, "ok（%d B）" % len(resp)
        return 2, b"", "重试 %d 次都不行" % tries

    # ---- 命令层 ----------------------------------------------------------
    def read_zone(self, zone, addr, mode=MODE_READ_32, n=32):
        pkt = ci.atecc_pkt(OP_READ, mode, addr, self.crc)
        rc, resp, note = self.cmd(pkt, 1 + n + (2 if self.crc else 0))
        if rc != 0:
            return None, note
        return resp[1:1 + n], note

    def read_config(self):
        """读配置区 128 B（四条 32 B；`param2` 是**字地址** = 字节地址 / 4，§8.4 的口径）。"""
        buf = b""
        for off in (0x00, 0x20, 0x40, 0x60):
            blk, note = self.read_zone(0x00, off // 4)
            if blk is None:
                return None, "块 @0x%02X 读失败：%s" % (off, note)
            buf += blk
        return buf, "4 块 × 32 B"

    def lock_zone(self, zone):
        pkt = ci.atecc_pkt(OP_CMD, zone, 0x0000, self.crc)
        # Lock 成功时回的是 **4 字节状态包且状态 0x00**（`04 00 …`）——所以不能按 want_len 判 ✓
        for k in range(3):
            self.wake()
            ack = self.dev.write_only(self.addr, pkt, WADDR_CMD, ret_ack=True)
            if self.verbose:
                print("      · 写 %s（ACK=%s）" % (hexs(pkt), ack))
            if not self._ready():
                continue
            resp = self.dev.read_from(self.addr, WADDR_RESP, 4)
            if len(resp) == 4 and resp[0] == 4:
                st = resp[1]
                nm = rct.py_atecc_status_name(st) if HAVE_RCT else "?"
                # 状态 0x00 = 成功；0x0F = cannot execute（多半**已经锁过了** ✓ 不是坏）
                return st, "状态 0x%02X（%s）" % (st, nm)
        return None, "没读到 4 字节状态包"

    def genkey(self, slot):
        pkt = ci.atecc_pkt(OP_GENKEY, GENKEY_MODE_NEW, slot, self.crc)
        rc, resp, note = self.cmd(pkt, 1 + 64 + (2 if self.crc else 0))
        if rc != 0:
            return None, note
        return resp[1:65], note

    def sign_digest(self, digest32, slot=SIGN_SLOT):
        pkt = ci.atecc_pkt(OP_NONCE, NONCE_MODE_MSGDIGBUF, 0x0000, self.crc, extra=digest32)
        rc, resp, note = self.cmd(pkt, 4 if not self.crc else 4)
        if rc != 0:
            return None, "Nonce：" + note
        pkt = ci.atecc_pkt(OP_SIGN, SIGN_MODE_EXTERNAL, slot, self.crc)
        rc, resp, note = self.cmd(pkt, 1 + 64 + (2 if self.crc else 0))
        if rc != 0:
            return None, "Sign：" + note
        return resp[1:65], note


# ---- 判据小工具 -------------------------------------------------------------
def lock_state(cfg):
    """配置区字节 86/87 = LockValue / LockConfig（`0x55` = 未锁、`0x00` = 已锁 ✓，§8.4）。"""
    return cfg[86], cfg[87]


def verify_sig(pub64, digest32, sig64):
    """用**独立实现**（`cryptography`）验签；返回 None = 过，否则返回错误串。"""
    try:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        return "本机没装 `cryptography` ⇒ 验不了（**不算过** ✗）"
    try:
        pk = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + pub64)
        der = utils.encode_dss_signature(int.from_bytes(sig64[:32], "big"),
                                         int.from_bytes(sig64[32:], "big"))
        pk.verify(der, digest32, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
        return None
    except Exception as e:                                       # noqa: BLE001
        return "验签不过：%r" % (e,)


# ---- 各步 ------------------------------------------------------------------
def step_read(se, args):
    cfg, note = se.read_config()
    if cfg is None:
        print("FAIL 读配置失败：%s" % note)
        return 2, None
    lv, lc = lock_state(cfg)
    print("  配置区 128 B（读法：%s）：" % note)
    for i in range(0, 128, 16):
        print("    cfg[%3d] %s" % (i, hexs(cfg[i:i + 16])))
    print("  LockValue = 0x%02X   LockConfig = 0x%02X   ⇒ Config %s / Data %s"
          % (lv, lc, "已锁" if lc == 0x00 else "未锁",
             "已锁" if lv == 0x00 else "未锁"))
    print("  I2C_Enable/地址：cfg[16] = 0x%02X（0xC0 = 使能、地址 0x60）" % cfg[16])
    ref = bytes.fromhex(REF_CONFIG_HEX)
    if len(ref) != 128:
        print("warn 参照配置常量长度不对（%d）—— 这是本工具的 bug" % len(ref))
        return 1, cfg
    # ★ 逐字节比时**排除 86/87**：那两个字节**就是**锁状态（`55`=未锁 / `00`=已锁 ✓）——
    #   拿它们去比“参照的 55 55”只会把“已经锁好”的片子判成不一致 ✗（今天这颗就是已锁 ✓）。
    skip = {86, 87}
    diff = [i for i in range(128) if i not in skip and cfg[i] != ref[i]]
    if not diff:
        print("PASS 除锁状态字节(86/87)外，126 B 与**参照配置**逐字节一致 ✓"
              "（2026-10-11 那颗的出厂配置，§8.4）")
        print("     ⇒ 这颗跟参照是同一批/同型号、没被动过 ✓（锁状态按上面那两行判）")
        return 0, cfg
    print("FAIL 与参照配置有 %d 处不同（前几处：%s）"
          % (len(diff), ", ".join("cfg[%d] %02X→%02X" % (i, ref[i], cfg[i]) for i in diff[:6])))
    print("     ⇒ **先停下** ✗：这颗芯片被动过 / 不是同批 / 不是同型号。\n"
          "       要么人工核过再说，要么别锁（锁是不可逆的 ✗）。")
    return 2, cfg


def step_lock_config(se, args, cfg=None):
    print("  ✗ 不可逆：锁**配置区**（槽位/I²C 参数从此定死）")
    st, note = se.lock_zone(LOCK_ZONE_CONFIG)
    print("  Lock(Config) → %s" % note)
    if st is None:
        return 2
    if st == 0x0F:
        print("  info 状态 0x0F = cannot execute ⇒ 多半**已经锁过了** ✓（继续按“读回”判，不按命令判）")
    elif st != 0x00:
        print("FAIL 锁配置区失败（状态 0x%02X）" % st)
        return 2
    cfg2, _ = se.read_config()
    if cfg2 is None:
        print("FAIL 锁后读回失败")
        return 2
    lv, lc = lock_state(cfg2)
    if lc != 0x00:
        print("FAIL 锁后 LockConfig = 0x%02X（应当 0x00）" % lc)
        return 2
    print("PASS 锁后读回：LockConfig = 0x00 ✓（LockValue = 0x%02X ⇒ 数据区%s）"
          % (lv, "仍未锁 ✓（故意留的）" if lv != 0x00 else "也已锁"))
    return 0


def step_genkey(se, args):
    print("  ✗ 生成 Slot 0 的 P-256 私钥（**只**导出公钥；私钥物理不出芯片 ✓）")
    pub, note = se.genkey(SIGN_SLOT)
    if pub is None:
        print("FAIL GenKey(0) 失败：%s" % note)
        return 2, None
    print("  Slot 0 公钥：X = %s" % hexs(pub[:32]))
    print("                Y = %s" % hexs(pub[32:]))
    print("PASS 公钥已导出（hex = %s）" % pub.hex())
    return 0, pub


def step_lock_data(se, args, cfg=None):
    print("  ✗ 不可逆：锁**数据区**（Slot 0 的私钥从此改不了 = 规范 §6 要的“一代终身” ✓）")
    st, note = se.lock_zone(LOCK_ZONE_DATA)
    print("  Lock(Data) → %s" % note)
    if st is None:
        return 2
    if st == 0x0F:
        print("  info 状态 0x0F ⇒ 多半已经锁过了 ✓（按读回判）")
    elif st != 0x00:
        print("FAIL 锁数据区失败（状态 0x%02X）" % st)
        return 2
    cfg2, _ = se.read_config()
    if cfg2 is None:
        print("FAIL 锁后读回失败")
        return 2
    lv, lc = lock_state(cfg2)
    if lv != 0x00:
        print("FAIL 锁后 LockValue = 0x%02X（应当 0x00）" % lv)
        return 2
    print("PASS 锁后读回：LockValue = 0x00、LockConfig = 0x%02X ⇒ **双锁完成** ✓" % lc)
    return 0


def step_verify(se, args, pub=None):
    rc = 0
    # ① 真随机数
    pkt = ci.atecc_pkt(0x1B, 0x00, 0x0000, se.crc)
    rc_, resp, note = se.cmd(pkt, 1 + 32 + (2 if se.crc else 0))
    if rc_ != 0:
        print("FAIL Random 失败：%s" % note)
        return 2, None
    rnd = resp[1:33]
    pat = bytes([0xFF, 0xFF, 0x00, 0x00]) * 8
    if rnd == pat:
        print("FAIL Random 回的是 `FFFF0000` 模式 ⇒ **配置区还没锁** ✗（未锁时器件就是回这个）")
        return 2, None
    print("PASS 真随机数 32 B：%s" % hexs(rnd[:16]))
    # ② 公钥（没给就现取）
    if pub is None:
        pub, note = se.genkey(SIGN_SLOT)
        if pub is None:
            print("FAIL 取公钥失败（GenKey）：%s" % note)
            return 2, None
        print("  Slot 0 公钥 = %s" % pub.hex())
    # ③ 签一条固定摘要 + **独立验签**
    dg = hashlib.sha256(SELFTEST_MSG).digest()
    sig, note = se.sign_digest(dg)
    if sig is None:
        print("FAIL 签名失败：%s" % note)
        return 2, pub
    print("  Sign(slot 0) = r %s / s %s" % (hexs(sig[:32]), hexs(sig[32:])))
    err = verify_sig(pub, dg, sig)
    if err:
        print("FAIL %s" % err)
        return 2, pub
    print("PASS 板上那条签名被 **Slot 0 公钥独立验过** ✓（Prehashed(SHA256)）")
    # ④ 反例：翻 1 bit 必须不过
    bad = bytearray(dg)
    bad[0] ^= 0x01
    if verify_sig(pub, bytes(bad), sig) is None:
        print("FAIL 反例居然也验过 ⇒ 第③条不算数 ✗")
        return 2, pub
    print("PASS 反例（摘要翻 1 bit）被拒 ✓ ⇒ 第③条是真验签 ✓")
    return rc, pub


STEPS = ("read", "lock-config", "genkey", "lock-data", "verify")

# 台架**实测**的那条摘要 = SHA-256(SELFTEST_MSG)（2026-10-11，见 §8.10）
REF_DIGEST = hashlib.sha256(SELFTEST_MSG).hexdigest()


def selftest():
    """离线自检：把**台架实测的包字节**当锚点，验本工具的组包/CRC（不碰硬件 ✓）。

    锚点全是 2026-10-11 用**原厂库**驱动时**原厂栈实发**的字节（§8.10）⇒ 不是我们自己说的 ✓。
    ⚠ 这几条同时把“我抄错一个字节”钉住 ✓：包尾那 2 字节 CRC 就是整包的校验和。
    还没实测过的那两条（`Lock(Data)` / 32 B 块 `Read`）**不写成锚点** ✗ —— 等首片上机两边对拍再补 ✓。"""
    anchors = [
        ("Lock(Config)", ci.atecc_pkt(OP_CMD, LOCK_ZONE_CONFIG, 0, True), "0717800000398d"),
        ("GenKey(slot0)", ci.atecc_pkt(OP_GENKEY, GENKEY_MODE_NEW, 0, True), "07400000000005"),
        ("Sign(slot0)", ci.atecc_pkt(OP_SIGN, SIGN_MODE_EXTERNAL, 0, True), "0741a000007b85"),
        ("Random", ci.atecc_pkt(0x1B, 0x00, 0, True), "071b00000024cd"),
        ("Nonce(摘要)", ci.atecc_pkt(OP_NONCE, NONCE_MODE_MSGDIGBUF, 0, True,
                                    extra=bytes.fromhex(REF_DIGEST)),
         "2716430000" + REF_DIGEST + "301a"),
        # ★★ 四条 `Read`（原厂栈读这颗 608B 配置区 128 B 时**实发**的字节，§8.12）——
        #   它们钉住「param2 = **字地址**」与「param2 **小端**」（字 8 在线上是 `08 00`）✓。
        #   ⚠ 这里 p1 照**原厂**给 `0x80`（32 B 块模式）⇒ 整包逐字节相同；本工具自己那条读路径
        #   用的是 p1 = zone(`0x00`) + 字地址（也实测可用 ✓，见 §8.12）—— 两种都别当成唯一写法 ✗。
        #   期望值 = 原厂线上原样（`07 02 80 00 00 09 AD` 四条，见 §8.12 表）✓。
        ("Read(原厂 0x80/字00)", ci.atecc_pkt(OP_READ, MODE_READ_32, 0x0000, True), "070280000009ad"),
        ("Read(原厂 0x80/字08)", ci.atecc_pkt(OP_READ, MODE_READ_32, 0x0008, True), "07028008000a4d"),
        ("Read(原厂 0x80/字10)", ci.atecc_pkt(OP_READ, MODE_READ_32, 0x0010, True), "07028010000a1d"),
        ("Read(原厂 0x80/字18)", ci.atecc_pkt(OP_READ, MODE_READ_32, 0x0018, True), "070280180009fd"),
    ]
    bad = 0
    for name, got, want in anchors:
        ok = got.hex() == want
        bad += 0 if ok else 1
        print("  %s %-14s %s" % ("ok  " if ok else "BAD ", name,
                                 got.hex() if ok else "%s ≠ 实测 %s" % (got.hex(), want)))
    ck = ci.crc_ok(ci.atecc_pkt(0x1B, 0x00, 0, True))
    print("  %s CRC 自验（crc_ok）＝%s" % ("ok  " if ck else "BAD ", ck))
    bad += 0 if ck else 1
    print("CRC 参考（proto/run_cross_test.py）%s" % ("已加载 ✓" if ci.HAVE_RCT else "**没加载到** ✗"))
    if bad:
        print("FAIL 产线工具离线自检：%d 项不符" % bad)
        return 2
    print("PASS 产线工具离线自检：组包/CRC 与台架实测字节一致（%d 条锚点）" % len(anchors))
    return 0


def main():
    ap = argparse.ArgumentParser(description="ATECC608B 产线流程（读配置→锁Config→GenKey→锁Data→复验）")
    ap.add_argument("step", nargs="?", choices=list(STEPS) + ["all"],
                    help="要跑哪一步；只跑离线自检时省略它 ✓")
    ap.add_argument("--selftest", action="store_true", help="离线自检组包/CRC（不碰硬件）")
    ap.add_argument("--yes", action="store_true", help="不可逆步骤（lock-config / lock-data / genkey / all）必须显式给")
    ap.add_argument("--index", type=int, default=0, help="CH347 的 iIndex（缺省 0）")
    ap.add_argument("--clk", type=int, default=1, help="I²C 时钟：0=20k 1=100k(缺省) 2=400k 3=750k")
    ap.add_argument("--addr", default="%02x" % ADDR_DEFAULT, help="SE 的 7 位地址（hex，缺省 60）")
    ap.add_argument("--no-crc", action="store_true", help="命令包不挂 CRC（**默认挂**：实测这颗要挂 ✓）")
    ap.add_argument("--record", help="把结果（公钥/锁状态/时间）**追加**写进这个文件（产线台账 ✓）")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.step:
        print("FAIL 要么给一个 step（%s / all），要么给 `--selftest`" % "/".join(STEPS))
        return 1

    addr = int(args.addr, 16)
    irreversible = args.step in ("lock-config", "lock-data", "genkey", "all")
    if irreversible and not args.yes:
        print("FAIL 「%s」里有**不可逆**动作 ⇒ 必须显式加 `--yes`（先跑 `read` 看一眼 ✗）" % args.step)
        return 1

    print("=" * 78)
    print("ATECC608B 产线流程：step=%s  addr=0x%02X  clk=%s  CRC=%s  iIndex=%d"
          % (args.step, addr, ci.I2C_CLK.get(args.clk, "?"), "不带" if args.no_crc else "带", args.index))
    print("⚠ 台架规矩：**nano 必须断电 / 按住 RST**（同一时刻只让一个 I²C 主机干活 ✗）")
    print("⚠ 先关掉其它 CH347 工装（一个进程独占 CH347 ✗ —— 2026-09-23 踩过）")

    try:
        se = Se(args.index, args.clk, addr, with_crc=not args.no_crc)
    except ci.cs.Ch347Error as exc:
        print("FAIL CH347 打不开（iIndex=%d）：%s" % (args.index, exc))
        print("⇒ 先跑 `python tools\\ch347_i2c.py list` 看它在不在 ✗")
        return 2
    rc, cfg, pub = 0, None, None
    try:
        ok, four = se.wake()
        print("唤醒：%s（读到 %s%s）" % ("拿到 04 11 33 43 ✓" if ok else "没拿到唤醒应答（器件可能本来就醒着 ✓ 或没接 ✗）",
                                        hexs(four),
                                        "" if ok or four[:4] not in (SELFTEST_FAIL,) else " ← 自检失败 ✗"))
        if four[:4] == SELFTEST_FAIL:
            print("FAIL 器件回了自检失败应答 ⇒ 查供电/器件（先别动锁 ✗）")
            return 2

        if args.step == "all":
            for s in ("read", "lock-config", "genkey", "lock-data", "verify"):
                print("-" * 78)
                print("▶ %s" % s)
                if s == "read":
                    rc, cfg = step_read(se, args)
                elif s == "lock-config":
                    rc = step_lock_config(se, args, cfg)
                elif s == "genkey":
                    rc, pub = step_genkey(se, args)
                elif s == "lock-data":
                    rc = step_lock_data(se, args, cfg)
                else:
                    rc, pub = step_verify(se, args, pub)
                if rc != 0:
                    print("⇒ **停在 `%s`**（后面不动 ✗）" % s)
                    break
        elif args.step == "read":
            rc, cfg = step_read(se, args)
        elif args.step == "lock-config":
            rc = step_lock_config(se, args, cfg)
        elif args.step == "genkey":
            rc, pub = step_genkey(se, args)
        elif args.step == "lock-data":
            rc = step_lock_data(se, args, cfg)
        else:
            rc, pub = step_verify(se, args, pub)
    except ci.cs.Ch347Error as exc:
        # ★ 实测踩过（2026-10-12）：总线上没有器件应答时 DLL 只报 "CH347StreamI2C 失败" ✗ ——
        #   抛 traceback 对现场毫无帮助 ✓ ⇒ 一行判语 + 下一步该查什么 ✓。
        print("FAIL I²C/USB 层失败：%s" % exc)
        print("⇒ 查三件事：① nano 断电 / 按住 RST ✓ ② 只有一个进程用 CH347 ✓"
              " ③ SDA/SCL/3V3/GND 还接着吗 ✓")
        print("   `python tools\\ch347_i2c.py list` = CH347 在不在；"
              "`… scan` = 总线上有没有器件（两个都空 = 线/供电的问题 ✗）")
        return 2
    finally:
        se.close()

    print("=" * 78)
    print("%s：step=%s%s" % ("PASS" if rc == 0 else "FAIL", args.step,
                             "" if rc == 0 else "（rc=%d ⇒ 看上面的判语）" % rc))
    if args.record and rc == 0:
        line = "%s\taddr=0x%02X\tstep=%s\tpubkey_hex=%s\n" % (
            datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), addr, args.step,
            pub.hex() if pub else "-")
        with open(args.record, "a", encoding="utf-8", newline="\n") as f:
            f.write(line)
        print("台账已追加：%s" % args.record)
    return rc


if __name__ == "__main__":
    sys.exit(main())
