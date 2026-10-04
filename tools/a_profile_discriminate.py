#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A 面偶发缺失的判别实验：同一 profile 连续复用 vs 每轮新建 profile。

背景（§7 第 46/50 条）：加 SETUP-VALID 前置断言后，10 遍里仍有 3 遍"扩展未在浏览器册内"，
且失败**成段出现**（第 2/3/4 连续三遍）。当时的解释是"新 profile 首次启动时 Edge 扩展注册未就绪"，
但那只记为强推断。本脚本用一次对照把它变成读数：

  face=reuse —— **同一个 user-data-dir 起一次 Edge，10 个页面在同一进程里逐个开**
                 ⇒ 扩展只会被注册一次，之后每轮都该在册。
  face=fresh —— 每轮**新建一个 user-data-dir + 重起 Edge**（就是前几轮的做法）
                 ⇒ 每轮都可能撞上首启注册窗口。

判据（每轮一条，不合并）：
  SETUP-VALID?  = 能不能连上 chrome-extension://<key 现算 ID>/ 的 target 并读到 getManifest().version
  A 面命中?     = popup 列表里有没有 network.mp4（沿用已证的顺序：activateTarget(页面) → 再 reload popup）

⚠ 两条已知测具形状，别改回去（都是本会话踩出来的）：
  · popup 在自己加载时才按 tabs.query({active:true}) 拉资源 ⇒ 必须先激活页面、再重载 popup；
  · headless 下 browser 级连接没有 Log.enable（-32601），SW 相关一律走它自己的 target。

