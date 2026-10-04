#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""定性实验：MV3 service worker 到底在哪一段丢掉 webRequest 事件。

三个候选机制各自对应一条**互斥**的读数（临时插桩在 background.js，跑完逐字还原）：
  (a) tabId < 0        —— 有 [DIAG cb] 且 tabId<0 ⇒ 被 `background.js` 那句 return 丢掉的
  (b) 回调没被调用      —— 完全没有 [DIAG cb]     ⇒ 事件根本没送到扩展
  (c) 写了却没落住      —— 有 [DIAG write] 但事后 storage 里没有该 URL

读 SW 的 console 不能靠 `/json`（headless 下 SW target 不上报），走 CDP 的 **Log.entryAdded**：
接 browser 级 WebSocket，开 `Log.enable` + `Runtime.enable`，service worker 的 console.log
会以 entryAdded 推过来。

用法：python tools/mv3_diag.py <rounds>
"""
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from nm_e2e import Ws, derive_id_from_key  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXT_DIR = ROOT / "browser-extension"
SERVER = ROOT / "tools" / "disk_e2e_server.py"
EDGE_CANDIDATES = [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
]
CDP_PORT = 9421
HTTP_PORT = 8261


def http_json(path):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}{path}", timeout=4) as r:
            return json.loads(r.read().decode())
    except Exception:
        return None


def collect_round(work):
    """跑一轮 A 面，返回 (是否列出 mp4, DIAG 日志列表, storage 直读)。"""
    edge = next((p for p in EDGE_CANDIDATES if pathlib.Path(p).exists()), None)
    proc = subprocess.Popen([edge, "--headless=new", f"--remote-debugging-port={CDP_PORT}",
                             f"--user-data-dir={work/'edge'}", "--no-first-run",
                             "--no-default-browser-check", "--disable-gpu",
                             f"--load-extension={EXT_DIR}",
                             f"--disable-extensions-except={EXT_DIR}", "about:blank"])
    entries = []
    listed = None
    stored = None
    cdp = None
    try:
        ver = None
        for _ in range(40):
            ver = http_json("/json/version")
            if ver:
                break
            time.sleep(0.5)
        if not ver:
            return None, ["[FAIL] CDP 无响应"], None
        cdp = Ws(ver["webSocketDebuggerUrl"])
        # ⚠ 通道实测：browser 级连接上 `Log.enable` 返回 -32601 'wasn't found' ⇒
        #   必须 attach 到具体 target 再按 sessionId 开域。之前"cb=0"是量具瞎，不是产品没调用。
        log_lines = []

        ext_id = derive_id_from_key()
        # 找扩展 service worker target：它才是 [DIAG] 的出处
        # ── 前置断言（SETUP-VALID）：扩展必须真的被浏览器注册 ────────────
        #   判据用扩展页面里的 chrome.runtime.getManifest() —— 只有扩展在册才打得开这个 URL。
        #   没有这条断言时，"A 面缺失"里混着"实验根本没成立"，两者会被算成同一件事。
        ext_probe = derive_id_from_key()
        setup_ok = False
        setup_why = ""
        for _ in range(40):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json", timeout=4) as r:
                    tg = json.loads(r.read().decode())
            except Exception:
                tg = []
            cand = [t for t in tg if str(t.get("url", "")).startswith(f"chrome-extension://{ext_probe}/")
                    and t.get("webSocketDebuggerUrl")]
            if cand:
                try:
                    ep = Ws(cand[0]["webSocketDebuggerUrl"])
                    v = (ep.send("Runtime.evaluate",
                                 {"expression": "(typeof chrome!=='undefined'&&chrome.runtime&&chrome.runtime.getManifest)"
                                               "?String(chrome.runtime.getManifest().version):''",
                                  "returnByValue": True}, timeout=20).get("result") or {}).get("value")
                    ep.close()
                    if v:
                        setup_ok = True
                        break
                except Exception:
                    pass
            time.sleep(0.5)
        if not setup_ok:
            return None, ["[SETUP-INVALID] 扩展未在浏览器册内（找不到 chrome-extension://<id>/ 的可连 target）"
                          " ⇒ 本轮不计入 A 面命中/缺失"], None
        sw_ws = None
        for _ in range(30):
            tg = http_json("/json") or []
            cand = [t for t in tg if t.get("type") == "service_worker"
                    and ext_id in str(t.get("url", "")) and t.get("webSocketDebuggerUrl")]
            if cand:
                try:
                    sw_ws = Ws(cand[0]["webSocketDebuggerUrl"])
                    sw_ws.send("Runtime.enable", {})
                    sw_ws.send("Log.enable", {})
                    break
                except Exception:
                    sw_ws = None
            time.sleep(0.5)
        page = cdp.send("Target.createTarget",
                        {"url": f"http://127.0.0.1:{HTTP_PORT}/network.html"}, timeout=20)["targetId"]
        psid = cdp.send("Target.attachToTarget", {"targetId": page, "flatten": True})["sessionId"]
        time.sleep(6)   # 给页面请求 + SW 冷启动留时间

        pop = cdp.send("Target.createTarget",
                       {"url": f"chrome-extension://{ext_id}/popup.html"}, timeout=20)["targetId"]
        pp_sid = cdp.send("Target.attachToTarget", {"targetId": pop, "flatten": True})["sessionId"]
        cdp.send("Target.activateTarget", {"targetId": page}, timeout=15)
        cdp.send("Runtime.evaluate", {"expression": "location.reload()", "returnByValue": True},
                 timeout=20, session=pp_sid)
        time.sleep(2.5)

        rows = cdp.send("Runtime.evaluate",
                        {"expression": "[].slice.call(document.querySelectorAll('.row'))"
                                       ".map(x=>String(x.innerText||'').replace(/\\s+/g,' ').trim()).join(' | ')",
                         "returnByValue": True}, timeout=20, session=pp_sid).get("result", {}).get("value", "")
        listed = ("network.mp4" in rows)
        stored = cdp.send("Runtime.evaluate",
                          {"expression": "(async()=>{const v=await chrome.storage.session.get(null);"
                                         "const k=Object.keys(v);return JSON.stringify({keys:k,"
                                         "urls:k.reduce((a,x)=>a.concat((v[x]||[]).map(r=>String(r.url).slice(-26))),[])})})()",
                           "awaitPromise": True, "returnByValue": True},
                          timeout=20, session=pp_sid).get("result", {}).get("value", "")
        # 从 SW 自己的连接里抽事件帧（Runtime.consoleAPICalled 优先，Log.entryAdded 兜底）
        if sw_ws is not None:
            deadline = time.time() + 4
            while time.time() < deadline:
                try:
                    payload = sw_ws._recv_payload()
                except Exception:
                    break
                try:
                    obj = json.loads(payload)
                except Exception:
                    continue
                m = obj.get("method")
                txt = ""
                if m == "Runtime.consoleAPICalled":
                    txt = " ".join(str(a.get("value", a.get("description", "")))
                                   for a in obj.get("params", {}).get("args", []))
                elif m == "Log.entryAdded":
                    txt = str(obj.get("params", {}).get("entry", {}).get("text", ""))
                if "DIAG" in txt:
                    entries.append(txt)
            try:
                sw_ws.close()
            except Exception:
                pass
        else:
            entries.append("[SETUP-INVALID] 扩展在册但 SW target 不可见 ⇒ cb=0 不算证据")
        return listed, entries, stored
    finally:
        try:
            if cdp:
                cdp.close()
        except Exception:
            pass
        try:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=15)
        except Exception:
            pass


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    work = pathlib.Path(os.environ.get("TEMP", "/tmp")) / f"mv3-diag-{time.strftime('%m%d-%H%M%S')}"
    work.mkdir(parents=True, exist_ok=True)
    srv = subprocess.Popen([sys.executable, str(SERVER), str(HTTP_PORT), "524288"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    lines = []
    try:
        time.sleep(2)
        miss = 0
        invalid = 0
        for i in range(rounds):
            listed, entries, stored = collect_round(work)
            if listed is None:
                invalid += 1
                tag = "SETUP-INVALID"
            else:
                tag = "命中" if listed else "缺失"
                if not listed:
                    miss += 1
            lines.append(f"=== 第{i+1}遍 A面={tag} ===")
            lines.append(f"  storage.session 直读={stored}")
            for e in entries:
                lines.append(f"  {e[:230]}")
            if not entries:
                lines.append("  （本轮没收到任何 [DIAG] 条目 ⇒ 需先确认 Log.entryAdded 通道本身是通的）")
            time.sleep(1)
        lines.append("")
        lines.append(f"[汇总] 轮数={rounds} 有效轮={rounds-invalid} A面缺失={miss} "
                     f"SETUP-INVALID={invalid}")
        lines.append("[Gate] 只看有效轮：A面缺失必须为 0；SETUP-INVALID 属实验未成立，不得计入产品缺陷")
        # 分类计数：只有拿到正例时这些计数才有意义
        cb_n = sum(1 for l in lines if "[DIAG cb]" in l)
        neg_tab = sum(1 for l in lines if '"tabId":-1' in l or '"tabId": -1' in l)
        wr_n = sum(1 for l in lines if "[DIAG write]" in l)
        sweval = sum(1 for l in lines if "[DIAG sw-eval]" in l)
        lines.append(f"[计数] sw-eval={sweval} 回调命中(cb)={cb_n} 其中 tabId<0={neg_tab} 写入(write)={wr_n}")
        lines.append("[判别] cb=0 ⇒ 候选(b)事件没送到；cb>0 且 tabId<0 ⇒ 候选(a)；"
                     "cb>0 且 write>0 但 storage 没有 ⇒ 候选(c)")
    finally:
        try:
            if srv.poll() is None:
                srv.terminate(); srv.wait(timeout=10)
        except Exception:
            pass
        out = "\n".join(lines) + "\n"
        (pathlib.Path(os.environ.get("TEMP", "/tmp")) / "mv3-diag-last.txt").write_text(out, encoding="utf-8")
        print(out)
        print(f"[读数档] {pathlib.Path(os.environ.get('TEMP','/tmp'))/'mv3-diag-last.txt'}")
    return 1 if miss else 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
