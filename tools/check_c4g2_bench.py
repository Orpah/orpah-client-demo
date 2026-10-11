# -*- coding: utf-8 -*-
"""check_c4g2_bench.py — c4-γ-2 **上机判据**的一键跑法（PC 侧）。

判的是「nonce 真的来自 ATECC608B」这一件事，全程用**实际字节**，不靠固件自报的结论：
  1. 打开 nano 的**控制台串口**（默认 COM23，115200 8N1）；
  2. 探活：`AT` → `OK`（不靠横幅 —— 板子已跑一会儿的话横幅早过去了，同 γ-1 口径）；
  3. 发 `se`（固件里的 `atecc_selftest()`）⇒ 看四件事：
       · `[se] 唤醒判据 … ：是 ✓`     —— 读回 4 字节 = `04 11 33 43`
         ⚠ 报 `不是 ✗` **不等于**硬件坏：器件**本来就醒着**时它必然不是
         （读到的是上一条响应的 count），以**随后的命令结果**为准 —— 见下面的判据 1
       · `[se] cmd=… crc_mode=…`     —— ★ **本轮实测**：线上要不要挂 CRC（两条待确认假设之一）
       · `[se] Random(0x1B) 32 B ok; 前 16 B = <32hex>`
       · 失败时把 `rc / last_resp_len / raw=` 原样带出来（是 `count` 口径不对还是别的问题）
  4. **再发一次 `se`** ⇒ 两次的 16 B **必须不同**（RNG 真的在动，不是常量/缓存）；
  5. 发 `id` ⇒ `nonce_src=se`（装配进流水线的那条路径确实在用 SE；`soft` 就是没接上）；
  6. ★ d 步（2026-10-11）：`se` 会额外对一条**固定摘要**签一次并打出 `r||s` ⇒
     本工具用 **Slot 0 的公钥**（下面常量，来自 2026-10-11 台架实测）**独立验签** ✓
     （板上**不**自己验自己：那只证明自洽 ✗）；同时判 `id` 的 `sign_src=se` ✓。
  6. （默认做）发 `idsend` + `idhex` ⇒ 把帧交给**上游** `orpah_id.verify_report` 判一次
     （与 `tools/check_report_hex.py` / `tools/check_c4g1_bench.py` **共用同一套判定**，不重写协议）；
     可选 `--mod-port COM24` 同时抓模组打印口，确认帧进了模组。

判据（退出码）：**0 = 全过**；2 = 有 FAIL（信息在输出里）；1 = 用法/环境问题（串口打不开、
              找不到上游参考实现）。

跑完请把 §6 那张表（`docs/atecc608b-se.md`）里的「实测」列填上 —— 尤其是 `crc_mode`：
它是两条**待上机确认**假设之一的答案，填上以后就不用再判了。

用法：
    python tools\\check_c4g2_bench.py                      # COM23 + 模组口 COM24，全流程
    python tools\\check_c4g2_bench.py --port COM7
    python tools\\check_c4g2_bench.py --no-idsend          # 只判 SE（不碰 ID 上报）
    python tools\\check_c4g2_bench.py --from-file log.txt  # 离线解析一段控制台日志
    python tools\\check_c4g2_bench.py --from-keystore       # ★ 用产线登记的那把钥验帧（HS256 也能判 ✓）
    python tools\\check_c4g2_bench.py --keep-open          # 只 dump 原始输出，不判定

⚠ **串口独占**：Windows 上一个口只能被一个进程打开 ⇒ 先关掉 WindTerm / SecureCRT /
   MounRiver 的串口窗口（否则 `PermissionError(13)`）。
"""
import argparse
import hashlib
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from check_report_hex import load_upstream, find_upstream, judge, strip_hex  # noqa: E402  （单一源：解析/判定）

