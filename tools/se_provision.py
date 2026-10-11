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
    config-hmac   ⚙ 把 HMAC 槽(slot 4)的 WriteConfig 由出厂 8 改成 0：**只能在锁配置之前** ✓
    lock-config   ✗ 锁配置区（之后 SE 的槽位/地址/I²C 参数就定死了）
    write-hmac    ✎ 把 32 B 降级 HMAC 钥**明文**写进 slot 4（★ 必须在 `lock-config` **之后** ✗）
    hmac-verify   ★ 在器件里用 slot 4 算一次 HMAC，与软件 HMAC **逐字节对拍**
                    （slot 4 是 `IsSecret` ⇒ 读不出来 ✗ ⇒ 这是“钥真写进去了”的唯一证据 ✓）
    genkey        ✗ 生成 Slot 0 的 P-256 私钥并**只**导出公钥（私钥物理不出芯片 ✓）
    lock-data     ✗ 锁数据区（之后私钥再也改不了 —— 这正是规范 §6 要的“一代终身”✓）
    verify        复验：真随机数 + `Nonce`+`Sign` 签一条固定摘要 + **公钥重算 = 登记** + 独立验签
    all           一条龙：按上面这个顺序逐步走（每步都先核对再动，哪步不过就**停在那儿** ✗）

⚠ **顺序有硬约束**（都不是审美问题 ✗）：`config-hmac` 必须在 `lock-config` **之前**（配置一锁就改不了 ✗）；
**一切数据区写**（`write-hmac` / `genkey`）必须在 `lock-config` **之后** ✗ ——
未锁配置时器件直接回 `0x0F cannot execute` ✓（实测 §8.18：`write-hmac` 放在 `lock-config` 之前时，
写 slot 4 与 slot 8 **都**被拒 ⇒ 与“哪个槽”无关 ✗，是**器件状态**不允许 ✓）；
`write-hmac` 又必须在 `lock-data` **之前**（数据区一锁，明文写就写不进去了 ✗）。

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
import hmac
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

# 原厂包只作**本机参照**（不入库、不作产品依赖 ✗，§8.3）：装上就用它复核配置位段 ✓
try:
    import cryptoauthlib.device as _dev      # noqa: F401
    HAVE_DEV = True
except Exception:                           # noqa: BLE001
    HAVE_DEV = False

# ---- 常量（出处逐条标；数值以**实测**为准）----------------------------------
ADDR_DEFAULT = 0x60
WADDR_CMD = 0x03        # 命令（写）—— 实测：原厂栈读写用 0x03/0x00，见 docs/atecc608b-se.md §8.2
WADDR_RESP = 0x00       # 响应（读）
OP_READ = 0x02
OP_WRITE = 0x12         # Write
OP_CMD = 0x17           # Lock
OP_GENKEY = 0x40
OP_NONCE = 0x16
OP_SIGN = 0x41
OP_SHA = 0x47           # SHA（HMAC 也走这一条：608 上 HMAC 的 END = 0x02 ✓）
MODE_READ_32 = 0x80     # `Read`/`Write` 的 32 B 位（`ATCA_ZONE_READWRITE_32` ✓）。
                        # ★ 出处 = 原厂 `calib_command.h`："Zone bit 7 set: Access 32 bytes" ✓
# 32 B 写的 param1 = zone | 0x80（原厂 `calib_write_zone()`：“If there are 32 bytes to write,
# then xor the bit into the mode” ✓）；param2 = 地址（config：`block<<3`；data：`slot<<3 | block<<8`
# —— 两种都照 `calib_get_addr()` ✓，与 §8.12 实测的字地址口径一致 ✓）。
ZONE32_CONFIG = 0x00 | MODE_READ_32      # = 0x80
ZONE32_DATA = 0x02 | MODE_READ_32        # = 0x82
SHA_MODE_HMAC_START = 0x04   # param1；param2 = **key slot**（原厂 `calib_sha_hmac_init()` ✓）
SHA_MODE_HMAC_UPDATE = 0x01  # param1；param2 = 64（一次一块 ✓）
SHA_MODE_608_HMAC_END = 0x02 # ★ 608 的 HMAC 结束码（`calib_sha_hmac_finish()` 里按器件选 ✓）
SHA_TARGET_TEMPKEY = 0x00    # 结果落点（0x00 = TempKey：同 `NONCE/KDF_MODE_TARGET_TEMPKEY` ✓）；
                             # 响应里总是会回 32 B 摘要 ✓ ⇒ 落点不影响我们取值 ✓
HMAC_SLOT = 4           # ★ 降级 HMAC 密钥的槽：照原厂自己的测试夹具（“HMAC 用 slot 4”✓，见 §8.5 注）
HMAC_WRITECONFIG_FACTORY = 8   # 出厂值（§8.14 解码所得）
HMAC_WRITECONFIG_CLEAR = 0     # 改成“明文可写”（原厂自己写 8–15 就是用这个值的明文写 ✓）
SHA_BLOCK = 64          # 一次 64 B（`SHA_DATA_MAX` ✓）
MODE_READ_32 = 0x80     # `Read` 的 32 B 模式（§8.4 记的就是这样读的四块）
LOCK_ZONE_CONFIG = 0x80  # `Lock` 的 zone：Config —— 实测包 `07 17 80 00 00 39 8D` ✓
# `Lock` 的 zone：Data。★ 2026-10-11 按**原厂源码**定案（不再靠猜 ✗）：
#   `calib_command.h`：`LOCK_ZONE_CONFIG 0x00` / `LOCK_ZONE_DATA 0x01` / `LOCK_ZONE_NO_CRC 0x80`
#   `calib_lock.c`：`lock_config_zone()` = `LOCK_ZONE_NO_CRC|LOCK_ZONE_CONFIG` = **0x80** ✓
#                  （与上一条实测包逐字节对得上 ✓ ⇒ 证明这套常量读对了 ✓）
#                  `lock_data_zone()`   = `LOCK_ZONE_NO_CRC|LOCK_ZONE_DATA`   = **0x81** ✓
#   ⚠ 原来写 0x00 是**错的** ✗（那会变成“不用 CRC 的 config 锁” ⇒ 锁错区/报错）。
LOCK_ZONE_DATA = 0x80 | 0x01
# GenKey 两档（出处 = 原厂 `calib_genkey.c`；`calib_command.h` 里是 `GENKEY_MODE_*`）：
#   `calib_genkey()`      → `GENKEY_MODE_PRIVATE (0x04)` = **在槽里新建**私钥 + 回公钥
#   `calib_get_pubkey()`  → `GENKEY_MODE_PUBLIC  (0x00)` = **只由已有私钥算**公钥（不写）
# ⚠ 0x00 **不是**“新建” ✗ —— 产线“生成密钥”那一步必须用 0x04 ✓
#   （拿 0x00 去当新建用，会拿到一个“不是槽里那把钥”的公钥 ⇒ 登记进密钥库就全盘验不过 ✗✗）。
GENKEY_MODE_PRIVATE = 0x04
GENKEY_MODE_PUBLIC = 0x00
NONCE_MODE_MSGDIGBUF = 0x43
SIGN_MODE_EXTERNAL = 0xA0
SIGN_SLOT = 0x0000
WAKE_REPLY = b"\x04\x11\x33\x43"
SELFTEST_FAIL = b"\x04\x07\xC4\x40"
# ★ 响应里的“无错误”状态（出处 `calib_command.h`：`CMD_STATUS_OK 0x00` ✓）
ATECC_STATUS_OK = 0x00
# ★ “刚被唤醒”的状态（出处同上：`CMD_STATUS_WAKEUP 0x11` ✓；唤醒应答 `04 11 33 43` 里就是它 ✓）
#   2026-10-11 实测：命令响应里读到它 = 读到了**旧的唤醒应答**（不是这条命令的响应 ✗）⇒ 重同步 ✓
ATECC_STATUS_WAKEUP = 0x11
SELFTEST_MSG = b"ORPAH d-step probe v1"     # 与固件 `se` 自检签的同一条摘要 ✓
# 只读半证的**第二条消息**：100 B > 64 ⇒ 能多走一轮 `SHA HMAC_UPDATE` ✓（1×64 + 36 B 尾巴）
# （第一条 23 B 只走 START + END ⇒ 两条合起来把 UPDATE 那条路也盖住 ✓）
HMAC_RO_MSG2 = bytes(range(100))
# 只读半证的**默认槽**：出厂这颗 slot 8 的 `SlotConfig = 0x0000`
# （byte 36/37 ⇒ `IsSecret=0`、`ReadKey=0` = 允许明文读 ✓；`KeyConfig = 0x003C` ⇒ KeyType=7 = SHA/HMAC ✓）
HMAC_RO_SLOT = 8

