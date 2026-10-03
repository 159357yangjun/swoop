#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ACTIVATE 量具标定：三种压力面各跑 N 轮，记录同一组读数。

负载实现只用标准库（不装任何东西）：
  cpu    —— 起 nproc 个忙等进程
  io     —— 起 k 个进程持续写/读 %TEMP% 下的大文件
  msgs   —— 用 ctypes 向所有顶层窗口广播一条自定义消息 + 反复 EnumWindows，
             制造 Windows 消息队列活动（这条最贴近我怀疑的混淆源）
每面跑完立刻停负载；负载进程只由本脚本创建、按 pid 精确回收（不 taskkill 别人的）。
"""
import ctypes
import ctypes.wintypes
import os
import random
import statistics
import subprocess
import sys
import threading
import time

user32 = ctypes.windll.user32
EnumWindows = user32.EnumWindows
PostMessageW = user32.PostMessageW
WM_USER, WM_CLOSE = 0x404, 0x0010


def broadcast_msgs(stop, rate_per_s=400):
    hwnds = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def collect(h, l):
        hwnds.append(h)
        return True

    while not stop.is_set():
        hwnds.clear()
        EnumWindows(collect, 0)
        for h in hwnds[:80]:
            PostMessageW(h, WM_USER + 7, 0, 0)
        time.sleep(1.0 / rate_per_s)


def cpu_load(stop, workers):
    procs = [subprocess.Popen([sys.executable, "-c",
                               "while True:\n  pass"]) for _ in range(workers)]
    stop.wait()
    for p in procs:
        p.terminate()
    for p in procs:
        try:
            p.wait(5)
        except Exception:
            p.kill()


def io_load(stop, workers=4):
    d = os.path.join(os.environ.get("TEMP", "/tmp"), "idm_calib_io")
    os.makedirs(d, exist_ok=True)
    def churn(i):
        f = os.path.join(d, "churn%d.bin" % i)
        blob = bytes(random.getrandbits(8) for _ in range(1 << 20))
        while not stop.is_set():
            with open(f, "wb") as fh:
                fh.write(blob)
            with open(f, "rb") as fh:
                fh.read()
    ts = [threading.Thread(target=churn, args=(i,), daemon=True) for i in range(workers)]
    for t in ts:
        t.start()
    stop.wait()
    for i in range(workers):
        try:
            os.remove(os.path.join(d, "churn%d.bin" % i))
        except OSError:
            pass
    try:
        os.rmdir(d)
    except OSError:
        pass


def run_probe(n, log_prefix):
    rows = []
    for i in range(n):
        rc = subprocess.run(["build/ui_snapshot.exe"], capture_output=True,
                            env=dict(os.environ, QT_QPA_PLATFORM="offscreen",
                                     IDM_ACTIVATE_PROBE="1"))
        out = rc.stdout.decode("utf-8", "replace")
        import re
        w = re.search(r"实测 (\d+)ms", out)
        dh = re.search(r"撞每轮预算次数=(\d+)", out)
        fail = out.count("[FAIL]")
        rows.append((int(w.group(1)) if w else -1,
                     int(dh.group(1)) if dh else -1, fail, rc.returncode))
        time.sleep(0.2)
    return rows


def pct(v, p):
    v = sorted(v)
    return v[max(0, min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1)))))]


def report(name, rows, fout):
    w = [r[0] for r in rows if r[0] >= 0]
    dh = [r[1] for r in rows if r[1] >= 0]
    reds = sum(1 for r in rows if r[2] or r[3])
    line = ("%-6s n=%d 红=%d ｜worst min/p50/p95/p99/max=%d/%d/%d/%d/%d ｜deadline_hits p50/p95/max=%d/%d/%d"
            % (name, len(rows), reds, min(w), pct(w, 50), pct(w, 95), pct(w, 99), max(w),
               pct(dh, 50), pct(dh, 95), max(dh)))
    print(line)
    fout.write(line + "\n")
    fout.write("   raw_worst=" + ",".join(str(x) for x in sorted(w)) + "\n")


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    ts = time.strftime("%m%d-%H%M%S")
    path = "docs/activate-baseline-load-%s.txt" % ts
    fout = open(path, "w", encoding="utf-8")
    head = subprocess.run("git rev-parse --short HEAD", shell=True, capture_output=True)
    exe = open("build/ui_snapshot.exe", "rb").read()
    import hashlib
    sha = hashlib.sha256(exe).hexdigest()[:12]
    fout.write("# repo=%s HEAD=%s exe_sha12=%s n=%d\n"
               % (os.getcwd().replace("\\", "/"), head.stdout.decode().strip(), sha, n))
    print("# repo=%s HEAD=%s exe_sha12=%s n=%d" % (os.getcwd(), head.stdout.decode().strip(), sha, n))
    fout.flush()

    stop = threading.Event()
    # 基线（idle）已由另一份档跑过 50 次；这里只做三个压力面 + 一次收尾对照
    for face in ("cpu", "io", "msgs", "post"):
        stop.clear()
        th = None
        if face == "cpu":
            th = threading.Thread(target=cpu_load, args=(stop, 14), daemon=True)
        elif face == "io":
            th = threading.Thread(target=io_load, args=(stop, 4), daemon=True)
        elif face == "msgs":
            th = threading.Thread(target=broadcast_msgs, args=(stop,), daemon=True)
        if th:
            th.start()
            time.sleep(3)                      # 让负载先起来再测
        rows = run_probe(n, face)
        stop.set()
        if th:
            th.join(30)
        report(face, rows, fout)
        fout.flush()

    fout.close()
    print("档=%s" % path)


if __name__ == "__main__":
    main()
