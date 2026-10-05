#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HLS 真站停滞定性：分片到达时间线 + 在途槽位是否永不释放。

带假设去测，不是只看现象。假设（读源码得到，本脚本负责证伪/证实）：
  H1 全仓没有任何 `setTransferTimeout`（grep = 0 命中），`launchSegments()` 的泵写在
     `onSegFinished()` 里 ⇒ 只要有一个 reply 永不 finished，它就**永久占住一个并发槽**；
     4 个槽都这样堵死 ⇒ 任务停在"下载中"、不再有新分片落盘，且不会报错。
  H2 若 H1 成立，症状应是：**先完成几片，然后长时间零增长**，而不是匀速慢。
  H3 若是网络整体慢（非槽位饿死），则所有在途分片应仍在缓慢写字节 ⇒ 目录大小会持续涨。

判据（每 5s 一次采样，全程不杀进程）：
  · 工作目录里 seg_*.ts 的文件名集合与各自 mtime ⇒ 谁何时完成
  · 目录总字节数 ⇒ 是否在长间隙里仍增长（区分 H1 与 H3）
  · GUI 的 list 行（已下载/总大小）⇒ 产品自己看到的进度
  · 结束时印出「最长两个完成之间的间隔」——这是 H1 的直接读数

⚠ 只读诊断：不改产品代码、不放宽任何判据。产物落在临时 dataDir，跑完列清单交回，
   不静默删用户 Downloads 里的东西。

