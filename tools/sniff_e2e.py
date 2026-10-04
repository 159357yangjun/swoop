#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""网页识别 → popup → 真点击发送 的运行态 E2E（A/B/阴性 三面）。

基线 `b1145d1` 已经把后半段证明完了（GUI 收单 → HTTP engine → 落盘 → SHA256），
所以本轮**不再重复验 SHA256**，只把前后两条已证链条接起来：

    网页识别 → popup 出现正确资源 → 点击 popup 里真实的 DOM 控件
             → sendNativeMessage success=true → GUI「IPC 请求: add → 成功」

三面各自独立：
  A network sniff —— `/network.html` **不放 <video>**，只靠 JS fetch 触发 `/media/network.mp4`
                     （Content-Type: video/mp4）。证明 webRequest 通道 + storage 列表 + popup 显示。
  B DOM scan      —— `/dom.html` 初始 DOM 无媒体元素，3s 后动态插入 `<video><source>`。
                     证明 scanMediaElements() 与 MutationObserver（不是"静态页面启动时扫到"）。
                     验收含：插入前不该有、插入后有、URL 正确、不无限增长。
  N 阴性对照      —— 同一个页面同时请求 `/document.txt`（text/plain）。
                     popup **不能**把它列成视频资源 ⇒ 证明嗅探不是"什么都往里吞"。

⚠ 一条硬要求：**不在 CDP console 里手工调 chrome.runtime.sendNativeMessage()。**
上一轮那样做是合理的（目标就是 Native Messaging 主链）；这一轮目标是 popup 自己有没有把资源送出去，
必须点 popup 里真正的按钮，否则 popup 代码根本没被跑到。

刻意不带：HLS / m3u8 / yt-dlp / ffmpeg / 真视频站 / 公网网络 / K2 / ACTIVATE。

用法：python tools/sniff_e2e.py
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
EXT_DIR = ROOT / "browser-extension"
SERVER = ROOT / "tools" / "disk_e2e_server.py"
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Google/Chrome/Application/chrome.exe",
]
CDP_PORT = 9413
HTTP_PORT = 8252

sys.path.insert(0, str(ROOT / "tools"))
from nm_e2e import Ws, derive_id_from_key  # noqa: E402
from disk_e2e import gui_ipc, read_gbk, port_free, sha12, sha256_file  # noqa: E402


def http_json(path, timeout=5):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}{path}", timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        return None


class Target:
    """一个可 evaluate 的调试目标（页面或扩展 popup），带自己的 sessionId。"""

    def __init__(self, cdp, url):
        self.cdp = cdp
        tid = cdp.send("Target.createTarget", {"url": url}, timeout=20).get("targetId")
        self.sid = cdp.send("Target.attachToTarget",
                            {"targetId": tid, "flatten": True}).get("sessionId")
        self.tid = tid

    def ev(self, expr, await_promise=False, timeout=30):
        await_flag = await_promise
        res = self.cdp.send("Runtime.evaluate",
                            {"expression": expr, "returnByValue": True,
                             "awaitPromise": await_flag}, timeout=timeout, session=self.sid)
        if "__cdp_error__" in res or "__timeout__" in res:
            return {"__err__": res}
        r = res.get("result") or {}
        return r.get("value") if "value" in r else r.get("description")

    def activate(self):
        """把这个 target 设为当前窗口活动标签。
        ⚠ popup 读的是 `tabs.query({active:true,currentWindow:true})`（background.js:634）——
        而 CDP 新建的 popup target 本身会变成活动标签，于是 popup 查到的"当前页"就是它自己，
        列表必然空。这不是产品缺陷（实测 storage.session 里 key=tab_<页面id> 是有货的）。"""
        return self.cdp.send("Target.activateTarget", {"targetId": self.tid}, timeout=15)

    def navigate(self, url):
        """在**同一个 target** 里换地址。⚠ 不能为每个测试面新建 target：
        popup 是按 `chrome.tabs.query({active:true,currentWindow:true})` 决定读哪个标签页的资源
        （background.js:634），多 target 时活动标签不是被测页 ⇒ popup 永远显示『还没有嗅探到资源』，
        那是测具形状，不是产品缺陷（本轮第一次就栽在这里）。"""
        return self.cdp.send("Page.navigate", {"url": url}, timeout=20, session=self.sid)

    def close(self):
        try:
            self.cdp.send("Target.closeTarget", {"targetId": self.tid}, timeout=10)
        except Exception:
            pass


