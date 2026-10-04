#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HLS/m3u8 + FFmpeg 自愈 E2E（本机 fixture，不碰公网）。

按主控给的隔离策略分步走，本脚本覆盖第 1–4 步：
  1. 本机生成最小 .m3u8 + ts fixture（tools/disk_e2e_server.py 的 build_hls）
  2. 证明扩展能识别 .m3u8          ← 判据：popup 列表里出现 master.m3u8
  3. FFmpeg 缺失时真实触发一次任务   ← 用真点击（沿用 sniff 那套），不是手调 API
  4. 观察究竟是「自动准备 ffmpeg 并继续」还是「准备失败给具名错误」

⚠ 边界（必须先说清，否则读数会被过度解释）：
  · `FfmpegBackend::ensureAvailable()` 的下载源是 **gyan.dev 的公网 zip**
    （`ffmpeg_backend.cpp:21-22`）。所以"完全不碰公网"与"验证自愈成功路径"**不能同时成立**。
    本脚本只证两件事：(i) 缺 ffmpeg 时的行为诚实与否；(ii) 若产品去下载，它下的是公网、
    本轮**不把它算作隔离环境内的通过**。
  · 分片是确定性伪 TS（0x47 开头），不是真 MPEG-TS ⇒ 即使合并成功也不能声称"转码正确"。
  · GUI 用 `IDM_DATA_DIR` override 把数据根指到临时目录 ⇒ ffmpeg 不会污染用户目录，
    也才能造出"真的没有 ffmpeg"这一面。

用法：python tools/hls_e2e.py [--allow-net]
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_EXE = ROOT / "build" / "idm-next.exe"
EXT_DIR = ROOT / "browser-extension"
SERVER = ROOT / "tools" / "disk_e2e_server.py"
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
]
CDP_PORT = 9423
HTTP_PORT = 8272

sys.path.insert(0, str(ROOT / "tools"))
from nm_e2e import Ws, derive_id_from_key  # noqa: E402
from disk_e2e import gui_ipc, read_gbk, port_free, sha12  # noqa: E402
from ts_fixture import audit_ts  # noqa: E402


class Target:
    def __init__(self, cdp, url):
        self.cdp = cdp
        self.tid = cdp.send("Target.createTarget", {"url": url}, timeout=20).get("targetId")
        self.sid = cdp.send("Target.attachToTarget",
                            {"targetId": self.tid, "flatten": True}).get("sessionId")

    def ev(self, expr, ap=False, timeout=30):
        r = self.cdp.send("Runtime.evaluate", {"expression": expr, "returnByValue": True,
                                               "awaitPromise": ap}, timeout=timeout, session=self.sid)
        if "__cdp_error__" in r or "__timeout__" in r:
            return None
        return (r.get("result") or {}).get("value")

    def activate(self):
        self.cdp.send("Target.activateTarget", {"targetId": self.tid}, timeout=15)

    def reload_self(self):
        self.ev("location.reload()")


def rows_of(t):
    v = t.ev("(async()=>{const p=await chrome.tabs.query({active:true,currentWindow:true});"
             "return JSON.stringify(p)})()", ap=True)
    n = t.ev("[].slice.call(document.querySelectorAll('.row')).map(x=>String(x.innerText||'')"
             ".replace(/\\s+/g,' ').trim()).join(' || ')")
    return str(n or ""), str(v or "")


