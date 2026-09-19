#!/usr/bin/env python3
"""带 Basic 认证的本地 HTTP 测试服务（供 tools/engine_selftest.c 使用）。

用法: python engine_selftest_server.py [port] [user] [pass]

行为:
  * 除 / 之外的所有路径都要求 Basic 认证；未通过返回 401 且**不返回文件**，
    这样自测里「无凭据必须失败」才是有效对照。
  * 支持 Range 请求（206），让引擎能真正分段下载。
   * /secret.bin 固定返回 12 MiB 确定性数据（12 MiB / 4 MiB = 3 片，
    用来验证「每服务器连接数」对分片数的封顶效果）。
   * /huge.bin 返回 40 MiB：40 MiB / 4 MiB = 10 片，用于验证**真实并发连接数**
     是否被 max_conn_per_server 封住（分片数只是弱代理，并发数才是这个设置的本意）。
   * /_stats 返回并发统计（免认证）；/_reset 清零统计（免认证）。
  * **每个文件响应固定延迟 IDM_SELFTEST_SLOW_MS 毫秒（默认 40）**：
    本机回环太快（1 MiB 一毫秒内写完），不延迟的话「并发」根本来不及重叠，
    实测并发峰值只有 2~3，无法区分「真封顶」和「跑太快没重叠」。
    设为 0 可关闭延迟（此时并发断言不可信）。
"""
import base64
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
USER = sys.argv[2] if len(sys.argv) > 2 else "idmuser"
PASS = sys.argv[3] if len(sys.argv) > 3 else "idmpass"

# 每个文件响应的固定延迟（毫秒）。见模块 docstring 里的说明：本机回环太快，
# 不人为放慢就无法观测并发，并发断言会变得不可信。
SLOW_MS = int(os.environ.get("IDM_SELFTEST_SLOW_MS", "40"))

EXPECTED = "Basic " + base64.b64encode(f"{USER}:{PASS}".encode()).decode()

# 确定性内容：非零、非重复，便于发现错位/空洞
def make_payload(n: int) -> bytes:
    # 用 i % 251 保证字节分布均匀且可复现
    return bytes((i % 251) for i in range(n))


PAYLOAD_12M = make_payload(12 * 1024 * 1024)
PAYLOAD_40M = make_payload(40 * 1024 * 1024)

FILES = {
    "/secret.bin": PAYLOAD_12M,
    "/big.bin": PAYLOAD_12M,
    "/huge.bin": PAYLOAD_40M,
    "/small.bin": make_payload(1024 * 1024),
}

# ── 两个「刁难」端点：分别复现两个真实缺陷 ──────────────────────────
# /norange.bin   ：HEAD 里**照样宣称** Accept-Ranges: bytes（真实 CDN/防盗链就是这么
#                  骗人的），GET 时却一律忽略 Range、回 200 + 整份文件。
#                  引擎若照旧按分片偏移顺序写，会把「从 0 开始的全量数据」灌进分片位置，
#                  产出一个大小看着对、内容却错位、状态还显示「已完成」的文件。
# /bigoffset.bin ：声明 3 GiB + 8 KiB（跨越 2^31）。只按请求的范围现算字节，
#                  绝不真的准备 3 GiB。用来直接探 32 位 long 偏移截断。
NORANGE_PATH = "/norange.bin"
BIGOFF_PATH = "/bigoffset.bin"
NORANGE_SIZE = len(PAYLOAD_12M)
BIGOFF_SIZE = 3 * 1024 * 1024 * 1024 + 8192
BIGOFF_MAX_BODY = 8 * 1024 * 1024   # 单次最多生成的字节：防止有人真按整份请求

_lock = threading.Lock()
_hits = {"auth_fail": 0, "auth_ok": 0, "ranges": 0, "cur": 0, "max_cur": 0,
         "norange_get": 0, "norange_range": 0}


def _enter():
    with _lock:
        _hits["cur"] += 1
        if _hits["cur"] > _hits["max_cur"]:
            _hits["max_cur"] = _hits["cur"]


