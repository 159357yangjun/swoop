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
PAGES = {}
MEDIA = {}
DOC_TEXT = b""


def make_payload(n):
    """确定性字节：位置派生的简单模式，不依赖随机种子也能逐字节复算。"""
    return bytes(((i * 31 + (i >> 8) * 17 + (i >> 16) * 7) & 0xFF) for i in range(n))


# ── A/B 面用的静态页面与媒体（本轮识别测试专用）───────────────────
MEDIA = {}


def build_media():
    """两份小媒体（确定性字节，带 mp4 ftyp 头），各 200KiB。"""
    head = bytes([0, 0, 0, 0x18]) + b"ftypmp42"
    return {"/media/network.mp4": head + bytes(200_000),
            "/media/dom.mp4": head + bytes([1] * 200_000)}


def build_pages(port):
    # A 面：页面里**不放** <video>，只靠 JS fetch 触发 /media/network.mp4；
    #       同时抓一份 text/plain 的 /document.txt 当阴性对照——它不该被列成视频资源。
    net_hits = "window.__netHits=(window.__netHits||0)+1;"
    doc_hits = "window.__docHits=(window.__docHits||0)+1;"
    network_html = (
        "<!doctype html><meta charset=utf-8><title>A network sniff</title>"
        "<h1>network-only</h1><script>"
        + "fetch('/media/network.mp4').then(function(r){return r.blob()}).then(function(b){" + net_hits + "});"
        + "fetch('/document.txt').then(function(r){return r.text()}).then(function(t){" + doc_hits + "});"
        + "</script>")

    # B 面：初始 DOM **没有**媒体元素，延迟注入 <video><source>，
    #       考的是 scanMediaElements() 与 MutationObserver，不是"静态页面启动时扫到"。
    src_tag = '<source src="/media/dom.mp4" type="video/mp4">'
    dom_html = (
        "<!doctype html><meta charset=utf-8><title>B dom scan</title>"
        "<h1>dom-later</h1><div id=host></div><script>"
        "window.__insertedAt=0;"
        "setTimeout(function(){"
        "var v=document.createElement('video');"
        "v.innerHTML='" + src_tag + "';"
        "document.getElementById('host').appendChild(v);"
        "window.__insertedAt=Date.now();"
        "},3000);</script>")
    return {"/network.html": network_html.encode("utf-8"),
            "/dom.html": dom_html.encode("utf-8")}


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
        path = self.path.split("?")[0]
        if path == "/payload.bin":
            self._send(200, b"", {"Content-Length": str(SIZE)})
        elif path in PAGES or path in MEDIA:
            body = PAGES.get(path) or MEDIA.get(path)
            self._send(200, b"", {"Content-Length": str(len(body))})
        else:
            self._send(404, b"", {"Content-Length": "0"})

    def do_GET(self):
        path = self.path.split("?")[0]
        rng = self.headers.get("Range")
        if path in PAGES:
            self._send(200, PAGES[path], {"Content-Type": "text/html; charset=utf-8"})
            return
        if path == "/document.txt":
            self._send(200, DOC_TEXT, {"Content-Type": "text/plain; charset=utf-8"})
            return
        if path in MEDIA:
            blob = MEDIA[path]
            if not rng:
                self._send(200, blob, {"Content-Type": "video/mp4"})
                return
            lo_s, hi_s = rng[6:].split(",")[0].strip().split("-")
            lo = int(lo_s or 0)
            hi = int(hi_s) if hi_s else len(blob) - 1
            hi = min(hi, len(blob) - 1)
            chunk = blob[lo:hi + 1]
            self._send(206, chunk, {"Content-Type": "video/mp4",
                                    "Content-Length": str(len(chunk)),
                                    "Content-Range": f"bytes {lo}-{hi}/{len(blob)}"})
            return
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
    global PAGES, MEDIA, DOC_TEXT
    PAYLOAD = make_payload(SIZE)
    MEDIA = build_media()
    PAGES = build_pages(PORT)
    DOC_TEXT = b"not a media resource -- negative control for webRequest sniffing" + bytes([10])
    SHA = hashlib.sha256(PAYLOAD).hexdigest()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    line = f"SERVER_READY port={PORT} sha256={SHA} size={SIZE}"
    print(line, flush=True)
    if ready:
        pathlib.Path(ready).write_text(line + "\n", encoding="utf-8")
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
