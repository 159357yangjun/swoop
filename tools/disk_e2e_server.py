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
HLS = {}
ENC_EXPECT = b""
TS_EXPECT = b""
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


def build_hls():
    """最小本机 HLS fixture：master.m3u8 → media.m3u8 → seg0/1/2.ts。

    分片是**确定性伪 TS**（以 0x47 开头、固定载荷），不是真 MPEG-TS：本轮要验的是
    「扩展能不能识别 .m3u8」与「缺 ffmpeg 时产品给什么反应」，不是转码正确性——
    那需要真 TS 流，属第 5 步之后的事。用 chr(10) 而不是字面 \\n，是为了让行尾在源码里
    不被任何 shell/转义层吃掉（这条在本会话里踩过三次）。
    """
    NL = chr(10)
    def seg(n):
        return bytes([0x47]) + bytes(((i * 13 + n) & 0xFF) for i in range(187)) * 40

    master = NL.join([
        "#EXTM3U",
        '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.4d401d"',
        "media.m3u8",
        "",
    ]).encode("utf-8")
    media = NL.join([
        "#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:0",
        "#EXTINF:2.0,", "seg0.ts",
        "#EXTINF:2.0,", "seg1.ts",
        "#EXTINF:2.0,", "seg2.ts",
        "#EXT-X-ENDLIST", "",
    ]).encode("utf-8")
    return {
        "/hls/master.m3u8": (master, "application/vnd.apple.mpegurl"),
        "/hls/media.m3u8": (media, "application/vnd.apple.mpegurl"),
        "/hls/seg0.ts": (seg(0), "video/mp2t"),
        "/hls/seg1.ts": (seg(1), "video/mp2t"),
        "/hls/seg2.ts": (seg(2), "video/mp2t"),
    }


def build_enc_hls():
    """AES-128 加密 HLS fixture（步骤 5：验 fetchKey 分支是否承重）。

    设计要点，都是为了「期望值能独立重算」而不是拿被测实现的输出当答案：
      · 密钥固定 16 字节 ⇒ 产品 `onKeyFinished` 的长度校验会过（hls_downloader.cpp:390）。
      · 明文分片长度是 **16 的整数倍** ⇒ 否则产品会打「跳过解密」并原样落盘（:476/:483），
        那测的就不是解密路径了。
      · **不写 IV=** ⇒ 强制走产品的"序号大端作 IV"分支（segmentIv()，:402-413），
        这条最容易写错（大小端、从 0 还是 1 起）。显式 IV 反而把它绕过去了。
      · 密文用 cryptography 库现算，与产品里那份手写 AES 是两套实现 ⇒ 二者一致才算证据。
    """
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    NL = chr(10)
    key = bytes(range(1, 17))                      # 0x01..0x10，固定可复算
    plain = []
    for n in range(3):
        body = bytes(((i * 11 + n * 7) & 0xFF) for i in range(16 * 60))   # 960 字节 = 16×60
        plain.append(body)

    def iv_of(idx):                                # 与 HLS spec / 产品 segmentIv() 同规则：大端序号
        return bytes(12) + bytes([(idx >> 24) & 0xFF, (idx >> 16) & 0xFF, (idx >> 8) & 0xFF, idx & 0xFF])

    enc = []
    for idx, p in enumerate(plain):
        c = Cipher(algorithms.AES(key), modes.CBC(iv_of(idx))).encryptor()
        enc.append(c.update(p) + c.finalize())

    media = NL.join([
        "#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:0",
        '#EXT-X-KEY:METHOD=AES-128,URI="key.bin"',
        "#EXTINF:2.0,", "eseg0.ts",
        "#EXTINF:2.0,", "eseg1.ts",
        "#EXTINF:2.0,", "eseg2.ts",
        "#EXT-X-ENDLIST", "",
    ]).encode("utf-8")
    master = NL.join([
        "#EXTM3U",
        '#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=320x180,CODECS="avc1.4d401d"',
        "emedia.m3u8",
        "",
    ]).encode("utf-8")
    routes = {
        "/hls/emaster.m3u8": (master, "application/vnd.apple.mpegurl"),
        "/hls/emedia.m3u8": (media, "application/vnd.apple.mpegurl"),
        "/hls/key.bin": (key, "application/octet-stream"),
    }
    for i, e in enumerate(enc):
        routes[f"/hls/eseg{i}.ts"] = (e, "video/mp2t")
    # 期望产物 = 三段明文按序拼接（独立于被测实现算出来的答案）
    return routes, b"".join(plain), key


