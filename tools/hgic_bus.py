#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
hgic_bus.py — 把「模组主机口 HGIC over UART0」包成 **host 数据口**的样子。

给 `orpah-over-halow/client.py` 的 `ClientHost(bus=…)` 用 —— 接口与那边的
`host_serial.SerialAtBus` **逐字一致**（`connect / close / send_frame / recv_frame`，
外加 `stats()`/`recent_lines()` 供排查），所以设备逻辑一行不用改。

为什么要有这个（2026-09-20 实测结论，详见 `docs/txah-uart-macbus.md` §八）：
  * 两块 V2.4 板（客户端 FMAC / TH-RJ45 WNB）的 **AT 都没有数据面命令** ——
    `AT+TXDATA`/`AT+RXDATA`/`AT+SOCKET`/`AT+DHCP` 全静默，上游
    `T-Halow-RJ45/docs/ethernet_bridge_linux.md` 也明确说**别用它**（会进 sticky
    数据模式、还会改写 EtherType）⇒ `SerialAtBus` 那套在真机上不成立；
  * 真机数据面 = **模块 UART0 上的 HGIC（mac_bus）**，帧格式/编解码在本仓
    `tools/txah_hgic.py`（单一源，含真机黄金样本自测）。

用法（与 `SerialAtBus` 对齐）：
    bus = HgicBus("COM23", at_port="COM6")     # data 口 + AT/打印口（后者用于发送确认）
    if not bus.connect():
        ...
    bus.send_frame(eth_frame)                   # 完整以太帧（≥14B）
    f = bus.recv_frame(timeout=2.0)             # 收下行以太帧，无则 None
    bus.close()

★ 为什么 `at_port` 值得接：CH347F 的 VCP 会**周期性抽风**（I/O 报 `PermissionError(13)`），
  PC 侧"写成功"不等于模块真收到。接了 AT/打印口就能用**模组自己的日志**确认
  （每收到一条主机帧它打 `[mbus rx] <8+len> byte(s)`）—— 确认不到就重发，
  见 `send_confirm` / `send_retries`。不接也能跑，但发送只算"写出去"。
