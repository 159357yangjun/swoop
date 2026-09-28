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
  * /retryafter.bin 首 GET 返回 429 + Retry-After: 3，后续 GET 返回 200：验证引擎重试时
    尊重服务器 Retry-After（而不是固定退避猛撞）。
  * /cut.bin 声明整份长度却只发前 8 KB 就关闭连接（提前断流），HEAD 故意不给
    Content-Length：验证「长度未知、一次流式取完」的路径不会把截断文件当成已完成，
    而是带 Range 从断点续传补齐。/cutnr.bin 同理断流但**不认 Range**：只能诚实失败。
  * /probe429.bin 首 HEAD 返回 429 + Retry-After: 2，之后 HEAD 正常 200：验证 HEAD 探测
    会重试并尊重 Retry-After —— 否则探测失败 → file_size=-1 → 不分段 → 退化成单连接。
  * 请求行是**绝对 URI**（http://…）时按「HTTP 代理」处理：没有正确的
    Proxy-Authorization 就回 407 + Proxy-Authenticate，用来测代理认证这条链路。
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

# /retryafter.bin：复现「重试应尊重 Retry-After」的真实缺口。
# 首 GET 回 429 + Retry-After: 3（模拟限流/过载），后续 GET 回 200 + 确定性内容。
# 引擎若尊重 Retry-After，重试等待≈3s；若只按固定 1s 退避，则≈1s —— 自测据此区分两者。
RETRYAFTER_PATH = "/retryafter.bin"
RETRYAFTER_PAYLOAD = make_payload(4096)   # 与 content_matches_pattern 同模式：byte[i] == i % 251
RETRYAFTER_AFTER = 3                      # Retry-After 秒数

# /cut.bin 与 /cutnr.bin：复现「服务器提前断流」。
# 两者都：HEAD **不给 Content-Length**（模拟分块传输 / 动态端点 —— 管理器拿不到总长度，
# 只能走「一次请求流式取完整个响应体」的路径），GET 声明整份长度却只发前 CUT_PART 字节
# 就关闭连接（Content-Length 未收满 = libcurl 的 CURLE_PARTIAL_FILE）。
#   /cut.bin  ：带 Range 的请求照常完整应答 → 断点续传能把文件补全。
#   /cutnr.bin：一律无视 Range、回 200 → 续传续不上，只能诚实失败。
# 为什么盯「长度未知」这条路径：已知总长度时 while (downloaded < total) 本身会兜住，
# 而这条路径没有任何字节数可对账 —— 截断文件被当成「已完成」的唯一入口。
CUT_PATH = "/cut.bin"
CUTNR_PATH = "/cutnr.bin"
CUT_TOTAL = 200000
CUT_PART = 8000
CUT_PAYLOAD = make_payload(CUT_TOTAL)

# /probe429.bin：复现「HEAD 探测被服务器瞬时限流」。
# 首 HEAD 回 429 + Retry-After: 2，之后 HEAD 回正常的 200 + Content-Length。
# 探测若不死重试也不看 Retry-After，管理器就永远拿不到大小 → 不分段 → 单连接下载。
PROBE429_PATH = "/probe429.bin"
PROBE429_PAYLOAD = make_payload(4096)
PROBE429_AFTER = 2

# ── 重定向 + 请求头回显：量「跳转到别的 host 时，什么东西跟着走了」──────────
# /redir.bin?to=<绝对URL>：**要求 Basic 认证**（必须先让客户端在原站完成认证，
#   才谈得上"凭据会不会被带过去"），然后回 302 把客户端支到 to 指的地方。
#   to 只允许 127.0.0.1 / localhost 两个 host —— 本机测试服务，不做开放重定向。
# /sink.json：**免认证**，把收到的请求头原样回显成 JSON。
#   泄漏与否在这里取证；「同一 host」那一发是对照组，
#   没有对照就无法区分"确实没带过去"和"这里根本看不见"。
REDIR_PATH = "/redir.bin"
REDIRFREE_PATH = "/_redir.bin"
SINK_PATH = "/sink.json"

# ── 兼职扮演「要求 Basic 认证的 HTTP 代理」────────────────────────────
# curl 走 HTTP 代理时请求行是**绝对 URI**（GET http://host/path），据此把
# 「代理请求」和「源站请求」分开处理。为什么要单独一条分支：407 必须由代理这一侧
# 出来，它和 401（源站要登录）在客户端走完全不同的凭据与提示路径 —— 混在一起，
# 用户就分不出该去填「代理账号」还是「站点登录」。
PROXY_USER = "proxuser"
PROXY_PASS = "proxpass"
PROXY_EXPECT = "Basic " + base64.b64encode(f"{PROXY_USER}:{PROXY_PASS}".encode()).decode()
PROXY_BODY = make_payload(4096)     # 与 content_matches_pattern 同模式：byte[i] == i % 251

