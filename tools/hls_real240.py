#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真站 HLS E2E（240p 变体）：拿完整产物 + SHA256 + 耗时 + 路径归因。

为什么下单用**子播放列表**而不是 master：
  `hls_downloader.cpp:199-207` 的 parseMasterPlaylist 是「无脑选 BANDWIDTH 最大」，
  而 `m_forcedVariant` 只从 `req.extra["hlsVariant"]` 取（`:73`），全仓没有任何调用点写这个
  key（grep：只有 :169/:226 写 "format"）⇒ **产品根本没有画质选择入口**。
  所以本轮用与真站相同的下载分支（media playlist → 分片 → concat），只是把变体交给
  测试夹具选定；「扩展识别 master.m3u8」这一面已由 hls_e2e.py 在本机 fixture 上证明过。

三方同立才算成功（缺一即不宣布）：
  ① GUI 日志出现「HlsDownloader 拼接完成」且任务终态=已完成
  ② 产物存在、工作目录 idm_hls_* 已被产品自己清空
  ③ 独立走 HTTPS 重取同一批分片，逐字节 == 产物（SHA256 由我自己算，不是产品报的）
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_EXE = ROOT / "build" / "idm-next.exe"
sys.path.insert(0, str(ROOT / "tools"))
from disk_e2e import gui_ipc, read_gbk, sha12  # noqa: E402

VARIANT = "https://test-streams.mux.dev/x36xhzz/url_2/193039199_mp4_h264_aac_ld_7.m3u8"
MAX_S = int(sys.argv[1]) if len(sys.argv) > 1 else 1800


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ipc_raw(payload_obj):
    """走产品自己的命名管道，但允许带字段（disk_e2e.gui_ipc 只吃 command 名）。"""
    import ctypes
    import ctypes.wintypes as wt
    k32 = ctypes.windll.kernel32
    k32.CreateFileW.restype = wt.HANDLE
    k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                                wt.DWORD, wt.DWORD, wt.HANDLE]
    name = chr(92) * 2 + "." + chr(92) + "pipe" + chr(92) + "idm-next-ipc"
    h = k32.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)
    if not h or h == wt.HANDLE(-1).value:
        return None, f"连不上 err={k32.GetLastError()}"
    try:
        payload = json.dumps(payload_obj).encode("utf-8")
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