"""
import re
import sys
import threading
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

import os                                                        # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from txah_hgic import (MAX_FRAME, CookieCounter, StreamParser,    # noqa: E402
                       data_frame)

try:
    import serial
except ImportError:                                              # pragma: no cover
    serial = None

_HDR = 8                      # HGIC 头长度（`[mbus rx] <8+len> byte(s)` 里的 8）
LOG_LINES = 400               # 保留多少条 AT 日志行


def _default_factory(port, baud):
    if serial is None:
        raise RuntimeError("没装 pyserial：pip install pyserial")
    return serial.Serial(port, baud, timeout=0.05, write_timeout=1.0)


class HgicBus:
    """HGIC（UART0）上的 host 数据口。线程模型与 `SerialAtBus` 相同：
    本类自己起一个读线程把帧塞进队列，`recv_frame()` 从队列里取（`ClientHost` 那边
    也有一个读线程，它只是消费，不碰串口）。"""

    def __init__(self, port, baud=115200, name="sta", log=None,
                 at_port=None, at_baud=None, serial_factory=None,
                 send_confirm=True, send_retries=4, line_log=LOG_LINES):
        self.port, self.baud, self.name = port, baud, name
        self.at_port = at_port
        self._at_baud = at_baud or baud
        self._factory = serial_factory or _default_factory
        self.log = log
        self.send_confirm = bool(send_confirm) and bool(at_port)
        self.send_retries = max(1, int(send_retries))
        # 数据口
        self.ser = None
        self._cookie = CookieCounter(1)
        self._parser = StreamParser(expect_from_module=True)
        self.rx_frames = []                      # 已解析出的下行**以太帧**
        self.ctrl = []                           # 控制面帧（CMD/EVENT 应答，供 ping/排查）
        self._lock = threading.Lock()
        self._tx_lock = threading.RLock()
        # AT/打印口（可选：发送确认 + 排查）
        self.at = None
        self.lines = []
        self._max_lines = int(line_log)
        self._at_buf = b""
        # 计数（分开计：丢帧/失败必须看得见）
        self.tx_frames = 0
        self.tx_fail = 0
        self.tx_retry = 0
        self.rx_frames_n = 0
        self.reopens = 0
        self.bad_bytes = 0
        self._stop = threading.Event()
        self._threads = []

    # ---------------- 连接 ----------------
    def _open_serial(self, port, baud):
        return self._factory(port, baud)

    def connect(self, retries=8, interval=0.5):
        if self.ser is not None:                 # 幂等（ClientHost.connect 也会调一次）
            return True
        for _ in range(max(1, int(retries))):
            try:
                self.ser = self._open_serial(self.port, self.baud)
            except Exception as e:               # noqa: BLE001
                self._say("打开数据口 %s 失败：%s" % (self.port, e))
                time.sleep(interval)
                continue
            try:
                self.ser.reset_input_buffer()
            except Exception:                    # noqa: BLE001
                pass
            self._stop.clear()
            t = threading.Thread(target=self._read_loop, daemon=True)
            t.start()
            self._threads.append(t)
            if self.at_port:
                try:
                    self.at = self._open_serial(self.at_port, self._at_baud)
                    t2 = threading.Thread(target=self._at_loop, daemon=True)
                    t2.start()
                    self._threads.append(t2)
                except Exception as e:           # noqa: BLE001
                    self._say("打开 AT 口 %s 失败（只是没有发送确认）：%s" % (self.at_port, e))
                    self.at = None
            return True
        return False

    def close(self):
        self._stop.set()
        for s in (self.ser, self.at):
            if s is not None:
                try:
                    s.close()
                except Exception:                # noqa: BLE001
                    pass

    def _say(self, *a):
        if self.log:
            self.log("[hgic]", *a)
        else:
            print("[hgic]", *a, flush=True)      # 没传 log 也要看得见（否则失败原因静默）

    # ---------------- 读线程：数据口 ----------------
    def _read_loop(self):
        while not self._stop.is_set():
            ser = self.ser
            if ser is None:
                return
            try:
                n = ser.in_waiting
                chunk = ser.read(min(n, 4096)) if n else b""
            except Exception as e:               # noqa: BLE001
                # CH347F 的 VCP 会周期性抽风 → 重开继续（不静默）
                self.reopens += 1
                self._say("数据口 I/O 异常（第 %d 次重开）：%r" % (self.reopens, e))
                try:
                    ser.close()
                except Exception:                # noqa: BLE001
                    pass
                time.sleep(0.5)
                try:
                    self.ser = self._open_serial(self.port, self.baud)
                except Exception:                # noqa: BLE001
                    time.sleep(0.8)
                continue
            if not chunk:
                time.sleep(0.005)
                continue
            for hdr, payload in self._parser.feed(chunk):
                tn = hdr.get("type_name")
                if tn == "FRM2":
                    with self._lock:
                        self.rx_frames.append(bytes(payload))
                        self.rx_frames_n += 1
                else:
                    with self._lock:              # 控制面（事件/应答）：留着给 ping/排查
                        self.ctrl.append((tn, bytes(payload)))
                        del self.ctrl[:-200]

    # ---------------- 读线程：AT/打印口（只用来确认与排查） ----------------
    def _at_loop(self):
        while not self._stop.is_set():
            ser = self.at
            if ser is None:
                return
            try:
                n = ser.in_waiting
                chunk = ser.read(min(n, 4096)) if n else b""
            except Exception:                    # noqa: BLE001
                time.sleep(0.5)
                try:
                    self.at = self._open_serial(self.at_port, self._at_baud)
                except Exception:                # noqa: BLE001
                    time.sleep(0.8)
                continue
            if not chunk:
                time.sleep(0.01)
                continue
            self._at_buf += chunk
            while b"\n" in self._at_buf:
                line, self._at_buf = self._at_buf.split(b"\n", 1)
                txt = line.decode("utf-8", "replace").strip()
                if not txt:
                    continue
                with self._lock:
                    self.lines.append(txt)
                    del self.lines[:-self._max_lines]

    # ---------------- host → 模块（上行） ----------------
    def send_frame(self, eth_frame):
        """注入一帧**完整以太网帧**（HGIC `FRM2` + 完整以太帧；≥14B）。

        与 `SerialAtBus.send_frame` 一样：**不能"写出去就算发成功"**。这里用模组自己的
        `[mbus rx] <8+len> byte(s)` 日志确认（`at_port` 给了才有），确认不到就重发。
        失败一律返回 False 并计数（`tx_fail`），不静默。
        """
        eth = bytes(eth_frame)
        if self.ser is None:
            self.tx_fail += 1
            return False
        if len(eth) < 14:
            self._say("帧太短（%dB < 14B 以太头）→ 不发" % len(eth))
            self.tx_fail += 1
            return False
        want = _HDR + len(eth)                   # `[mbus rx] <n> byte(s)` 里的 n
        pat = re.compile(r"\[mbus rx\]\s+%d byte" % want)
        with self._tx_lock:                      # 一次只走一条帧（cookie 有顺序检查）
            for attempt in range(1, self.send_retries + 1):
                if attempt > 1:
                    self.tx_retry += 1
                if self.at is not None:
                    with self._lock:
                        del self.lines[:-5]      # 丢掉旧日志，避免认成上一次的确认
                try:
                    self.ser.write(data_frame(eth, cookie=self._cookie.next(), lean=True))
                except Exception as e:           # noqa: BLE001
                    self._say("写数据口失败：%r" % (e,))
                    time.sleep(0.3)
                    continue
                if self.at is None or not self.send_confirm:
                    self.tx_frames += 1
                    return True
                end = time.monotonic() + 1.5
                while time.monotonic() < end:
                    with self._lock:
                        if any(pat.search(x) for x in self.lines):
                            self.tx_frames += 1
                            return True
                    time.sleep(0.02)
            self.tx_fail += 1
            self._say("发帧失败：%d 次都没在 AT 口看到 `[mbus rx] %d byte(s)`" % (self.send_retries, want))
            return False

    # ---------------- 控制面探针（真判据：有没有应答，而不是"发出去了"） ----------------
    def ping(self, cmd_id=None, timeout=3.0):
        """发一条只读命令（默认 `GET_UART_FIXLEN`）并等**真的** CMD 应答。

        为什么要它："写出去"不等于模块活着 —— 固件不对（没烧 MACBUS_UART 版）/线不共地/
        跳线不对时，写进去没人应。返回 True 才算数据口是活的。
        """
        from txah_hgic import CMD_GET_UART_FIXLEN, cmd_frame
        cmd_id = cmd_id if cmd_id is not None else CMD_GET_UART_FIXLEN
        if self.ser is None:
            return False
        with self._lock:
            base = len(self.ctrl)
        try:
            self.ser.write(cmd_frame(cmd_id, b"", cookie=self._cookie.next()))
        except Exception as e:                   # noqa: BLE001
            self._say("发命令帧失败：%r" % (e,))
            return False
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self._lock:
                new = self.ctrl[base:]
            if any(tn == "CMD" for tn, _ in new):
                return True
            time.sleep(0.02)
        return False

    # ---------------- 模块 → host（下行） ----------------
    def recv_frame(self, timeout=2.0):
        """取一帧收到的**以太网帧**（无则 None）。带超时，便于轮询/多线程。"""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self._lock:
                if self.rx_frames:
                    return self.rx_frames.pop(0)
            time.sleep(0.01)
        return None

    # ---------------- 观测 ----------------
    def stats(self):
        return {"port": self.port, "tx_frames": self.tx_frames, "tx_fail": self.tx_fail,
                "tx_retry": self.tx_retry, "rx_frames": self.rx_frames_n,
                "reopens": self.reopens, "queued": len(self.rx_frames),
                "bad_bytes": self._parser.garbage + self._parser.bad_length}

    def recent_lines(self, n=20):
        with self._lock:
            return list(self.lines[-n:])


__all__ = ["HgicBus", "MAX_FRAME"]