BAUD = 115200
# ⚠ `crc_mode=` 后面那串**含空格**（`crc(响应 35 B)`）⇒ 不能用 \S+
# ★ 2026-10-11：唤醒判据**两种格式都要认** —— 固件 `e0c1483` 起改成"读 4 字节比对"
#   （照 CryptoAuthLib `hal_check_wake()`）⇒ 打印格式也变了；只认旧格式会把"全过"
#   误报成 `FAIL … `se` 未输出` ✗（2026-10-11 实测踩过：五条判据全过、却被旧正则判失败）。
RE_WAKE = re.compile(r"\[se\]\s*唤醒判据\s*=[^：]*：\s*(是|不是)")
RE_WAKE_OLD = re.compile(r"\[se\]\s+wake:\s*(ACK|NO ACK)")
RE_STATS = re.compile(r"\[se\]\s+cmd=(\d+)\s+ok=(\d+)\s+fail=(\d+)\s+crc_mode=(.*?)"
                      r"(?:\s+resp_waddr=(\S+))?\s*$")
RE_RAND = re.compile(r"\[se\]\s+Random\(0x1B\)\s+32\s*B\s*ok;\s*前\s*16\s*B\s*=\s*([0-9a-fA-F]{32})")
RE_SEFAIL = re.compile(r"\[se\]\s+Random FAILED\s+rc=(-?\d+)\s+last_resp_len=(\d+)\s+raw=([0-9a-fA-F]*)")
RE_SENORES = re.compile(r"\[se\]\s*没应答")
RE_NONCESRC = re.compile(r"\[id\]\s+nonce_src=(\S+)")
# ★ d 步（2026-10-11）：`id` 现在把两个来源写在同一行（`nonce_src=se sign_src=se`）
RE_SIGNSRC = re.compile(r"\[id\]\s+nonce_src=(\S+)\s+sign_src=(\S+)")
# `se` 的签名判据：两行（r 在 Sign 行上、s 在下一行）
RE_SE_SIGN = re.compile(r"\[se\]\s+Sign\(slot\s*(\d+)\)\s+ok;\s*r=([0-9a-fA-F]{64})")
RE_SE_S = re.compile(r"\[se\]\s+s=([0-9a-fA-F]{64})")
RE_SE_SIGNCNT = re.compile(r"\[se\]\s+sign_ok=(\d+)\s+sign_fail=(\d+)")
# ★ Slot 0 的公钥（台架 2026-10-11 实测，见 docs/atecc608b-se.md §8.10）——
#   用来**独立验**板上那条签名；换芯片/换钥时这两个常量必须跟着换 ✓
SLOT0_PUB_X = bytes.fromhex("91b25aacf84a88e8b457d04f289cdf9a079f96ff5f4331c5ea382a445cb1ab44")
SLOT0_PUB_Y = bytes.fromhex("657458472353c3be6b4c9ce64257a5eaf09713d655db4776611272980b53f306")
D_SELFTEST_MSG = b"ORPAH d-step probe v1"      # 固件 `se` 自检签的就是它 ✓
RE_LOADLINE = re.compile(r"\[id\]\s+nonce <- (se|soft)", re.IGNORECASE)
RE_HEXLINE = re.compile(r"^[0-9a-fA-F]{2,64}$")
RE_SENT = re.compile(r"\[id\]\s+sent\s+level=(\d+)\s+alg=(\S+)\s+why=(\S+)\s+nonce=(\S+)"
                     r"\s+len=(\d+)\s+build_ms=(\d+)")


