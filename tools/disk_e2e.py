#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GUI 真落盘 E2E（localhost，四层证据一起取）。

判定口径（主控锁定，本脚本逐条实现，不做扩大解释）：

  正面 200 —— **四层全成立**才允许写"已观测级打通"：
    ① Native Host   success=true
    ② GUI IPC       GUI 日志出现 `IPC 请求: add → 成功`
    ③ 磁盘状态      最终文件存在 且 .idmtmp 不存在
    ④ 内容证明      size == 源 size 且 sha256 == 源 sha256
    ⚠ 只有 ①② 不能宣布下载成功——那只代表 GUI **收单成功**。

  404 面 —— 目标不是"必须输出某句错误"，而是证明不伪成功：
    · 不得生成看起来成功的最终文件；
    · 不得把失败任务标成「已完成」；
    · 若留下 .idmtmp，记录其大小与最终任务状态（保留供重试不算缺陷）；
    · 日志/任务状态里要有失败证据（没有只记录，不当缺陷）。

刻意不测：popup / webRequest / DOM 扫描 / MP4 页面识别 / yt-dlp / ffmpeg / HLS / 公网 URL。
任何一个混进来，红了都会重新扩大归因空间。

三处踩过的形状，别改回去：
  · 本机 anaconda 的 python **没有 socket.AF_UNIX** ⇒ Windows 命名管道只能走 Win32 CreateFileW。
  · 产品 IPC 协议是**裸 JSON**（`ipc_server.cpp:108` 起），不是 native messaging 那种 4 字节长度前缀。
  · GUI 的 Log 走 qDebug ⇒ Windows 下按 **GBK** 落盘，用 utf-8 读会把证人读没。