# /stall.bin：连接建立后**先睡 IDM_SELFTEST_STALL_MS（默认 3000）毫秒再回东西**，
# HEAD 与 GET 都睡。用来量「探测把调用线程占住多久」——服务器慢是真实场景
# （海外源站 / CDN 拥塞 / TLS 握手排队），而 GUI 线程一旦被占住，整个窗口就不响应了。
STALL_PATH = "/stall.bin"
STALL_MS = int(os.environ.get("IDM_SELFTEST_STALL_MS", "3000"))
STALL_PAYLOAD = make_payload(4096)

_lock = threading.Lock()
_hits = {"auth_fail": 0, "auth_ok": 0, "ranges": 0, "cur": 0, "max_cur": 0,
         "norange_get": 0, "norange_range": 0, "retryafter_gets": 0,
         "cut_range": 0, "cutnr_ignored": 0, "probe429_heads": 0, "sink_hits": 0,
         "proxy_req": 0, "proxy_407": 0, "proxy_ok": 0, "stall_hits": 0}
_RESET_KEYS = {"auth_fail": 0, "auth_ok": 0, "ranges": 0, "cur": 0, "max_cur": 0,
               "norange_get": 0, "norange_range": 0, "retryafter_gets": 0,
               "cut_range": 0, "cutnr_ignored": 0, "probe429_heads": 0,
               "sink_hits": 0, "proxy_req": 0, "proxy_407": 0, "proxy_ok": 0,
               "stall_hits": 0}


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
        if path.startswith("http://"):
            return self._serve_proxy()      # 代理侧的 HEAD：不带响应体
        if path == "/_stats":
            with _lock:
                return self._send_json(dict(_hits))
        if path == "/_reset":
            with _lock:
                _hits.update(_RESET_KEYS)
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
        if path == RETRYAFTER_PATH:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(RETRYAFTER_PAYLOAD)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            return
        if path in (CUT_PATH, CUTNR_PATH):
            # 刻意不回 Content-Length：让探测得到 file_size=-1，任务只能走
            # 「长度未知、一次流式取完」这条没有字节数可对账的路径。
            # HEAD 绝不能带响应体（理由见 _write_body 的说明）。
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            return
        if path == PROBE429_PATH:
            # 只限流首 HEAD：探测重试并尊重 Retry-After 的话，第二次就拿到正常大小。
            with _lock:
                _hits["probe429_heads"] += 1
                limited = _hits["probe429_heads"] == 1
            if limited:
                self.send_response(429)
                self.send_header("Retry-After", str(PROBE429_AFTER))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(PROBE429_PAYLOAD)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            return
        if path == STALL_PATH:
            # 先睡再回：模拟"源站很慢"。测的是调用线程被占住多久，不是字节数。
            with _lock:
                _hits["stall_hits"] += 1
            time.sleep(STALL_MS / 1000.0)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(STALL_PAYLOAD)))
            self.send_header("Accept-Ranges", "bytes")
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
        if path.startswith("http://"):
            return self._serve_proxy()      # 绝对 URI = 走代理的请求，先于源站逻辑处理
        if path == "/_stats":
            with _lock:
                return self._send_json(dict(_hits))
        if path == "/_reset":
            with _lock:
                _hits.update(_RESET_KEYS)
            return self._send_json({"ok": 1})
        if path == SINK_PATH:
            # 免认证：sink 的任务是「看客户端主动带了什么过来」，自己不设门槛
            return self._serve_sink()

        if path == REDIRFREE_PATH:
            # 免认证的跳转：用来测「客户端根本没配凭据/Cookie/Referer 时，
            # 会不会被 libcurl 自动补一个 Referer（= 把带 token 的源 URL 递出去）」
            return self._serve_redirect()

        if not self._auth_ok():
            return self._send_401()

        if path == REDIR_PATH:
            # 到这里说明客户端已经在本 host 通过 Basic 认证 —— 接下来看它跳走时带什么
            return self._serve_redirect()

        if path == RETRYAFTER_PATH:
            # 仅首 GET 回 429 + Retry-After；后续 GET 回 200 + 确定性内容。
            # 引擎若尊重 Retry-After，重试等待≈RETRYAFTER_AFTER 秒；否则按固定 1s 退避。
            with _lock:
                _hits["retryafter_gets"] += 1
                first = _hits["retryafter_gets"] == 1
            if first:
                self.send_response(429)
                self.send_header("Retry-After", str(RETRYAFTER_AFTER))
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(RETRYAFTER_PAYLOAD)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            self.wfile.write(RETRYAFTER_PAYLOAD)
            return

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

        if path == CUT_PATH:
            _enter()
            try:
                self._serve_cut(honor_range=True)
            finally:
                _leave()
            return

        if path == CUTNR_PATH:
            _enter()
            try:
                self._serve_cut(honor_range=False)
            finally:
                _leave()
            return

        if path == PROBE429_PATH:
            _enter()
            try:
                self._serve_file(PROBE429_PAYLOAD)
            finally:
                _leave()
            return

        if path == STALL_PATH:
            with _lock:
                _hits["stall_hits"] += 1
            time.sleep(STALL_MS / 1000.0)
            _enter()
            try:
                self._serve_file(STALL_PAYLOAD)
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

    def _serve_redirect(self):
        """302 到 ?to= 指定的地址。to 只允许本机 127.0.0.1 / localhost 两个 host 串，
        既够模拟「跳到另一个 host」，也不会把自己变成开放重定向。"""
        raw = self.path.split("?", 1)[1] if "?" in self.path else ""
        to = ""
        if raw.startswith("to=http://127.0.0.1:") or raw.startswith("to=http://localhost:"):
            to = raw[3:]      # 只去 "to="；本端点由自测自己拼，原样传不再解码
        if not to:
            body = b"bad to\n"
            self.send_response(400)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self._write_body(body)
            return
        self.send_response(302)
        self.send_header("Location", to)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _serve_proxy(self):
        """要求 Basic 认证的 HTTP 代理：没有（或不对的）Proxy-Authorization 就回 407。

        这是真的代理行为，不是模拟：请求行是绝对 URI、认证头是 Proxy-Authorization、
        挑战头是 Proxy-Authenticate，状态码是 407 而不是 401。
        自测靠它证明两件事：① 没填代理账号时报错文案指向「代理」而不是站点；
        ② 填了之后代理凭据真的下发到了下载线程（而不是只存在设置页里）。
        """
        got = self.headers.get("Proxy-Authorization", "")
        with _lock:
            _hits["proxy_req"] += 1
        if got != PROXY_EXPECT:
            with _lock:
                _hits["proxy_407"] += 1
            self.send_response(407)
            self.send_header("Proxy-Authenticate", 'Basic realm="idm-selftest-proxy"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with _lock:
            _hits["proxy_ok"] += 1
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(PROXY_BODY)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("X-Proxied-Url", self.path)
        self.end_headers()
        self._write_body(PROXY_BODY)

    def _serve_sink(self):
        """把收到的请求头回显成 JSON —— 重定向泄漏的取证点。

        ⚠️ 必须有「同一 host」那一发作对照：没有对照，"这里没看到 Authorization"
        既可能是客户端真的没带，也可能是这个观测点根本收不到，两者完全不同。
        """
        with _lock:
            _hits["sink_hits"] += 1
            hits = _hits["sink_hits"]
        self._send_json({
            "host":    self.headers.get("Host", ""),
            "auth":    self.headers.get("Authorization", ""),
            "cookie":  self.headers.get("Cookie", ""),
            "referer": self.headers.get("Referer", ""),
            "ua":      self.headers.get("User-Agent", ""),
            "hits":    hits,
        })

    def _serve_cut(self, honor_range: bool):
        """声明整份长度却只发前 CUT_PART 字节，然后关闭连接 —— 复现「服务器提前断流」。

        ⚠️ 必须显式置 close_connection：HTTP/1.1 keep-alive 下，Content-Length 没收满
        就从 handler 返回，连接其实并不会断，客户端只会一直等到超时——那样测到的是
        CURLE_OPERATION_TIMEDOUT 而不是 CURLE_PARTIAL_FILE，断言就指错了地方。
        honor_range=False 时连 Range 也不认（回 200 + 整份），用来验证「续不上就诚实失败」。
        """
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes=") and honor_range:
            with _lock:
                _hits["cut_range"] += 1
            spec = rng[len("bytes="):].split(",")[0].strip()
            s, _, e = spec.partition("-")
            start = int(s) if s else 0
            end = int(e) if e else CUT_TOTAL - 1
            end = min(end, CUT_TOTAL - 1)
            if start > end or start >= CUT_TOTAL:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{CUT_TOTAL}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = CUT_PAYLOAD[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Range", f"bytes {start}-{end}/{CUT_TOTAL}")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            self.wfile.write(body)
            return

        if rng and not honor_range:
            with _lock:
                _hits["cutnr_ignored"] += 1
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(CUT_TOTAL))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(CUT_PAYLOAD[:CUT_PART])
        self.close_connection = True

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