# ★★ 等器件算完的**固定等待**（2026-10-11 起不再“轮询地址” ✗）
#   起因（实测，§8.16）：那条“探地址”事务（`START + 地址 + STOP`、**不带数据**）会把
#   **CH347 的 I²C 引擎搞死** —— 之后连板载 `0x50` 都读不到，且**只有关掉再打开设备会话**
#   能救 ✓（重设速度无效 ✗）。同条件下实测：带“探地址” 3/12 成功 ✗、改固定等待 **12/12** ✓
#   （整份 128 B（4 块 × 5 轮）**20/20** ✓）。
#   ⚠ 下面两个数是**台面余量**，不是数据手册的 tEXEC ⇒ 宁多勿少：多等无害 ✓；
#     少等会读到 NACK、把引擎又搞死 ✗。将来有实测值再收窄 ✓。
WAIT_MS_DEFAULT = 20     # Read/Info/Random/Write/Lock：实测 ~3.5 ms 就绪 ⇒ 20 ms 有余量 ✓
WAIT_MS_CRYPTO = 150     # Sign / GenKey / HMAC-END 这类要算 ECC/SHA 的（一次 0.15 s，台面可接受）

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
        self.clk = clk                    # 重开会话时要按同一个速度档重设 ✓
        self.reopens = 0                  # 传输层累计重开会话次数（可观测 ✓）
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
    def _dll(self, fn, what, tries=3):
        """跑一次 DLL 事务；失败就**关设备 + 重开**再试（有界 ✓ 计数可见 ✓）。

        ★ 2026-10-11 实测（§8.16）：CH347 的 I²C 引擎一报错就**卡住** —— 之后连板载 `0x50`
        都读不到 ✗；**只有关掉再打开设备会话能救** ✓（`i2c_usable()` 重设速度**无效** ✗）。
        ⇒ 一次毛刺不再等于整条流程失败 ✗⇒✓。
        ⚠ 本函数只包**一次传输调用**（如“读响应”）—— 它**不会重发命令**：
          重发与否由调用方定（有状态的命令不能瞎重发 ✗，见 `cmd()` 的 `retry_write`）。"""
        last = ""
        for k in range(tries):
            try:
                return fn()
            except ci.cs.Ch347Error as exc:
                last = str(exc)
                self.reopens += 1
                if self.verbose:
                    print("      · DLL 报错（%s）⇒ 重开 CH347 会话后重试本步（累计第 %d 次）"
                          % (exc, self.reopens))
                if not self.dev.reopen(self.clk):
                    time.sleep(0.05)
        raise ci.cs.Ch347Error("%s：重开 %d 次仍失败（%s）" % (what, tries, last))

    def _settle(self, ms):
        """等器件算完：**固定等待**（取代原来的“轮询地址直到 ACK” ✗，见文件头那段说明）。"""
        if ms > 0:
            time.sleep(ms / 1000.0)

    def wake(self, tries=6):
        """唤醒 + **把 4 字节唤醒应答读掉**（不读掉它，后面读响应会把旧包读回来 ✗，见 §8.2）。

        返回 (ok, 读到的 4 字节)。⚠ 器件**本来就醒着**时这里拿不到 `04 11 33 43`（读到的是
        输出缓冲里的旧内容）⇒ **不当失败** ✓（同 `docs/atecc608b-se.md` §8.8 那条教训）。
        ★ 唤醒令牌（对地址 `0x00` 写 0 字节）与“探地址”**形状相近、但没有那个副作用** ✓
          （2026-10-11 实测：去掉它成功率反而更低 ⇒ 该发还得发 ✓）。"""
        last = b""
        for _ in range(tries):
            self._dll(lambda: self.dev.xfer([0x00], 0), "发唤醒令牌")
            time.sleep(0.002)
            last = self._dll(lambda: self.dev.read_from(self.addr, WADDR_RESP, 4), "读唤醒应答")
            if last[:4] == WAKE_REPLY:
                return True, last
            if last[:4] == SELFTEST_FAIL:
                return False, last           # 器件自检失败 —— 真问题 ✗
            time.sleep(0.002)
        return False, last

    def cmd(self, pkt, want_len, tries=3, wait_ms=WAIT_MS_DEFAULT, retry_write=True):
        """发一条命令、读一条响应。返回 (rc, resp, note)；rc==0 且 len(resp)==want_len 才算好。

        `want_len` = **期望的完整响应长度**（含 1 字节 count）。4 字节错误包会被翻成状态文字 ✓。

        ★★ 响应必须**一次读完**（`want_len` 个字节），再按 count 切片 ✓ ——
        不能“先读 1 字节 count、再去读剩下的” ✗：那第二次会**重发字地址 0x00** ⇒
        器件以为“从头读” ⇒ 读到的是 `count` 开头的一整份新流（错位）⇒ CRC 必错 ✗
        （2026-10-11 实测踩到：`Read` 命令被完整 ACK、回 35 B、但 CRC 不对 ✓）。
        多读的字节（器件回短响应时）是 don't-care ✓，只按 `count` 取前面那一段用 ✓。

        ★ 就绪等待 = **固定等 `wait_ms`**（不再“探地址” ✗，见文件头那段）。
        ★ `retry_write=False` = **写被 DLL 报错时不许重发本命令** ✗（有状态的命令：如
          `GenKey(创建)` 会重新生成钥、`SHA UPDATE` 会把同一段数据再喂一遍 ⇒ 结果错 ✗✗）。
        """
        for k in range(tries):
            self.wake()
            try:
                ack = self.dev.write_only(self.addr, pkt, WADDR_CMD, ret_ack=True)
            except ci.cs.Ch347Error as exc:
                self.reopens += 1
                self.dev.reopen(self.clk)
                if not retry_write:
                    raise ci.cs.Ch347Error(
                        "写命令时 DLL 报错（%s）—— 本步属**有状态命令、不自动重发** ✗；"
                        "先人工确认器件状态，再决定要不要重跑" % exc)
                if self.verbose:
                    print("      · 写命令 DLL 报错（%s）⇒ 重开会话后**重发本命令**（累计第 %d 次）"
                          % (exc, self.reopens))
                continue
            if self.verbose:
                print("      · 写命令 %s（ACK=%s，期望 %d）"
                      % (hexs(pkt), ack, 2 + len(pkt)))
            if ack < 2 + len(pkt):
                continue                     # 中途被 NACK（通常是包长/CRC 不对）
            self._settle(wait_ms)
            raw = self._dll(lambda: self.dev.read_from(self.addr, WADDR_RESP, max(4, want_len)),
                            "读响应")
            if not raw:
                continue
            cnt = raw[0]                     # ★ count 只占 1 字节
            if cnt == 4:
                # ★★ 4 字节包 = **状态包**：**状态 0x00 就是“成功、无数据”** ✓（不是错误 ✗）——
                #   很多命令（`Write` / `Lock` / `SHA HMAC_START` / `HMAC_UPDATE` / `Nonce`）
                #   成功时就只回这 4 字节 ✓。
                #   ★ 口径与**固件同一份** ✓：`firmware/Periph/atecc.c:502`
                #     `atecc_msg_err_pkt_status(…) == 0 && st == ATECC_STATUS_OK` = 成功的状态包 ✓
                #   （2026-10-11 实测踩到：`HMAC_START` 明明成功，却在这里被当成错误 ✗）
                st = raw[1]
                nm = rct.py_atecc_status_name(st) if HAVE_RCT else "?"
                if st == ATECC_STATUS_OK:
                    return 0, raw[:4], "状态包：状态 0x00（ok，无数据）"
                if st == ATECC_STATUS_WAKEUP:
                    # ★ 0x11 = **“刚被唤醒”** ⇒ 读回来的是**唤醒应答**，不是这条命令的响应 ✗ ——
                    #   器件输出缓冲里还留着旧内容（§8.8 那条教训的同一形状 ✓）。
                    #   2026-10-11 实测踩到：中途"重开会话"自愈之后，`HMAC_START` 读回 0x11 ✗。
                    #   ⇒ **不当错误** ✗：重新同步（下一轮会重发唤醒令牌 + 重发本命令 ✓）
                    if self.verbose:
                        print("      · 读到唤醒应答（状态 0x11）⇒ 缓冲里是旧内容，"
                              "重新同步后重发本命令")
                    continue
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
        # ★ `Lock` 是**幂等**的（已锁过 ⇒ 器件回 `0x0F` cannot execute ✓ 不是坏）⇒ 这里可以重发 ✓
        for k in range(3):
            self.wake()
            try:
                ack = self.dev.write_only(self.addr, pkt, WADDR_CMD, ret_ack=True)
            except ci.cs.Ch347Error as exc:
                self.reopens += 1
                self.dev.reopen(self.clk)
                if self.verbose:
                    print("      · 写 %s：DLL 报错（%s）⇒ 重开会话后重发（累计第 %d 次）"
                          % (hexs(pkt), exc, self.reopens))
                continue
            if self.verbose:
                print("      · 写 %s（ACK=%s）" % (hexs(pkt), ack))
            self._settle(WAIT_MS_DEFAULT)
            resp = self._dll(lambda: self.dev.read_from(self.addr, WADDR_RESP, 4), "读状态包")
            if len(resp) == 4 and resp[0] == 4:
                st = resp[1]
                nm = rct.py_atecc_status_name(st) if HAVE_RCT else "?"
                # 状态 0x00 = 成功；0x0F = cannot execute（多半**已经锁过了** ✓ 不是坏）
                return st, "状态 0x%02X（%s）" % (st, nm)
        return None, "没读到 4 字节状态包"

    def genkey(self, slot, mode=GENKEY_MODE_PRIVATE):
        """`mode` 缺省 = **创建**（0x04）。只想按已存的钥算公钥就传 `GENKEY_MODE_PUBLIC` ✓。

        ★ **创建那一步（0x04）不许自动重发** ✗（`retry_write=False`）：重发会把槽里的私钥
        **重新生成** ⇒ 已经登记进密钥库的公钥就对不上了 ✗✗（而“只算公钥”的 `0x00` 无所谓 ✓）。
        失败就**停下来报错**，让人先确认槽里到底有没有钥，再决定下一步 ✓。
        """
        pkt = ci.atecc_pkt(OP_GENKEY, mode, slot, self.crc)
        rc, resp, note = self.cmd(pkt, 1 + 64 + (2 if self.crc else 0),
                                 wait_ms=WAIT_MS_CRYPTO,
                                 retry_write=(mode != GENKEY_MODE_PRIVATE))
        if rc != 0:
            return None, note
        return resp[1:65], note

    def write_word_config(self, word_addr, data4):
        """写 config 区的一个 **4 B 字**（param1 = zone、**不**带 32 B 位）。

        ⚠ config 区只能按 4 B 字改：前 16 B（序列号）**不可写** ✓（原厂 `calib_write_config_zone()`
        自己就“skip the first 16 bytes” ✓）；32 B 块写会连序列号一起写 ⇒ 必然失败 ✗。"""
        if len(data4) != 4:
            return None, "字长不是 4（%d）" % len(data4)
        pkt = ci.atecc_pkt(OP_WRITE, 0x00, word_addr, self.crc, extra=data4)
        rc, resp, note = self.cmd(pkt, 4)
        if rc != 0:
            return None, note
        return resp, note

    def write_block(self, zone32, addr, data32):
        """写一个 32 B 块（`Write`，param1 = zone|0x80、param2 = 地址，地址算法见文件头注释 ✓）。"""
        if len(data32) != 32:
            return None, "块长度不是 32（%d）" % len(data32)
        pkt = ci.atecc_pkt(OP_WRITE, zone32, addr, self.crc, extra=data32)
        rc, resp, note = self.cmd(pkt, 4)
        if rc != 0:
            return None, note
        return resp, note

    def read_slot(self, slot, block=0):
        """读一个 **数据槽** 的 32 B（`Read` 的 data 区 32 B 块：param1 = **0x82**、param2 = **slot<<3 | block<<8** ✓）。

        出处（与文件头常量同源 ✓）：原厂 `calib_read_zone()` ：“If there are 32 bytes to write,
        then xor the bit into the mode”（⇒ param1 = `ATCA_ZONE_DATA(0x02) | 0x80`）+ `calib_get_addr()`
        对 `ATCA_ZONE_DATA` 的地址算法（`slot<<3 | block<<8 | offset`）✓。

        ⚠ **不是所有槽都能读** ✗ —— 能不能读由 `SlotConfig` 的 `IsSecret`/`ReadKey` 定
        （见 `slotcfg_decode()`）⇒ 调用方**先判再读** ✓（别把私钥槽读出来 ✗）。"""
        pkt = ci.atecc_pkt(OP_READ, ZONE32_DATA, (slot << 3) | (block << 8), self.crc)
        rc, resp, note = self.cmd(pkt, 1 + 32 + (2 if self.crc else 0))
        if rc != 0:
            return None, note
        return resp[1:33], note

    def sha_hmac(self, slot, msg):
        """在器件里算 HMAC-SHA256（密钥在 `slot` 里、**不出芯片** ✓）。

        三条命令（顺序/模式照原厂 `calib_sha_hmac_{init,update,finish}()` ✓）：
          `SHA(HMAC_START, param2=slot)` → 若干个 `SHA(HMAC_UPDATE, 64 B)` →
          `SHA(608_HMAC_END | TEMPKEY, 余下<64 B)` ⇒ 响应 32 B 摘要 ✓。
        ★ 这个函数的**判据在调用方**：算出来的必须与软件 HMAC 逐字节相同 ✓（不靠“长得对”✓）。"""
        pkt = ci.atecc_pkt(OP_SHA, SHA_MODE_HMAC_START, slot, self.crc)
        rc, resp, note = self.cmd(pkt, 4, wait_ms=WAIT_MS_CRYPTO)
        if rc != 0:
            return None, "HMAC_START：" + note
        rest = len(msg) % SHA_BLOCK if len(msg) else 0
        off = 0
        while len(msg) - off >= SHA_BLOCK:
            pkt = ci.atecc_pkt(OP_SHA, SHA_MODE_HMAC_UPDATE, SHA_BLOCK, self.crc,
                               extra=bytes(msg[off:off + SHA_BLOCK]))
            # ★ UPDATE **不自动重发** ✗：重发会把同一段数据再喂一遍 ⇒ 摘要就错了 ✗✗
            rc, resp, note = self.cmd(pkt, 4, wait_ms=WAIT_MS_CRYPTO, retry_write=False)
            if rc != 0:
                return None, "HMAC_UPDATE@%d：%s" % (off, note)
            off += SHA_BLOCK
        tail = bytes(msg[off:off + rest])
        pkt = ci.atecc_pkt(OP_SHA, SHA_MODE_608_HMAC_END | SHA_TARGET_TEMPKEY, len(tail),
                           self.crc, extra=tail)
        # ★ END 同样**不自动重发** ✗（它会消耗/结束那段上下文 ⇒ 重发语义不明）
        rc, resp, note = self.cmd(pkt, 1 + 32 + (2 if self.crc else 0),
                                  wait_ms=WAIT_MS_CRYPTO, retry_write=False)
        if rc != 0:
            return None, "HMAC_END：" + note
        return resp[1:33], note

    def sign_digest(self, digest32, slot=SIGN_SLOT):
        pkt = ci.atecc_pkt(OP_NONCE, NONCE_MODE_MSGDIGBUF, 0x0000, self.crc, extra=digest32)
        rc, resp, note = self.cmd(pkt, 4)
        if rc != 0:
            return None, "Nonce：" + note
        pkt = ci.atecc_pkt(OP_SIGN, SIGN_MODE_EXTERNAL, slot, self.crc)
        rc, resp, note = self.cmd(pkt, 1 + 64 + (2 if self.crc else 0), wait_ms=WAIT_MS_CRYPTO)
        if rc != 0:
            return None, "Sign：" + note
        return resp[1:65], note