def _leave():
    with _lock:
        _hits["cur"] -= 1


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):   # 安静一点
        pass

    def _auth_ok(self) -> bool:
        got = self.headers.get("Authorization", "")
        if got == EXPECTED:
            with _lock:
                _hits["auth_ok"] += 1
            return True
        with _lock:
            _hits["auth_fail"] += 1
        return False

    def _write_body(self, body: bytes):
        """HEAD 响应绝不能带响应体（RFC 9110 §9.3.2）。

        ⚠️ 这里踩过坑：服务端若对 HEAD 也写 body，keep-alive 连接上这段字节会被
        客户端当成「下一个响应的开头」。于是走「先收 401 挑战、再带凭据重试」这条
        路（libcurl 的 CURLAUTH_ANY、curl --anyauth）的客户端在 HEAD 上永远拿不到
        200 —— 现象是探测失败但下载正常，实测 `curl -I --anyauth -u` 也复现 401，
        而 `curl -I -u`（Basic 预发送、无挑战往返）是 200。"""
        if self.command == "HEAD":
            return
        self.wfile.write(body)

    def _send_json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self._write_body(body)

    def _send_401(self):
        body = b"authentication required\n"
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="idm-selftest"')
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self._write_body(body)

    def do_HEAD(self):
        path = self.path.split("?")[0]
        if path == "/_stats":
            with _lock:
                return self._send_json(dict(_hits))
        if path == "/_reset":
            with _lock:
                _hits.update({"auth_fail": 0, "auth_ok": 0, "ranges": 0,
                              "cur": 0, "max_cur": 0,
                              "norange_get": 0, "norange_range": 0})
            return self._send_json({"ok": 1})
        if not self._auth_ok():
            return self._send_401()
        if path == BIGOFF_PATH:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(BIGOFF_SIZE))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            return
        if path == NORANGE_PATH:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(NORANGE_SIZE))
            self.send_header("Accept-Ranges", "bytes")   # 骗人的：GET 时根本不认 Range
            self.end_headers()
            return
        data = FILES.get(self.path.split("?")[0])
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/_stats":
            with _lock:
                return self._send_json(dict(_hits))
        if path == "/_reset":
            with _lock:
                _hits.update({"auth_fail": 0, "auth_ok": 0, "ranges": 0,
                              "cur": 0, "max_cur": 0,
                              "norange_get": 0, "norange_range": 0})
            return self._send_json({"ok": 1})
        if not self._auth_ok():
            return self._send_401()

        if path == NORANGE_PATH:
            _enter()
            try:
                ranged = 1 if self.headers.get("Range") else 0
                with _lock:
                    _hits["norange_get"] += 1
                    _hits["norange_range"] += ranged
                if SLOW_MS > 0:
                    time.sleep(SLOW_MS / 1000.0)
                # 无视 Range：永远回 200 + 整份文件
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(NORANGE_SIZE))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                self.wfile.write(PAYLOAD_12M)
            finally:
                _leave()
            return

        if path == BIGOFF_PATH:
            _enter()
            try:
                start, end = 0, BIGOFF_SIZE - 1
                rng = self.headers.get("Range")
                if rng and rng.startswith("bytes="):
                    spec = rng[len("bytes="):].split(",")[0].strip()
                    s, _, e = spec.partition("-")
                    if s:
                        start = int(s)
                    if e:
                        end = min(int(e), BIGOFF_SIZE - 1)
                    if start > end or start >= BIGOFF_SIZE:
                        self.send_response(416)
                        self.send_header("Content-Range", f"bytes */{BIGOFF_SIZE}")
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                n = end - start + 1
                if n > BIGOFF_MAX_BODY:
                    # 想整份拉走 3 GiB？这不是本用例的目的，直接拒。
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{BIGOFF_SIZE}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                body = bytes(((start + k) % 251) for k in range(n))
                self.send_response(206 if rng else 200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Range", f"bytes {start}-{end}/{BIGOFF_SIZE}")
                self.send_header("Content-Length", str(n))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                self.wfile.write(body)
            finally:
                _leave()
            return

        data = FILES.get(path)
        if data is None:
            body = b"not found\n"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        _enter()
        try:
            self._serve_file(data)
        finally:
            _leave()

    def _serve_file(self, data: bytes):
        if SLOW_MS > 0:
            # 占着并发槽位稍等一下：让并发的多个请求真正重叠，否则「并发峰值」测不出来
            time.sleep(SLOW_MS / 1000.0)
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            with _lock:
                _hits["ranges"] += 1
            spec = rng[len("bytes="):].split(",")[0].strip()
            start_s, _, end_s = spec.partition("-")
            start = int(start_s) if start_s else 0
            end = int(end_s) if end_s else len(data) - 1
            end = min(end, len(data) - 1)
            if start > end or start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(data)}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            chunk = data[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
            self.send_header("Content-Length", str(len(chunk)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            self.wfile.write(chunk)
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if not self._auth_ok():
            return self._send_401()
        n = int(self.headers.get("Content-Length", "0"))
        if n:
            self.rfile.read(n)
        body = b"ok\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def _summary():
    print(f"[server] auth_ok={_hits['auth_ok']} auth_fail={_hits['auth_fail']} "
          f"range_requests={_hits['ranges']}", flush=True)
    if _hits["ranges"] == 0:
        print("[server] 警告：没有收到任何 Range 请求，分段逻辑可能没跑起来", flush=True)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print(f"[server] listening on http://127.0.0.1:{PORT} user={USER} "
          f"slow={SLOW_MS}ms", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        _summary()
