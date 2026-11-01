#!/usr/bin/env python3
"""ECSP serial console.

The page at http://127.0.0.1:8765 shows every frame. The same process owns
the port, so an agent reads GET /api/events and sends POST /api/tx.

    python tools/ecsp-console.py --port COM18
"""

from __future__ import annotations

import argparse
import json
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import serial

CMDS = {
    0x01: "握手",
    0x02: "软件版本",
    0x03: "设置硬件版本",
    0x04: "获取硬件版本",
    0x05: "设置序列号",
    0x06: "获取序列号",
    0x07: "系统复位",
    0x09: "获取自检",
    0x0A: "自检上报",
    0x0B: "低功耗",
    0x0E: "升级结果",
    0x10: "心跳",
    0x11: "故障上报",
    0x12: "故障查询",
    0x30: "芯片标识",
    0x31: "获取测试字",
    0x32: "设置测试字",
    0x33: "计数加一",
}

RS = {
    0x00: "成功",
    0x01: "执行失败",
    0x02: "长度不符",
    0x03: "取值非法",
    0x04: "不支持",
    0x10: "未握手",
    0x11: "忙",
    0x20: "拒绝复位",
}

PRESETS = [
    ("handshake", "握手", "FA 09 00 03 01 02 88 50 0D"),
    ("version", "软件版本", "FA 09 00 03 02 02 88 A0 0D"),
    ("uid", "芯片标识", "FA 09 00 03 30 02 9D C0 0D"),
    ("test-get", "读测试字", "FA 09 00 03 31 02 9C 50 0D"),
    ("test-set", "写测试字 A5", "FA 0A 00 03 32 02 A5 60 21 0D"),
    ("count", "计数加一", "FA 0A 00 03 33 02 01 30 5A 0D"),
]


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def parse_hex(text: str) -> bytes:
    cleaned = "".join(ch for ch in text if ch not in " \t\r\n,")
    if cleaned.lower().startswith("0x"):
        cleaned = cleaned[2:]
    if len(cleaned) % 2:
        raise ValueError("十六进制长度为奇数")
    return bytes.fromhex(cleaned)