用法：python tools/hls_stall_probe.py [max_seconds]
"""
import json
import os
import shutil
import pathlib
import subprocess
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_EXE = ROOT / "build" / "idm-next.exe"
SERVER_NAME = "idm-next-ipc"

sys.path.insert(0, str(ROOT / "tools"))
from disk_e2e import gui_ipc, read_gbk, port_free, sha12  # noqa: E402

MASTER = "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
MAX_S = int(sys.argv[1]) if len(sys.argv) > 1 else 900


def workdirs():
    return sorted(pathlib.Path(os.environ.get("TEMP", "/tmp")).glob("idm_hls_*"))


def snapshot(d):
    files = {}
    for f in sorted(d.glob("seg_*.ts")):
        try:
            st = f.stat()
            files[f.name] = (st.st_mtime, st.st_size)
        except OSError:
            pass
    total = sum(v[1] for v in files.values())
    return files, total


def main():
    work = pathlib.Path(tempfile_dir := os.environ.get("TEMP", "/tmp")) / f"hls-stall-{time.strftime('%H%M%S')}"
    work.mkdir(parents=True, exist_ok=True)
    guilog = work / "gui.log"
    data_root = work / "data"; data_root.mkdir()
    env = dict(os.environ, IDM_DATA_DIR=str(data_root),
               QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
    lines = []

    def say(s):
        print(s)
        lines.append(s)

    app = None
    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True).stdout.decode().strip()
        say(f"[身份] 仓根={ROOT} HEAD={head} app_exe_sha12={sha12(APP_EXE)}")
        # ⚠ 不能按"目录名"预先排除：任务号是复用的（idm_hls_2000000），上一轮遗留的同名目录
        #   会被当成"已知"而整个吃掉本轮进展 —— 第一版就是这么误判成"一片都没完成"的。
        #   正确做法：下单前把遗留工作目录清掉（脚本自己负责的临时产物），再按 mtime 晚于 t0 认新文件。
        for d in workdirs():
            shutil.rmtree(d, ignore_errors=True)
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
        say(f"[GUI] pid={app.pid} 监听就绪 ｜数据根={data_root}")

        obj, err = gui_ipc("add") if False else (None, None)
        # 走产品自己的 IPC：command=add + url（等价于扩展那一跳，但去掉浏览器变量）
        import ctypes
        import ctypes.wintypes as wt
        k32 = ctypes.windll.kernel32
        k32.CreateFileW.restype = wt.HANDLE
        name = chr(92) * 2 + "." + chr(92) + "pipe" + chr(92) + SERVER_NAME
        h = k32.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)
        if not h or h == wt.HANDLE(-1).value:
            say(f"[FAIL] 管道连不上 err={k32.GetLastError()}"); return 6
        payload = json.dumps({"command": "add", "url": MASTER, "filename": ""}).encode()
        written = wt.DWORD(0)
        k32.WriteFile(h, payload, len(payload), ctypes.byref(written), None)
        k32.FlushFileBuffers(h)
        buf = ctypes.create_string_buffer(4096); got = wt.DWORD(0)
        k32.ReadFile(h, buf, 4096, ctypes.byref(got), None)
        say(f"[下单] {buf.raw[:got.value].decode('utf-8', 'replace')[:160]}")
        k32.CloseHandle(h)

        t0 = time.time()
        completions = []          # (相对秒, 文件名, 大小)
        seen = {}
        last_total = 0
        grew_since_last_done = False
        new_dirs = []
        while time.time() - t0 < MAX_S:
            time.sleep(5)
            el = round(time.time() - t0, 1)
            for d in workdirs():
                if d not in new_dirs:
                    new_dirs.append(d)
            if not new_dirs:
                say(f"[{el:6.1f}s] 还没出现 HLS 工作目录")
                continue
            d = new_dirs[0]
            files, total = snapshot(d)
            for fn, (mt, sz) in files.items():
                if fn not in seen:
                    seen[fn] = True
                    completions.append((el, fn, sz))
            obj, err = gui_ipc("list")
            row = [x for x in str((obj or {}).get("message", "")).splitlines() if "x36xhzz" in x.lower()]
            term = any(any(k in (row[0] if row else "")) for k in ())
            done_state = bool(row) and any(k in row[0] for k in ("已完成", "失败", "已取消"))
            say(f"[{el:6.1f}s] 完成分片={len(files)} 目录字节={total} (+{total-last_total}) "
                f"| GUI行={row[0][:78] if row else '无'}{'' if not err else ' err=' + str(err)}")
            last_total = total
            if done_state:
                say(f"[终态] {row[0]}")
                break
        say("")
        say(f"[采样结束] 时长={round(time.time()-t0,1)}s 完成分片数={len(completions)} / 期望 64")
        if completions:
            say("[分片到达时间线]")
            prev = 0.0
            gaps = []
            for el, fn, sz in completions:
                gap = round(el - prev, 1)
                gaps.append(gap)
                say(f"    +{gap:7.1f}s  {fn}  {sz} B")
                prev = el
            say(f"[关键读数] 最长完成间隔={max(gaps)}s ｜最后一片之后已过 "
                f"{round(time.time()-t0-completions[-1][0],1)}s")
        txt = read_gbk(guilog)
        hl = [x.strip()[:160] for x in txt.splitlines() if "Hls" in x or "ffmpeg" in x.lower()]
        say("[GUI 日志 HLS 相关]")
        for x in hl[-8:]:
            say("    " + x)
        say("")
        if len(completions) >= 1 and max(gaps) > 120:
            say("[VERDICT·H1 成立方向] 有分片完成后出现 >120s 的零完成长间隙 ⇒ 与"
                "「reply 永不 finished、并发槽被永久占住」一致（全仓 grep setTransferTimeout=0）。")
        elif len(completions) == 0:
            say("[VERDICT] 一片都没完成 ⇒ 卡在解析或首片之前，看上面 HLS 日志定位。")
        else:
            say(f"[VERDICT·H1 不成立方向] 最大间隔仅 {max(gaps)}s，说明仍在推进 ⇒ 更像整体网速/体积问题（H3）。")
        say(f"[产物位置] 新出现的 HLS 工作目录={[d.name for d in new_dirs]}")
    finally:
        out = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "hls-stall-last.txt"
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"[读数档] {out}")
        print(f"[工作目录] {work}")
        # 按主控要求：本轮**不杀进程前先确认已拿到终态或已到采样上限**
        if app and app.poll() is None:
            say_note = "[收尾] 采样到上限，终止 GUI（上一轮我提前杀进程导致误读，这次先拿满读数）"
            print(say_note)
            app.terminate()
            try:
                app.wait(timeout=20)
            except Exception:
                app.kill()
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