def parse_se_log(text):
    """从控制台文本里抽 `se` 的判据字段。**按出现顺序**收：一个 `se` 的完整判据 = 一轮。"""
    out = {"wake": None, "nores": False, "stats": None, "rands": [], "fails": [],
           "nonce_src": None, "loadline": None, "sent": None, "hex": "",
           "sign_src": None, "sig_r": None, "sig_s": None, "signcnt": None,
           "sign_load": None}
    for ln in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        s = ln.strip()
        m = RE_WAKE.search(s)
        if m:
            out["wake"] = m.group(1)            # 是 / 不是（2026-10-11 起的新口径）
        m = RE_WAKE_OLD.search(s)
        if m:
            out["wake"] = m.group(1)            # ACK / NO ACK（旧固件）
        if RE_SENORES.search(s):
            out["nores"] = True
        m = RE_STATS.search(s)
        if m:
            # ★ 2026-10-11：`crc_mode=` 后面同一行还有 ` resp_waddr=0x00` ⇒ **必须非贪婪**
            #   （原来 `(.*)` 一口吞到行尾，把 `resp_waddr` 也算进 crc_mode ⇒ 判据 2 误报 ✗）。
            out["stats"] = {"cmd": int(m.group(1)), "ok": int(m.group(2)),
                            "fail": int(m.group(3)), "crc_mode": m.group(4).strip(),
                            "resp_waddr": m.group(5)}
        m = RE_RAND.search(s)
        if m:
            out["rands"].append(m.group(1).upper())
        m = RE_SEFAIL.search(s)
        if m:
            out["fails"].append({"rc": int(m.group(1)), "len": int(m.group(2)),
                                 "raw": m.group(3)})
        m = RE_NONCESRC.search(s)
        if m:
            out["nonce_src"] = m.group(1)
        m = RE_SIGNSRC.search(s)
        if m:
            out["nonce_src"], out["sign_src"] = m.group(1), m.group(2)
        m = RE_SE_SIGN.search(s)
        if m:
            out["sig_r"] = m.group(2)
        m = RE_SE_S.search(s)
        if m:
            out["sig_s"] = m.group(1)
        m = RE_SE_SIGNCNT.search(s)
        if m:
            out["signcnt"] = {"ok": int(m.group(1)), "fail": int(m.group(2))}
        if "ES256 sign <- ATECC608B" in s:
            out["sign_load"] = "se"
        elif "ES256 sign <- **软件替身**" in s or "ES256 sign <- soft" in s:
            out["sign_load"] = "soft"
        m = RE_LOADLINE.search(s)
        if m:
            out["loadline"] = m.group(1).lower()
        m = RE_SENT.search(s)
        if m:
            out["sent"] = {"level": int(m.group(1)), "alg": m.group(2), "why": m.group(3),
                           "nonce": m.group(4), "len": int(m.group(5)),
                           "build_ms": int(m.group(6))}
        if RE_HEXLINE.match(s):
            out["hex"] += s
    return out


def read_until(ser, pred, timeout, chunk=0.05):
    """读到 pred(text) 为真或超时；返回读到的全部文本（**不猜次数**，按截止时间）。"""
    buf = ""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        data = ser.read(4096)
        if data:
            buf += data.decode("utf-8", "replace")
        if pred(buf):
            return buf
        time.sleep(chunk)
    return buf


_RE_SN = re.compile(r"\[id\]\s*sn=([A-Za-z0-9\-]+)")


def slot0_pub_by_sn(dbp, sn):
    """按 SN 从**真密钥库**里取那颗芯片的 Slot 0 **公钥**（64 B `X||Y`）；取不到回 None。

    ★ 为什么必须按 SN 取、不能只用硬编码常量 ✗（2026-10-12 跑台架闸门才抓到 ✓）：
      `SLOT0_PUB_X/Y` 是**某一颗**量出来的 ⇒ 换一颗就必然 `InvalidSignature`
      —— 那是**尺子错、不是签名错** ✗（与 2026-10-11 `check_report_hex.py` 那个坑同一类）。
    读法 = **直读 `keys` 表的 `pubkey_pem`**（与 `tools/which_chip.py` 同一张表、同一种读法 ✓）；
      用 `public_numbers()` 拼 `X||Y`（**绕开各种序列化枚举** ✓ 版本无尖）。
    """
    if not dbp or not sn:
        return None
    try:
        import sqlite3                                            # noqa: PLC0415

        from cryptography.hazmat.primitives import serialization as _ser   # noqa: PLC0415

        con = sqlite3.connect(dbp)
        try:
            row = con.execute("SELECT pubkey_pem FROM keys WHERE sn=?"
                              " ORDER BY gen DESC LIMIT 1", (sn,)).fetchone()
        finally:
            con.close()
        if not row or not row[0]:
            return None
        n = _ser.load_pem_public_key(row[0].encode("ascii")).public_numbers()
        return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")
    except Exception as e:                                        # noqa: BLE001
        print("warn 从库里取 Slot 0 公钥失败（sn=%s）：%r" % (sn, e))
        return None


