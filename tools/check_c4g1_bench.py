# -*- coding: utf-8 -*-
"""check_c4g1_bench.py — c4-γ-1 **上机判据**的一键跑法（PC 侧）。

它做的事（全都靠"实际字节"，不靠固件自报）：
  1. 打开 nano 的**控制台串口**（默认 COM23，115200 8N1）——那就是本仓固件的 `USART1` 窗口；
  2. 等横幅（含 `orpah-client`）⇒ 证明固件在跑；
  3. 发 `idsend`（默认做）⇒ 固件**当场组装一条已签上报**并发进模组数据口；
     等 `[id] sent level=… alg=… why=… nonce=… len=… build_ms=…` 那一行；
  4. 发 `idhex` ⇒ 固件把**刚发出去的那一帧**打成 hex；
  5. 把那串 hex 交给**上游**（`orpah-over-halow` 的 `orpah_id.verify_report`）判一次 ——
     与 `tools/check_report_hex.py` 共用同一套解析/判定（单一源，不重写协议）。

判据（退出码）：**0 = 固件在跑 + 拿到帧 + 上游接受**；2 = 有环节失败（信息在输出里）；
              1 = 用法/环境问题（串口打不开、找不到上游参考实现）。

用法：
    python tools\\check_c4g1_bench.py                     # COM23，自动 idsend + idhex + 判定
    python tools\\check_c4g1_bench.py --port COM7
    python tools\\check_c4g1_bench.py --no-idsend         # 只 dump 上一帧（不主动注入）
    python tools\\check_c4g1_bench.py --from-file log.txt # 离线解析一段控制台日志（不用串口）
    python tools\\check_c4g1_bench.py --keep-open         # 只 dump 不判（看原始输出用）

⚠ **串口独占**：Windows 上一个口只能被一个进程打开。跑之前先关掉 WindTerm / SecureCRT /
   MounRiver 的串口窗口（否则报 `PermissionError(13)`）。也可以反过来：**你自己在终端里**
   敲 `idsend` + `idhex`，把 hex 贴给 `python tools\\check_report_hex.py <hex>` —— 同一个判据。
⚠ 固件侧控制台的"行尾"：`\\r` 与 `\\n` 都当行尾（见 `firmware/README.md`），所以这里发 `\\r\\n`。
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
RE_SENT = re.compile(r"\[id\]\s+sent\s+level=(\d+)\s+alg=(\S+)\s+why=(\S+)\s+nonce=(\S+)"
                     r"\s+len=(\d+)\s+build_ms=(\d+)")
RE_HEXLINE = re.compile(r"^[0-9a-fA-F]{2,64}$")


def parse_console_log(text):
    """从控制台文本里抽 (banner, sent 行信息, held, hex)。串口与 --from-file 共用。"""
    out = {"banner": False, "sent": None, "held": False, "hex": "", "no_frame": False}
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    for ln in lines:
        s = ln.strip()
        if "orpah-client" in s and "firmware" in s:
            out["banner"] = True
        m = RE_SENT.search(s)
        if m:
            out["sent"] = {"level": int(m.group(1)), "alg": m.group(2), "why": m.group(3),
                           "nonce": m.group(4), "len": int(m.group(5)),
                           "build_ms": int(m.group(6))}
        if "held (self-limit)" in s:
            out["held"] = True
        if "no frame yet" in s:
            out["no_frame"] = True
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
    ap = argparse.ArgumentParser(description="c4-γ-1 上机判据（PC 侧一键）")
    ap.add_argument("--port", default="COM23", help="固件控制台串口（默认 COM23）")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--timeout", type=float, default=25.0, help="每一步的等待上限（秒）")
    ap.add_argument("--no-idsend", action="store_true", help="不主动 idsend，只 dump 上一帧")
    ap.add_argument("--from-file", help="离线：解析一段控制台日志（不用串口）")
    ap.add_argument("--keep-open", action="store_true", help="只 dump 原始输出，不做判定")
    ap.add_argument("--upstream", help="orpah-over-halow 目录（默认自动找）")
    args = ap.parse_args()

    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                       # noqa: BLE001
            pass

    if args.from_file:
        text = open(args.from_file, encoding="utf-8", errors="replace").read()
        print("（离线模式：解析 %s，%d 字符）" % (args.from_file, len(text)))
        got = parse_console_log(text)
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
        try:
            # ① 探活：**发命令看回答**（不靠横幅 —— 板子已跑一会儿的话横幅早过去了）
            ser.reset_input_buffer()
            ser.write(b"AT\r\n")
            t = read_until(ser, lambda b: "OK (rx=" in b, 5.0)
            if "OK (rx=" not in t:
                ser.write(b"AT\r\n")                      # 再试一次（掉一个字节也救回来）
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
            if "orpah-client" in t:
                print("（顺便收到横幅片段）")
            # ② idsend（默认）
            if not args.no_idsend:
                ser.write(b"idsend\r\n")
                t += read_until(ser, lambda b: "[id] sent" in b or "held (self-limit)" in b,
                                args.timeout)
            # ③ idhex
            ser.write(b"idhex\r\n")
            t += read_until(ser, lambda b: "no frame yet" in b or RE_HEXLINE.search(b) is not None,
                            args.timeout)
        finally:
            ser.close()
        print("--- 控制台（截尾）---")
        print(t[-900:])
        got = parse_console_log(t)

    # ---- 判定 ----
    sent = got["sent"]
    if sent:
        print("固件自报：level=%d alg=%s why=%s nonce=%s len=%d build_ms=%d"
              % (sent["level"], sent["alg"], sent["why"], sent["nonce"], sent["len"],
                 sent["build_ms"]))
        print("   ⚠ 这是**固件自己打印的**，只当线索；判据用下面那条真实字节")
    elif got["held"]:
        print("info 本次 `idsend` 被自限频**延后**（<600 ms），下面用上一帧判 —— 延后不是丢弃")
    else:
        print("FAIL 没看到 `[id] sent …` 行（60 s 周期还没到，或 idsend 没成功 —— 用 `id`/`stat` 查）")

    if got["no_frame"]:
        print("FAIL 固件说 `no frame yet`（还没发过任何帧）—— 先 `idsend`")
        return 2
    h = got["hex"] or strip_hex("")
    if len(h) < 28:
        print("FAIL 没从控制台里拿到帧 hex（`idhex` 的输出）")
        return 2
    if len(h) % 2:
        h = h[:-1]
    print("拿到帧 %d 字节（hex %d 字符）" % (len(h) // 2, len(h)))

    o, proto = load_upstream(args.upstream)
    if o is None:
        return 1
    rc = judge(o, proto, bytes.fromhex(h))
    print("=" * 60)
    if rc == 0:
        print("PASS c4-γ-1 上机判据：固件产出的帧**被上游接受**")
        if sent and sent["level"] in (0, 1, 2):
            print("     （level=%d；build_ms=%d 秒级是预期的：8 MHz 上费马求逆）"
                  % (sent["level"], sent["build_ms"]))
    return rc


if __name__ == "__main__":
    sys.exit(main())