# ---- 判据小工具 -------------------------------------------------------------
def lock_state(cfg):
    """配置区字节 86/87 = LockValue / LockConfig（`0x55` = 未锁、`0x00` = 已锁 ✓，§8.4）。"""
    return cfg[86], cfg[87]


def cfg_ident_diff(cfg):
    """**身份字节**（`cfg[0..15]`）与参照的差异 —— **只作 info，不是判据** ✗。

    ★★ 2026-10-11 **实测**（两颗同型号、都没被动过的片子）：参照那颗与这颗的差异
      **恰好全落在 0..15** ✓（`cfg[2] ED→F2`、`cfg[3] D5→12`、`cfg[8..11]`、`cfg[14] 59→55`）——
      那是**每颗都不一样**的序列号/出厂字节 ✓（`SN03(4) + RevNum(4) + SN48(5) + AES/I2C…`，
      字段顺序见 §8.4 与 `HMAC_SLOT_BYTE` 那段注释）。
    ⇒ 把它当“这颗被动过”的判据是**错的** ✗ —— 会把**每一颗新片都拒掉** ✗✗
      （本轮就真拦了一次 ✓：`read` 在**没锁的新片**上报 FAIL，而那颗完全正常 ✓）。
    """
    ref = bytes.fromhex(REF_CONFIG_HEX)
    return [i for i in range(16) if cfg[i] != ref[i]]


