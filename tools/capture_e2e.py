#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""④ capture 卡片真浏览器 E2E —— 用真实 Edge + 真实扩展，跑「可交互」而不是「存在」。

为什么不能沿用 tools/ext_shot/run.sh（它已经把卡片画出来并量了几何）：
  content.js 是**隔离世界**的 IIFE，页面里没有任何入口能调 showCaptureDialog；
  那个 harness 是在页面里 `<script src=content.js>` **原样引入** + 自己 stub chrome.*。
  ⇒ 渲染/几何面它是真的（本脚本不重复它的判据），但「点了之后真发到 IDM」它证不了：
    它的 sendMessage 是本机字典，永远 success:true。

本脚色的分工（只补它证不了的那一段）：
  · 触发 = **真实用户路径**：hover <video> → 浮出「下载此视频」→ CDP 真鼠标点它 → 卡片弹出
  · 输入 = CDP Input.dispatchMouseEvent 真点击聚焦 + insertText 真键入（不是 .value=）
  · 画质下拉 = **真 host 数据**：卡片向 SW 发 listFormats → host.exe → 本机 HLS fixture
  · 提交 = 真 downloadWithOptions → 真 sendNativeMessage → 真 idm-next-host.exe
            → 真命名管道 → 真 GUI
  · 回填 = 卡片自删 + toast 文案 + **GUI 自己写的 `IPC add 参数:` 行**（含 format/hlsVariant）

用法：python tools/capture_e2e.py
退出码 0 = 全部判据绿。临时产物按 LOGDIR 原样留档（被报告引用过，不当散件清）。
"""
import base64
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXT_DIR = ROOT / "browser-extension"
HOST_EXE = EXT_DIR / "native-messaging-host" / "idm-next-host.exe"
APP_EXE = ROOT / "build" / "idm-next.exe"
GEN_MANIFEST = (pathlib.Path(os.environ["LOCALAPPDATA"])
                / "IDMNext" / "NativeMessagingHosts" / "com.tencent.idm_next.json")
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Google/Chrome/Application/chrome.exe",
]
CDP_PORT = 9451
HTTP_PORT = 8251
PIPE_NAME = r"\.\pipe\idm-next-ipc"

BANDWIDTH_HI = "6239287"     # 与 fixture master 一致（对齐真站 mux.dev 的两个变体）
BANDWIDTH_LO = "246440"


def seg_blob(seed, n=4096):
    """一个可辨识的"分片"：内容随 seed 变化 ⇒ 高/低档拼出来的文件必然不同。"""
    return bytes((seed * 7 + i) & 0xFF for i in range(n))


def ts_list(names, seed0):
    """把若干 .ts 拼成子播放列表对应的期望产物（引擎无 ffmpeg 时按序拼接）。"""
    return b"".join(seg_blob(seed0 + i) for i in range(len(names)))


HI_SEGS = ["hseg0.ts", "hseg1.ts", "hseg2.ts"]
LO_SEGS = ["lseg0.ts", "lseg1.ts", "lseg2.ts"]
HI_EXPECT = ts_list(HI_SEGS, 100)
LO_EXPECT = ts_list(LO_SEGS, 900)

LOG = []


def say(s):
    # 控制台可能是 GBK（本机实测）：⇒ / ∈ 这类字符直接 print 会 UnicodeEncodeError，
    # 把整轮跑在最后一句上炸掉。读数档才是权威副本，控制台只尽力显示。
    txt = str(s)
    try:
        print(txt, flush=True)
    except UnicodeEncodeError:
        print(txt.encode("gbk", "replace").decode("gbk", "replace"), flush=True)
    LOG.append(txt)


def sha12(p):
    import hashlib
    try:
        return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()[:12]
    except Exception:
        return "?"


def read_gbk(path):
    try:
        return pathlib.Path(path).read_bytes().decode("gbk", "replace")
    except Exception:
        return ""


# ────────────────────────── 本机 fixture：HLS master + 页面 ──────────────────────────
MASTER = (
    "#EXTM3U\n#EXT-X-VERSION:3\n"
    f"#EXT-X-STREAM-INF:BANDWIDTH={BANDWIDTH_HI},RESOLUTION=1080x1920,CODECS=\"avc1.640028\"\n"
    "media_hi.m3u8\n"
    f"#EXT-X-STREAM-INF:BANDWIDTH={BANDWIDTH_LO},RESOLUTION=96x540,CODECS=\"avc1.4d401f\"\n"
    "media_lo.m3u8\n"
).encode()


def sub(names):
    L = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:0"]
    for n in names:
        L += ["#EXTINF:2.0,", n]
    L += ["#EXT-X-ENDLIST", ""]
    return "\n".join(L).encode()


def m3u8_routes():
    """master + 两条子列表 + 各自的 .ts：低/高档分片内容互不相同 ⇒ 产物能分辨选了哪一档。"""
    r = {"/hls/master.m3u8": (MASTER, "application/vnd.apple.mpegurl"),
         "/hls/media_hi.m3u8": (sub(HI_SEGS), "application/vnd.apple.mpegurl"),
         "/hls/media_lo.m3u8": (sub(LO_SEGS), "application/vnd.apple.mpegurl")}
    for i, n in enumerate(HI_SEGS):
        r["/hls/" + n] = (seg_blob(100 + i), "video/mp2t")
    for i, n in enumerate(LO_SEGS):
        r["/hls/" + n] = (seg_blob(900 + i), "video/mp2t")
    return r


ROUTES = m3u8_routes()

PAGE = f"""<!doctype html><html><head><meta charset="utf-8"><title>capture e2e</title></head>
<body style="margin:0;background:#111">
<video id="v" width="640" height="360" controls preload="metadata"
       src="http://127.0.0.1:{HTTP_PORT}/hls/master.m3u8"></video>