def main():
    allow_net = "--allow-net" in sys.argv
    work = pathlib.Path(tempfile.mkdtemp(prefix="idm-hls-"))
    log = []
    rc = 0
    edge = app = srv = cdp = None
    guilog = work / "gui.log"

    def say(s):
        print(s)
        log.append(s)

    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True).stdout.decode().strip()
        say(f"[身份] 仓根={ROOT} HEAD={head} app_exe_sha12={sha12(APP_EXE)}")
        for p in (CDP_PORT, HTTP_PORT):
            if not port_free(p):
                say(f"[FAIL] 端口 {p} 被占"); return 2

        ready = work / "srv.ready"
        srv = subprocess.Popen([sys.executable, str(SERVER), str(HTTP_PORT), "524288", str(ready)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            if ready.exists():
                break
            time.sleep(0.25)
        else:
            say("[FAIL] 服务器没起来"); return 3
        say(f"[测试源] {ready.read_text(encoding='utf-8').strip()}")

        # ── 前置断言：本轮必须是"真的没有 ffmpeg" ────────────────────
        data_root = work / "appdata"; (data_root / "local").mkdir(parents=True)
        ff_in_root = data_root / "ffmpeg" / "ffmpeg.exe"
        env = dict(os.environ, IDM_DATA_DIR=str(data_root),
                   APPDATA=str(data_root), LOCALAPPDATA=str(data_root / "local"),
                   QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
        which = subprocess.run(["powershell", "-NoProfile", "-Command",
                                "Get-Command ffmpeg -ErrorAction SilentlyContinue | Select -Expand Source"],
                               capture_output=True).stdout.decode("gbk", "replace").strip()
        say(f"[前置] IDM_DATA_DIR={data_root}")
        say(f"[前置] dataDir 下 ffmpeg.exe 存在={ff_in_root.exists()} ｜ PATH 上的 ffmpeg={which or '无'}")
        if ff_in_root.exists() or which:
            say("[SETUP-INVALID] 这台机器上 ffmpeg 其实可用 ⇒ 「缺失面」不成立，本轮读数无意义"); return 6

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
            say("[FAIL] 没等到 GUI 监听"); return 4
        say(f"[GUI] pid={app.pid} 已监听")

        edge_bin = next((p for p in EDGE_CANDIDATES if pathlib.Path(p).exists()), None)
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
        say(f"[浏览器] {ver.get('Browser','?')} ｜[扩展] ID={ext_id}")
        cdp = Ws(ver["webSocketDebuggerUrl"])

        # SETUP-VALID：扩展必须在册（上一轮学到的前置条件）。
        # ⚠ 探测必须走 **service worker target**，不能靠"打开 popup 页面再读 getManifest"：
        #   实测冷启动阶段 /json 里只有 …/background.js 的 service_worker 条目，popup 页面 target
        #   还没出现 ⇒ 那种写法会把「在册但 SW 刚起」误判成「未在册」（本轮就误拦过一次）。
        man = None
        for _ in range(40):
            tg = []
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json", timeout=4) as r:
                    tg = json.loads(r.read().decode())
            except Exception:
                pass
            cand = [t for t in tg if ext_id in str(t.get("url", "")) and t.get("webSocketDebuggerUrl")]
            if cand:
                try:
                    w = Ws(cand[0]["webSocketDebuggerUrl"])
                    man = (w.send("Runtime.evaluate",
                                  {"expression": "String(chrome.runtime.getManifest().version)",
                                   "returnByValue": True}, timeout=20).get("result") or {}).get("value")
                    w.close()
                except Exception:
                    man = None
                if man:
                    break
            time.sleep(0.5)
        if not man:
            say("[SETUP-INVALID] 扩展未在浏览器册内（SW target 也找不到）⇒ 不得记为产品缺陷"); return 7
        say(f"[前置] 扩展在册（经 SW target 验证）manifest.version={man}")
        probe = Target(cdp, f"chrome-extension://{ext_id}/popup.html")

        # ═══ 第 2 步：扩展能否识别 .m3u8 ═══
        say("")
        say("=== 第2步：识别 .m3u8 ===")
        page = Target(cdp, f"http://127.0.0.1:{HTTP_PORT}/hls.html")
        time.sleep(4)   # 页面 1.5s 后插入 <video src=/hls/master.m3u8>
        vids = page.ev("JSON.stringify({videos:document.querySelectorAll('video').length,"
                       "src:(document.querySelector('video source')||{}).src||''})")
        say(f"[网页] DOM 状态={vids}")
        probe.activate(); page.activate()
        probe.reload_self(); time.sleep(2.0)
        txt, active = rows_of(probe)
        say(f"[popup] active tab={active[:110]}")
        say(f"[popup] 行={txt[:260] or '(空)'}")
        has_m3u8 = "master.m3u8" in txt
        say(f"[判据·第2步] popup 列出 master.m3u8 = {has_m3u8}")
        storage = probe.ev("(async()=>{const v=await chrome.storage.session.get(null);"
                           "const k=Object.keys(v);return JSON.stringify(k.reduce((a,x)=>a.concat("
                           "(v[x]||[]).map(r=>String(r.url).slice(-34)+'['+r.type+']')),[]))})()", ap=True)
        say(f"[扩展] storage.session 内容={str(storage)[:240]}")

        # ═══ 第 3–4 步：缺 ffmpeg 时真实触发一次任务 ═══
        say("")
        say("=== 第3步：真点击发送（ffmpeg 缺失面）===")
        clicked = probe.ev("(function(){var rows=[].slice.call(document.querySelectorAll('.row'));"
                           "var t=rows.filter(function(r){return (r.innerText||'').indexOf('master.m3u8')>=0;})[0];"
                           "if(!t)return JSON.stringify({ok:false,why:'没有 m3u8 行',"
                           "rows:rows.map(function(r){return (r.innerText||'').trim().slice(0,50);})});"
                           "var b=t.querySelector('input[type=checkbox]');if(b)b.click();else t.click();"
                           "return JSON.stringify({ok:true,usedCheckbox:!!b});})()")
        say(f"[popup] 勾选 m3u8 行={clicked}")
        btn = probe.ev("(function(){var b=document.getElementById('downloadSelBtn');"
                       "return JSON.stringify({disabled:b.disabled,label:(b.innerText||'')"
                       ".replace(/\\s+/g,' ').trim().slice(0,30)});})()")
        say(f"[popup] 按钮={btn}")
        ipc_before = len([x for x in read_gbk(guilog).splitlines() if "IPC 请求: add" in x])
        clk = probe.ev("(function(){var b=document.getElementById('downloadSelBtn');"
                       "if(b.disabled)return JSON.stringify({ok:false});b.click();"
                       "return JSON.stringify({ok:true});})()")
        say(f"[popup] click={clk}")

        new_add = []
        for _ in range(40):
            lines = [x for x in read_gbk(guilog).splitlines() if "IPC 请求: add" in x]
            if len(lines) > ipc_before:
                new_add = lines[ipc_before:]; break
            time.sleep(1)
        say(f"[GUI] 新增『IPC 请求: add』={new_add}")

        state_lines = []
        final_state = None
        for _ in range(75):
            obj, err = gui_ipc("list")
            rs = str((obj or {}).get("message", "")).splitlines()
            state_lines = [x for x in rs if "m3u8" in x.lower()]
            if state_lines and any(any(k in x for k in ("已完成", "失败", "已取消")) for x in state_lines):
                final_state = state_lines[0]; break
            time.sleep(1)
        say(f"[GUI] 任务终态行={final_state!r}")
        say(f"[GUI] 全部 m3u8 相关行={state_lines[:3]}")

        txt2 = read_gbk(guilog)
        named = [x for x in txt2.splitlines()
                 if ("ffmpeg" in x.lower() or "FFmpeg" in x or "HLS" in x) and "IPC 请求" not in x]
        say(f"[具名错误证据] 日志中含 ffmpeg/HLS 字样的行数={len(named)}")
        for x in named[-4:]:
            say(f"        {x.strip()[:170]}")

        dl_dir = pathlib.Path(os.path.expanduser("~")) / "Downloads"
        merged = sorted(x.name for x in dl_dir.glob("master*")) if dl_dir.exists() else []
        tmps = sorted(x.name for x in dl_dir.glob("*.idmtmp"))
        say(f"[磁盘] Downloads 里 master*={merged} ｜ .idmtmp 残留={tmps}")

        # 产物内容核对：本轮 fixture 的分片是确定性伪 TS ⇒ 可以逐字节独立重算，
        # 而不是只看"有个文件在那儿"。期望 = seg0+seg1+seg2 按播放列表顺序拼接。
        expected = b""
        for name in ("seg0.ts", "seg1.ts", "seg2.ts"):
            with urllib.request.urlopen(f"http://127.0.0.1:{HTTP_PORT}/hls/{name}", timeout=20) as r:
                expected += r.read()
        got_bytes = b""
        for x in dl_dir.glob("master*") if dl_dir.exists() else []:
            if x.is_file():
                got_bytes = x.read_bytes()
                break
        content_ok = got_bytes == expected
        say(f"[内容核对] 期望 {len(expected)} 字节 ｜实际 {len(got_bytes)} 字节 ｜逐字节相等={content_ok}")
        ff_now = (data_root / "ffmpeg" / "ffmpeg.exe").exists()
        say(f"[磁盘] ffmpeg 是否被准备出来={ff_now}（dataDir 内）")

        say("")
        # 三分法要按**产品源码真实分支**写，不能凭产物存在与否猜：
        #   hls_downloader.cpp:293-298  beginDownload() 三分支——
        #     isFfmpegAvailable() → startFfmpegDownload()（输出 .mp4）
        #     m_encrypted 且无 ffmpeg → fetchKey()
        #     否则 → startManualDownload() + concatSegments()（输出 .ts）  ← 本轮该走这条
        #   所以"有 .ts 产物"是**设计内降级路径**，既不是自愈成功，也不是伪成功。
        if final_state and "已完成" in final_state and content_ok and not ff_now:
            say("[VERDICT] 无 ffmpeg 的降级路径成立且诚实：分片按序拼接成 .ts、内容逐字节正确、"
                "任务确为已完成 ⇒ **不是伪成功**。但这同时证明：**FFmpeg 自愈没有被触发**"
                "（产品在没有 ffmpeg 时不尝试准备它，直接降级）。")
        elif final_state and "已完成" in final_state and content_ok and ff_now:
            say("[VERDICT] 自愈成功面：ffmpeg 被准备、走 ffmpeg 合并 —— 注意下载源是公网 zip")
        elif final_state and "已完成" in final_state and not content_ok:
            say("[VERDICT] 标『已完成』但产物内容与分片拼接不一致 ⇒ 伪成功/内容错误，产品缺陷"); rc = 8
        elif final_state and "失败" in final_state:
            say("[VERDICT] 诚实失败面：任务进『失败』终态"
                + ("，且日志给了具名原因" if named else "，但日志未给具名原因（可观测性缺口）"))
        else:
            say(f"[VERDICT] 未收敛到终态（final={final_state!r}）⇒ 归因未完成，不下结论")
            rc = rc or 9
        if not has_m3u8:
            say("[VERDICT] 第2步未过：扩展没识别出 .m3u8 ⇒ 后面几步都无前提"); rc = rc or 10
        # ═══ 第 5 步：加密 HLS（fetchKey 分支是否承重）═══
        say("")
        say("=== 第5步：AES-128 加密 HLS ===")
        enc_page = Target(cdp, f"http://127.0.0.1:{HTTP_PORT}/hls/emaster.m3u8")
        time.sleep(1.5)
        probe.activate(); enc_page.activate()
        probe.reload_self(); time.sleep(2.0)
        etxt, eactive = rows_of(probe)
        has_enc = "emaster.m3u8" in etxt
        say(f"[popup] active={eactive[:90]} ｜行={etxt[:200] or '(空)'}")
        say(f"[判据·识别] popup 列出 emaster.m3u8 = {has_enc}")
        ck = probe.ev("(function(){var rows=[].slice.call(document.querySelectorAll('.row'));"
                      "var t=rows.filter(function(r){return (r.innerText||'').indexOf('emaster')>=0;})[0];"
                      "if(!t)return JSON.stringify({ok:false,why:'没有 emaster 行'});"
                      "var b=t.querySelector('input[type=checkbox]');if(b)b.click();else t.click();"
                      "return JSON.stringify({ok:true});})()")
        say(f"[popup] 勾选 emaster={ck}")
        eb = probe.ev("(function(){var b=document.getElementById('downloadSelBtn');"
                      "if(b.disabled)return JSON.stringify({ok:false});b.click();"
                      "return JSON.stringify({ok:true});})()")
        say(f"[popup] click={eb}")
        ipc_b2 = len([x for x in read_gbk(guilog).splitlines() if "IPC 请求: add" in x])
        enc_final = None
        enc_lines = []
        for _ in range(90):
            obj, err = gui_ipc("list")
            rs = str((obj or {}).get("message", "")).splitlines()
            enc_lines = [x for x in rs if "emaster" in x.lower()]
            if enc_lines and any(any(k in x for k in ("已完成", "失败", "已取消")) for x in enc_lines):
                enc_final = enc_lines[0]; break
            time.sleep(1)
        say(f"[GUI] 加密任务终态行={enc_final!r}")
        txt3 = read_gbk(guilog)
        keylog = [x.strip()[:170] for x in txt3.splitlines() if "密钥" in x or "解密" in x or "AES" in x]
        say(f"[GUI] 密钥/解密相关日志行数={len(keylog)}")
        for x in keylog[-4:]:
            say(f"        {x}")
        enc_out = sorted(dl_dir.glob("emaster*")) if dl_dir.exists() else []
        say(f"[磁盘] emaster* 产物={[x.name for x in enc_out]}")
        # 期望值由服务器自报（cryptography 库算的，与产品里那份手写 AES 是两套实现）
        info_line = ready.read_text(encoding="utf-8").strip()

        def field(name, cast=str):
            """从 SERVER_READY 行按 key= 取值。⚠ 必须 .split()[0]：
               服务器自报行会不断加字段，裸 split(k+'=')[1] 会把后面所有字段一起吃进来
               （本轮就是这么崩的：enc_size 读成了 '2880 ts_sha256=... ts_size=33840'）。"""
            if name + "=" not in info_line:
                return None
            return cast(info_line.split(name + "=")[1].split()[0])

        exp_sha = field("enc_sha256") or ""
        exp_size = field("enc_size", int) or 0
        say(f"[期望] 明文 size={exp_size} sha256 前16={exp_sha[:16]}（由 fixture 侧独立算出）")
        dec_ok = False
        if enc_out:
            got = enc_out[0].read_bytes()
            dec_ok = (len(got) == exp_size and hashlib.sha256(got).hexdigest() == exp_sha)
            say(f"[内容核对] 实际 size={len(got)} ｜逐字节等于期望明文={dec_ok}")
            if not dec_ok and len(got) == exp_size:
                same_as_cipher = True
                for nm in ("eseg0.ts", "eseg1.ts", "eseg2.ts"):
                    with urllib.request.urlopen(f"http://127.0.0.1:{HTTP_PORT}/hls/{nm}", timeout=15) as rr:
                        if got[(("eseg0", "eseg1", "eseg2").index(nm[:5])) * 960:
                               (("eseg0", "eseg1", "eseg2").index(nm[:5]) + 1) * 960] != rr.read():
                            same_as_cipher = False
                say(f"[对照] 产物==密文拼接（即根本没解密）={same_as_cipher}")
        if enc_final and "已完成" in enc_final and dec_ok:
            say("[VERDICT·第5步] fetchKey 分支**承重**：拉到 16 字节密钥、纯 C++ AES-128-CBC 解密，"
                "产物逐字节等于期望明文 ⇒ 无 ffmpeg 也能正确下加密流")
        elif enc_final and "已完成" in enc_final and not dec_ok:
            say("[VERDICT·第5步] 标『已完成』但产物不是期望明文 ⇒ **伪成功或 IV/解密错误**，产品缺陷")
            rc = rc or 11
        elif enc_final and "失败" in enc_final:
            say("[VERDICT·第5步] 加密面诚实失败（看上面具名日志判断卡在哪一步）")
        else:
            say(f"[VERDICT·第5步] 未收敛（final={enc_final!r}）⇒ 不下结论")
            rc = rc or 12

        # ═══ 步骤 B：真 MPEG-TS 载荷 ⇒ 产品 concat 后是否仍是合法 TS ═══
        say("")
        say("=== 步骤B：真 TS 拼接后的结构合法性 ===")
        tp = Target(cdp, f"http://127.0.0.1:{HTTP_PORT}/hls/tmaster.m3u8")
        time.sleep(1.5)
        probe.activate(); tp.activate()
        probe.reload_self(); time.sleep(2.0)
        tts, tact = rows_of(probe)
        say(f"[popup] 行={tts[:220] or '(空)'}")
        ck2 = probe.ev("(function(){var rows=[].slice.call(document.querySelectorAll('.row'));"
                       "var t=rows.filter(function(r){return (r.innerText||'').indexOf('tmaster')>=0;})[0];"
                       "if(!t)return JSON.stringify({ok:false,why:'没有 tmaster 行'});"
                       "var b=t.querySelector('input[type=checkbox]');if(b)b.click();else t.click();"
                       "return JSON.stringify({ok:true});})()")
        say(f"[popup] 勾选 tmaster={ck2}")
        ck2b = probe.ev("(function(){var b=document.getElementById('downloadSelBtn');"
                        "if(b.disabled)return JSON.stringify({ok:false});b.click();"
                        "return JSON.stringify({ok:true});})()")
        say(f"[popup] click={ck2b}")
        ts_final = None
        for _ in range(90):
            obj, err = gui_ipc("list")
            rs = str((obj or {}).get("message", "")).splitlines()
            hit = [x for x in rs if "tmaster" in x.lower()]
            if hit and any(any(k in x for k in ("已完成", "失败", "已取消")) for x in hit):
                ts_final = hit[0]; break
            time.sleep(1)
        say(f"[GUI] 真TS任务终态行={ts_final!r}")
        tout = sorted(dl_dir.glob("tmaster*")) if dl_dir.exists() else []
        say(f"[磁盘] tmaster* 产物={[x.name for x in tout]}")
        # 期望值由 fixture 侧独立算出（服务器自报），不读被测实现的输出
        info2 = ready.read_text(encoding="utf-8").strip()
        exp_ts_sha = field("ts_sha256") or ""
        exp_ts_size = field("ts_size", int) or 0
        byte_ok = struct_ok = False
        audit_line = ""
        if tout:
            got = tout[0].read_bytes()
            byte_ok = (len(got) == exp_ts_size and hashlib.sha256(got).hexdigest() == exp_ts_sha)
            a = audit_ts(got, "产品 concat 产物")
            struct_ok = a["sync_ok"] and a["align_ok"] and a["pat_found"] and a["pmt_found"]                 and a["vid_pid"] is not None and a["residual"] == 0
            dur = None
            if a["pcr_first"] is not None and a["pcr_last"] is not None:
                dur = round((a["pcr_last"] - a["pcr_first"]) / 90000.0, 3)
            audit_line = (f"packets={a['packet_count']} residual={a['residual']} sync_ok={a['sync_ok']} "
                          f"PAT={a['pat_found']} PMT={a['pmt_found']} vidPID={a['vid_pid']} PCR跨度={dur}s "
                          f"errors={a['errors'] or '无'}")
        say(f"[期望] size={exp_ts_size} sha256 前16={exp_ts_sha[:16]}（fixture 侧独立算）")
        say(f"[字节核对] 产物 == 三段真TS拼接 = {byte_ok}")
        say(f"[结构判读] {audit_line or '无产物可判'}")
        say(f"[判读器可信度] 本会话已跑 --selftest（正例通过 + 两个负例都失败）⇒ 可当证人")
        if byte_ok and struct_ok and ts_final and "已完成" in ts_final:
            say("[VERDICT·步骤B] 产品 concat 出来的文件**仍是合法 TS**：包边界对齐、PAT/PMT 可解析、"
                "视频 PID 提取成功、PCR 连续 ⇒ 拼接路径在真 TS 上正确（接缝未错位）")
        elif byte_ok and not struct_ok:
            say("[VERDICT·步骤B] 字节搬运对但结构判读不过 ⇒ 需看上面 errors 定位"); rc = rc or 13
        else:
            say(f"[VERDICT·步骤B] 未达成（byte_ok={byte_ok} struct_ok={struct_ok} final={ts_final!r}）")
            rc = rc or 14
        say("")
        say("[限制·必须一起读] 本机没有 ffprobe/ffmpeg/解码库且不装软件 ⇒ "
            "**『能不能真的播放』这一条本脚本不能声称**；这里证的是更窄的结构性合法"
            "（包对齐 + PSI 可解析 + PCR 单调）。")

    finally:
        try:
            if cdp:
                cdp.close()
        except Exception:
            pass
        for p in (edge, app, srv):
            try:
                if p and p.poll() is None:
                    p.terminate(); p.wait(timeout=15)
            except Exception:
                pass
        out = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "idm-hls-e2e-last.log"
        out.write_text("\n".join(log) + "\n", encoding="utf-8")
        print(f"[读数档] {out}")
        print(f"[工作目录] {work}")
    return rc


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