def build_real_ts_hls():
    """真 MPEG-TS fixture（步骤 B）：PAT/PMT/连续视频包，分片长度是 188 的整数倍。

    与伪 TS 的区别在于它**可被独立判读**（tools/ts_fixture.audit_ts），
    于是能问一个更硬的问题：产品 concat 出来的文件还是不是合法 TS、
    PCR 跨度是否等于 #EXTINF 声明的时长。伪 TS 只能验字节搬运。
    """
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import ts_fixture
    NL = chr(10)
    segs = [ts_fixture.build_segment(60, i * 90000 * 2, i * 90000 * 2, i) for i in range(3)]
    media = NL.join([
        "#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:0",
        "#EXTINF:2.0,", "tseg0.ts",
        "#EXTINF:2.0,", "tseg1.ts",
        "#EXTINF:2.0,", "tseg2.ts",
        "#EXT-X-ENDLIST", "",
    ]).encode("utf-8")
    master = NL.join([
        "#EXTM3U",
        '#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=640x360,CODECS="avc1.4d401d"',
        "tmedia.m3u8",
        "",
    ]).encode("utf-8")
    routes = {"/hls/tmaster.m3u8": (master, "application/vnd.apple.mpegurl"),
              "/hls/tmedia.m3u8": (media, "application/vnd.apple.mpegurl")}
    for i, sg in enumerate(segs):
        routes[f"/hls/tseg{i}.ts"] = (sg, "video/mp2t")
    return routes, b"".join(segs)


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
            "/dom.html": dom_html.encode("utf-8"),
            # HLS 播放页：用 <video> 指向 master.m3u8（浏览器自己不播，但 DOM 里就有这个 URL）
            "/hls.html": (
                "<!doctype html><meta charset=utf-8><title>HLS page</title>"
                "<h1>hls-later</h1><div id=host></div><script>"
                "setTimeout(function(){"
                "var v=document.createElement('video');"
                "v.innerHTML='" + '<source src="/hls/master.m3u8" type="application/vnd.apple.mpegurl">' + "';"
                "document.getElementById('host').appendChild(v);"
                "window.__hlsAt=Date.now();"
                "},1500);</script>").encode("utf-8")}


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
        elif path in HLS:
            self._send(200, b"", {"Content-Length": str(len(HLS[path][0]))})
        elif path in PAGES or path in MEDIA:
            body = PAGES.get(path) or MEDIA.get(path)
            self._send(200, b"", {"Content-Length": str(len(body))})
        else:
            self._send(404, b"", {"Content-Length": "0"})

    def do_GET(self):
        path = self.path.split("?")[0]
        rng = self.headers.get("Range")
        if path in HLS:
            blob, ctype = HLS[path]
            self._send(200, blob, {"Content-Type": ctype})
            return
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
    global PAGES, MEDIA, HLS, ENC_EXPECT, TS_EXPECT, DOC_TEXT
    PAYLOAD = make_payload(SIZE)
    MEDIA = build_media()
    HLS = build_hls()
    enc_routes, enc_expect, _k = build_enc_hls()
    HLS.update(enc_routes)
    ENC_EXPECT = enc_expect
    ts_routes, ts_expect = build_real_ts_hls()
    HLS.update(ts_routes)
    TS_EXPECT = ts_expect
    PAGES = build_pages(PORT)
    DOC_TEXT = b"not a media resource -- negative control for webRequest sniffing" + bytes([10])
    SHA = hashlib.sha256(PAYLOAD).hexdigest()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    line = (f"SERVER_READY port={PORT} sha256={SHA} size={SIZE} "
            f"enc_sha256={hashlib.sha256(ENC_EXPECT).hexdigest()} enc_size={len(ENC_EXPECT)} "
            f"ts_sha256={hashlib.sha256(TS_EXPECT).hexdigest()} ts_size={len(TS_EXPECT)}")
    print(line, flush=True)
    if ready:
        pathlib.Path(ready).write_text(line + "\n", encoding="utf-8")
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