def cfg_func_diff(cfg, against=None):
    """**功能位段**（`cfg[16..127]`，跳过 86/87 两个锁字节）与参照的差异 —— **这才是判据** ✓。

    为什么到 16 开始 ✓：`SlotConfig`(20..51) + 计数器 + `KeyConfig`(96..127) 全在里面 ——
    产线流程**只依赖这些**（槽能不能读/写、钥类型、地址…）✓；
    而 `0..15` 是每颗都不同的身份字节 ⇒ 排除 ✗（理由见 `cfg_ident_diff()`）。
    锁字节 86/87 排除 ✗ —— 它们本来就会因**锁/未锁**而变（`55`↔`00` ✓）。

    `against` = 要比对的那份参照（缺省 = 出厂那份 ✓）；可用 `ref_variants()` 里的任一合法态 ✓。
    """
    ref = bytes.fromhex(REF_CONFIG_HEX) if against is None else against
    return [i for i in range(16, 128) if i not in (86, 87) and cfg[i] != ref[i]]


def ref_variants():
    """可接受的**功能位段参照**（多种**已知好**状态 ⇒ 逐字节比时**任一命中即可** ✓）。

    为什么不止一份 ✗：`config-hmac` 会**按设计**改掉 slot 4 的两个字节
    （`byte 28/29`：`8F 8F`(WriteConfig=8) → `0F 8F`(WriteConfig=0) ✓）⇒
    只拿“出厂那份”去比，会把**我们自己改对的地方**判成不一致 ✗
    （2026-10-11 `all` 续跑实测踩到：`read` 在**已经跑过 config-hmac** 的片上报 FAIL ✓）。
    两份 = ① 出厂（`8`）② 已打补丁（`0`，正是 `config-hmac` 的目标态 ✓）。
    ⚠ 槽 4 那两个字节的合法性另有**专门判据**（`step_config_hmac` 查 `wc ∈ {8, 0}` ✓ +
      写后读回不变量 ✓）⇒ 这里放两个合法态不会放过乱改 ✗。
    """
    ref = bytearray(bytes.fromhex(REF_CONFIG_HEX))
    out = [("出厂（slot 4 WriteConfig=8）", bytes(ref))]
    off = HMAC_SLOT_BYTE
    w = (ref[off] | (ref[off + 1] << 8)) & 0xFFFF
    w = (w & ~HMAC_WC_MASK & 0xFFFF) | (HMAC_WRITECONFIG_CLEAR << HMAC_WC_SHIFT)
    ref[off], ref[off + 1] = w & 0xFF, (w >> 8) & 0xFF
    out.append(("已跑 config-hmac（WriteConfig=0）", bytes(ref)))
    return out