用法：python tools/disk_e2e.py [size_bytes]
"""
import hashlib
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_EXE = ROOT / "build" / "idm-next.exe"
HOST_EXE = ROOT / "browser-extension" / "native-messaging-host" / "idm-next-host.exe"
EXT_DIR = ROOT / "browser-extension"
SERVER = ROOT / "tools" / "disk_e2e_server.py"
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Google/Chrome/Application/chrome.exe",
]
CDP_PORT = 9412
HTTP_PORT = 8241
DEFAULT_SIZE = 512 * 1024

sys.path.insert(0, str(ROOT / "tools"))
from nm_e2e import Ws, derive_id_from_key  # noqa: E402


def sha12(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()[:12]


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 16), b""):
            h.update(blk)
    return h.hexdigest()


def read_gbk(path):
    if not path.exists():
        return ""
    raw = path.read_bytes()
    for enc in ("gbk", "utf-8"):
        t = raw.decode(enc, errors="replace")
        if "IPC server" in t or "IPC 请求" in t or "HistoryStore" in t:
            return t
    return raw.decode("utf-8", errors="replace")


def port_free(p):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p))
        return True
    except OSError:
        return False
    finally:
        s.close()


def gui_ipc(command):
    """向运行中的 GUI 发一条 IPC 命令并取回执（走产品自己的通道，不猜它的数据库）。"""
    import ctypes
    import ctypes.wintypes as wt
    k32 = ctypes.windll.kernel32
    k32.CreateFileW.restype = wt.HANDLE
    k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                                wt.DWORD, wt.DWORD, wt.HANDLE]
    name = chr(92) * 2 + "." + chr(92) + "pipe" + chr(92) + "idm-next-ipc"
    h = k32.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)  # GENERIC_RW, OPEN_EXISTING
    if not h or h == wt.HANDLE(-1).value:
        return None, f"连不上 err={k32.GetLastError()}"
    try:
        payload = json.dumps({"command": command}).encode("utf-8")
        written = wt.DWORD(0)
        if not k32.WriteFile(h, payload, len(payload), ctypes.byref(written), None):
            return None, f"写失败 err={k32.GetLastError()}"
        k32.FlushFileBuffers(h)
        buf = ctypes.create_string_buffer(1 << 16)
        got = wt.DWORD(0)
        chunks = bytearray()
        while True:
            ok = k32.ReadFile(h, buf, 1 << 16, ctypes.byref(got), None)
            if not ok or not got.value:
                break
            chunks += buf.raw[:got.value]
            if len(chunks) > (1 << 20):
                break
        k32.DisconnectNamedPipe(h)
    finally:
        k32.CloseHandle(h)
    raw = bytes(chunks)
    try:
        return json.loads(raw.decode("utf-8")), None
    except Exception as e:
        return None, f"解析失败({e}) 原始前 120={raw[:120]!r}"


def task_rows():
    obj, err = gui_ipc("list")
    return str((obj or {}).get("message", "")).splitlines(), err


def main():
    size = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SIZE
    work = pathlib.Path(tempfile.mkdtemp(prefix="idm-diske2e-"))
    log_lines = []
    rc = 0
    edge = app = srv = ws = None
    sid = None
    guilog = work / "gui.log"

    def say(s):
        print(s)
        log_lines.append(s)

    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True).stdout.decode().strip()
        say(f"[身份] 仓根={ROOT} HEAD={head}")
        say(f"[身份] app_exe_sha12={sha12(APP_EXE)} host_exe_sha12={sha12(HOST_EXE)}")
        for p in (CDP_PORT, HTTP_PORT):
            if not port_free(p):
                say(f"[FAIL] 端口 {p} 已被占用 ⇒ 读数会串到别人身上"); return 2

        ready = work / "srv.ready"
        srv = subprocess.Popen([sys.executable, str(SERVER), str(HTTP_PORT), str(size), str(ready)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(60):
            if ready.exists():
                break
            time.sleep(0.25)
        else:
            say("[FAIL] 测试服务器没起来"); return 3
        info = ready.read_text(encoding="utf-8").strip()
        src_sha = info.split("sha256=")[1].split()[0]
        src_size = int(info.split("size=")[1])
        say(f"[测试源] {info}")
        if src_size != size:
            say(f"[FAIL] 服务器给的 size={src_size} != 要求 {size}"); return 3
        url_ok = f"http://127.0.0.1:{HTTP_PORT}/payload.bin"
        url_404 = f"http://127.0.0.1:{HTTP_PORT}/missing.bin"

        appdir = work / "appdata"; (appdir / "local").mkdir(parents=True)
        env = dict(os.environ, APPDATA=str(appdir), LOCALAPPDATA=str(appdir / "local"),
                   QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
        app = subprocess.Popen([str(APP_EXE)], cwd=str(ROOT / "build"), env=env,
                               stdout=open(guilog, "wb"), stderr=subprocess.STDOUT)
        started = False
        for _ in range(80):
            time.sleep(0.5)
            if "IPC server 已启动" in read_gbk(guilog):
                started = True; break
            if app.poll() is not None:
                say(f"[FAIL] GUI 退出 rc={app.returncode}"); return 4
        if not started:
            say("[FAIL] 40s 内没等到「IPC server 已启动」⇒ 无证人"); return 4
        say(f"[GUI] pid={app.pid} 已监听 idm-next-ipc")

        edge_bin = next((p for p in EDGE_CANDIDATES if pathlib.Path(p).exists()), None)
        if not edge_bin:
            say("[FAIL] 没有 Edge/Chrome"); return 5
        ext_id = derive_id_from_key()
        edge = subprocess.Popen([edge_bin, "--headless=new", f"--remote-debugging-port={CDP_PORT}",
                                 f"--user-data-dir={work/'edge'}", "--no-first-run",
                                 "--no-default-browser-check", "--disable-gpu",
                                 f"--load-extension={EXT_DIR}",
                                 f"--disable-extensions-except={EXT_DIR}", "about:blank"])
        ver = None
        for _ in range(40):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json/version", timeout=3) as r:
                    ver = json.loads(r.read().decode()); break
            except Exception:
                time.sleep(0.5)
        if not ver:
            say("[FAIL] CDP 无响应"); return 5
        say(f"[浏览器] {ver.get('Browser','?')}")

        cdp = Ws(ver["webSocketDebuggerUrl"])
        tid = cdp.send("Target.createTarget", {"url": f"chrome-extension://{ext_id}/popup.html"},
                       timeout=20).get("targetId")
        sid = cdp.send("Target.attachToTarget", {"targetId": tid, "flatten": True}).get("sessionId")
        real_id = None
        for _ in range(30):
            r = cdp.send("Runtime.evaluate",
                         {"expression": "(typeof chrome!=='undefined'&&chrome.runtime&&chrome.runtime.id)||''",
                          "returnByValue": True}, timeout=20, session=sid)
            real_id = (r.get("result") or {}).get("value")
            if real_id:
                break
            time.sleep(0.5)
        say(f"[扩展] 实际 ID={real_id} ｜与 key 派生一致={real_id == ext_id}")
        if real_id != ext_id:
            say("[FAIL] 扩展 ID 不符 ⇒ 后面的回执不算这条链"); return 6
        ws = cdp

        def send_native(action, payload=""):
            expr = ("new Promise((resolve)=>{try{chrome.runtime.sendNativeMessage('com.tencent.idm_next',"
                    + "{action:'" + action + "'" + payload + "}"
                    + ",(r)=>{resolve(JSON.stringify({resp:r||null,"
                    "lastError:(chrome.runtime.lastError&&chrome.runtime.lastError.message)||null}));"
                    "});}catch(e){resolve(JSON.stringify({thrown:String(e)}));}})")
            r = ws.send("Runtime.evaluate", {"expression": expr, "awaitPromise": True,
                                             "returnByValue": True}, timeout=60, session=sid)
            v = (r.get("result") or {}).get("value")
            return json.loads(v) if v else {}

        # ══════════════ 正面 200：四层证据 ══════════════
        dl_dir = work / "dl"; dl_dir.mkdir()
        j = send_native("add_download",
                        ",url:'" + url_ok + "',filename:'payload.bin',saveDir:'"
                        + str(dl_dir).replace("\\", "/") + "'")
        resp = j.get("resp") or {}
        l1 = bool(resp.get("success")) and not j.get("lastError")
        say(f"[层① Native Host] success={resp.get('success')} lastError={j.get('lastError')!r} "
            f"msg={resp.get('message')!r}")

        final = dl_dir / "payload.bin"
        deadline = time.time() + 90
        while time.time() < deadline:
            if final.exists() and not list(dl_dir.glob("*.idmtmp")) and final.stat().st_size == size:
                break
            time.sleep(0.5)

        txt = read_gbk(guilog)
        l2 = "IPC 请求: add → 成功" in txt
        say(f"[层② GUI IPC] 出现『IPC 请求: add → 成功』={l2}")
        for line in [x for x in txt.splitlines() if "IPC 请求" in x][-3:]:
            say(f"        {line.strip()[:150]}")

        tmp_left = sorted(x.name for x in dl_dir.glob("*.idmtmp"))
        l3 = bool(final.exists()) and not tmp_left
        say(f"[层③ 磁盘] 最终文件存在={final.exists()} .idmtmp 残留={tmp_left or '无'}")

        if final.exists():
            got_size = final.stat().st_size
            got_sha = sha256_file(final)
            l4 = (got_size == size) and (got_sha == src_sha)
            say(f"[层④ 内容] size {got_size} == {size} : {got_size == size}")
            say(f"[层④ 内容] sha256 前16 {got_sha[:16]} == {src_sha[:16]} : {got_sha == src_sha}")
        else:
            l4 = False
            say("[层④ 内容] 无最终文件可比对")

        four = [l1, l2, l3, l4]
        say(f"[正面 200] 四层={'/'.join('OK' if x else 'NO' for x in four)}"
            f" ⇒ {'已观测级打通' if all(four) else '未打通（缺项见上）'}")

        # ══════════════ 404 面：不得伪成功 ══════════════
        d404 = work / "dl404"; d404.mkdir()
        j2 = send_native("add_download",
                         ",url:'" + url_404 + "',filename:'missing.bin',saveDir:'"
                         + str(d404).replace("\\", "/") + "'")
        r2 = j2.get("resp") or {}
        say(f"[404 层①] host 回执 success={r2.get('success')} msg={r2.get('message')!r}")

        # 终态要轮询到收敛：引擎重试是有界的（retry_count 递增到上限，download_core.c:1095），
        # 所以「下载中」只是中间态，睡固定秒数会把中间态当终态读。
        missing_lines, poll_err = [], None
        for _ in range(90):
            rows, poll_err = task_rows()
            missing_lines = [x for x in rows if "missing" in x.lower()]
            if missing_lines and any(k in missing_lines[0] for k in ("失败", "已完成", "已取消")):
                break
            time.sleep(1)
        say(f"[404 轮询] 终态行={missing_lines} err={poll_err}")

        files404 = sorted(x.name for x in d404.iterdir()) if d404.exists() else []
        finals = [f for f in files404 if not f.endswith(".idmtmp")]
        tmps = [f for f in files404 if f.endswith(".idmtmp")]
        say(f"[404 层③] 目录内容={files404 or '空'}")
        for f in finals + tmps:
            say(f"          {f} size={(d404 / f).stat().st_size}")

        fake_success = bool(finals)
        fake_done = any("已完成" in x for x in missing_lines)
        rows_all, _ = task_rows()
        payload_lines = [x for x in rows_all if "payload" in x.lower()]
        say(f"[正面 对照] payload 任务行={payload_lines}")
        say(f"[404 判定] 生成看似成功的最终文件={fake_success}（必须 False）｜"
            f"任务被标『已完成』={fake_done}（必须 False）")
        txt2 = read_gbk(guilog)
        fail_evidence = [x for x in txt2.splitlines()
                         if ("失败" in x or "error" in x.lower()) and "IPC 请求" not in x]
        say(f"[404 判定] 日志里的失败证据行数={len(fail_evidence)}（0 不等于缺陷，只记录）")
        for line in fail_evidence[-3:]:
            say(f"          {line.strip()[:150]}")

        if fake_success or fake_done:
            say("[VERDICT] 404 被当成成功 ⇒ 伪成功，产品缺陷"); rc = 7
        if not all(four):
            rc = rc or 8
        say("")
        say(f"[总结] 正面四层={'全绿' if all(four) else '有缺项'}｜"
            f"404 无伪成功={not (fake_success or fake_done)}")
        say(f"[工作目录] {work}")
    finally:
        try:
            if ws:
                ws.close()
        except Exception:
            pass
        for p in (edge, app, srv):
            try:
                if p and p.poll() is None:
                    p.terminate(); p.wait(timeout=10)
            except Exception:
                pass
        out = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "idm-diske2e-last.log"
        out.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
        print(f"[读数档] {out}")
    return rc


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