def hex_of(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def summarize(frame: bytes, direction: str) -> str:
    if len(frame) < 9 or frame[0] != 0xFA or frame[-1] != 0x0D:
        return "原始字节"
    length = frame[1] | (frame[2] << 8)
    cmd = frame[4]
    payload = frame[6 : length - 3]
    body = frame[1 : length - 3]
    crc_ok = crc16(body) == (frame[length - 3] | (frame[length - 2] << 8))
    name = CMDS.get(cmd, f"命令 0x{cmd:02X}")
    parts = [name]
    if direction == "rx" and payload:
        rs = payload[0]
        parts.append(RS.get(rs, f"应答 0x{rs:02X}"))
        data = payload[1:]
    else:
        data = payload
    if data:
        ascii_text = "".join(chr(b) if 32 <= b < 127 else "" for b in data)
        if ascii_text and len(ascii_text) == len(data):
            parts.append(ascii_text)
        else:
            parts.append(hex_of(data))
    if not crc_ok:
        parts.append("CRC 不符")
    return " · ".join(parts)


class Console:
    def __init__(self, port: str, baud: int) -> None:
        self.port_name = port
        self.baud = baud
        self.lock = threading.Lock()
        self.events: list[dict] = []
        self.next_id = 1
        self.buf = bytearray()
        self.error = ""
        self.ser: serial.Serial | None = None
        self.stop = threading.Event()

    def open(self) -> None:
        self.ser = serial.Serial(
            self.port_name,
            self.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.05,
            dsrdtr=False,
            rtscts=False,
        )
        self.ser.dtr = False
        self.ser.rts = False
        threading.Thread(target=self._read_loop, daemon=True).start()

    def close(self) -> None:
        self.stop.set()
        if self.ser is not None:
            self.ser.close()

    def status(self) -> dict:
        with self.lock:
            return {
                "port": self.port_name,
                "baud": self.baud,
                "open": self.ser is not None and self.ser.is_open,
                "error": self.error,
                "count": len(self.events),
            }

    def since(self, after: int) -> list[dict]:
        with self.lock:
            return [e for e in self.events if e["id"] > after]

    def tx(self, data: bytes) -> dict:
        if self.ser is None or not self.ser.is_open:
            raise RuntimeError(self.error or "串口未打开")
        self.ser.write(data)
        self.ser.flush()
        return self._add("tx", data)

    def _add(self, direction: str, data: bytes) -> dict:
        event = {
            "id": self.next_id,
            "t": time.strftime("%H:%M:%S"),
            "dir": direction,
            "hex": hex_of(data),
            "summary": summarize(data, direction),
        }
        self.next_id += 1
        self.events.append(event)
        if len(self.events) > 400:
            del self.events[:200]
        return event

    def _take_frames(self) -> list[bytes]:
        out: list[bytes] = []
        buf = self.buf
        while True:
            try:
                start = buf.index(0xFA)
            except ValueError:
                if buf:
                    out.append(bytes(buf))
                    buf.clear()
                break
            if start:
                out.append(bytes(buf[:start]))
                del buf[:start]
            if len(buf) < 3:
                break
            length = buf[1] | (buf[2] << 8)
            if length < 9 or length > 128:
                del buf[0]
                continue
            if len(buf) < length:
                break
            if buf[length - 1] != 0x0D:
                del buf[0]
                continue
            out.append(bytes(buf[:length]))
            del buf[:length]
        return out

    def _read_loop(self) -> None:
        assert self.ser is not None
        while not self.stop.is_set():
            try:
                chunk = self.ser.read(256)
            except serial.SerialException as exc:
                with self.lock:
                    self.error = str(exc)
                return
            if not chunk:
                continue
            with self.lock:
                self.buf.extend(chunk)
                frames = self._take_frames()
                for frame in frames:
                    self._add("rx", frame)


HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ECSP 串口</title>
<style>
  :root {
    color-scheme: light;
    --bg: #f3f0e8;
    --ink: #1c1915;
    --muted: #6d655c;
    --line: #d9d1c4;
    --paper: #fffdf8;
    --tx: #8a5a12;
    --rx: #1f6b45;
    --bad: #8d2f2f;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    height: 100vh;
    display: flex;
    flex-direction: column;
    background: var(--bg);
    color: var(--ink);
    font: 14px/1.45 "Segoe UI", sans-serif;
  }
  header, footer {
    display: flex;
    gap: 12px;
    align-items: center;
    padding: 12px 16px;
    background: var(--paper);
    border-bottom: 1px solid var(--line);
  }
  footer { border-bottom: 0; border-top: 1px solid var(--line); flex-wrap: wrap; }
  h1 { font-size: 16px; font-weight: 600; margin: 0; }
  #status { color: var(--muted); font-size: 13px; }
  main { flex: 1; overflow: auto; padding: 8px 0; }
  table { width: 100%; border-collapse: collapse; }
  th, td { text-align: left; padding: 6px 12px; vertical-align: top; }
  th { color: var(--muted); font-weight: 600; font-size: 12px; position: sticky; top: 0; background: var(--bg); }
  tr.tx td.who { color: var(--tx); }
  tr.rx td.who { color: var(--rx); }
  td.hex { font: 13px/1.4 Consolas, monospace; word-break: break-all; }
  td.sum.bad { color: var(--bad); }
  input[type="text"] {
    flex: 1;
    min-width: 240px;
    font: 13px Consolas, monospace;
    padding: 8px 10px;
    border: 1px solid var(--line);
    background: var(--bg);
    color: var(--ink);
  }
  button {
    font: inherit;
    padding: 7px 10px;
    border: 1px solid var(--line);
    background: var(--paper);
    color: var(--ink);
    cursor: pointer;
  }
  button.primary { background: var(--ink); color: var(--paper); border-color: var(--ink); }
</style>
</head>
<body>
<header>
  <h1>ECSP 串口</h1>
  <div id="status">连接中</div>
</header>
<main id="log">
<table>
  <thead><tr><th>时间</th><th>方向</th><th>字节</th><th>含义</th></tr></thead>
  <tbody id="rows"></tbody>
</table>
</main>
<footer>
  <input id="hex" type="text" spellcheck="false" placeholder="FA 09 00 03 01 02 88 50 0D">
  <button class="primary" id="send">发送</button>
  <span id="presets"></span>
  <button id="clear" type="button">清空显示</button>
</footer>
<script>
const PRESETS = __PRESETS__;
const rows = document.getElementById("rows");
const statusEl = document.getElementById("status");
const hex = document.getElementById("hex");
const presets = document.getElementById("presets");
let since = 0;
let follow = true;

document.getElementById("log").addEventListener("scroll", (ev) => {
  const el = ev.currentTarget;
  follow = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
});

PRESETS.forEach((p) => {
  const b = document.createElement("button");
  b.textContent = p[1];
  b.addEventListener("click", () => sendHex(p[2]));
  presets.appendChild(b);
});

document.getElementById("send").addEventListener("click", () => sendHex(hex.value));
hex.addEventListener("keydown", (ev) => {
  if (ev.key === "Enter") sendHex(hex.value);
});
document.getElementById("clear").addEventListener("click", () => { rows.innerHTML = ""; });

async function sendHex(text) {
  const res = await fetch("/api/tx", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ hex: text })
  });
  if (!res.ok) {
    const body = await res.json();
    statusEl.textContent = body.error || "发送失败";
  }
}