<p id="note">fixture page for IDM Next capture-card E2E</p>
</body></html>""".encode()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if self.command == "GET":
            self.wfile.write(body)

    def do_HEAD(self):
        p = self.path.split("?")[0]
        table = dict(ROUTES)
        table["/"] = (PAGE, "text/html; charset=utf-8")
        if p in table:
            self._send(table[p][0], table[p][1])
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_GET(self):
        p = self.path.split("?")[0]
        table = dict(ROUTES)
        table["/"] = (PAGE, "text/html; charset=utf-8")
        if p in table:
            self._send(*table[p])
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()


def http_json(path, timeout=5):
    with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode())


def port_free(p):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p))
        return True
    except OSError:
        return False
    finally:
        s.close()


def pipe_openable():
    """GUI 是否真在监听产品管道名（CreateFile 探一次，不发数据）。"""
    import ctypes
    GENERIC_READ, OPEN_EXISTING = 0x80000000, 3
    h = ctypes.windll.kernel32.CreateFileW(PIPE_NAME, GENERIC_READ, 3, None, OPEN_EXISTING, 0, None)
    INVALID = ctypes.c_void_p(-1).value
    if h == INVALID or h == -1:
        return False
    ctypes.windll.kernel32.CloseHandle(h)
    return True


def rid_from_key():
    """扩展 ID = manifest.key（RSA-SPKI DER）sha256 前 32 hex，逐位 a-p。
    现算而不是抄常量：抄来的 ID 一旦 manifest 换了 key 就会静默连到别的扩展上。"""
    import hashlib
    m = json.loads((EXT_DIR / "manifest.json").read_text(encoding="utf-8"))
    der = base64.b64decode(m["key"])
    digest = hashlib.sha256(der).hexdigest()[:32]
    return "".join(chr(ord("a") + int(c, 16)) for c in digest)


# ─────────────────────────────── 最小 CDP（复用 nm_e2e 的形状）───────────────────────────────
class Ws:
    def __init__(self, url):
        u = urlparse(url)
        self.host, self.path, self.port = u.hostname, u.path or "/", (u.port or 80)
        self.sock = socket.create_connection((self.host, self.port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {self.path} HTTP/1.1\r\nHost: {self.host}:{self.port}\r\n"
                           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            c = self.sock.recv(4096)
            if not c:
                raise RuntimeError("CDP 握手失败")
            buf += c
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self._id = 0

    def _exact(self, n):
        while len(self.buf) < n:
            c = self.sock.recv(65536)
            if not c:
                raise RuntimeError("CDP 断开")
            self.buf += c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, method, params=None, timeout=30, session=None):
        self._id += 1
        mid = self._id
        msg = {"id": mid, "method": method, "params": params or {}}
        if session:
            msg["sessionId"] = session
        raw = json.dumps(msg).encode()
        mask = os.urandom(4)
        head = bytearray([0x81, 0x80 | (len(raw) if len(raw) < 126 else 126)])
        if len(raw) >= 126:
            head += len(raw).to_bytes(2, "big")
        head += mask + bytes(b ^ mask[i % 4] for i, b in enumerate(raw))
        self.sock.sendall(bytes(head))
        end = time.time() + timeout
        while time.time() < end:
            try:
                obj = json.loads(self._recv_payload())
            except Exception:
                continue
            if obj.get("id") == mid and obj.get("sessionId") == session:
                return (obj.get("result", {}) if "error" not in obj
                        else {"__cdp_error__": obj["error"]})
        return {"__timeout__": method}

    def _recv_payload(self):
        hdr = self._exact(2)
        ln = hdr[1] & 0x7F
        if ln == 126:
            ln = int.from_bytes(self._exact(2), "big")
        elif ln == 127:
            ln = int.from_bytes(self._exact(8), "big")
        return self._exact(ln).decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def main():
    ts = time.strftime("%y%m%d-%H%M%S")
    logdir = pathlib.Path(os.environ["TEMP"]) / f"idm-capture-e2e-{ts}"
    logdir.mkdir(parents=True, exist_ok=True)
    guilog = logdir / "gui.log"
    edge_proc = srv = app_proc = None
    cdp = None
    rc = 1

    say("[身份] 仓根=%s HEAD=%s" % (ROOT, subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
        capture_output=True).stdout.decode().strip()))
    say(f"[身份] app_exe_sha12={sha12(APP_EXE)} host_exe_sha12={sha12(HOST_EXE)}")

    for p in (CDP_PORT, HTTP_PORT):
        if not port_free(p):
            say(f"[SETUP-INVALID] 端口 {p} 已被占用 ⇒ 读数会归错对象"); return 6
    if not GEN_MANIFEST.is_file():
        say(f"[SETUP-INVALID] native host manifest 不在册：{GEN_MANIFEST}"); return 6
    edge_bin = next((p for p in EDGE_CANDIDATES if pathlib.Path(p).exists()), None)
    if not edge_bin:
        say("[SETUP-INVALID] 没有 Edge/Chrome"); return 3

    try:
        # ── 1) fixture 服务器 ──
        srv = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        say(f"[fixture] http://127.0.0.1:{HTTP_PORT}/ ｜master 两档 BANDWIDTH="
            f"{BANDWIDTH_HI}/{BANDWIDTH_LO}")

        # ── 2) 真 GUI（独立数据目录；等它自己写「IPC server 已启动」）──
        data = logdir / "appdata"
        (data / "local").mkdir(parents=True)
        env = dict(os.environ, IDM_DATA_DIR=str(data), APPDATA=str(data),
                   LOCALAPPDATA=str(data / "local"),
                   QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
        app_proc = subprocess.Popen([str(APP_EXE)], cwd=str(ROOT / "build"), env=env,
                                    stdout=open(guilog, "wb"), stderr=subprocess.STDOUT)
        ready = False
        for _ in range(60):
            time.sleep(0.5)
            if "IPC server 已启动" in read_gbk(guilog):
                ready = True
                break
            if app_proc.poll() is not None:
                say(f"[FAIL] GUI 退出 rc={app_proc.returncode}"); return 4
        say(f"[GUI] pid={app_proc.pid} 监听就绪={ready}（日志按 gbk 解出）")
        if not ready:
            say("[FAIL] 30s 内没等到「IPC server 已启动」⇒ 后面所有读数无证人"); return 5
        say(f"[GUI] 产品命名管道可开={pipe_openable()}")

        # ── 3) 真 Edge + 真扩展（unpacked），空 profile ⇒ 首次加载即扩展更新页 ──
        args = [edge_bin, "--headless=new", f"--remote-debugging-port={CDP_PORT}",
                f"--user-data-dir={logdir / 'edge-profile'}", "--no-first-run",
                "--no-default-browser-check", "--disable-gpu",
                f"--load-extension={EXT_DIR}",
                f"--disable-extensions-except={EXT_DIR}", "about:blank"]
        edge_proc = subprocess.Popen(args)
        ver = None
        for _ in range(60):
            try:
                ver = http_json("/json/version")
                break
            except Exception:
                time.sleep(0.5)
        if not ver:
            say("[FAIL] CDP 无响应"); return 5
        say(f"[浏览器] {ver.get('Browser', '?')}")
        cdp = Ws(ver["webSocketDebuggerUrl"])

        # ── 4) SETUP-VALID：扩展必须在册（走 service worker target，别靠 popup 页面）──
        sw = None
        for _ in range(40):
            tg = http_json("/json")
            sw = next((t for t in tg if t.get("type") == "service_worker"
                       and "background.js" in str(t.get("url", ""))), None)
            if sw:
                break
            time.sleep(0.5)
        if not sw:
            say("[SETUP-INVALID] /json 里没有扩展 service_worker target ⇒ 扩展没起来"); return 6
        say(f"[扩展] SW target 在册 url={sw.get('url')}")

        # 扩展自身的可注入执行页：popup.html（沿用 nm_e2e 的既有做法——它已证明这个上下文里
        # chrome.runtime.sendNativeMessage 可用）。不靠"Edge 恰好开了扩展更新页"。
        ext_id = rid_from_key()
        popup_url = f"chrome-extension://{ext_id}/popup.html"
        att = sid = None
        for _ in range(10):
            r = cdp.send("Target.createTarget", {"url": popup_url}, timeout=20)
            tid = r.get("targetId")
            if not tid:
                time.sleep(0.6)
                continue
            att = cdp.send("Target.attachToTarget", {"targetId": tid, "flatten": True}, timeout=20)
            sid = att.get("sessionId")
            if sid:
                break
            time.sleep(0.6)
        if not sid:
            say("[SETUP-INVALID] 打不开/attach 不了扩展 popup 页面 ⇒ 无法在扩展上下文里发消息"); return 6
        say(f"[扩展] 执行页={popup_url}")

        def ev(expr, ap=False, timeout=40, session=None):
            r = cdp.send("Runtime.evaluate",
                         {"expression": expr, "returnByValue": True, "awaitPromise": ap},
                         timeout=timeout, session=session or sid)
            if "__cdp_error__" in r or "__timeout__" in r:
                return None
            return (r.get("result") or {}).get("value")

        rid = None
        for _ in range(30):
            rid = ev("(typeof chrome!=='undefined'&&chrome.runtime&&chrome.runtime.id)||''")
            if rid:
                break
            time.sleep(0.5)
        say(f"[扩展] chrome.runtime.id={rid}")
        if not rid:
            say("[SETUP-INVALID] 扩展页里拿不到 chrome.runtime.id"); return 6

        # ── 5) 打开 fixture 页（新 target），等 content script 注入 ──
        tr = cdp.send("Target.createTarget", {"url": f"http://127.0.0.1:{HTTP_PORT}/"}, timeout=20)
        ptid = tr.get("targetId")
        patt = cdp.send("Target.attachToTarget", {"targetId": ptid, "flatten": True}, timeout=20)
        psid = patt.get("sessionId")
        if not psid:
            say("[FAIL] attach 页面 target 失败"); return 5
        got_card_style = False
        for _ in range(40):
            v = ev("!!document.getElementById('idm-next-tokens')", session=psid)
            if v:
                got_card_style = True
                break
            time.sleep(0.5)
        say(f"[页面] content script 已注入（token style 存在={got_card_style}）")
        if not got_card_style:
            say("[SETUP-INVALID] content.js 没进这个页面 ⇒ 后面全是空谈"); return 6
        # 让 video 元素真的有尺寸（hover 判定要看 rect）
        ev("document.getElementById('v').play().catch(function(){})", session=psid)
        time.sleep(0.6)

        results = {}

        def check(name, ok, detail=""):
            results[name] = bool(ok)
            say(f"  {'[PASS]' if ok else '[FAIL]'} {name}" + (f"  -> {detail}" if detail else ""))

        say("\n=== 判据 ===")

        # ① hover 真鼠标移到 video 上 ⇒ 浮出「下载此视频」按钮（这是真实用户路径，不是 stub）
        rect = ev("""(function(){var r=document.getElementById('v').getBoundingClientRect();
                     return JSON.stringify({x:r.x+r.width/2,y:r.y+r.height/2,w:r.width,h:r.height});})()""",
                  session=psid)
        rx = json.loads(rect or "{}")
        say(f"[hover] video 中心=({rx.get('x')},{rx.get('y')}) 尺寸={rx.get('w')}x{rx.get('h')}")
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mouseMoved", "x": rx.get("x", 0), "y": rx.get("y", 0)},
                 timeout=20, session=psid)
        time.sleep(0.8)
        btn = ev("""(function(){var b=document.querySelector('.idm-next-float-btn');
                     if(!b) return JSON.stringify({present:false});
                     var r=b.getBoundingClientRect();
                     return JSON.stringify({present:true,shown:b.classList.contains('show'),
                                            text:b.textContent.trim(),x:r.x+r.width/2,y:r.y+r.height/2,
                                            w:r.width,h:r.height});})()""", session=psid)
        bj = json.loads(btn or "{}")
        say(f"[hover] 浮钮={btn}")
        check("① hover 后「下载此视频」浮钮出现且可见",
              bj.get("present") and bj.get("shown") and bj.get("w", 0) > 20, str(bj))

        # ② 真鼠标点浮钮 ⇒ 卡片弹出（不是我们调用函数，是它自己的 click handler）
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mousePressed", "x": bj.get("x"), "y": bj.get("y"),
                  "button": "left", "clickCount": 1}, timeout=20, session=psid)
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mouseReleased", "x": bj.get("x"), "y": bj.get("y"),
                  "button": "left", "clickCount": 1}, timeout=20, session=psid)
        card = None
        for _ in range(20):
            card = ev("""(function(){var c=document.getElementById('idm-next-capture');
                          if(!c) return null;
                          var r=c.getBoundingClientRect();
                          return JSON.stringify({w:r.width,h:r.height,right:r.right,bottom:r.bottom,
                            vw:innerWidth,vh:innerHeight,
                            name:!!c.querySelector('.idm-cap-name'),dir:!!c.querySelector('.idm-cap-dir'),
                            fmt:!!c.querySelector('.idm-cap-fmt'),
                            btns:[].slice.call(c.querySelectorAll('.idm-cap-actions button'))
                                 .map(function(b){return b.textContent.trim();}),
                            title:(c.querySelector('.idm-cap-head .hd-t')||{}).textContent});})()""",
                      session=psid)
            if card:
                break
            time.sleep(0.3)
        cj = json.loads(card or "{}")
        say(f"[卡片] {card}")
        # 像素证据：卡片在场时截一帧（几何判据在下面，图给人看）
        try:
            shot = cdp.send("Page.captureScreenshot", {"format": "png"}, timeout=30, session=psid)
            if shot and shot.get("data"):
                (logdir / "capture-card.png").write_bytes(base64.b64decode(shot["data"]))
                say(f"[截图] {logdir / 'capture-card.png'}")
        except Exception as e:
            say(f"[截图] 失败（不影响判据）：{e}")
        check("② 点击浮钮后卡片真的弹出（含文件名/目录/画质三个控件）",
              cj.get("w", 0) > 100 and cj.get("name") and cj.get("dir") and cj.get("fmt"),
              str(cj))
        check("③ 卡片未被截断（完整落在视口内）",
              0 < cj.get("right", 1e9) <= cj.get("vw", 0) and 0 < cj.get("bottom", 1e9) <= cj.get("vh", 0),
              f"right={cj.get('right')} bottom={cj.get('bottom')} vw={cj.get('vw')} vh={cj.get('vh')}")

        # ③b 关键元素**互不重叠且都在卡片框内**（截图上看得见「画质（HLS 变体）」标签把
        #     自己撑成三行、压进控件列 ⇒ 光测"整卡在不在视口里"盖不住这类缺陷）
        geom = ev("""(function(){
          var c=document.getElementById('idm-next-capture'); if(!c) return null;
          var cr=c.getBoundingClientRect();
          var parts=[].slice.call(c.querySelectorAll('.idm-cap-row label,.idm-cap-row input,.idm-cap-row select,.idm-cap-actions button'));
          var out=[];
          for(var i=0;i<parts.length;i++){
            var r=parts[i].getBoundingClientRect();
            out.push({n:(parts[i].tagName+':'+(parts[i].textContent||'').trim().slice(0,12)),
                      x:Math.round(r.x),y:Math.round(r.y),w:Math.round(r.width),h:Math.round(r.height),
                      inCard:(r.left>=cr.left-1&&r.right<=cr.right+1&&r.top>=cr.top-1&&r.bottom<=cr.bottom+1)});
          }
          return JSON.stringify({card:{x:Math.round(cr.x),y:Math.round(cr.y),w:Math.round(cr.width),h:Math.round(cr.height)},parts:out});})()""",
                  session=psid)
        gj = json.loads(geom or "{}")
        parts = gj.get("parts") or []

        def rects_overlap(a, b):
            return not (a["x"] + a["w"] <= b["x"] or b["x"] + b["w"] <= a["x"]
                        or a["y"] + a["h"] <= b["y"] or b["y"] + b["h"] <= a["y"])

        pairs = [(p["n"], q["n"]) for i, p in enumerate(parts) for q in parts[i + 1:]
                 if rects_overlap(p, q)]
        outside = [p["n"] for p in parts if not p.get("inCard")]
        say(f"[几何] 卡片={gj.get('card')} 部件数={len(parts)}")
        for p in parts:
            say("        %-28s %4dx%-4d @(%4d,%4d) 在卡内=%s"
                % (p["n"], p["w"], p["h"], p["x"], p["y"], p.get("inCard")))
        check("③b 卡片内关键元素不重叠、不越出卡片边界",
              bool(parts) and not pairs and not outside,
              f"重叠对={pairs} 越界={outside}")

        # ④ 画质下拉必须由**真 host**填出两档（证明卡片↔host↔fixture 这条数据路活着）
        opts = None
        for _ in range(30):
            opts = ev("""(function(){var s=document.querySelector('.idm-cap-fmt');
                          if(!s) return null;
                          return JSON.stringify({n:s.options.length,disabled:s.disabled,
                                                 vals:[].slice.call(s.options).map(function(o){return o.value;}),
                                                 txt:[].slice.call(s.options).map(function(o){return o.textContent;})});})()""",
                      session=psid)
            oj = json.loads(opts or "{}")
            if oj.get("n", 0) >= 3:
                break
            time.sleep(0.5)
        oj = json.loads(opts or "{}")
        say(f"[画质] {opts}")
        has_two = (BANDWIDTH_HI in (oj.get("vals") or [])) and (BANDWIDTH_LO in (oj.get("vals") or []))
        check("④ 画质下拉由真 host 填出两个真实变体（BANDWIDTH 值在场）",
              has_two, f"n={oj.get('n')} vals={oj.get('vals')}")

        # ⑤ 输入框可聚焦、可**真键入**（insertText 走 IME/输入事件通道，不是 .value=）
        nm = ev("""(function(){var i=document.querySelector('.idm-cap-name');
                     var r=i.getBoundingClientRect();
                     return JSON.stringify({x:r.x+8,y:r.y+r.height/2});})()""", session=psid)
        nj = json.loads(nm or "{}")
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mousePressed", "x": nj.get("x"), "y": nj.get("y"),
                  "button": "left", "clickCount": 1}, timeout=20, session=psid)
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mouseReleased", "x": nj.get("x"), "y": nj.get("y"),
                  "button": "left", "clickCount": 1}, timeout=20, session=psid)
        focused = ev("JSON.stringify(document.activeElement && document.activeElement.className)",
                     session=psid)
        ev("document.execCommand('selectAll',false,null)", session=psid)
        cdp.send("Input.insertText", {"text": "clicked_name.bin"}, timeout=20, session=psid)
        val = ev("(document.querySelector('.idm-cap-name')||{}).value", session=psid)
        say(f"[输入] activeElement={focused} 键入后 value={val!r}")
        check("⑤ 文件名框可聚焦且能真键入",
              "idm-cap-name" in str(focused) and val == "clicked_name.bin",
              f"active={focused} value={val!r}")

        # ⑥ 选中低档位（真 select 交互：改 value 并派发 change，等价于用户选一项）
        sel = ev("""(function(){var s=document.querySelector('.idm-cap-fmt');
                     var want='%s'; var hit=false;
                     for(var i=0;i<s.options.length;i++){if(s.options[i].value===want){s.selectedIndex=i;hit=true;break;}}
                     if(hit){s.dispatchEvent(new Event('change',{bubbles:true}));}
                     return JSON.stringify({hit:hit,value:s.value});})()""" % BANDWIDTH_LO,
                 session=psid)
        sj = json.loads(sel or "{}")
        say(f"[选择] {sel}")
        check("⑥ 画质下拉可选中低档位", sj.get("hit") and sj.get("value") == BANDWIDTH_LO, str(sj))

        # ⑦ 提交前记录 GUI 日志基线，事后只看新增行 ⇒ 证人必须是这一轮点的
        before_add = len([x for x in read_gbk(guilog).splitlines() if "IPC add 参数:" in x])

        # ⑧ 真点「立即下载」：卡片自己的 handler → SW → host → 管道 → GUI
        bw = ev("""(function(){var b=document.querySelector('.idm-cap-now');
                    var r=b.getBoundingClientRect();
                    return JSON.stringify({x:r.x+r.width/2,y:r.y+r.height/2,label:b.textContent.trim(),
                                           disabled:b.disabled});})()""", session=psid)
        bjw = json.loads(bw or "{}")
        say(f"[提交按钮] {bw}")
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mousePressed", "x": bjw.get("x"), "y": bjw.get("y"),
                  "button": "left", "clickCount": 1}, timeout=20, session=psid)
        cdp.send("Input.dispatchMouseEvent",
                 {"type": "mouseReleased", "x": bjw.get("x"), "y": bjw.get("y"),
                  "button": "left", "clickCount": 1}, timeout=20, session=psid)

        # ⑨ 结果回填：toast 文案 + 卡片自删（success 分支才会 remove）
        toast, gone = None, False
        for _ in range(30):
            toast = ev("""(function(){var t=document.querySelector('.idm-next-toast');
                            return t?JSON.stringify({text:t.textContent.trim(),cls:t.className}):null;})()""",
                       session=psid)
            gone = bool(ev("!document.getElementById('idm-next-capture')", session=psid))
            if toast and gone:
                break
            time.sleep(0.4)
        tj = json.loads(toast or "{}")
        say(f"[回填] toast={toast} 卡片已关={gone}")
        check("⑦ 提交后结果回填到页面（成功 toast + 卡片自删）",
              ("已添加" in str(tj.get("text"))) and ("success" in str(tj.get("cls"))) and gone,
              f"toast={tj} gone={gone}")

        # ⑩ GUI 侧逐字证人：本轮新增的 `IPC add 参数:` 行必须带上选中的 hlsVariant
        witness = []
        for _ in range(20):
            lines = [x for x in read_gbk(guilog).splitlines() if "IPC add 参数:" in x]
            if len(lines) > before_add:
                witness = lines[before_add:]
                break
            time.sleep(0.5)
        say(f"[证人] GUI 新增『IPC add 参数』行数={len(witness)}")
        for w in witness:
            say("        " + w.strip()[:200])
        joined = " ".join(witness)
        check("⑧ GUI 收到的 hlsVariant == 我在下拉里选的低档位",
              f"hlsVariant={BANDWIDTH_LO}" in joined,
              f"期望 hlsVariant={BANDWIDTH_LO}；实际见上行")
        check("⑨ GUI 收到的文件名 == 我真键入的那个",
              "file=clicked_name.bin" in joined, "见上行 file= 字段")

        # ⑪ 反向对照：**不选画质**时该字段必须为空（否则上面那条 ⑧ 可能是恒真）
        # ⚠ 不用"再 hover 一次浮钮"：那个按钮是产品自己在 click handler 里 opacity→0、
        #   并在 document 级捕获 mousemove 里摘掉 .show 的控件，第二次唤出时序不稳
        #   （实测坐标跳到 (574,21)、卡片不再出现）。
        #   改用 popup.js:431 `openCaptureFor` 走的**同一条产品消息**（chrome.tabs.sendMessage
        #   → content 的 showCaptureDialogFor），这才是可重复的对照面。
        before_noselect = len([x for x in read_gbk(guilog).splitlines() if "IPC add 参数:" in x])
        opened = ev("""(function(){return new Promise(function(res){
                     chrome.tabs.query({active:true,currentWindow:true},function(ts){
                       var tab=ts&&ts[0]; if(!tab){res('no-tab');return;}
                       chrome.tabs.sendMessage(tab.id,
                         {action:'showCaptureDialogFor',
                          resource:{url:'http://127.0.0.1:%d/hls/master.m3u8',filename:'',type:'video',size:-1}},
                         function(r){res(JSON.stringify(chrome.runtime.lastError||r||'sent'));});});});})()""" % HTTP_PORT,
                    ap=True, timeout=30, session=sid)
        say(f"[反向对照] showCaptureDialogFor 回执={opened}")
        card2 = None
        for _ in range(20):
            card2 = ev("!!document.getElementById('idm-next-capture')", session=psid)
            if card2:
                break
            time.sleep(0.3)
        say(f"[反向对照] 第二次卡片是否弹出={card2}")
        if card2:
            # 这次什么都不选，直接按默认「最佳画质（默认）」提交
            d = ev("""(function(){var b=document.querySelector('.idm-cap-now');
                        var r=b.getBoundingClientRect();
                        return JSON.stringify({x:r.x+r.width/2,y:r.y+r.height/2,
                                               fmt:(document.querySelector('.idm-cap-fmt')||{}).value});})()""",
                   session=psid)
            dj = json.loads(d or "{}")
            cdp.send("Input.dispatchMouseEvent",
                     {"type": "mousePressed", "x": dj.get("x"), "y": dj.get("y"),
                      "button": "left", "clickCount": 1}, timeout=20, session=psid)
            cdp.send("Input.dispatchMouseEvent",
                     {"type": "mouseReleased", "x": dj.get("x"), "y": dj.get("y"),
                      "button": "left", "clickCount": 1}, timeout=20, session=psid)
            noselect = []
            for _ in range(24):
                ls = [x for x in read_gbk(guilog).splitlines() if "IPC add 参数:" in x]
                if len(ls) > before_noselect:
                    noselect = ls[before_noselect:]
                    break
                time.sleep(0.5)
            say(f"[反向对照] 不选画质时的提交行：{noselect}")
            nz = " ".join(noselect)
            check("⑩ 反向对照：不选画质 ⇒ hlsVariant 为空（证明 ⑧ 不是恒真）",
                  "hlsVariant=" in nz and f"hlsVariant={BANDWIDTH_LO}" not in nz, str(dj.get("fmt")))
        else:
            check("⑩ 反向对照：不选画质 ⇒ hlsVariant 为空（证明 ⑧ 不是恒真）", False,
                  "第二次没能唤出卡片 ⇒ 对照未跑成")

        # ⑫ 第三张卡：**填目录 + 选低档**提交，等它真下完并逐字节比对产物。
        #     这一步才把"用户点的档位"落到盘上（⑧ 只到 GUI 收到参数为止）。
        say("\n=== 落盘核对（低档变体是否真的被下下来）===")
        dl_dir = logdir / "dl"
        dl_dir.mkdir(parents=True, exist_ok=True)
        before_disk = len([x for x in read_gbk(guilog).splitlines() if "IPC add 参数:" in x])
        # ⚠ 反向对照留下的第二张卡**必须等它真的消失**再开第三张：content.js 的 remove()
        #   是 `classList.remove('show')` + 220ms 后真删，而 showCaptureDialogFor 一进来会先
        #   把已存在的卡片移掉 ⇒ 抢跑的话新卡会被那次延迟删除一起带走（实测点"立即下载"没反应）。
        for _ in range(30):
            if not ev("!!document.getElementById('idm-next-capture')", session=psid):
                break
            time.sleep(0.2)
        opened3 = ev("""(function(){return new Promise(function(res){
             chrome.tabs.query({active:true,currentWindow:true},function(ts){
               var tab=ts&&ts[0]; if(!tab){res('no-tab');return;}
               chrome.tabs.sendMessage(tab.id,
                 {action:'showCaptureDialogFor',
                  resource:{url:'http://127.0.0.1:%d/hls/master.m3u8',filename:'',type:'video',size:-1}},
                 function(r){res(JSON.stringify(r||'sent'));});});});})()""" % HTTP_PORT,
           ap=True, timeout=30, session=sid)
        for _ in range(20):
            if ev("!!document.querySelector('#idm-next-capture .idm-cap-now')", session=psid):
                break
            time.sleep(0.3)
        card3 = ev("!!document.getElementById('idm-next-capture')", session=psid)
        say(f"[落盘面] 第三张卡回执={opened3} 在场={card3}")
        # 画质列表是**异步**填的（卡片建好时只有"最佳画质（默认）"，listFormats 回来才有两档）。
        # 不等到它就选 ⇒ selectedIndex 找不到目标、ok:false（实测踩过一次）。
        for _ in range(40):
            n = ev("(document.querySelector('.idm-cap-fmt')||{}).length || 0", session=psid)
            if (n or 0) >= 3:
                break
            time.sleep(0.3)
        filled = ev("""(function(){
            var dir=document.querySelector('.idm-cap-dir'), s=document.querySelector('.idm-cap-fmt');
            var b=document.querySelector('.idm-cap-now');
            if(!dir||!s||!b) return JSON.stringify({ok:false});
            dir.value=%r; dir.dispatchEvent(new Event('input',{bubbles:true}));
            var hit=false;
            for(var i=0;i<s.options.length;i++){if(s.options[i].value==='%s'){s.selectedIndex=i;hit=true;break;}}
            if(hit) s.dispatchEvent(new Event('change',{bubbles:true}));
            var r=b.getBoundingClientRect();
            return JSON.stringify({ok:hit,x:r.x+r.width/2,y:r.y+r.height/2,fmt:s.value,d:dir.value});})()"""
                    % (str(dl_dir), BANDWIDTH_LO), session=psid)
        fj = json.loads(filled or "{}")
        say(f"[落盘面] 卡片已填={filled}")
        if fj.get("ok"):
            cdp.send("Input.dispatchMouseEvent",
                     {"type": "mousePressed", "x": fj.get("x"), "y": fj.get("y"),
                      "button": "left", "clickCount": 1}, timeout=20, session=psid)
            cdp.send("Input.dispatchMouseEvent",
                     {"type": "mouseReleased", "x": fj.get("x"), "y": fj.get("y"),
                      "button": "left", "clickCount": 1}, timeout=20, session=psid)
            lines = []
            t0 = time.time()
            while time.time() - t0 < 90:
                ls = [x for x in read_gbk(guilog).splitlines() if "IPC add 参数:" in x]
                if len(ls) > before_disk:
                    lines = ls[before_disk:]
                    break
                time.sleep(0.5)
            say(f"[落盘面] GUI 新增行={lines}")
            joined_d = " ".join(lines)
            check("⑪ dir 字段真的传到 GUI（卡片里填的目录原样到达）",
                  f"dir={str(dl_dir)}" in joined_d.replace("\\\\", "\\"), str(dl_dir))
            found = None
            for _ in range(60):
                hits = [p for p in dl_dir.glob("*") if p.is_file() and p.stat().st_size > 0]
                for p in hits:
                    try:
                        body = p.read_bytes()
                    except Exception:
                        continue
                    if body in (LO_EXPECT, HI_EXPECT):
                        found = p
                        break
                if found:
                    break
                time.sleep(1.0)
            if found:
                import hashlib
                body = found.read_bytes()
                say(f"[落盘] 文件={found.name} 大小={len(body)} "
                    f"sha256={hashlib.sha256(body).hexdigest()[:12]} "
                    f"等于低档拼接={body == LO_EXPECT} 等于高档拼接={body == HI_EXPECT}"
                    f"（lo={len(LO_EXPECT)}B hi={len(HI_EXPECT)}B）")
                # 判别式必须自己先成立：hi/lo 两档的期望值若相同，这个比对就永远分不出档位。
                distinct = LO_EXPECT != HI_EXPECT
                say(f"[落盘] 判别式自检：lo/hi 期望内容互不相同={distinct}")
                check("⑫ 产物逐字节等于**我在卡片上选的那一档**（低档 3 段拼接）",
                      distinct and body == LO_EXPECT and body != HI_EXPECT, "见上行 sha256/大小")
            else:
                say(f"[落盘] {dl_dir} 里 60s 内没有与任一档逐字节相等的成品："
                    f"{[p.name for p in dl_dir.glob('*')]}")
                check("⑫ 产物逐字节等于**我在卡片上选的那一档**（低档 3 段拼接）", False,
                      "没找到成品 ⇒ 落盘面没有证人")
        else:
            check("⑪ dir 字段真的传到 GUI（卡片里填的目录原样到达）", False, "卡片没填成")
            check("⑫ 产物逐字节等于**我在卡片上选的那一档**（低档 3 段拼接）", False,
                  "卡片没填成 ⇒ 未提交")

        bad = [k for k, v in results.items() if not v]
        say(f"\n[汇总] 判据 {len(results)} 条，红 {len(bad)} 条" + (f"：{bad}" if bad else ""))
        rc = 0 if not bad else 1
        say(f"[VERDICT] {'全部通过' if rc == 0 else '有未通过项'}")
        return rc
    finally:
        try:
            if cdp:
                cdp.close()
        except Exception:
            pass
        for proc in (edge_proc, app_proc):
            if proc is not None and proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(8)
                except Exception:
                    try:
                        proc.kill()
                    except Exception:
                        pass
        if srv:
            try:
                srv.shutdown()
            except Exception:
                pass
        # 归档读数（本文件被报告引用 ⇒ 不覆盖固定名，带时间戳）
        try:
            (logdir / "readings.txt").write_text("\n".join(LOG) + "\n", encoding="utf-8")
        except Exception:
            pass
        shutil.rmtree(logdir / "edge-profile", ignore_errors=True)
        say(f"[读数档] {logdir}")


if __name__ == "__main__":
    sys.exit(main())
