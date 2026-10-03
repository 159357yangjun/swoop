#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真实 Native Messaging E2E（运行态证据，不是静态契约）。

链路：headless Edge 加载 unpacked 扩展 → service worker 里 sendNativeMessage
      → idm-next-host.exe → QLocalSocket(idm-next-ipc) → ipc_server "add" → GUI 收下

两面（同一枚二进制只切一个变量）：
  gui    —— GUI 在跑 ⇒ host 转发成功，**证人 = GUI 自己写的 `IPC server 已收到请求` 日志行**
  nogui  —— 不启 GUI ⇒ host 的 connected=false 分支被走到（第 8 步 fallback）

为什么证人不是"任务列表里出现了它"：offscreen/真窗口都不拍的情况下，唯一能独立复算的是
GUI 进程自己印进日志的那一行。任务列表那一格属于像素面，仍挂在未验里。

用法：python tools/nm_e2e.py <gui|nogui> [URL]
"""
import base64
import ctypes
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request
from urllib.parse import urlparse

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXT_DIR = ROOT / "browser-extension"
NMH = EXT_DIR / "native-messaging-host"
HOST_EXE = NMH / "idm-next-host.exe"
APP_EXE = ROOT / "build" / "idm-next.exe"
GEN_MANIFEST = (pathlib.Path(os.environ["LOCALAPPDATA"])
                / "IDMNext" / "NativeMessagingHosts" / "com.tencent.idm_next.json")
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Google/Chrome/Application/chrome.exe",
]
PORT = 9411
LOGDIR = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "idm-probes" / f"e2e-{time.strftime('%m%d-%H%M%S')}"
TEST_URL = "http://ipv4.download.thinkbroadband.com/512KB.zip"

k32 = ctypes.windll.kernel32
k32.CreateFileW.restype = ctypes.c_void_p


def read_gui_log(path):
    """GUI 的 Log 走 qDebug ⇒ Windows 下按 ANSI(GBK) 落盘。用 utf-8 读会把中文
    解成乱码 ⇒ 判据永远匹配不上（本轮第一次就栽在这里：证人明明在文件里）。
    两边都试，取能认出关键词的那个，并把用的编码印出来。"""
    if not path.exists():
        return "", "missing"
    raw = path.read_bytes()
    for enc in ("gbk", "utf-8"):
        try:
            t = raw.decode(enc, errors="replace")
        except Exception:
            continue
        if "IPC server" in t or "IPC 请求" in t:
            return t, enc
    return raw.decode("utf-8", errors="replace"), "utf-8(fallback)"


def sha12(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()[:12]


def pipe_openable(name=r"\.\pipe\idm-next-ipc"):
    h = k32.CreateFileW(name, 0x80000000, 0, None, 3, 0, None)
    if h in (None, ctypes.c_void_p(-1).value):
        return False, k32.GetLastError()
    k32.CloseHandle(h)
    return True, 0


def derive_id_from_key():
    ext = json.loads((EXT_DIR / "manifest.json").read_text(encoding="utf-8"))
    h = hashlib.sha256(base64.b64decode(ext["key"])).hexdigest()[:32]
    return "".join(chr(ord("a") + int(c, 16)) for c in h)


def http_json(path, timeout=5):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}", timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception as e:
        return {"__error__": str(e)}


class Ws:
    def __init__(self, url):
        u = urlparse(url)
        self.host, self.path, self.port = u.hostname, u.path or "/", (u.port or 80)
        self.sock = socket_conn(self.host, self.port)
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
                return obj.get("result", {}) if "error" not in obj else {"__cdp_error__": obj["error"]}
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


import socket as _socket


def socket_conn(host, port):
    return _socket.create_connection((host, port), timeout=20)


def main():
    face = sys.argv[1] if len(sys.argv) > 1 else "gui"
    url = sys.argv[2] if len(sys.argv) > 2 else TEST_URL
    LOGDIR.mkdir(parents=True, exist_ok=True)
    log = []

    def say(s):
        print(s)
        log.append(s)

    rc = 0
    edge_proc = app_proc = ws = None
    appdir = LOGDIR / "appdata"
    guilog = LOGDIR / "gui-idm.log"
    try:
        say(f"[NM-E2E] face={face}")
        say(f"[身份] 仓根={ROOT} HEAD={subprocess.run(['git','-C',str(ROOT),'rev-parse','--short','HEAD'],capture_output=True).stdout.decode().strip()}")
        say(f"[身份] app_exe_sha12={sha12(APP_EXE)} host_exe_sha12={sha12(HOST_EXE)}")
        derived = derive_id_from_key()
        say(f"[身份] key 现算 ID={derived}")
        say(f"[前置] 生成 manifest 存在={GEN_MANIFEST.is_file()} path={GEN_MANIFEST}")

        edge = next((p for p in EDGE_CANDIDATES if pathlib.Path(p).exists()), None)
        if not edge:
            say("[FAIL] 没有 Edge/Chrome"); return 3

        if face == "gui":
            appdir.mkdir(parents=True, exist_ok=True)
            env = dict(os.environ, APPDATA=str(appdir), LOCALAPPDATA=str(appdir / "local"),
                         QT_LOGGING_TO_CONSOLE="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
            app_proc = subprocess.Popen([str(APP_EXE)], cwd=str(ROOT / "build"),
                                        stdout=open(guilog, "wb"), stderr=subprocess.STDOUT, env=env)
            ok = False
            for i in range(60):
                time.sleep(0.5)
                txt, enc = read_gui_log(guilog)
                if "IPC server 已启动" in txt:
                    ok = True
                    say(f"[GUI] pid={app_proc.pid} 监听就绪（等 {(i+1)*0.5:.1f}s，日志按 {enc} 解出）")
                    break
                if app_proc.poll() is not None:
                    say(f"[FAIL] GUI 退出 rc={app_proc.returncode}"); return 4
            if not ok:
                say("[FAIL] 30s 内没等到「IPC server 已启动」⇒ 后面所有读数都无证人"); return 5
            live, err = pipe_openable()
            say(f"[GUI] 命名管道可开={live}（err={err}）")

        args = [edge, "--headless=new", f"--remote-debugging-port={PORT}",
                f"--user-data-dir={LOGDIR/'edge-profile'}", "--no-first-run",
                "--no-default-browser-check", "--disable-gpu",
                f"--load-extension={EXT_DIR}", f"--disable-extensions-except={EXT_DIR}",
                "about:blank"]
        args = [a.replace("\\", "/") if isinstance(a, str) else a for a in args]
        edge_proc = subprocess.Popen(args)
        ver = None
        for _ in range(40):
            v = http_json("/json/version")
            if "__error__" not in v:
                ver = v; break
            time.sleep(0.5)
        if not ver:
            say("[FAIL] CDP 无响应"); return 6
        say(f"[浏览器] {ver.get('Browser','?')}")

        # 先拿"我们自己的扩展 ID"：不靠 SW target（headless 下 SW 可能压根不上报，
        # 而且 Edge 自带的 chrome-extension://…/background.rollup.js 会冒充成"扩展 SW"）。
        targets = http_json("/json")
        pages = [t for t in targets if isinstance(t, list)]
        say(f"[CDP] /json 条目数={len(pages)} 类型分布="
            f"{sorted({t.get('type') for t in pages})}")

        import base64 as _b64
        ext_id = derive_id_from_key()
        popup_url = f"chrome-extension://{ext_id}/popup.html"
        created = None
        for attempt in range(6):
            cdp = Ws(ver["webSocketDebuggerUrl"])
            r = cdp.send("Target.createTarget", {"url": popup_url}, timeout=20)
            tid = r.get("targetId")
            if not tid:
                time.sleep(1.0)
                continue
            att = cdp.send("Target.attachToTarget", {"targetId": tid, "flatten": True}, timeout=20)
            sid = att.get("sessionId")
            if not sid:
                cdp.close(); time.sleep(1.0); continue
            # 等页面真起来：轮询 chrome.runtime.id
            got = None
            for _ in range(30):
                rr = cdp.send("Runtime.evaluate",
                              {"expression": "(typeof chrome!=='undefined'&&chrome.runtime&&chrome.runtime.id)||''",
                               "returnByValue": True}, timeout=20, session=sid)
                got = (rr.get("result") or {}).get("value")
                if got:
                    break
                time.sleep(0.5)
            created = (cdp, sid, tid, got)
            break
        if not created:
            say("[FAIL] 打不开扩展自己的 popup ⇒ 没有可执行 chrome.* 的上下文"); return 8
        cdp, sid, tid, got = created
        say(f"[扩展] 由 key 现算的 ID={ext_id}")
        say(f"[扩展] 扩展页面里 chrome.runtime.id={got} ｜与 key 派生一致={got == ext_id}")
        ws = cdp
        SESSION = sid

        def native(action, payload=""):
            expr = ("new Promise((resolve)=>{try{chrome.runtime.sendNativeMessage('com.tencent.idm_next',"
                    + "{action:'" + action + "'" + payload + "}"
                    + ",(r)=>{resolve(JSON.stringify({"
                    "resp:r||null,lastError:(chrome.runtime.lastError&&chrome.runtime.lastError.message)||null}));"
                    "});}catch(e){resolve(JSON.stringify({thrown:String(e)}));}})")
            res = ws.send("Runtime.evaluate", {"expression": expr, "awaitPromise": True,
                                               "returnByValue": True}, timeout=40, session=SESSION)
            return (res.get("result") or {}).get("value")

        ping_raw = native("ping")
        say(f"[第7跳-a] ping 回执={ping_raw}")
        try:
            ping = json.loads(ping_raw) if ping_raw else {}
        except Exception:
            ping = {}
        if ping.get("lastError"):
            say(f"[VERDICT] native host 被浏览器拒绝：{ping['lastError']}")
            rc = 8
        elif not (ping.get("resp") or {}).get("success"):
            say(f"[VERDICT] host 回了话但 success!=true：{ping.get('resp')}")
            rc = 8

        if rc == 0:
            add_raw = native("add_download", ",url:'" + url + "',filename:''")
            say(f"[第7跳-b] add_download 回执={add_raw}")
            try:
                j = json.loads(add_raw) if add_raw else {}
            except Exception:
                j = {}
            resp = j.get("resp") or {}
            if face == "gui":
                if j.get("lastError") or not resp.get("success"):
                    say("[VERDICT] 发送到 GUI 没成功"); rc = 9
                else:
                    time.sleep(1.0)
                    txt, enc = read_gui_log(guilog)
                    hits = [l for l in txt.splitlines() if "IPC 请求:" in l]
                    say(f"[证人] GUI 日志里『IPC 请求:』行数={len(hits)}")
                    for h in hits[-2:]:
                        say(f"[证人] {h.strip()[:220]}")
                    # 产品那行日志只印 `命令 → 结果（排队/处理/送达/客户端）`，**不印 URL**
                    # （ipc_server.cpp:350）。所以证人形状是「add → 成功」这一行本身；
                    # 想按 URL 匹配是取不到证据的——上一轮我就栽在这里，把已通的链读成没通。
                    mine = [h for h in hits if "add" in h and "成功" in h]
                    say(f"[证人] 其中 add→成功 行数={len(mine)}")
                    if not mine:
                        say("[VERDICT] GUI 没有一条 add→成功 ⇒ 不算跑通"); rc = 9
                    else:
                        say("[VERDICT] 主链跑通：浏览器→host→管道→GUI 收下（逐字证人见上）")
            else:
                say(f"[第8步] nogui 面：connected 分支回执 lastError={j.get('lastError')!r} "
                    f"resp={resp}")
                if resp.get("success"):
                    say("[VERDICT] 意外：没 GUI 却报成功 ⇒ 有别的实例在服务，本轮读数作废"); rc = 10
                else:
                    say("[VERDICT] 没 GUI 时 host 走的是「连不上」分支（fallback 入口被到达）")
    finally:
        for p in (edge_proc, app_proc):
            if p and p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=10)
                except Exception:
                    p.kill()
        if ws:
            ws.close()
        (LOGDIR / "nm-e2e.log").write_text("\n".join(log) + "\n", encoding="utf-8")
        say(f"[读数目录] {LOGDIR}")
        say(f"[NM-E2E] 退码={rc}")
    return rc


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