def main():
    ap = argparse.ArgumentParser(description="c4-γ-2 上机判据（ATECC608B nonce 后端，PC 侧一键）")
    ap.add_argument("--port", default="COM23", help="固件控制台串口（默认 COM23）")
    ap.add_argument("--mod-port", default="COM24",
                    help="模组 AT/打印口（默认 COM24；打不开就只报「模组侧未确认」，不当失败）")
    ap.add_argument("--no-mod-port", action="store_true", help="不看模组打印口")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--timeout", type=float, default=25.0, help="每一步的等待上限（秒）")
    ap.add_argument("--no-idsend", action="store_true", help="跳过 ID 上报（只判 SE）")
    ap.add_argument("--from-file", help="离线：解析一段控制台日志（不用串口）")
    ap.add_argument("--keep-open", action="store_true", help="只 dump 原始输出，不做判定")
    ap.add_argument("--upstream", help="orpah-over-halow 目录（默认自动找）")
    ap.add_argument("--from-keystore", action="store_true",
                    help="★ 用 orpah-over-halow/orpah.db 里**产线登记**的那把钥验"
                         "（HS256 也能验 ✓）；不传则用硬编码的 Slot 0 公钥常量（只认 ES256）")
    args = ap.parse_args()

    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                       # noqa: BLE001
            pass

    fails = []
    mod_buf = ""
    # ★ 密钥库路径（`--from-keystore` 时）—— **只解析一次**，帧判据与 `se` 判据共用 ✓
    dbp = None
    if args.from_keystore:
        dbp = os.path.join(find_upstream(args.upstream) or "", "orpah.db")
        if not os.path.exists(dbp):
            print("FAIL --from-keystore 要能找到 %s（用 --upstream <dir> 指路）" % dbp)
            return 1
        print("info 用**密钥库**验（产线登记给该 SN 的那把钥 ✓）：%s" % dbp)
    if args.from_file:
        text = open(args.from_file, encoding="utf-8", errors="replace").read()
        print("（离线模式：解析 %s，%d 字符）" % (args.from_file, len(text)))
        t = text
    else:
        try:
            import serial                                           # noqa: PLC0415
        except ImportError:
            print("FAIL 需要 pyserial（用 C:\\Python313\\python.exe，它带 pyserial）")
            return 1
        try:
            ser = serial.Serial(args.port, args.baud, timeout=0.2)
        except Exception as e:                                      # noqa: BLE001
            print("FAIL 打不开 %s：%r" % (args.port, e))
            print("     ⚠ 串口独占：先把 WindTerm/SecureCRT/MounRiver 的串口窗口关掉再跑")
            return 1
        print("已打开 %s @ %d 8N1（固件控制台）" % (args.port, args.baud))
        t = ""
        try:
            # ① 探活：发命令看回答（不靠横幅）
            ser.reset_input_buffer()
            ser.write(b"AT\r\n")
            t += read_until(ser, lambda b: "OK (rx=" in b, 5.0)
            if "OK (rx=" not in t:
                ser.write(b"AT\r\n")
                t += read_until(ser, lambda b: "OK (rx=" in b, 5.0)
            if args.keep_open:
                time.sleep(args.timeout)
                print(ser.read(65536).decode("utf-8", "replace"))
                return 0
            if "OK (rx=" not in t:
                print("FAIL 固件**没回 AT**（timeout 5s×2）—— 板子真的在跑吗？按一次 RST 再看；"
                      "或 `--port` 选错口了")
                print("--- 实际读到 ---\n" + t[-800:])
                return 2
            print("PASS 固件活着（`AT` → `OK`）")
            if "rng=" in t:
                for ln in t.replace("\r", "\n").split("\n"):
                    if "rng=" in ln or "nonce <-" in ln:
                        print("     | " + ln.strip())

            # ② `se` 两次（第二次用来证明 RNG 在动）
            for i in (1, 2):
                ser.write(b"se\r\n")
                chunk = read_until(ser, lambda b: "Random(0x1B) 32 B ok" in b or
                                   "Random FAILED" in b or "没应答" in b, args.timeout)
                t += chunk
                print("（第 %d 次 `se` 已收回）" % i)
                # ★ `se` 的原始输出要**当场打出来**（2026-09-22 加）：
                #   原来只打一句"已收回"，而末尾的"控制台（截尾）"早被模组 chatter 挤掉了 ——
                #   结果是 SE 不工作时我们**看不到** wake 有没有 ACK、rc/last_resp_len/raw 是多少，
                #   而那三个数正是判断"供电 / 接线 / 上拉 / 响应长度"的唯一依据。
                se_lines = [ln.strip() for ln in chunk.replace("\r", "\n").split("\n")
                            if "[se]" in ln]
                if se_lines:
                    for ln in se_lines:
                        print("     | " + ln)
                else:
                    print("     | ⚠ 这一轮没读到带 `[se]` 的行 —— `se` 命令没被识别？"
                          "（固件 `help` 里应有它；看下面的原始输出）")

            # ③ `id` ⇒ nonce_src
            ser.write(b"id\r\n")
            t += read_until(ser, lambda b: "nonce_src=" in b, args.timeout)

            # ④ idsend + idhex（可选）+ 模组打印口
            mod = None
            if args.mod_port and not args.no_mod_port and not args.no_idsend:
                try:
                    mod = serial.Serial(args.mod_port, args.baud, timeout=0.2)
                    mod.reset_input_buffer()
                    print("已打开 %s（模组 AT/打印口）" % args.mod_port)
                except Exception as e:                              # noqa: BLE001
                    print("warn 打不开模组打印口 %s：%r（下面只报内容判据）" % (args.mod_port, e))
                    mod = None
            if not args.no_idsend:
                ser.write(b"idsend\r\n")
                t += read_until(ser, lambda b: "[id] sent" in b or "held (self-limit)" in b,
                                args.timeout)
                if mod is not None:
                    end = time.monotonic() + 3.0
                    while time.monotonic() < end:
                        d = mod.read(4096)
                        if d:
                            mod_buf += d.decode("utf-8", "replace")
                    mod.close()
                ser.write(b"idhex\r\n")
                t += read_until(ser, lambda b: "no frame yet" in b or
                                RE_HEXLINE.search(b) is not None, args.timeout)
        finally:
            ser.close()
        print("--- 控制台（截尾）---")
        print(t[-1200:])

    got = parse_se_log(t)

    # ---- 判据 1：唤醒 ----
    # ★★ 2026-10-11 改口径：这一条**不再单独定罪** ✗。器件**本来就醒着**时（上一条响应还在
    #   它的输出缓冲里）唤醒判据必然是 `不是 ✗` —— 实测：`se` 连按两次，第 2 次打印
    #   `最后一轮读到：23 00 00 00`（`0x23` = 35 = 上一条 `Random` 响应的 count），
    #   **而紧接着的 `Random` 成功** ✓。⇒ 真正的判据 = 下面那条命令的结果。
    w = got["wake"]
    if got["nores"]:
        print("FAIL `se` 报「没应答」⇒ 查三件事：SE 的 3V3/GND 供电、"
              "`PB6↔pin6(SCL)`/`PB7↔pin5(SDA)` 接线、**4.7k 上拉**")
        fails.append("SE 无应答")
    elif w in ("ACK", "是"):
        print("PASS 唤醒判据过了（%s）" % ("读回 04 11 33 43" if w == "是" else "ACK"))
    elif w in ("NO ACK", "不是"):
        if got["rands"]:
            print("warn 唤醒判据没过（%s），但随后的 `Random` 成功 ⇒ **器件本来就醒着**"
                  "（这一条只在「它从睡眠里被叫醒」时才有意义）—— 不算失败" % w)
        else:
            print("FAIL 唤醒判据没过（%s）**且**随后也没取到随机数 ⇒ 查 3V3 供电 / "
                  "`PB6↔pin6(SCL)`/`PB7↔pin5(SDA)` 接线 / 4.7k 上拉" % w)
            fails.append("唤醒判据不过 + 无随机数")
    else:
        print("FAIL 控制台里没看到唤醒判据那一行（`[se] 唤醒判据 …`，或旧格式 `[se] wake: …`）"
              "—— `se` 命令没被识别？看上面原始输出")
        fails.append("`se` 未输出")

    # ---- 判据 2：Random + ★ 实测的 CRC 模式 ----
    st = got["stats"]
    if st:
        print("PASS `se` 统计：cmd=%d ok=%d fail=%d  ★ crc_mode=%s"
              % (st["cmd"], st["ok"], st["fail"], st["crc_mode"]))
        if st.get("resp_waddr"):
            # ★ `resp_waddr` 也该是 `0x00`（`0x03` = 旧写法兜底通了）—— §6 表第 2b 行。
            if st["resp_waddr"] == "0x00":
                print("PASS 响应字地址 resp_waddr=0x00（公开实现里的正确值）")
            else:
                print("warn resp_waddr=%s（不是 0x00 ⇒ 看 §3b 那条推断）" % st["resp_waddr"])
        # ★ 2026-10-11：口径是 `no-crc(响应 33 B)` / `crc(响应 35 B)` —— count 只占 1 字节
        #   （32 B 数据 + 2 B CRC）⇒ 旧的 36/38 是"4 字节大端 count"那版**已作废** ✗。
        if st["crc_mode"] not in ("no-crc(响应 33 B)", "crc(响应 35 B)"):
            print("warn crc_mode 不是 33/35 B 两种之一（=%s）⇒ 看下面的 FAILED 行"
                  % st["crc_mode"])
    if got["fails"]:
        for f in got["fails"]:
            print("FAIL Random 失败：rc=%d last_resp_len=%d raw=%s"
                  % (f["rc"], f["len"], f["raw"] or "(空)"))
            print("     ⇒ 若 len 不是 33/35：把 raw 贴进 docs/atecc608b-se.md §4，"
                  "按实测改 `ATECC_RESP_LEN_*`（**就一处**；别连试好几轮）")
        fails.append("Random 失败")
    if len(got["rands"]) >= 1:
        print("PASS 取到 32 B 随机数；前 16 B = %s" % got["rands"][0])
    else:
        if not got["fails"] and not got["nores"]:
            print("FAIL 没看到 `[se] Random(0x1B) 32 B ok; 前 16 B = …`")
            fails.append("没拿到随机数")

    # ---- 判据 3：两次必须不同（RNG 在动）----
    if len(got["rands"]) >= 2:
        if got["rands"][0] == got["rands"][1]:
            print("FAIL 两次 `se` 的 16 B **完全相同** ⇒ 不是真的随机"
                  "（缓存/常量/口没通）—— 这比“取不到”更该查")
            fails.append("两次随机数相同")
        else:
            print("PASS 两次 `se` 的 16 B 不同（RNG 在动）：%s / %s"
                  % (got["rands"][0][:16] + "…", got["rands"][1][:16] + "…"))
    elif len(got["rands"]) == 1 and not args.no_idsend:
        print("info 只收到一次 `se` 的输出（第二次可能被 echo/缓冲吃掉）—— 不算失败")

    # ---- 判据 4：装配路径确实在用 SE ----
    if got["nonce_src"] == "se":
        print("PASS `id` 显示 nonce_src=se（流水线取的是 SE 的随机数）")
    elif got["nonce_src"] == "soft":
        print("FAIL `id` 显示 nonce_src=soft ⇒ 没接上或没探到，**如实回退**了软熵"
              "（规范 §5.4 要求 SE 的 RNG）")
        fails.append("nonce_src=soft")
    elif got["nonce_src"] is None:
        print("warn 没看到 `[id] nonce_src=…`（`id` 没输出？）")
    else:
        print("warn `nonce_src=%s`（不是 se/soft？看原始输出）" % got["nonce_src"])
    if got["loadline"] == "soft" and got["nonce_src"] != "soft":
        print("warn 启动时那行 `[id] nonce <- soft` 与 `id` 不一致 —— 看原始输出")

    # ---- 判据 4b（★ d 步）：签名器装的是 SE，而且**外部实现**认它签的东西 ----
    # ⚠ 板上还是 d 步之前的旧固件时（两行都没有）⇒ **只 warn 不 FAIL**：
    #   那说明"本轮没跑 d 步判据"，不等于器件坏了 ✗（同 `--no-idsend` 的口径 ✓）。
    d_step = (got["sign_load"] is not None) or (got["sign_src"] is not None)
    if not d_step:
        print("warn 板上固件没有 d 步那两行（`ES256 sign <- …` / `sign_src=`）"
              "⇒ **本轮没跑 d 步判据**（固件是旧的；烧了新版再看）")
    elif got["sign_src"] is None and got["sign_load"] is None:
        print("warn 没看到签名来源那一行（`sign_src=` / `ES256 sign <- …`）")
    elif got["sign_src"] == "se" or got["sign_load"] == "se":
        print("PASS `id` 显示签名来源 = SE（level=0 的 ES256 在 SE 里签）")
    else:
        print("FAIL 签名来源是 %s（不是 se）⇒ 要么 SE 没接上、要么签名器没装上"
              % (got["sign_src"] or got["sign_load"]))
        fails.append("sign_src != se")
    if got["signcnt"]:
        print("info `se` 计数：sign_ok=%d sign_fail=%d"
              % (got["signcnt"]["ok"], got["signcnt"]["fail"]))
    if not d_step:
        pass
    elif got["sig_r"] and got["sig_s"]:
        sig = bytes.fromhex(got["sig_r"] + got["sig_s"])
        digest = hashlib.sha256(D_SELFTEST_MSG).digest()
        # ★★ 公钥从**哪来**也有对错 ✗（2026-10-12 实测）：按 SN 从库里取 ✓；
        #    取不到才回退硬编码常量，而且要**报警**（它只属于被量过的那一颗 ✗）。
        _m = _RE_SN.search(t or "")
        sn_seen = _m.group(1) if _m else None
        pub64 = slot0_pub_by_sn(dbp, sn_seen)
        psrc = "密钥库里登记的（sn=%s ✓）" % sn_seen
        if pub64 is None:
            pub64 = SLOT0_PUB_X + SLOT0_PUB_Y
            psrc = "**硬编码常量**（★ 只属于被量过的那一颗）"
            print("warn 没按 SN 从库里取到 Slot 0 公钥（sn=%s）⇒ 回退用**硬编码常量** ✗："
                  "换一颗芯片就会 `InvalidSignature`（**尺子错、不是签名错** ✗）；"
                  "要按库里的钥验就加 `--from-keystore`" % (sn_seen or "?"))
        try:
            from cryptography.hazmat.primitives import hashes as _h
            from cryptography.hazmat.primitives.asymmetric import ec as _ec, utils as _u
            pk = _ec.EllipticCurvePublicKey.from_encoded_point(
                _ec.SECP256R1(), b"\x04" + pub64)
            der = _u.encode_dss_signature(int.from_bytes(sig[:32], "big"),
                                          int.from_bytes(sig[32:], "big"))
            pk.verify(der, digest, _ec.ECDSA(_u.Prehashed(_h.SHA256())))
            print("PASS 板上那条签名被 **Slot 0 的公钥独立验过**（Prehashed(SHA256)；公钥来源 = %s）✓"
                  % psrc)
        except ImportError:
            print("warn 本机没装 `cryptography` ⇒ 跳过独立验签（**不算过**）")
        except Exception as e:                                # noqa: BLE001
            print("FAIL 独立验签不过：%r（签名/公钥/摘要三者对不上）" % (e,))
            fails.append("板上签名验不过")
    else:
        print("FAIL `se` 没打印出 r||s（`Sign(slot 0) ok; r=…` / `[se] s=…`）"
              "⇒ 签名没成，看上面的 `[se] Sign … FAILED/状态` 行")
        fails.append("没拿到板上签名")

    # ---- 判据 5：帧仍被上游接受（可选，默认做）----
    if args.no_idsend or args.from_file:
        print("（跳过 ID 上报判据：--no-idsend / --from-file）")
    else:
        sent = got["sent"]
        if sent:
            print("固件自报：level=%d alg=%s nonce=%s len=%d build_ms=%d"
                  % (sent["level"], sent["alg"], sent["nonce"], sent["len"], sent["build_ms"]))
        h = got["hex"]
        if len(h) % 2:
            h = h[:-1]
        if len(h) < 28:
            print("FAIL 没从控制台里拿到帧 hex（`idhex` 的输出）")
            fails.append("没拿到帧 hex")
        else:
            print("拿到帧 %d 字节（hex %d 字符）" % (len(h) // 2, len(h)))
            o, proto = load_upstream(args.upstream)
            if o is None:
                return 1
            # ★★ d 步：板上这条帧是 **SE 里签的** ⇒ 密钥库里要登记**这颗芯片的公钥**
            #   （= 产线烧录/登记那一步，见 `docs/atecc608b-se.md` §6.2/§8.10 ✓）；
            #   演示密钥库里那把是从 SN 派生的**另一把** ✗ ⇒ 不换钥必然 `signature_invalid`
            #   （那不是 bug，是"密钥库还没登记这颗芯片"）。
            pk_hex = (SLOT0_PUB_X + SLOT0_PUB_Y).hex()
            ksdb = None
            if dbp:
                import keystore as kst                       # noqa: PLC0415
                ksdb = kst.KeyStoreDB(dbp)                   # 真库（产线登记的那把 ✓）
            elif d_step:
                print("info 用 **Slot 0 的公钥**验这条帧（硬编码常量、产线该登记的那把 ✓），"
                      "不是 demo 派生钥；★ 它**只认这一颗 + 只走 ES256** ✗ ⇒ 要按库里的钥验就加"
                      " `--from-keystore`")
            if judge(o, proto, bytes.fromhex(h),
                     pubkey_hex=(None if ksdb is not None else (pk_hex if d_step else None)),
                     ksdb=ksdb) != 0:
                fails.append("上游不接受帧")
            if mod_buf or (args.mod_port and not args.no_mod_port):
                want = "%d byte(s)" % (len(h) // 2 + 8)
                if not mod_buf.strip():
                    print("warn 模组打印口没有任何输出（模组在跑吗？供电/接线/跳线帽）")
                elif want in mod_buf:
                    print("PASS 模组侧确认：打印口出现 `[mbus rx] %s`" % want)
                else:
                    print("FAIL 模组打印口没看到 `[mbus rx] %s`；本轮的字节数：%s"
                          % (want, re.findall(r"\[mbus rx\]\s+(\d+)\s+byte", mod_buf)[:8] or "(无)"))
                    fails.append("模组侧未确认")

    print("=" * 60)
    print("★ 本轮实测结论（请填回 docs/atecc608b-se.md §6 的「实测」列）：")
    print("   crc_mode = %s" % (st["crc_mode"] if st else "?"))
    print("   nonce_src = %s" % (got["nonce_src"] or "?"))
    print("   sign_src  = %s（★ d 步）" % (got["sign_src"] or got["sign_load"] or "?"))
    if fails:
        print("FAIL c4-γ-2 上机判据：%d 项未过 -> %s" % (len(fails), "；".join(fails)))
        return 2
    # ⚠ 收尾这句**必须按实际跑过哪几步说**（`--no-idsend` 时没验过帧，就别声称验过）
    if args.no_idsend or args.from_file:
        print("PASS c4-γ-2 上机判据：nonce 来自 ATECC608B 的 Random(0x1B)"
              "（本轮**没跑** ID 上报判据）")
    else:
        print("PASS c4-γ-2 上机判据：nonce 来自 ATECC608B 的 Random(0x1B)，且帧仍被上游接受")
    if d_step:
        print("     ★ d 步：`sign_src=se` ✓ 且板上那条签名已被 **Slot 0 的公钥独立验过** ✓"
              "（ES256 在 SE 里签、私钥不出芯片）")
    else:
        print("     ⚠ 如实：板上是**旧固件** ⇒ 只验了 nonce 来自 SE；"
              "签名来源（`sign_src`）与 SE 签名本轮**没验** ✗")
    return 0


if __name__ == "__main__":
    sys.exit(main())