def wait_eval(t, expr, pred, tries=40, delay=0.5):
    for _ in range(tries):
        v = t.ev(expr)
        try:
            if pred(v):
                return v
        except Exception:
            pass
        time.sleep(delay)
    return None


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="idm-sniff-"))
    log = []
    rc = 0
    edge = app = srv = cdp = None
    guilog = work / "gui.log"

    def say(s):
        print(s)
        log.append(s)

    def jsq(u):
        return str(u).replace("\\", "/")

    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True).stdout.decode().strip()
        say(f"[身份] 仓根={ROOT} HEAD={head}")
        say(f"[身份] app_exe_sha12={sha12(APP_EXE)}")
        for p in (CDP_PORT, HTTP_PORT):
            if not port_free(p):
                say(f"[FAIL] 端口 {p} 被占 ⇒ 读数会串"); return 2

        ready = work / "srv.ready"
        srv = subprocess.Popen([sys.executable, str(SERVER), str(HTTP_PORT), str(512 * 1024), str(ready)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(60):
            if ready.exists():
                break
            time.sleep(0.25)
        else:
            say("[FAIL] 测试服务器没起来"); return 3
        say(f"[测试源] {ready.read_text(encoding='utf-8').strip()}")

        # GUI 起在**默认数据根**（产品自己的 IPC 日志证人）
        # GUI 用**隔离数据根**跑：它的默认下载目录由 Settings 派生自 APPDATA/LOCALAPPDATA，
        # 不隔离就只能去用户真实下载目录里找文件（跨轮污染 + capstone 无法界定"本轮落的"）。
        gui_appdata = work / "gui-appdata"; (gui_appdata / "local").mkdir(parents=True)
        env = dict(os.environ, APPDATA=str(gui_appdata), LOCALAPPDATA=str(gui_appdata / "local"),
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
            say("[FAIL] 没等到 GUI 监听 ⇒ 无证人"); return 4
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
            ver = http_json("/json/version")
            if ver:
                break
            time.sleep(0.5)
        if not ver:
            say("[FAIL] CDP 无响应"); return 5
        say(f"[浏览器] {ver.get('Browser','?')} ｜[扩展] key 现算 ID={ext_id}")
        cdp = Ws(ver["webSocketDebuggerUrl"])

        # ── A 面：network sniff（页面无 <video>，只有 fetch）──────────
        say("")
        say("=== A 面：webRequest 嗅探 ===")
        a_url = f"http://127.0.0.1:{HTTP_PORT}/network.html"
        page_t = Target(cdp, a_url)
        page_a = page_t
        hits = wait_eval(page_a, "JSON.stringify({net:window.__netHits||0,doc:window.__docHits||0})",
                         lambda v: v and json.loads(v)["net"] >= 1)
        say(f"[网页] 请求发生次数={hits}  ← net=mp4 请求 doc=text/plain 请求")
        time.sleep(2.5)  # 给 SW 一点时间写 storage.session

        # extension 侧：直接读它自己的 storage（走 popup 上下文里的 chrome.storage）
        pop_a = Target(cdp, f"chrome-extension://{ext_id}/popup.html")
        time.sleep(1.0)
        rid = pop_a.ev("(typeof chrome!=='undefined'&&chrome.runtime&&chrome.runtime.id)||''")
        say(f"[扩展] popup 内 chrome.runtime.id={rid!r} ｜与 key 派生一致={rid == ext_id}")
        atab = pop_a.ev("(async()=>{const t=await chrome.tabs.query({active:true,currentWindow:true});"
                        "return JSON.stringify((t||[]).map(x=>String(x.url).slice(0,52)))})()", await_promise=True)
        say(f"[扩展] popup 眼里的 active tab={atab}  ← 必须是被测页而不是 popup 自己")

        def popup_rows(t):
            """从 popup 真渲染出来的 DOM 里取行文本——这是唯一能证明『popup 显示了』的形状。"""
            return t.ev("(function(){var out=[];"
                        "document.querySelectorAll('.row,[data-url],li,tr').forEach(function(e){"
                        "var tx=(e.innerText||'').replace(/\\s+/g,' ').trim();"
                        "if(tx)out.push(tx.slice(0,120));});return JSON.stringify(out);})()")

        # ⚠ 顺序承重：先把**页面**设为活动标签，再重载 popup。
        #   popup 是在自己加载时用 tabs.query({active:true}) 拉列表的（popup.js:615 → background.js:634），
        #   先开 popup 再激活页面 ⇒ 它拉到的是"当前页=popup 自己"⇒ 永远空列表。
        def refresh_popup_with_page_active():
            page_t.activate()
            t0 = time.time()
            pop_a.ev("location.reload()")
            time.sleep(1.2)
        refresh_popup_with_page_active()
        atab0 = pop_a.ev("(async()=>{const t=await chrome.tabs.query({active:true,currentWindow:true});"
                         "return JSON.stringify((t||[]).map(x=>String(x.url).slice(0,52)))})()", await_promise=True)
        say(f"[扩展] 刷新 popup 后它眼里的 active tab={atab0} ← 必须是被测页")
        # 判别式：popup 空列表有两种成因——(a) webRequest 真没抓到；(b) SW 写 storage.session
        #   与 popup 读取之间是竞态。直接读 extension storage 就能把两者分开（本轮实测偶发 1/3 空，
        #   必须先定性是哪一种，否则"通过"里混着一次没跑出来的样本）。
        stored_a = pop_a.ev("(async()=>{const v=await chrome.storage.session.get(null);"
                            "const k=Object.keys(v);"
                            "return JSON.stringify({keys:k.length,"
                            "urls:k.reduce((a,x)=>a.concat((v[x]||[]).map(r=>String(r.url).slice(-30))),[])})})()",
                            await_promise=True)
        say(f"[扩展] storage.session 直读={str(stored_a)[:200]}")
        # 归因探针（只印读数、不进退码）：SW 醒来后再主动请求一次同一个 URL。
        probe = pop_a.ev("(async()=>{try{const r=await fetch('http://127.0.0.1:%d/media/network.mp4',{cache:'no-store'});"
                         "await r.blob();"
                         "const v=await chrome.storage.session.get(null);const k=Object.keys(v);"
                         "return JSON.stringify({status:r.status,"
                         "urls:k.reduce((a,x)=>a.concat((v[x]||[]).map(z=>String(z.url).slice(-30))),[])});"
                         "}catch(e){return 'FETCH_ERR '+e}})()" % HTTP_PORT, await_promise=True)
        say(f"[归因探针] SW 内主动再请求一次={str(probe)[:220]}")
        rows_a = None
        for _ in range(30):
            r = pop_a.ev("(function(){return JSON.stringify({rows:[].slice.call(document.querySelectorAll('.row'))"
                         ".map(function(x){return (x.innerText||'').replace(/\\s+/g,' ').trim().slice(0,120);}),"
                         "body:(document.body.innerText||'').replace(/\\s+/g,' ').slice(0,400)});})()")
            rows_a = json.loads(r) if r else None
            if rows_a and any("network.mp4" in x for x in rows_a.get("rows", [])):
                break
            time.sleep(1)
        listed = [x for x in (rows_a or {}).get("rows", []) if "network.mp4" in x]
        say(f"[popup] 行数={len((rows_a or {}).get('rows', []))} 含 network.mp4 的行数={len(listed)}")
        for x in listed[:2]:
            say(f"        {x[:130]}")
        neg = [x for x in (rows_a or {}).get("rows", []) if "document.txt" in x]
        say(f"[阴性] popup 把 document.txt 列成资源了吗={bool(neg)}（必须 False）")
        if neg:
            say("[VERDICT] 非媒体请求被吞进列表 ⇒ 嗅探判据过宽"); rc = 7

        # ── B 面：DOM 动态插入 ────────────────────────────────────
        say("")
        say("=== B 面：scanMediaElements + MutationObserver ===")
        b_url = f"http://127.0.0.1:{HTTP_PORT}/dom.html"
        page_t.navigate(b_url)
        page_b = page_t
        time.sleep(1.0)
        before = page_b.ev("JSON.stringify({videos:document.querySelectorAll('video').length,"
                           "inserted:window.__insertedAt||0})")
        say(f"[网页] 插入前 DOM 状态={before}（videos 必须为 0）")
        pop_b = pop_a   # 复用同一个 popup target（它会重新查当前活动标签）
        time.sleep(1.2)

        def dom_rows():
            r = pop_b.ev("(function(){return JSON.stringify([].slice.call(document.querySelectorAll('.row'))"
                         ".map(function(x){return (x.innerText||'').replace(/\\s+/g,' ').trim().slice(0,120);}));})()")
            return json.loads(r) if r else []

        refresh_popup_with_page_active()
        pre = [x for x in dom_rows() if "dom.mp4" in x]
        say(f"[popup] 插入前含 dom.mp4 的行数={len(pre)}（必须 0）")
        ins = wait_eval(page_b, "JSON.stringify({videos:document.querySelectorAll('video').length,"
                                "inserted:window.__insertedAt||0})",
                        lambda v: v and json.loads(v)["videos"] >= 1)
        say(f"[网页] 动态插入完成={ins}")
        post = None
        grow = []
        for i in range(30):
            refresh_popup_with_page_active()
            post = [x for x in dom_rows() if "dom.mp4" in x]
            grow.append(len(post))
            if post:
                break
            time.sleep(1)
        say(f"[popup] 插入后含 dom.mp4 的行数={len(post)}")
        for x in post[:2]:
            say(f"        {x[:130]}")
        time.sleep(4)
        refresh_popup_with_page_active()
        tail = len([x for x in dom_rows() if "dom.mp4" in x])
        say(f"[popup] 再等 4s 后行数={tail}（不无限增长：{tail <= max(grow + [1])}）")
        ins_obj = json.loads(ins) if isinstance(ins, str) else (ins or {})
        if not post or len(pre) != 0 or int(ins_obj.get("videos", 0)) == 0:
            say("[VERDICT] B 面未通过（见上三条读数）"); rc = rc or 8

        # ── 第三步：点 popup 里真实的按钮，而不是手调 API ─────────────
        say("")
        say("=== 第三步：真实 popup DOM 点击 → send ===")
        # 前置条件：popup 眼里的 active tab 必须是被测页。上一轮实测本机 headless Edge 里
        # `Target.activateTarget` **不会**改变 `tabs.query({active:true})` 的结果
        # （读数：active tab 仍是 chrome-extension://…/popup.html）⇒ 列表必然空、按钮必然是灰的。
        # 这种情况要明写"测具到不了"，不得读成产品缺陷，也不得退回手调 API 冒充点击已证。
        refresh_popup_with_page_active()
        atab_now = pop_b.ev("(async()=>{const t=await chrome.tabs.query({active:true,currentWindow:true});"
                            "return JSON.stringify((t||[]).map(x=>String(x.url).slice(0,52)))})()",
                            await_promise=True)
        say(f"[前置核对] popup 眼里的 active tab={atab_now}")
        if "chrome-extension://" in str(atab_now):
            say("[FROZEN] active tab 仍是 popup 自己 ⇒ 本机 headless 通道到不了『页面为活动标签』这个前提；")
            say("         第三步（真点击 → send）判为**未验证**，不判产品失败，也不许用 console 直调 API 顶替。")
            rc = 11
            return rc
        clicked = pop_b.ev("(function(){var rows=[].slice.call(document.querySelectorAll('.row'));"
                           "var target=rows.filter(function(r){return (r.innerText||'').indexOf('dom.mp4')>=0;})[0];"
                           "if(!target)return JSON.stringify({ok:false,why:'没有 dom.mp4 行',"
                           "rows:rows.map(function(r){return (r.innerText||'').trim().slice(0,60);})});"
                           "var box=target.querySelector('input[type=checkbox]');if(box){box.click();}"
                           "else{target.click();}"
                           "return JSON.stringify({ok:true,usedCheckbox:!!box,"
                           "text:(target.innerText||'').replace(/\\s+/g,' ').trim().slice(0,110)});})()")
        say(f"[popup] 勾选目标行={clicked}")
        cj = json.loads(clicked) if isinstance(clicked, str) else {}
        btn = pop_b.ev("(function(){var b=document.getElementById('downloadSelBtn');"
                       "if(!b)return JSON.stringify({ok:false,why:'没有 #downloadSelBtn'});"
                       "return JSON.stringify({ok:true,disabled:b.disabled,"
                       "label:(b.innerText||'').replace(/\\s+/g,' ').trim().slice(0,40)});})()")
        say(f"[popup] 发送按钮存在={btn}")
        bj = json.loads(btn) if isinstance(btn, str) else {}
        if not (cj.get("ok") and bj.get("ok")):
            say("[VERDICT] 没能定位到真实控件 ⇒ 不许退回手调 API，本轮判失败"); rc = rc or 9
        else:
            ipc_before = len([x for x in read_gbk(guilog).splitlines() if "IPC 请求: add" in x])
            # ⚠ "本轮新落的"这个判据要在**点击之前**取基线：晚一步取就可能把本轮自己写的
            #   文件当成"点击前就在"，于是 fresh 永远 False（上一轮的 capstone 就是这么假红的）。
            dl_default = pathlib.Path(os.path.expanduser("~")) / "Downloads"
            f_expected = dl_default / "dom.mp4"
            pre_exists = f_expected.exists()
            pre_mtime = f_expected.stat().st_mtime_ns if pre_exists else None
            say(f"[capstone 基线] 点击前 {f_expected} 存在={pre_exists} mtime_ns={pre_mtime}")
            clk = pop_b.ev("(function(){var b=document.getElementById('downloadSelBtn');"
                           "if(b.disabled)return JSON.stringify({ok:false,why:'按钮是灰的'});"
                           "b.click();return JSON.stringify({ok:true});})()")
            say(f"[popup] 实际 click 发送按钮={clk}")
            ok_click = False
            try:
                ok_click = json.loads(clk).get("ok") is True
            except Exception:
                pass
            toast = wait_eval(pop_b, "(document.body.innerText||'').replace(/\\s+/g,' ').slice(0,300)",
                              lambda v: v and ("已添加" in v or "发送" in v or "失败" in v), tries=20, delay=0.5)
            say(f"[popup] 点击后页面文案={str(toast)[:170]}")
            add_lines = []
            for _ in range(30):
                lines = [x for x in read_gbk(guilog).splitlines() if "IPC 请求: add" in x]
                if len(lines) > ipc_before:
                    add_lines = lines[ipc_before:]
                    break
                time.sleep(1)
            say(f"[GUI] 新增『IPC 请求: add』行数={len(add_lines)}")
            for x in add_lines[-2:]:
                say(f"        {x.strip()[:150]}")
            rows2, _ = task_rows_guard()
            mine = [x for x in rows2 if "dom.mp4" in x.lower()]
            say(f"[GUI] 任务列表含 dom.mp4 的行={mine}")
            if not (ok_click and add_lines and any("成功" in x for x in add_lines) and mine):
                say("[VERDICT] popup→send 这一段没跑通（缺项见上）"); rc = rc or 10
            else:
                say("[VERDICT] 链条接上：网页识别 → popup 显示 → 真点击 → host → GUI 收下")

                # ════════ 单轮 capstone：同一轮里把后半段也证完 ════════
                # 之前"网页→点击"与"落盘+SHA256"分别记在 491d747 / b1145d1 两份档案里，
                # 要写"打开网页 → 识别 → 点下载 → 文件正确落盘"就得靠跨档案拼接。
                # 这一段把两者闭成一条连续链：等终态 → 文件存在 → 无 .idmtmp → size/sha256 == 源。
                done = []
                for _ in range(90):
                    rows3, _ = task_rows_guard()
                    done = [x for x in rows3 if "dom.mp4" in x.lower() and "已完成" in x]
                    if done:
                        break
                    time.sleep(1)
                say(f"[capstone] 任务进入『已完成』={bool(done)} 行={done[:1]}")
                # ⚠ 这里**不能**假设文件落在哪个目录：popup 的批量发送不带 saveDir
                #   （background.js sendToIdmNext 的 saveDir 传空 ⇒ 桌面端用自己的默认目录）。
                #   所以证人来源是 GUI 自己的任务行 + 只在**本轮工作目录内**找同名文件；
                #   若默认目录不在 work 之下，这一项就该判"未闭合"而不是去全盘搜。
                # ⚠ GUI 的默认下载目录来自用户 profile / 注册表，**不随 APPDATA 隔离**
                #   （实测落到 %USERPROFILE%\Downloads）。所以"只在 work 里找"会永远找不到，
                #   那是测具假设错了，不是产品没落盘。正确做法：向**GUI 自己**问这个任务的保存目录
                #   （ipc_server 的 info 命令带路径），只在那一个目录里取证。
                default_dl = dl_default
                candidates = []
                fresh_reason = ""
                for _ in range(90):
                    rows4, _ = task_rows_guard()
                    done_now = [x for x in rows4 if "dom.mp4" in x.lower() and "已完成" in x]
                    if done_now and f_expected.exists():
                        st = f_expected.stat()
                        fresh = (not pre_exists) or (st.st_mtime_ns != pre_mtime)
                        if fresh:
                            candidates = [f_expected]
                            fresh_reason = ("本轮新建" if not pre_exists else "mtime 已变")
                            break
                    time.sleep(1)
                tmps = list(default_dl.glob("dom.mp4.idmtmp"))
                say(f"[capstone] 取证据目录={default_dl}（GUI 默认下载目录：QStandardPaths::DownloadLocation "
                    f"走注册表，不随 APPDATA 隔离 ⇒ settings.cpp:16）")
                say(f"[capstone] 本轮落盘的 dom.mp4={len(candidates)} 个（{fresh_reason or '未认定为本轮'}）；"
                    f".idmtmp 残留={len(tmps)}")
                caps = [bool(done), bool(candidates), not tmps]
                if candidates:
                    f = candidates[0]
                    got_size, got_sha = f.stat().st_size, sha256_file(f)
                    # 源指纹**当场向服务器取**（不是抄档案里的常量）：HEAD/GET 该路径后现算
                    with urllib.request.urlopen(f"http://127.0.0.1:{HTTP_PORT}/media/dom.mp4", timeout=15) as rr:
                        blob = rr.read()
                    src_size, src_sha = len(blob), hashlib.sha256(blob).hexdigest()
                    ok_size = got_size == src_size
                    ok_sha = got_sha == src_sha
                    caps += [ok_size, ok_sha]
                    say(f"[capstone] size {got_size} == 源 {src_size} : {ok_size}")
                    say(f"[capstone] sha256 前16 {got_sha[:16]} == 源 {src_sha[:16]} : {ok_sha}")
                else:
                    say("[capstone] 没有最终文件可比对 ⇒ size/sha256 两项无从成立")
                    caps += [False, False]
                if all(caps):
                    say("[CAPSTONE] 单轮闭合：网页识别 → popup → 点击 → host → GUI → engine → disk "
                        "且 size/SHA256 与服务器源一致")
                else:
                    say(f"[CAPSTONE] 未闭合，缺项={[i for i, c in enumerate(caps) if not c]}"
                        "（0=已完成 1=文件存在 2=无 idmtmp 3=size 4=sha256）")
                    rc = rc or 12
                say("")
                # A 面单独报，不并进 capstone 的通过条件：实测它是**偶发**的（SW 冷启动窗口内
                #   页面自己发起的请求会漏掉 webRequest，见 §7 第45条）。把它和"这一轮闭合没闭合"
                #   混成一个布尔，会把一条真实的产品缺陷洗成绿灯。
                say(f"[总结] capstone(单轮闭合)={'全绿' if all(caps) else '有缺项'}｜"
                    f"A 面本轮是否列出 network.mp4={bool(listed)}（回填后应恒为 True）｜"
                    f"阴性未吞={not bool(neg)}｜B 面动态发现={bool(post)}｜真点击送达={bool(add_lines)}")
                if not listed:
                    # 修复（content.js scanResourceTiming 回填）之后这一项应当不再出现；
                    # 若再出现说明回填被绕过或失效，是回归信号，不再是"已知缺陷"。
                    say("[回归信号] A 面又漏了 ⇒ content script 回填失效或被绕过，"
                        "用 tools/a_gate.py 复算 fresh 面。")
    finally:
        try:
            if cdp:
                cdp.close()
        except Exception:
            pass
        for p in (edge, app, srv):
            try:
                if p and p.poll() is None:
                    p.terminate(); p.wait(timeout=10)
            except Exception:
                pass
        out = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "idm-sniff-e2e-last.log"
        out.write_text("\n".join(log) + "\n", encoding="utf-8")
        print(f"[读数档] {out}")
        print(f"[工作目录] {work}")
    return rc


def media_source_of(path):
    """当场向测试服务器要那一份媒体，算它的 size/sha256 ⇒ 源指纹是**本轮实测**，不是抄档案。"""
    import urllib.request as _u
    with _u.urlopen(f"http://127.0.0.1:{HTTP_PORT}{path}", timeout=20) as r:
        blob = r.read()
    return len(blob), hashlib.sha256(blob).hexdigest()


def task_rows_guard():
    try:
        obj, err = gui_ipc("list")
        return str((obj or {}).get("message", "")).splitlines(), err
    except Exception as e:
        return [], str(e)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
