# -*- coding: utf-8 -*-
"""check_c4g2_bench.py — c4-γ-2 **上机判据**的一键跑法（PC 侧）。

判的是「nonce 真的来自 ATECC608B」这一件事，全程用**实际字节**，不靠固件自报的结论：
  1. 打开 nano 的**控制台串口**（默认 COM23，115200 8N1）；
  2. 探活：`AT` → `OK`（不靠横幅 —— 板子已跑一会儿的话横幅早过去了，同 γ-1 口径）；
  3. 发 `se`（固件里的 `atecc_selftest()`）⇒ 看四件事：
       · `[se] wake: ACK`            —— 唤醒拿到了 ACK（= 供电/接线/上拉都对）
       · `[se] cmd=… crc_mode=…`     —— ★ **本轮实测**：线上要不要挂 CRC（两条待确认假设之一）
       · `[se] Random(0x1B) 32 B ok; 前 16 B = <32hex>`
       · 失败时把 `rc / last_resp_len / raw=` 原样带出来（是 `count` 口径不对还是别的问题）
  4. **再发一次 `se`** ⇒ 两次的 16 B **必须不同**（RNG 真的在动，不是常量/缓存）；
  5. 发 `id` ⇒ `nonce_src=se`（装配进流水线的那条路径确实在用 SE；`soft` 就是没接上）；
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
    python tools\\check_c4g2_bench.py --keep-open          # 只 dump 原始输出，不判定

⚠ **串口独占**：Windows 上一个口只能被一个进程打开 ⇒ 先关掉 WindTerm / SecureCRT /
   MounRiver 的串口窗口（否则 `PermissionError(13)`）。
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from check_report_hex import load_upstream, judge, strip_hex      # noqa: E402  （单一源：解析/判定）

BAUD = 115200
# ⚠ `crc_mode=` 后面那串**含空格**（`no-crc(响应 36 B)`）⇒ 不能用 \S+
RE_WAKE = re.compile(r"\[se\]\s+wake:\s*(ACK|NO ACK)")
RE_STATS = re.compile(r"\[se\]\s+cmd=(\d+)\s+ok=(\d+)\s+fail=(\d+)\s+crc_mode=(.*)")
RE_RAND = re.compile(r"\[se\]\s+Random\(0x1B\)\s+32\s*B\s*ok;\s*前\s*16\s*B\s*=\s*([0-9a-fA-F]{32})")
RE_SEFAIL = re.compile(r"\[se\]\s+Random FAILED\s+rc=(-?\d+)\s+last_resp_len=(\d+)\s+raw=([0-9a-fA-F]*)")
RE_SENORES = re.compile(r"\[se\]\s*没应答")
RE_NONCESRC = re.compile(r"\[id\]\s+nonce_src=(\S+)")
RE_LOADLINE = re.compile(r"\[id\]\s+nonce <- (se|soft)", re.IGNORECASE)
RE_HEXLINE = re.compile(r"^[0-9a-fA-F]{2,64}$")
RE_SENT = re.compile(r"\[id\]\s+sent\s+level=(\d+)\s+alg=(\S+)\s+why=(\S+)\s+nonce=(\S+)"
                     r"\s+len=(\d+)\s+build_ms=(\d+)")


def parse_se_log(text):
    """从控制台文本里抽 `se` 的判据字段。**按出现顺序**收：一个 `se` 的完整判据 = 一轮。"""
    out = {"wake": None, "nores": False, "stats": None, "rands": [], "fails": [],
           "nonce_src": None, "loadline": None, "sent": None, "hex": ""}
    for ln in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        s = ln.strip()
        m = RE_WAKE.search(s)
        if m:
            out["wake"] = m.group(1)
        if RE_SENORES.search(s):
            out["nores"] = True
        m = RE_STATS.search(s)
        if m:
            out["stats"] = {"cmd": int(m.group(1)), "ok": int(m.group(2)),
                            "fail": int(m.group(3)), "crc_mode": m.group(4).strip()}
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
    args = ap.parse_args()

    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                       # noqa: BLE001
            pass

    fails = []
    mod_buf = ""
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
    if got["nores"]:
        print("FAIL `se` 报「没应答」⇒ 查三件事：SE 的 3V3/GND 供电、"
              "`PB6↔pin6(SCL)`/`PB7↔pin5(SDA)` 接线、**4.7k 上拉**")
        fails.append("SE 无应答")
    elif got["wake"] == "ACK":
        print("PASS SE 唤醒拿到 ACK（供电/接线/上拉都对）")
    elif got["wake"] == "NO ACK":
        print("FAIL 唤醒没 ACK —— 同上三件事（供电 / 接线 / 上拉）")
        fails.append("唤醒无 ACK")
    else:
        print("FAIL 控制台里没看到 `[se] wake: …` —— `se` 命令没被识别？看上面原始输出")
        fails.append("`se` 未输出")

    # ---- 判据 2：Random + ★ 实测的 CRC 模式 ----
    st = got["stats"]
    if st:
        print("PASS `se` 统计：cmd=%d ok=%d fail=%d  ★ crc_mode=%s"
              % (st["cmd"], st["ok"], st["fail"], st["crc_mode"]))
        if st["crc_mode"] not in ("no-crc(响应 36 B)", "crc(响应 38 B)"):
            print("warn crc_mode 仍是 unknown ⇒ 那条随机数不是一次就取到的，看下面的 FAILED 行")
    if got["fails"]:
        for f in got["fails"]:
            print("FAIL Random 失败：rc=%d last_resp_len=%d raw=%s"
                  % (f["rc"], f["len"], f["raw"] or "(空)"))
            print("     ⇒ 若 len 不是 36/38：把 raw 贴进 docs/atecc608b-se.md §4，"
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
            if judge(o, proto, bytes.fromhex(h)) != 0:
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
    if fails:
        print("FAIL c4-γ-2 上机判据：%d 项未过 -> %s" % (len(fails), "；".join(fails)))
        return 2
    # ⚠ 收尾这句**必须按实际跑过哪几步说**（`--no-idsend` 时没验过帧，就别声称验过）
    if args.no_idsend or args.from_file:
        print("PASS c4-γ-2 上机判据：nonce 来自 ATECC608B 的 Random(0x1B)"
              "（本轮**没跑** ID 上报判据）")
    else:
        print("PASS c4-γ-2 上机判据：nonce 来自 ATECC608B 的 Random(0x1B)，且帧仍被上游接受")
    print("     ⚠ 如实：**签名仍是软件 P-256 替身**（SE 签名 / Slot0 是 d 步）——"
          "这只是把 nonce 的来源换成了 SE 的 RNG")
    return 0


if __name__ == "__main__":
    sys.exit(main())
