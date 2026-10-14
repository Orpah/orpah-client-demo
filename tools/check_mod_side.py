# -*- coding: utf-8 -*-
"""check_mod_side.py — §9.0 第 2 条判据：**帧到底有没有进模组**（一键跑）。

台架 = `hardware/wiring/cstep-ch347f-txah-evb-nanoch32.*`（nano + TX-AH EVB + CH347F，
**不带** ATECC608B）⇒ 本工具**只判两件事**，**故意不判 SE**（那几条要有 SE 的台架，
见 `tools/check_c4g2_bench.py`；在无 SE 台架上它们必然 FAIL ✗，判了只会噪声）：

  1. `idsend` 之后，模组 AT/打印口出现 **`[mbus rx] <帧长 + 8> byte(s)`** ——
     `+8` = HGIC 头（`2b 1a 09 00 <len:4B LE> …`）✓；帧长以固件自报的 `[id] sent … len=` 为准；
  2. 同一帧交给**上游** `orpah_id.verify_report` ⇒ `accepted=True` ✓。

★ 「尺子」这一条最要紧（2026-10-12 实测踩过 ✗）：本台架**没有 SE** ⇒ 固件如实回退
  `nonce_src=soft sign_src=soft`（软件 P-256 = 演示级）⇒ 判帧**必须用 demo 派生钥**
  （= `check_report_hex.py` **不带** `--from-keystore`）。
  拿 `check_c4g2_bench.py` 默认那把**硬编码的 Slot 0 公钥**去判 ⇒ 必然 `signature_invalid` ✗
  —— 那不是签名错，是**尺子拿错了**（同 §8.30 ② 那条教训）。
  板上是**真 SE 签名**时（另一台架），加 `--from-keystore` 用产线登记的那把 ✓。

★ 另一个已踩到的问题：`idhex` 的 hex 是**按行**打出来的（≤64 字符/行），而控制台会把
  `[mod] …` 行**插在 hex 行之间** ⇒ **不能**用"连续 hex 行"拼接（会中途断，实测只收到 64 字符 ✗）；
  正确做法 = **收所有纯 hex 行**再按已知帧长截断 ✓（本工具这么干 ✓）。

用法：
    py -3.13 tools\\check_mod_side.py                      # COM23 控制台 / COM24 模组口
    py -3.13 tools\\check_mod_side.py --port COM7 --mod-port COM8
    py -3.13 tools\\check_mod_side.py --from-keystore      # 板上是真 SE 签名时
    py -3.13 tools\\check_mod_side.py --keep-frame out.hex # 指定整帧落盘位置

退出码：0 = 两条判据都过；2 = 有 FAIL；1 = 用法/环境问题（串口打不开等）。
⚠ 串口独占：先关掉 WindTerm / SecureCRT / MounRiver 的串口窗口（否则 PermissionError(13)）。
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

import serial  # noqa: E402
from serial.tools import list_ports  # noqa: E402

BAUD = 115200
HEXLINE = re.compile(r"[0-9a-fA-F]{2,64}")
RE_SENT = re.compile(r"\[id\]\s+sent\b.*?\blen=(\d+)", re.S)
RE_MBUS_RX = re.compile(r"\[mbus rx\]\s*(\d+)\s*byte")


def pump(ser, secs, until=None):
    """读串口 secs 秒；until 命中提前返回。返回 (text, hit)。"""
    buf, end = "", time.monotonic() + secs
    while time.monotonic() < end:
        try:
            d = ser.read(4096)
        except Exception:                                       # noqa: BLE001
            break
        if d:
            buf += d.decode("utf-8", "replace")
            if until and until(buf):
                return buf, True
    return buf, bool(until and until(buf))


def all_hex(txt, want_chars):
    """把所有**纯 hex 行**按出现次序拼起来，截到 want_chars（容忍插进来的非 hex 行）。"""
    parts = [ln.strip() for ln in txt.replace("\r", "\n").split("\n")
             if ln.strip() and HEXLINE.fullmatch(ln.strip())]
    s = "".join(parts)
    return s[:want_chars] if want_chars else s


def main():
    ap = argparse.ArgumentParser(description="模组侧判据（§9.0 第 2 条）")
    ap.add_argument("--port", default="COM23", help="固件控制台串口（默认 COM23）")
    ap.add_argument("--mod-port", default="COM24", help="模组 AT/打印口（默认 COM24）")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--timeout", type=float, default=30.0, help="每步等待上限（秒）")
    ap.add_argument("--from-keystore", action="store_true",
                    help="板上是**真 SE 签名**时用产线登记的钥判；无 SE 台架不要加 ✗")
    ap.add_argument("--keep-frame", help="整帧 hex 落盘路径（默认 %TEMP%\\orpah-frame.hex）")
    ap.add_argument("--list-ports", action="store_true", help="只列串口")
    args = ap.parse_args()

    if args.list_ports:
        for p in list_ports.comports():
            print("   %-8s | %s | %s" % (p.device, p.description, p.hwid))
        return 0

    out_hex = args.keep_frame or os.path.join(os.environ.get("TEMP", "."), "orpah-frame.hex")

    print("== 串口 ==")
    for p in list_ports.comports():
        if "CH347" in (p.description or ""):
            print("   %-8s | %s" % (p.device, p.description))
    print("   控制台 %s / 模组口 %s" % (args.port, args.mod_port))

    try:
        con = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as e:                                      # noqa: BLE001
        print("FAIL 打不开控制台 %s：%r" % (args.port, e))
        print("     ⚠ 串口独占：先关掉 WinTerm/SecureCRT/MounRiver 的串口窗口")
        return 1
    try:
        con.reset_input_buffer()
        con.write(b"AT\r\n")
        t, ok = pump(con, 5.0, lambda b: "OK (rx=" in b)
        if not ok:
            con.write(b"AT\r\n")
            t2, ok = pump(con, 5.0, lambda b: "OK (rx=" in b)
            t += t2
        print("\n%s 固件活着（`AT` → `OK`）" % ("PASS" if ok else "FAIL"))
        if not ok:
            print("     —— 板子在跑吗？按一次 RST 再看；或 `--port` 选错口了")
            return 2

        con.write(b"id\r\n")
        t, _ = pump(con, 8.0, lambda b: "nonce_src=" in b)
        print("\n== `id`（看 sn / nonce_src / sign_src —— 无 SE 台架应为 soft）==")
        for ln in t.replace("\r", "\n").split("\n"):
            if ln.strip():
                print("   | " + ln.strip())

        mod = None
        try:
            mod = serial.Serial(args.mod_port, args.baud, timeout=0.2)
            mod.reset_input_buffer()
            print("\n已打开 %s（模组口）" % args.mod_port)
        except Exception as e:                                  # noqa: BLE001
            print("\nwarn 打不开模组口 %s：%r ⇒ 判据 1 判不了" % (args.mod_port, e))

        con.reset_input_buffer()
        con.write(b"idsend\r\n")
        t, _ = pump(con, args.timeout, lambda b: "[id] sent" in b or "held (self-limit)" in b)
        print("\n== `idsend` ==")
        for ln in t.replace("\r", "\n").split("\n"):
            if "[id]" in ln or "[mod] **" in ln:
                print("   | " + ln.strip())
        m = RE_SENT.search(t)
        len_sent = int(m.group(1)) if m else None

        modbuf = ""
        if mod is not None:
            modbuf, _ = pump(mod, 8.0, lambda b: "[mbus rx]" in b)
            mod.close()
        rx_lens = [int(x) for x in RE_MBUS_RX.findall(modbuf)]
        print("\n== 模组口（找 `[mbus rx]`）==")
        for ln in modbuf.replace("\r", "\n").split("\n"):
            if "[mbus rx]" in ln:
                print("   | " + ln.strip()[:150])
        print("   mbus rx 字节数 = %s" % (rx_lens[:8] or "(无)"))

        want_chars = (len_sent * 2) if len_sent else 0
        con.reset_input_buffer()
        con.write(b"idhex\r\n")
        b1, _ = pump(con, 6.0, lambda b: len(all_hex(b, want_chars)) >= (want_chars or 400))
        b2, _ = pump(con, 6.0)                       # 再收一会儿，补上被插行打断的部分
        hx = all_hex(b1 + b2, want_chars)
        print("\n== `idhex` ==")
        if hx and len(hx) >= (want_chars or 1):
            with open(out_hex, "w", encoding="ascii") as f:
                f.write(hx)
            print("   帧长 %d 字节（期望 %s）⇒ 已写 %s" % (len(hx) // 2, len_sent, out_hex))
        else:
            print("   只收到 %d 字符（期望 %s）—— 没收全" % (len(hx), want_chars))
    finally:
        con.close()

    rc = 0
    print("\n==== 判据 ====")
    # 判据 1：模组侧
    if len_sent is None:
        print("FAIL 没读到 `[id] sent … len=` ⇒ 拿不到帧长")
        rc = 2
    elif len_sent + 8 in rx_lens:
        print("PASS 判据 1（模组侧）：`[mbus rx] %d byte(s)` ✓（帧 %d + 8 B HGIC 头）"
              % (len_sent + 8, len_sent))
    else:
        print("FAIL 判据 1（模组侧）：期望 `[mbus rx] %d`，实收 %s"
              % (len_sent + 8, rx_lens[:8] or "(无)"))
        rc = 2

    # 判据 2：上游接受（★ 尺子：无 SE 台架用 demo 派生钥）
    if hx and len(hx) >= (want_chars or 1):
        cmd = [sys.executable, os.path.join(HERE, "check_report_hex.py"), "--file", out_hex]
        if args.from_keystore:
            cmd.append("--from-keystore")
        print("→ 交给上游判（%s）" % ("产线登记的钥" if args.from_keystore else "demo 派生钥 ✓"))
        p = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        for ln in ((p.stdout or "") + (p.stderr or "")).splitlines():
            print("   " + ln)
        if p.returncode != 0:
            rc = 2
    else:
        print("FAIL 判据 2（上游接受）：没收到完整帧，判不了")
        rc = 2

    print("\n%s" % ("全部通过 ✓" if rc == 0 else "有未过的判据 ✗（见上）"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
