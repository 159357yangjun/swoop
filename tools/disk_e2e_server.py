#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GUI 真落盘 E2E 专用的最小 localhost HTTP 服务（两路，无认证、无延迟）。

刻意不复用 tools/engine_selftest_server.py：它带 Basic 认证、40ms 固定延迟、
三十来条分支路由（Range/429/重定向/代理…）。本轮口径是"归因空间最小"，
任何一条多余行为都可能把红推到服务器身上。这里只有两条路径：

  /payload.bin  → 200，固定长度、确定性字节（源文件当场算 SHA256，源本身也是证人）
  /missing.bin  → 404，不返回任何文件体

支持 Range 是**必要的最小集**：产品引擎对已知长度的资源会分段并发取，
不支持 206 会让"下载失败"的成因落到服务器上而不是链路上。

用法：python tools/disk_e2e_server.py <port> <size_bytes> [ready_file]
启动后打印 `SERVER_READY port=<p> sha256=<hex> size=<n>` 一行，供调用方核对源指纹。
"""
import hashlib
import os
import pathlib
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SIZE = 512 * 1024
PAYLOAD = b""
SHA = ""
PORT = 8199


def make_payload(n):
    """确定性字节：位置派生的简单模式，不依赖随机种子也能逐字节复算。"""
    return bytes(((i * 31 + (i >> 8) * 17 + (i >> 16) * 7) & 0xFF) for i in range(n))


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        pass  # 别让访问日志混进 E2E 读数

    def _send(self, code, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_HEAD(self):
        if self.path.split("?")[0] == "/payload.bin":
            self._send(200, b"", {"Content-Length": str(SIZE)})
        else:
            self._send(404, b"", {"Content-Length": "0"})

    def do_GET(self):
        path = self.path.split("?")[0]
        rng = self.headers.get("Range")
        if path != "/payload.bin":
            self._send(404, b"")
            return
        if not rng or not rng.startswith("bytes="):
            self._send(200, PAYLOAD)
            return
        try:
            spec = rng[6:].split(",")[0].strip()
            lo_s, hi_s = spec.split("-")
            lo = int(lo_s or 0)
            hi = int(hi_s) if hi_s else SIZE - 1
            hi = min(hi, SIZE - 1)
            if lo > hi or lo >= SIZE:
                self._send(416, b"")
                return
            chunk = PAYLOAD[lo:hi + 1]
            self._send(206, chunk, {
                "Content-Length": str(len(chunk)),
                "Content-Range": f"bytes {lo}-{hi}/{SIZE}",
            })
        except Exception:
            self._send(400, b"")


def main():
    global SIZE, PAYLOAD, SHA, PORT
    PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8199
    SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 512 * 1024
    ready = sys.argv[3] if len(sys.argv) > 3 else None
    PAYLOAD = make_payload(SIZE)
    SHA = hashlib.sha256(PAYLOAD).hexdigest()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    line = f"SERVER_READY port={PORT} sha256={SHA} size={SIZE}"
    print(line, flush=True)
    if ready:
        pathlib.Path(ready).write_text(line + "\n", encoding="utf-8")
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