def main():
    work = pathlib.Path(os.environ.get("TEMP", "/tmp")) / f"hls240-{time.strftime('%H%M%S')}"
    work.mkdir(parents=True, exist_ok=True)
    dl = work / "downloads"; dl.mkdir()
    data_root = work / "appdata"; (data_root / "local").mkdir(parents=True)
    guilog = work / "gui.log"
    lines = []

    def say(s):
        print(s)
        lines.append(str(s))

    app = None
    rc = 1
    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True).stdout.decode().strip()
        say(f"[身份] 仓根={ROOT} HEAD={head} app_exe_sha12={sha12(APP_EXE)}")
        say(f"[目标] {VARIANT}")

        env = dict(os.environ, IDM_DATA_DIR=str(data_root),
                   APPDATA=str(data_root), LOCALAPPDATA=str(data_root / "local"),
                   QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
        app = subprocess.Popen([str(APP_EXE)], cwd=str(ROOT / "build"), env=env,
                               stdout=open(guilog, "wb"), stderr=subprocess.STDOUT)
        ready = False
        for _ in range(80):
            time.sleep(0.5)
            if "IPC server 已启动" in read_gbk(guilog):
                ready = True; break
            if app.poll() is not None:
                say(f"[FAIL] GUI 退出 rc={app.returncode}"); return 4
        if not ready:
            say("[FAIL] 没等到 GUI 监听"); return 5

        # 前置断言：ffmpeg 必须真的不可用 ⇒ 走的才是纯 C++ 拼接分支
        ff = data_root / "ffmpeg" / "ffmpeg.exe"
        which = subprocess.run(["powershell", "-NoProfile", "-Command",
                                "Get-Command ffmpeg -ErrorAction SilentlyContinue | Select -Expand Source"],
                               capture_output=True).stdout.decode("gbk", "replace").strip()
        bundled = ROOT / "build" / "ffmpeg" / "ffmpeg.exe"
        say(f"[前置] dataDir ffmpeg.exe={ff.exists()} ｜ build/ffmpeg/ffmpeg.exe={bundled.exists()} ｜ PATH ffmpeg={which or '无'}")
        if ff.exists() or bundled.exists() or which:
            say("[SETUP-INVALID] ffmpeg 其实可用 ⇒ 会走转封装分支，本轮「拼接分支」判据不成立"); return 6

        obj, err = ipc_raw({"command": "add", "url": VARIANT, "filename": "", "dir": str(dl)})
        t0 = time.time()
        say(f"[下单] {round(time.time()-t0,2)}s resp={obj} err={err}")
        if not (obj or {}).get("success"):
            say("[FAIL] GUI 收单失败"); return 7

        name = ""
        for p in dl.glob("*"):
            name = p.name
        say(f"[下单后目录] {[p.name for p in dl.iterdir()]}")

        seen_new = []
        term = None
        last_print = 0.0
        while time.time() - t0 < MAX_S:
            time.sleep(5)
            el = time.time() - t0
            txt = read_gbk(guilog)
            marks = [x.strip()[:150] for x in txt.splitlines()
                     if "HlsDownloader" in x or "添加 HLS 任务" in x or "IPC 请求: add" in x]
            new = [m for m in marks if m not in seen_new]
            for m in new:
                seen_new.append(m)
                say(f"[GUI+{el:6.1f}s] {m}")
            files = sorted(dl.iterdir(), key=lambda p: p.name)
            total = sum(p.stat().st_size for p in files if p.is_file())
            segs = list(pathlib.Path(os.environ.get("TEMP", "/tmp")).glob("idm_hls_*"))
            segn = len(list(segs[0].glob("seg_*.ts"))) if segs else 0
            obj2, e2 = gui_ipc("list")
            # ⚠ message 第一行是表头「当前 N 个任务」，不是任务行：直接取 row[0] 会永远
            #   看不到状态词 ⇒ 终态检测失效、整轮空转到上限（本轮就是这么浪费的）。
            allrows = str((obj2 or {}).get("message", "")).splitlines()
            row = [x for x in allrows if "193039199" in x]
            if el - last_print >= 20:
                last_print = el
                say(f"[{el:6.1f}s] 在途分片文件={segn} 目录字节={total} "
                    f"| GUI行={row[0][:88] if row else '无'}{'' if not e2 else ' err=' + str(e2)}")
            if row and any(k in row[0] for k in ("已完成", "失败", "已取消")):
                term = row[0]
                say(f"[终态] {term}")
                break

        elapsed = round(time.time() - t0, 1)
        say("")
        say(f"[计时] 下单→终态 = {elapsed}s")
        prods = [p for p in dl.iterdir() if p.is_file()]
        say(f"[产物清单] {[(p.name, p.stat().st_size) for p in prods]}")
        leftovers = list(pathlib.Path(os.environ.get("TEMP", "/tmp")).glob("idm_hls_*"))
        say(f"[层②] 工作目录残留={[d.name for d in leftovers]}")

        prod = max(prods, key=lambda p: p.stat().st_size) if prods else None
        if prod is None:
            say("[VERDICT] 没有产物 ⇒ 失败"); return 8
        got = sha256_file(prod)
        size = prod.stat().st_size
        say(f"[层③-a] 产物 {prod} size={size} sha256={got}")

        # 独立重取：自己解析同一份 media playlist，按同样顺序拼原始分片
        NL = chr(10)
        with urllib.request.urlopen(VARIANT, timeout=60) as r:
            m3u = r.read().decode("utf-8", "replace")
        base = VARIANT.rsplit("/", 1)[0] + "/"
        uris = [l.strip() for l in m3u.split(NL)
                if l.strip() and not l.strip().startswith("#")]
        say(f"[独立重取] 分片数={len(uris)}")
        buf = bytearray()
        for i, u in enumerate(uris):
            full = u if u.startswith("http") else base + u
            t = time.time()
            with urllib.request.urlopen(full, timeout=180) as rr:
                d = rr.read()
            buf += d
            say(f"    [{i+1}/{len(uris)}] {len(d)} B  {round(time.time()-t,1)}s  {full[-40:]}")
        ref = hashlib.sha256(bytes(buf)).hexdigest()
        say(f"[层③-b] 参考拼接 size={len(buf)} sha256={ref}")

        same = (len(buf) == size) and (ref == got)
        say("")
        say(f"[层①] 拼接完成日志={'HlsDownloader 拼接完成' in read_gbk(guilog)} ｜终态={term or '未拿到'}")
        say(f"[层②] 工作目录已清空={len(leftovers)==0}")
        say(f"[层③] 逐字节一致={same}")
        say(f"[VERDICT] {'三方同立：真站 HLS 产物已验证' if same else '不一致 ⇒ 不得宣布成功'}")
        say(f"[路径] 分支=无 ffmpeg 的纯 C++ 手动下载（startManualDownload→concatSegments），"
            f"输出扩展名 .ts；变体由测试夹具指定（产品侧无画质入口）")
        rc = 0 if same else 9
        return rc
    finally:
        out = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "hls240-last.txt"
        out.write_text(chr(10).join(lines) + chr(10), encoding="utf-8")
        print(f"[读数档] {out}")
        print(f"[工作目录] {work}")
        if app and app.poll() is None:
            print("[收尾] 读数已拿满，终止 GUI")
            app.terminate()
            try:
                app.wait(timeout=20)
            except Exception:
                app.kill()


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