function add(ev) {
  const tr = document.createElement("tr");
  tr.className = ev.dir;
  const who = ev.dir === "tx" ? "主机发出" : "从机收到";
  const bad = ev.summary.indexOf("CRC") >= 0;
  tr.innerHTML = "<td>" + ev.t + "</td><td class='who'>" + who +
    "</td><td class='hex'>" + ev.hex + "</td><td class='sum" +
    (bad ? " bad" : "") + "'>" + ev.summary + "</td>";
  rows.appendChild(tr);
  if (follow) {
    const log = document.getElementById("log");
    log.scrollTop = log.scrollHeight;
  }
}

async function poll() {
  const res = await fetch("/api/events?since=" + since);
  const body = await res.json();
  const s = body.status;
  statusEl.textContent = s.open
    ? (s.port + "  " + s.baud + " 8N1  已打开  " + s.count + " 帧")
    : ("未打开  " + (s.error || ""));
  body.events.forEach((ev) => { since = ev.id; add(ev); });
}
setInterval(poll, 200);
poll();
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    console: Console

    def log_message(self, fmt: str, *args) -> None:
        return

    def _json(self, code: int, payload: dict) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/":
            page = HTML.replace(
                "__PRESETS__",
                json.dumps(PRESETS, ensure_ascii=False),
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
            return
        if parsed.path == "/api/events":
            qs = parse_qs(parsed.query)
            after = int(qs.get("since", ["0"])[0] or 0)
            self._json(200, {"status": self.console.status(), "events": self.console.since(after)})
            return
        if parsed.path == "/api/status":
            self._json(200, self.console.status())
            return
        self._json(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        if urlparse(self.path).path != "/api/tx":
            self._json(404, {"error": "not found"})
            return
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        try:
            body = json.loads(raw.decode("utf-8"))
            data = parse_hex(str(body.get("hex", "")))
            if not data:
                raise ValueError("空帧")
            event = self.console.tx(data)
        except (ValueError, RuntimeError, json.JSONDecodeError) as exc:
            self._json(400, {"error": str(exc)})
            return
        self._json(200, {"event": event})


def main() -> None:
    parser = argparse.ArgumentParser(description="ECSP serial console")
    parser.add_argument("--port", default="COM18")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--http", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    console = Console(args.port, args.baud)
    try:
        console.open()
    except serial.SerialException as exc:
        raise SystemExit(f"无法打开 {args.port}: {exc}") from exc

    Handler.console = console
    server = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    url = f"http://127.0.0.1:{args.http}/"
    print(f"ECSP console  {args.port} {args.baud}  {url}", flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        console.close()


if __name__ == "__main__":
    main()