用法：python tools/a_profile_discriminate.py <reuse|fresh> [rounds]
"""
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXT_DIR = ROOT / "browser-extension"
SERVER = ROOT / "tools" / "disk_e2e_server.py"
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
]
CDP_PORT = 9433
HTTP_PORT = 8296

sys.path.insert(0, str(ROOT / "tools"))
from nm_e2e import Ws, derive_id_from_key  # noqa: E402


def cdp_version(timeout_s=25):
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json/version", timeout=3) as r:
                return json.loads(r.read().decode())
        except Exception:
            time.sleep(0.5)
    return None


def targets():
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json", timeout=5) as r:
            return json.loads(r.read().decode())
    except Exception:
        return []


def ev(cdp, sid, expr, ap=False, timeout=25):
    r = cdp.send("Runtime.evaluate", {"expression": expr, "returnByValue": True, "awaitPromise": ap},
                 timeout=timeout, session=sid)
    if "__cdp_error__" in r or "__timeout__" in r:
        return None
    return (r.get("result") or {}).get("value")


def open_target(cdp, url):
    tid = cdp.send("Target.createTarget", {"url": url}, timeout=20).get("targetId")
    sid = cdp.send("Target.attachToTarget", {"targetId": tid, "flatten": True}).get("sessionId")
    return tid, sid


def setup_valid(ext_id, tries=40):
    """扩展是否在册：能连上它自己的 target 且读到 manifest.version。返回 (ok, version, 用时秒)。"""
    t0 = time.time()
    for _ in range(tries):
        cand = [t for t in targets() if ext_id in str(t.get("url", "")) and t.get("webSocketDebuggerUrl")]
        if cand:
            try:
                w = Ws(cand[0]["webSocketDebuggerUrl"])
                v = (w.send("Runtime.evaluate",
                            {"expression": "String(chrome.runtime.getManifest().version)",
                             "returnByValue": True}, timeout=20).get("result") or {}).get("value")
                w.close()
                if v:
                    return True, v, round(time.time() - t0, 2)
            except Exception:
                pass
        time.sleep(0.5)
    return False, None, round(time.time() - t0, 2)


def one_round_a(cdp, ext_id, http_port):
    """在已有浏览器里跑一遍 A 面识别。返回 (setup_ok, listed, note)。"""
    page_tid, page_sid = open_target(cdp, f"http://127.0.0.1:{http_port}/network.html")
    ok, ver, waited = setup_valid(ext_id)
    if not ok:
        return False, False, f"扩展不可连（等 {waited}s）"
    time.sleep(3.0)                       # 让页面 fetch 与 SW 写 storage 有时间
    pop_tid, pop_sid = open_target(cdp, f"chrome-extension://{ext_id}/popup.html")
    # 顺序承重：先激活页面，再重载 popup（popup 在加载时才按 active tab 取资源）
    cdp.send("Target.activateTarget", {"targetId": page_tid}, timeout=15)
    ev(cdp, pop_sid, "location.reload()")
    time.sleep(2.0)
    rows = ev(cdp, pop_sid, "[].slice.call(document.querySelectorAll('.row'))"
                            ".map(x=>String(x.innerText||'').replace(/\\s+/g,' ').trim()).join(' || ')") or ""
    active = ev(cdp, pop_sid, "(async()=>{const t=await chrome.tabs.query({active:true,currentWindow:true});"
                              "return JSON.stringify((t||[]).map(x=>String(x.url).slice(0,40)))})()", ap=True)
    for t in (pop_tid,):
        cdp.send("Target.closeTarget", {"targetId": t}, timeout=10)
    cdp.send("Target.closeTarget", {"targetId": page_tid}, timeout=10)
    return True, ("network.mp4" in rows), f"ver={ver} active={str(active)[:60]} 行数={rows.count('||') + 1 if rows else 0}"


def launch(profile, extra_args=()):
    edge = next((p for p in EDGE_CANDIDATES if pathlib.Path(p).exists()), None)
    if not edge:
        return None
    args = [edge, "--headless=new", f"--remote-debugging-port={CDP_PORT}",
            f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
            "--disable-gpu", f"--load-extension={EXT_DIR}",
            f"--disable-extensions-except={EXT_DIR}", *extra_args, "about:blank"]
    return subprocess.Popen(args)


def main():
    face = sys.argv[1] if len(sys.argv) > 1 else "reuse"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    work = pathlib.Path(os.environ.get("TEMP", "/tmp")) / f"a-discrim-{face}-{time.strftime('%H%M%S')}"
    work.mkdir(parents=True, exist_ok=True)
    out = []

    def say(s):
        print(s)
        out.append(s)

    srv = subprocess.Popen([sys.executable, str(SERVER), str(HTTP_PORT), "524288"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs = []
    cdp = None
    try:
        time.sleep(2)
        ext_id = derive_id_from_key()
        say(f"[身份] 仓根={ROOT} HEAD={subprocess.run(['git','-C',str(ROOT),'rev-parse','--short','HEAD'],capture_output=True).stdout.decode().strip()}")
        say(f"[实验面] {face} ｜轮数={rounds} ｜扩展 ID={ext_id}")

        if face == "reuse":
            prof = work / "shared-profile"
            p = launch(prof)
            procs.append(p)
            ver = cdp_version()
            if not ver:
                say("[FAIL] CDP 无响应"); return 3
            say(f"[浏览器] {ver.get('Browser','?')}（单实例，profile 复用）")
            cdp = Ws(ver["webSocketDebuggerUrl"])
            invalid = miss = 0
            for i in range(rounds):
                ok, listed, note = one_round_a(cdp, ext_id, HTTP_PORT)
                if not ok:
                    invalid += 1
                    tag = "SETUP-INVALID"
                else:
                    tag = "命中" if listed else "缺失"
                    if not listed:
                        miss += 1
                say(f"  第{i+1:2d}遍 [{tag}] {note}")
        else:
            invalid = miss = 0
            for i in range(rounds):
                prof = work / f"profile-{i}"
                p = launch(prof)
                procs.append(p)
                ver = cdp_version()
                if not ver:
                    say(f"  第{i+1:2d}遍 [FAIL] CDP 无响应")
                    invalid += 1
                    continue
                cdp = Ws(ver["webSocketDebuggerUrl"])
                ok, listed, note = one_round_a(cdp, ext_id, HTTP_PORT)
                try:
                    cdp.close()
                except Exception:
                    pass
                cdp = None
                try:
                    if p.poll() is None:
                        p.terminate(); p.wait(timeout=15)
                except Exception:
                    pass
                if not ok:
                    invalid += 1
                    say(f"  第{i+1:2d}遍 [SETUP-INVALID] {note}")
                else:
                    if not listed:
                        miss += 1
                    say(f"  第{i+1:2d}遍 [{'命中' if listed else '缺失'}] {note}")

        say("")
        say(f"[汇总·{face}] 轮数={rounds} SETUP-INVALID={invalid} 有效轮里A面缺失={miss}")
        say("[判别读法] 只有 fresh 面 INVALID 高 ⇒ 自动化时序问题（产品不用改）；"
            "两面都高 ⇒ 回头查扩展注册链路；两面都 0 ⇒ 之前的 3 次不是 profile 形状，需另找变量")
    finally:
        try:
            if cdp:
                cdp.close()
        except Exception:
            pass
        for p in procs:
            try:
                if p.poll() is None:
                    p.terminate(); p.wait(timeout=15)
            except Exception:
                pass
        try:
            if srv.poll() is None:
                srv.terminate(); srv.wait(timeout=10)
        except Exception:
            pass
        dst = pathlib.Path(os.environ.get("TEMP", "/tmp")) / f"a-discrim-{face}-last.txt"
        dst.write_text("\n".join(out) + "\n", encoding="utf-8")
        print(f"[读数档] {dst}")
        print(f"[工作目录] {work}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