def slotcfg_decode(cfg, slot):
    """解 `SlotConfig[slot]` 的位段；越界返回 None。

    **位段口径已在仓内核对过** ✓（不是猜的）：`SlotConfig` 从 **byte 20** 起、每槽 **2 B**、小端；
    位段 = `ReadKey(0-3) NoMac(4) LimitedUse(5) EncryptRead(6) **IsSecret(7)**
    WriteKey(8-11) **WriteConfig(12-15)**` ✓ —— 对账：出厂这颗 slot 4 = `8F 8F`(LE 0x8F8F)
    ⇒ `WriteConfig = 8` ✓，与原厂结构体解出来的值一致（§8.14 + `HMAC_SLOT_BYTE` 那段注释）✓。
    """
    if not 0 <= slot <= 15:
        return None
    off = 20 + 2 * slot
    w = cfg[off] | (cfg[off + 1] << 8)
    return {"word": w, "off": off, "ReadKey": w & 0xF, "NoMac": (w >> 4) & 1,
            "LimitedUse": (w >> 5) & 1, "EncryptRead": (w >> 6) & 1, "IsSecret": (w >> 7) & 1,
            "WriteKey": (w >> 8) & 0xF, "WriteConfig": (w >> 12) & 0xF}


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
    # ★ 判据 = **功能位段**（`cfg[16..127]`，跳 86/87 锁字节）✓；
    #   `cfg[0..15]`（SN + 出厂字节）**每颗都不一样** ⇒ 只作 info ✗（2026-10-11 实测踩过，见 §8.18）。
    ident = cfg_ident_diff(cfg)
    if ident:
        print("  info 身份字节（cfg[0..15]）与参照差 %d 处：%s ✓ **这不是判据**"
              % (len(ident),
                 ", ".join("cfg[%d] %02X→%02X" % (i, ref[i], cfg[i]) for i in ident[:6])))
        print("       （序列号段每颗都不同 ✓ —— 实测两颗同型号的差异**恰好全落在 0..15**，"
              "功能位段全同 ✓）")
    diff = cfg_func_diff(cfg)
    hits = [nm for nm, rf in ref_variants() if not cfg_func_diff(cfg, rf)]
    if hits:
        print("PASS 功能位段（cfg[16..127]，跳 86/87 锁字节）与**已知好参照**逐字节一致 ✓"
              "（匹配：%s）" % " / ".join(hits))
        print("     ⇒ 这颗与参照是**同型号/同批**、位段没被动过 ✓（锁状态按上面那两行判）")
        return 0, cfg
    best = min(ref_variants(), key=lambda kv: len(cfg_func_diff(cfg, kv[1])))
    diff = cfg_func_diff(cfg, best[1])
    print("FAIL 功能位段与**任何已知好参照**都不一致（最接近的一份 = %s，仍差 %d 处：%s）"
          % (best[0], len(diff),
             ", ".join("cfg[%d] %02X→%02X" % (i, best[1][i], cfg[i]) for i in diff[:6])))
    print("     ⇒ **先停下** ✗：这颗芯片的**位段**被动过 / 不是同型号。\n"
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
    # ★ 把**锁后的 cfg** 交给调用方 ✓：后面几步（如 `write-hmac` 的守卫）判的是**当前状态**，
    #   拿“锁之前那份快照”会误报 ✗（2026-10-11 `all` 实测踩到：守卫看到 `0x55` 就拦下了 ✓）。
    return 0, cfg2


def step_genkey(se, args):
    print("  ✗ 生成 Slot 0 的 P-256 私钥（**只**导出公钥；私钥物理不出芯片 ✓）")
    print("    模式 = 0x%02X（原厂 `calib_genkey()` = GENKEY_MODE_PRIVATE ⇒ **真的新建** ✓）"
          % GENKEY_MODE_PRIVATE)
    pub, note = se.genkey(SIGN_SLOT)
    if pub is None:
        print("FAIL GenKey(0) 失败：%s" % note)
        if "0x0F" in note:
            print("     ⇒ `0x0F cannot execute` 的常见成因：**数据区已经锁了** ✗ ⇒ 创建不了新钥 ✓")
            print("        （已锁的片子上只用 `verify`：它按 `GENKEY_MODE_PUBLIC` 只算公钥 ✓，不写芯片 ✓）")
        return 2, None
    print("  Slot 0 公钥：X = %s" % hexs(pub[:32]))
    print("                Y = %s" % hexs(pub[32:]))
    print("PASS 公钥已导出（hex = %s）" % pub.hex())
    print("     ⇒ 拿它去密钥库登记（见 `--record`；登记后再 `lock-data` ✗）")
    # ★★ **关键闸门**（2026-10-11 加）：创建完**立刻**用“只算公钥”那档（`mode 0x00`）**再算一遍**
    #   并逐字节比对 ⇒ 证明“创建那步**真的把私钥写进槽里了**” ✓
    #   ⚠ 为什么要有它 ✗：“创建”那档 `0x04` 只是**引原厂源码**（`calib_genkey.c`）、**未实测** ✗；
    #     若它其实“只算公钥、不创建”，**只有到 `lock-data` 之后重算才会发现** ✗ ——
    #     那时槽 0 里没钥、**这颗片子就废了** ✗✗。本闸门在**锁数据之前**就能判 ✓，
    #     判不过就停下 ⇒ 片子**还救得回来** ✓。
    #   ★ 用来重算的 `GenKey(0x00)` 包字节 `07 40 00 00 00 00 05` 是**原厂栈实发**（§8.10）⇒ 有实测支撑 ✓。
    pub2, note2 = se.genkey(SIGN_SLOT, GENKEY_MODE_PUBLIC)
    if pub2 is None:
        print("FAIL 创建后用 `GenKey(0x00)` 重算公钥失败：%s" % note2)
        print("     ⇒ **先别锁数据区** ✗：说明槽 0 里可能没有那把私钥")
        return 2, pub
    if pub2 != pub:
        print("FAIL 创建时导出的公钥 ≠ 槽内重算的公钥 ✗")
        print("      创建：%s" % pub.hex())
        print("      重算：%s" % pub2.hex())
        print("     ⇒ `GenKey(0x04)` 那步**没有真把钥写进槽** ✗（或模式写错）⇒ **别锁数据区** ✗")
        return 2, pub2
    print("PASS 创建后重算公钥 = 创建时导出的公钥 ✓（逐字节相同）⇒ 槽 0 里**确实有**那把私钥 ✓")
    print("     ⇒ 这才敢往下走 `lock-data` ✓（锁数据后 `verify` 会再验一次 ✓）")
    return 0, pub


def step_lock_data(se, args, cfg=None):
    print("  ✗ 不可逆：锁**数据区**（Slot 0 的私钥从此改不了 = 规范 §6 要的“一代终身” ✓）")
    print("    param1 = 0x%02X（引原厂 `calib_lock_data_zone()` ✓；★ 本条**未实测** —— 首片看读回）"
          % LOCK_ZONE_DATA)
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
    # ② 公钥：**只按槽里已有的钥算**（`GENKEY_MODE_PUBLIC` = 不写芯片 ✓）。
    #   ★★ 这一条是“创建那一步真的生效了”的**第二个声音** ✓：
    #     若创建时用错模式（算了个不属于槽里的公钥），这里重算出来的就会**与登记的不等** ✗；
    #     而且它是在 `lock-data` **之后**算的 ⇒ 槽里的钥已经改不了 ⇒ 相等才算数 ✓。
    pub_now, note = se.genkey(SIGN_SLOT, GENKEY_MODE_PUBLIC)
    if pub_now is None:
        print("FAIL 取公钥失败（GenKey mode 0x00）：%s" % note)
        return 2, pub
    if pub is None:
        print("  Slot 0 公钥（按槽内已有钥算）= %s" % pub_now.hex())
        pub = pub_now
    elif pub_now != pub:
        print("FAIL 锁后重算的公钥与登记的公钥**不等** ✗")
        print("      登记：%s" % pub.hex())
        print("      现算：%s" % pub_now.hex())
        print("     ⇒ 说明“创建私钥”那一步没真把钥写进槽 ✗（别拿这个公钥去登记 ✗）")
        return 2, pub
    else:
        print("PASS 锁后按槽内已有的钥重算公钥 = 登记的公钥 ✓（逐字节相同，%s…）"
              % pub_now.hex()[:32])
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


def load_hmac_key(args, need=True):
    """从 `--hmac-key-file` 读 32 B 降级 HMAC 钥（hex 文本或原始 32 B 都行 ✓）。

    ★ 纪律：**绝不把钥打印出来、也不写进台账** ✗ —— 只报指纹（SHA-256 前 4 B ✓）。
      文件不在且给了 `--gen-hmac-key` ⇒ 现生成 32 B 随机并**存到那个文件**（自行保管 ✓）。"""
    path = getattr(args, "hmac_key_file", None)
    if not path:
        print("FAIL 没给 `--hmac-key-file <文件>` ⇒ 不知道要写哪把钥 ✗")
        return None
    if not os.path.exists(path):
        if not getattr(args, "gen_hmac_key", False):
            print("FAIL 密钥文件不存在：%s（要现生成就加 `--gen-hmac-key` ✓）" % path)
            return None
        key = os.urandom(32)
        with open(path, "wb") as f:
            f.write(key)
        print("  已生成 32 B 随机 HMAC 钥 ⇒ %s（**这是密钥文件，自行保管 ✗**）" % path)
    else:
        raw = open(path, "rb").read()
        if len(raw) == 32:
            key = raw
        else:
            txt = raw.decode("utf-8", "replace")
            hx = "".join(c for c in txt if c in "0123456789abcdefABCDEF")
            if len(hx) != 64:
                print("FAIL 密钥文件既不是 32 B 原始、也不是 64 位 hex（len=%d）" % len(raw))
                return None
            key = bytes.fromhex(hx)
    print("  HMAC 钥指纹：%s…（sha256 前 4 B；**不打印钥本身 ✗**）"
          % hashlib.sha256(key).hexdigest()[:8])
    if need and len(key) != 32:
        print("FAIL 钥长不是 32（%d）" % len(key))
        return None
    return key


def step_config_hmac(se, args, cfg=None):
    """把 HMAC 槽（slot 4）的 `WriteConfig` 由出厂值改成“明文可写”。

    ⚠ **只在配置区未锁时可行** ✓（锁了就永远改不了 ✗）。改动是**可重复**的（未锁时可覆盖 ✓）。"""
    print("  ⚙ 改 slot %d 的 WriteConfig：%d → %d" % (HMAC_SLOT, HMAC_WRITECONFIG_FACTORY,
                                                   HMAC_WRITECONFIG_CLEAR))
    # ★ 看**当前状态**（不赌快照 ✗）：本步可能在同一次 `all` 里被“续跑”，传下来的 cfg 可能旧 ✓
    cfg, note = se.read_config()
    if cfg is None:
        print("FAIL 读配置失败：%s" % note)
        return 2, None
    off = HMAC_SLOT_BYTE
    word = cfg[off] | (cfg[off + 1] << 8)
    wc = (word & HMAC_WC_MASK) >> HMAC_WC_SHIFT
    if wc == HMAC_WRITECONFIG_CLEAR:
        # ★ **幂等**（必须先判它 ✓）：已经放开了（哪怕配置区后来锁了 ✓）⇒ 本步无事可做 ✓ ——
        #   否则“已锁 + 已完成”的片子在续跑/重跑 `all` 时会被误判成 FAIL ✗。
        print("PASS slot %d 已经是明文可写（WriteConfig = %d）⇒ 不必改 ✓" % (HMAC_SLOT, wc))
        return 0, cfg
    lv, lc = lock_state(cfg)
    if lc != 0x55:
        print("FAIL 配置区已经锁了（LockConfig = 0x%02X）⇒ 改不了 ✗" % lc)
        print("     （这一步必须赶在 `lock-config` **之前** ✓；已锁的片子只能重换一颗 ✗）")
        return 2, cfg
    ref = bytes.fromhex(REF_CONFIG_HEX)
    # ⚠ 比对时**排掉本步要管的那两个字节**（`HMAC_SLOT_BYTE`/`+1` ✗）—— 它们已经被我们改成
    #   “明文可写”了 ⇒ 拿出厂参照去比只会把**我们自己改对的地方**报成不一致 ✗（续跑时必踩 ✓）。
    #   这两个字节的合法性由**上面那条 wc 检查 + 下面的读回不变量**守 ✓（不是不判 ✗）。
    diff = [i for i in cfg_func_diff(cfg) if i not in (off, off + 1)]
    if diff:
        print("FAIL 功能位段与参照不一致（%d 处，如 %s）⇒ 先停下 ✗（不改来历不明的片子）"
              % (len(diff), ", ".join("cfg[%d] %02X→%02X" % (i, ref[i], cfg[i]) for i in diff[:4])))
        return 2, cfg
    if wc != HMAC_WRITECONFIG_FACTORY:
        print("FAIL slot %d 的 WriteConfig = %d（既非出厂 %d 也非目标 %d）⇒ 先停下问人 ✗"
              % (HMAC_SLOT, wc, HMAC_WRITECONFIG_FACTORY, HMAC_WRITECONFIG_CLEAR))
        return 2, cfg
    new_word = (word & ~HMAC_WC_MASK & 0xFFFF) | (HMAC_WRITECONFIG_CLEAR << HMAC_WC_SHIFT)
    new4 = bytes([new_word & 0xFF, (new_word >> 8) & 0xFF, cfg[off + 2], cfg[off + 3]])
    print("  config 字 %d（byte %d）0x%04X → 0x%04X ⇒ 写 4 B %s"
          % (HMAC_CFG_WORD, off, word, new_word, hexs(new4)))
    _, note = se.write_word_config(HMAC_CFG_WORD, new4)
    print("  Write(config 字 %d) → %s" % (HMAC_CFG_WORD, note))
    cfg2, _ = se.read_config()
    if cfg2 is None:
        print("FAIL 写后读回失败")
        return 2, cfg
    changed = [i for i in range(128) if cfg2[i] != cfg[i]]
    # ⚠ 不变量 = “只允许 byte off/off+1 里变”（哪几个真变看数据 ✓）—— 不是“必须变两个” ✗
    if not set(changed) <= {off, off + 1}:
        print("FAIL 写后差异不干净：%s（只应涉及 byte %d/%d）" % (changed, off, off + 1))
        return 2, cfg2
    wc2 = (cfg2[off] | (cfg2[off + 1] << 8) & 0xFFFF) & HMAC_WC_MASK
    if (wc2 >> HMAC_WC_SHIFT) != HMAC_WRITECONFIG_CLEAR:
        print("FAIL 读回 WriteConfig = %d（应为 %d）" % (wc2 >> HMAC_WC_SHIFT, HMAC_WRITECONFIG_CLEAR))
        return 2, cfg2
    print("PASS slot %d WriteConfig = %d 已生效 ✓（读回只差那两个字节 ✓）"
          % (HMAC_SLOT, HMAC_WRITECONFIG_CLEAR))
    return 0, cfg2


def step_write_hmac(se, args, cfg=None):
    """把 32 B 降级 HMAC 钥**明文写**进 slot 4（数据区未锁 + WriteConfig 已放开 ✓）。

    ⚠ 数据区未锁时**只允许 32 B 写** ✓（原厂 `calib_write_bytes_zone()` 的 docstring 原话），
      所以这里必须整块写、不能按字写 ✗。
    ⚠★ **必须在 `lock-config` 之后** ✗：未锁配置时器件直接回 `0x0F cannot execute` ✓
      （2026-10-11 实测：slot 4 与 slot 8 **都**被拒 ⇒ 与槽无关 ✓，见 §8.18）⇒ 先在这里拦下 ✓。"""
    # ★ 看**当前状态**（自己重读一遍 ✓）：调用方传下来的 cfg 可能是**锁之前**的快照 ✗
    #   （2026-10-11 `all` 实测：`lock-config` 之后仍传旧快照 ⇒ 守卫误报、把好片子拦下 ✓）。
    cfg, note = se.read_config()
    if cfg is None:
        print("FAIL 读配置失败：%s" % note)
        return 2, None
    lv, lc = lock_state(cfg)
    if lc != 0x00:
        print("FAIL 配置区**还没锁**（LockConfig = 0x%02X）⇒ 现在写数据槽器件会拒 ✗" % lc)
        print("     实测（2026-10-11）：`Write` 到数据槽回 `0x0F cannot execute` ✓（§8.18）")
        print("     ⇒ 顺序应是 `config-hmac` → **`lock-config`** → `write-hmac` ✓"
              "（未锁配置时器件拒一切数据区写 ✓）")
        return 2, cfg
    print("  ✎ 把 32 B HMAC 钥写进 slot %d（param1 = 0x%02X、param2 = 0x%02X ✓）"
          % (HMAC_SLOT, ZONE32_DATA, HMAC_SLOT << 3))
    key = load_hmac_key(args)
    if key is None:
        return 2, cfg
    resp, note = se.write_block(ZONE32_DATA, HMAC_SLOT << 3, key)
    if resp is None:
        print("FAIL 写 slot %d 失败：%s" % (HMAC_SLOT, note))
        return 2, cfg
    print("  器件回：%s ⇒ %s" % (hexs(resp), note))
    print("PASS 写命令被器件接受（状态 0x00 ✓）")
    print("     ⚠ 这还不是“钥对”的证明 ✗ ⇒ 跟一条 `hmac-verify`（在器件里算一次、与软件对拍 ✓）")
    return 0, cfg


def step_hmac_verify(se, args, cfg=None):
    """★ 判据 = **器件算的 HMAC 与软件 HMAC 逐字节相同** ✓。

    为什么只能这样验 ✗：slot %d 是 `IsSecret=1` ⇒ **读不出来** ✗（原厂也没有“读密钥”的命令 ✓）。
    所以“钥真写进去了”的唯一证据就是：拿它算一次、跟软件算的比 ✓（两边算不出来就说明写错了/没写 ✓）。"""
    print("  ▶ 在器件里用 slot %d 算 HMAC-SHA256('%s')，再与软件对拍"
          % (HMAC_SLOT, SELFTEST_MSG.decode("ascii")))
    key = load_hmac_key(args)
    if key is None:
        return 2, cfg
    want = hmac.new(key, SELFTEST_MSG, hashlib.sha256).digest()
    got, note = se.sha_hmac(HMAC_SLOT, SELFTEST_MSG)
    if got is None:
        print("FAIL 器件内 HMAC 失败：%s" % note)
        return 2, cfg
    print("  器件：%s" % hexs(got))
    print("  软件：%s" % hexs(want))
    if got != want:
        print("FAIL 两者**不同** ✗ ⇒ slot %d 里的钥不是这一把（或 HMAC 命令档写错了）"
              % HMAC_SLOT)
        print("     ⇒ **别往下锁** ✗：先把钥重写一遍再跑本条 ✓")
        return 2, cfg
    print("PASS 器件内 HMAC = 软件 HMAC ✓ ⇒ slot %d 里确实是我们写进去的那把钥 ✓" % HMAC_SLOT)
    return 0, cfg


def step_hmac_readonly(se, args, cfg=None):
    """★ **只读半证**（2026-10-11 用户选 B）：**不往芯片写任何字节**，只验“HMAC 命令序列”对不对。

    为什么值得单列 ✗：`Se.sha_hmac()` 的三条命令（`SHA HMAC_START` → `UPDATE` → `608 HMAC_END`）
    是**照原厂源码写的、到现在还没在硬件上跑过** ✗（`hmac-verify` 得先把钥写进槽 ⇒ 只能在新片上跑）。
    而**已锁的片子**写不了：数据区锁了 ✗、配置区也锁了 ⇒ 连 `WriteConfig` 都改不了 ✗
    ⇒ 换一条**不需要写**的路：

      ① 挑一个**可读**的槽（默认 slot 8：出厂 `SlotConfig = 0x0000` ⇒ `IsSecret=0`/`ReadKey=0` ✓），
         把它的 32 B **读出来**当“已知钥” ✓；
      ② 在**器件里**算 HMAC（钥不出芯片 ✓）；
      ③ PC 侧用**同一把钥**算 HMAC（标准库 `hmac` ✓）⇒ **逐字节比对** ✓；
      ④ 两条消息各跑一遍：**23 B**（只走 START+END）与 **100 B**（多走一轮 `HMAC_UPDATE` ✓）。

    过了就证明：命令档 / 包形状 / `param2 = 槽号` / 多块 UPDATE / END 的取法**都对** ✓
    —— 而这几条恰好是**引原厂源码、未经硬件验证**的部分 ✗ ⇒ 本条正好把它们钉住 ✓。

    ⚠ 只允许读 **`IsSecret = 0` 且 `ReadKey = 0`** 的槽 ✓（私钥槽一律拒 ✗ —— 别把钥读出来 ✗）。
    ⚠ 判据**不是**“器件回了 32 B” ✗，而是**与软件 HMAC 逐字节相同** ✓。
    """
    slot = args.slot
    if cfg is None:
        cfg, note = se.read_config()
        if cfg is None:
            print("FAIL 读配置失败：%s" % note)
            return 2, None
    sc = slotcfg_decode(cfg, slot)
    if sc is None:
        print("FAIL slot 越界（%d）" % slot)
        return 2, cfg
    print("  slot %d 的 SlotConfig[byte %d..%d] = 0x%04X ⇒ ReadKey=%d IsSecret=%d NoMac=%d "
          "LimitedUse=%d EncryptRead=%d WriteKey=%d WriteConfig=%d"
          % (slot, sc["off"], sc["off"] + 1, sc["word"], sc["ReadKey"], sc["IsSecret"],
             sc["NoMac"], sc["LimitedUse"], sc["EncryptRead"], sc["WriteKey"], sc["WriteConfig"]))
    if sc["IsSecret"] or sc["ReadKey"] != 0:
        print("FAIL 这个槽**不允诺明文读**（IsSecret=%d / ReadKey=%d）⇒ 拒 ✗（别把私钥读出来）"
              % (sc["IsSecret"], sc["ReadKey"]))
        print("     ⇒ 换一个 `IsSecret=0 且 ReadKey=0` 的槽（出厂那颗上 slot 8..14 都是 ✓）")
        return 2, cfg
    key, note = se.read_slot(slot)
    if key is None:
        print("FAIL 读 slot %d 失败：%s" % (slot, note))
        return 2, cfg
    print("  槽内 32 B 已读出（%s）—— 指纹 %s…（**不打印钥本身 ✗**）"
          % (note, hashlib.sha256(key).hexdigest()[:8]))
    for name, msg in (("23 B（只走 START+END）", SELFTEST_MSG),
                      ("100 B（多走一轮 HMAC_UPDATE）", HMAC_RO_MSG2)):
        got, note = se.sha_hmac(slot, msg)
        if got is None:
            print("FAIL 器件内 HMAC 失败（%s）：%s" % (name, note))
            return 2, cfg
        want = hmac.new(key, msg, hashlib.sha256).digest()
        if got != want:
            print("FAIL %s：器件 %s" % (name, hexs(got)))
            print("                   软件 %s" % hexs(want))
            return 2, cfg
        print("PASS %s：器件内 HMAC = 软件 HMAC ✓（%s…）" % (name, hexs(got[:8])))
    print("⇒ **只读半证通过** ✓：HMAC 命令序列（START / UPDATE / END）在这台台上**可用** ✓"
          "（本步**一个字节也没写进芯片** ✓）")
    return 0, cfg


# ★★ 步骤顺序 = **硬约束**（2026-10-11 实测把错误顺序撞出来 ✗，见 §8.18）：
#   ① `config-hmac` 必须**在 `lock-config` 之前**（配置区一锁 ⇒ `WriteConfig` 永远改不了 ✗）；
#   ② 而**一切数据区写**（`write-hmac` / `genkey`）必须**在 `lock-config` 之后** ✗ ——
#      未锁配置时器件回 `0x0F cannot execute` ✓（实测：写 slot 4 与 slot 8 **都**被拒 ⇒
#      与“哪个槽/IsSecret”无关 ✗，是器件状态不允许 ✓）；
#   ③ `write-hmac` 再必须**在 `lock-data` 之前**（数据区一锁 ⇒ 明文写不进去 ✗）。
STEPS = ("read", "config-hmac", "lock-config", "write-hmac", "hmac-verify",
         "genkey", "lock-data", "verify")


# ★ `hmac-readonly` **故意不放进 `STEPS`** ✗ —— 它是“已锁的片子也能跑”的**只读**验证，
#   不属于产线顺序（`all` 里插它会改变 `all` 的行为 ✗）。用法单独给 ✓。
EXTRA_STEPS = ("hmac-readonly",)

# ---- HMAC 槽的字节位置（★ 不是猜的：由原厂结构体的字段顺序**推**、并与它自己的解码结果对账 ✓）----
#   `Atecc608Config` 字段顺序 ⇒ SlotConfig 从 **byte 20** 开始、每槽占 **2 B**、小端：
#     SN03(4) RevNum(4) SN48(5) AES(1) I2C_En(1) Rsvd(1) I2C_Addr(1) Rsvd(1) CountMatch(1)
#     ChipMode(1) ⇒ 共 20 ✓；旁证：原厂写计数器用的字节偏移正是 `52 + i*8` = 20 + 16×2 ✓✓。
#   16 位字内的位段（ctypes 位域，低位在先）：ReadKey(0-3) NoMac(4) LimitedUse(5)
#     EncryptRead(6) IsSecret(7) WriteKey(8-11) **WriteConfig(12-15)** ✓
#   对账（实测出厂 128 B ✓）：slot 0 = `83 20`(LE 0x2083) ⇒ ReadKey=3 IsSecret=1 WriteConfig=2 ✓
#     与解码表一致 ✓；slot 4 = `8F 8F`(LE 0x8F8F) ⇒ ReadKey=15 WriteKey=15 **WriteConfig=8** ✓✓
HMAC_SLOT_BYTE = 20 + 2 * HMAC_SLOT          # = 28
HMAC_CFG_WORD = HMAC_SLOT_BYTE // 4          # = 7（= block 0 里的第 7 个字 ✓）
HMAC_WC_MASK = 0xF000                        # WriteConfig 占高 4 位 ✓
HMAC_WC_SHIFT = 12

# 台架**实测**的那条摘要 = SHA-256(SELFTEST_MSG)（2026-10-11，见 §8.10）
REF_DIGEST = hashlib.sha256(SELFTEST_MSG).hexdigest()


def selftest():
    """离线自检：把**台架实测的包字节**当锚点，验本工具的组包/CRC（不碰硬件 ✓）。

    锚点全是 2026-10-11 用**原厂库**驱动时**原厂栈实发**的字节（§8.10/§8.12）⇒ 不是我们自己说的 ✓。
    ⚠ 这几条同时把“我抄错一个字节”钉住 ✓：包尾那 2 字节 CRC 就是整包的校验和。
    ★ 没实测过的包**不写成锚点** ✗ —— 它们的值只能靠**引原厂源码**（包字节仍是没见过的）：
      `Lock(Data)` 的 param1 = `0x81`（`calib_lock.c` + `calib_command.h`，见文件头常量处 ✓）。
      这类“引证值”在**首片上机**时第一次真发出去 —— 判据是**读回锁状态**（`LockValue` 变 0x00 ✓）
      与 `verify` 里的“锁后重算公钥 = 登记公钥” ✓，不是“包长得对” ✗。"""
    anchors = [
        ("Lock(Config)", ci.atecc_pkt(OP_CMD, LOCK_ZONE_CONFIG, 0, True), "0717800000398d"),
        ("GetPubKey(slot0)", ci.atecc_pkt(OP_GENKEY, GENKEY_MODE_PUBLIC, 0, True), "07400000000005"),
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
    # ★★ 配置补丁的**数值真值校验**（纯离线 ✓，不碰芯片 ✓）：
    #   拿 §8.4 记录的出厂 128 B，按 `config-hmac` 同一套算法打补丁 ⇒ 要求
    #     ① 只有 byte 28/29 两个字节变 ✓；② 位段只有 WriteConfig 4→0、其余一个不变 ✓。
    #   有原厂结构体时**再用它解一遍**做第二声音 ✓（没装就跳过、并说明 ✓）。
    cfg = bytearray(bytes.fromhex(REF_CONFIG_HEX))
    off, wc_shift, wc_mask = HMAC_SLOT_BYTE, HMAC_WC_SHIFT, HMAC_WC_MASK

    def _fields(w):
        return {"ReadKey": w & 0xF, "NoMac": (w >> 4) & 1, "LimitedUse": (w >> 5) & 1,
                "EncryptRead": (w >> 6) & 1, "IsSecret": (w >> 7) & 1,
                "WriteKey": (w >> 8) & 0xF, "WriteConfig": (w >> wc_shift) & 0xF}

    w_old = cfg[off] | (cfg[off + 1] << 8)
    w_new = (w_old & ~wc_mask & 0xFFFF) | (HMAC_WRITECONFIG_CLEAR << wc_shift)
    chk = bytearray(cfg)
    chk[off], chk[off + 1] = w_new & 0xFF, (w_new >> 8) & 0xFF
    diff = [i for i in range(128) if chk[i] != cfg[i]]
    fo, fn = _fields(w_old), _fields(w_new)
    # ⚠ 别赌“变了两个字节”✗：0x8F8F→0x0F8F 只有**高字节**变（低字节一样 ✓）⇒
    #   不变量 = “只允许 byte off/off+1 里变” **加** “word 值对” ✓（不是“必须变两个”✗）。
    ok = (w_old == 0x8F8F and w_new == 0x0F8F and set(diff) <= {off, off + 1}
          and fo["WriteConfig"] == HMAC_WRITECONFIG_FACTORY and fn["WriteConfig"] == HMAC_WRITECONFIG_CLEAR
          and all(fn[k] == fo[k] for k in fo if k != "WriteConfig"))
    print("  %s 配置补丁：word@%d 0x%04X→0x%04X（diff=%s）⇒ WriteConfig %d→%d，其余位段 %s"
          % ("ok  " if ok else "BAD ", off, w_old, w_new, diff,
             fo["WriteConfig"], fn["WriteConfig"],
             "全不变 ✓" if all(fn[k] == fo[k] for k in fo if k != "WriteConfig") else "**变了** ✗"))
    bad += 0 if ok else 1
    try:
        if not HAVE_DEV:
            raise ImportError
        from cryptoauthlib.device import Atecc608Config as _Cfg      # 本机参照，不入库 ✓
        before = _Cfg.from_buffer_copy(bytes(cfg))
        after = _Cfg.from_buffer_copy(bytes(chk))
        v_ok = (before.SlotConfig[HMAC_SLOT].WriteConfig == HMAC_WRITECONFIG_FACTORY
                and after.SlotConfig[HMAC_SLOT].WriteConfig == HMAC_WRITECONFIG_CLEAR)
        v_ok = v_ok and all(
            getattr(after.SlotConfig[i], f) == getattr(before.SlotConfig[i], f)
            for i in range(16) for f in ("ReadKey", "NoMac", "LimitedUse", "EncryptRead",
                                         "IsSecret", "WriteKey")
            if not (i == HMAC_SLOT))
        print("  %s 原厂结构体复核（slot4.WriteConfig %d→%d，其余槽全部位段不变）"
              % ("ok  " if v_ok else "BAD ",
                 before.SlotConfig[HMAC_SLOT].WriteConfig, after.SlotConfig[HMAC_SLOT].WriteConfig))
        bad += 0 if v_ok else 1
    except Exception:                                                # noqa: BLE001
        print("  --  原厂结构体复核：本机没装 `cryptoauthlib` ⇒ 跳过（只跑了上面那条自算的 ✓）")
    if bad:
        print("FAIL 产线工具离线自检：%d 项不符" % bad)
        return 2
    print("PASS 产线工具离线自检：组包/CRC 与台架实测字节一致（%d 条锚点）" % len(anchors))
    return 0


def main():
    ap = argparse.ArgumentParser(description="ATECC608B 产线流程（读配置→锁Config→GenKey→锁Data→复验）")
    ap.add_argument("step", nargs="?", choices=list(STEPS) + list(EXTRA_STEPS) + ["all"],
                    help="要跑哪一步；只跑离线自检时省略它 ✓（`hmac-readonly` = 只读半证，已锁的片也能跑 ✓）")
    ap.add_argument("--selftest", action="store_true", help="离线自检组包/CRC（不碰硬件）")
    ap.add_argument("--yes", action="store_true", help="不可逆步骤（lock-config / lock-data / genkey / all）必须显式给")
    ap.add_argument("--index", type=int, default=0, help="CH347 的 iIndex（缺省 0）")
    ap.add_argument("--clk", type=int, default=1, help="I²C 时钟：0=20k 1=100k(缺省) 2=400k 3=750k")
    ap.add_argument("--addr", default="%02x" % ADDR_DEFAULT, help="SE 的 7 位地址（hex，缺省 60）")
    ap.add_argument("--no-crc", action="store_true", help="命令包不挂 CRC（**默认挂**：实测这颗要挂 ✓）")
    ap.add_argument("--record", help="把结果（公钥/锁状态/时间）**追加**写进这个文件（产线台账 ✓）")
    ap.add_argument("--hmac-key-file", help="32 B 降级 HMAC 钥（hex 文本或原始 32 B；**不入库 ✗**）")
    ap.add_argument("--slot", type=int, default=HMAC_RO_SLOT,
                    help="`hmac-readonly` 用哪个槽（缺省 %d：出厂那颗上它是可读的 ✓）" % HMAC_RO_SLOT)
    ap.add_argument("--pub-hex",
                    help="登记在案的公钥（128 位 hex = X||Y 各 32 B）—— 给了就**单独跑 `verify` 也能比对** ✓"
                         "（不给就只能“报公钥”、比不了 ✗）")
    ap.add_argument("--gen-hmac-key", action="store_true",
                    help="密钥文件不存在时就地生成 32 B 随机并写进它（首次上机用 ✓）")
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
    print("⚠ 本步**不探地址**（用固定等待 ✓）：2026-10-11 实测，把“探地址”当就绪判据会把"
          "CH347 的 I²C 引擎搞死 ✗（详见 docs/atecc608b-se.md §8.16）")
    print("⚠ 万一卡死（连 `0x50` 都不应答）：**拔插一次 CH347 的 USB** —— 软重开会话救不回来 ✗")

    try:
        se = Se(args.index, args.clk, addr, with_crc=not args.no_crc)
    except ci.cs.Ch347Error as exc:
        print("FAIL CH347 打不开（iIndex=%d）：%s" % (args.index, exc))
        print("⇒ 先跑 `python tools\\ch347_i2c.py list` 看它在不在 ✗")
        return 2
    rc, cfg, pub = 0, None, None
    if getattr(args, "pub_hex", None):
        # ★ 让**单独跑 `verify`** 也能做“与登记公钥对比”那条判据 ✓
        #   （不然只有 `all`（`genkey` → … → `verify` 同一进程）才带得上那个对照 ✗ —— 2026-10-11 实测踩到）
        hx = "".join(c for c in args.pub_hex if c in "0123456789abcdefABCDEF")
        if len(hx) != 128:
            print("FAIL `--pub-hex` 不是 128 位 hex（应为 X||Y 各 32 B = 64 B）")
            return 1
        pub = bytes.fromhex(hx)
        print("登记公钥（由 `--pub-hex` 给）：%s…" % hx[:32])
    try:
        ok, four = se.wake()
        print("唤醒：%s（读到 %s%s）" % ("拿到 04 11 33 43 ✓" if ok else "没拿到唤醒应答（器件可能本来就醒着 ✓ 或没接 ✗）",
                                        hexs(four),
                                        "" if ok or four[:4] not in (SELFTEST_FAIL,) else " ← 自检失败 ✗"))
        if four[:4] == SELFTEST_FAIL:
            print("FAIL 器件回了自检失败应答 ⇒ 查供电/器件（先别动锁 ✗）")
            return 2

        if args.step == "all":
            for s in STEPS:
                print("-" * 78)
                print("▶ %s" % s)
                if s == "read":
                    rc, cfg = step_read(se, args)
                elif s == "config-hmac":
                    rc, cfg = step_config_hmac(se, args, cfg)
                elif s == "write-hmac":
                    rc, cfg = step_write_hmac(se, args, cfg)
                elif s == "hmac-verify":
                    rc, cfg = step_hmac_verify(se, args, cfg)
                elif s == "lock-config":
                    rc, cfg = step_lock_config(se, args, cfg)
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
        elif args.step == "config-hmac":
            rc, cfg = step_config_hmac(se, args, cfg)
        elif args.step == "write-hmac":
            rc, cfg = step_write_hmac(se, args, cfg)
        elif args.step == "hmac-verify":
            rc, cfg = step_hmac_verify(se, args, cfg)
        elif args.step == "hmac-readonly":
            rc, cfg = step_hmac_readonly(se, args, cfg)
        elif args.step == "lock-config":
            rc, cfg = step_lock_config(se, args, cfg)
        elif args.step == "genkey":
            rc, pub = step_genkey(se, args)
        elif args.step == "lock-data":
            rc = step_lock_data(se, args, cfg)
        else:
            rc, pub = step_verify(se, args, pub)
    except ci.cs.Ch347Error as exc:
        # ★ 实测踩过（2026-10-11）：总线上没有器件应答时 DLL 只报 "CH347StreamI2C 失败" ✗ ——
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
    if se.reopens:
        print("（传输层：中途重开 CH347 会话 %d 次 ✓ —— 2026-10-11 实测：引擎卡住后**只有重开会话能救** ✗）"
              % se.reopens)
    if args.record and rc == 0:
        # ★ 台账只记**指纹**（HMAC 钥是密钥材料 ⇒ 不许写进文件 ✗；§2「仓库里绝不放真实私钥/密钥文件」同理 ✓）
        fp = "-"
        if args.step in ("write-hmac", "hmac-verify", "all"):
            try:
                k = load_hmac_key(args, need=False)
                fp = hashlib.sha256(k).hexdigest()[:8] if k else "-"
            except Exception:                                        # noqa: BLE001
                fp = "-"
        line = "%s\taddr=0x%02X\tstep=%s\tpubkey_hex=%s	hmac_fp=%s\n" % (
            datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), addr, args.step,
            pub.hex() if pub else "-", fp)
        with open(args.record, "a", encoding="utf-8", newline="\n") as f:
            f.write(line)
        print("台账已追加：%s" % args.record)
    return rc


if __name__ == "__main__":
    sys.exit(main())
