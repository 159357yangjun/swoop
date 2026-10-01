#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""清扫自检的"会红"演示：把 sweep_leftovers 的转义写回它当初坏掉的那一版。

为什么专门给这条配演示（2026-09-30 主控原话的形状）：**本轮最贵的发现就是"清扫一直在静默空转"**，
而它的成因是一个 C 未知转义（`"%s\%s"` ⇒ `\%`）；一个能放过这种事的工具，自己没被验证会红，
它就还是放得过。所以这条自检在升档之前只能算"有正例、未做变异"。

两面都用**手工摆进去的 canary**（1 字节的 `rst_a (9).bin` / `s2_ok (9).bin`），不靠停滞跑偶现的产物：
  基线    ：清扫有效 ⇒ 启动时数到 0 ⇒ 自检 PASS，且跑完后 canary 不在盘上（文件级独立确认）；
  MUT-ESC：把 `snprintf(pat, ..., "%s\\%s", ...)` 改回一个反斜杠 ⇒ 模式变成"目录和文件名粘住"，
           FindFirstFileW 匹配不到 ⇒ 清扫零命中 ⇒ 自检必须 **红**，且 canary 必须**还在盘上**。
两个方向都有"日志读数 + 盘上事实"两条证据，不接受只有一边。

跑法：python tools/mutation_fixture_sweep.py
退出码 0 = 基线绿且 canary 被删 + 变异红且 canary 存活 + 源码与 HEAD 逐字一致。
"""

import io
import os
import re
import subprocess
import sys
import time
import urllib.request

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "engine_selftest.c")
EXE = os.path.join(ROOT, "build", "engine_selftest.exe")
SRV = os.path.join(ROOT, "tools", "engine_selftest_server.py")
PORT, SUSER, SPASS = 18080, "idmuser", "idmpass"
CHECK = "启动清扫确实生效"
CANARY = ["rst_a (9).bin", "s2_ok (9).bin"]

# 变异：把清扫用的那个格式串改回"掉了一个反斜杠"的原样（就是本次事故本身）
GOOD = 'snprintf(pat, sizeof(pat), "%s' + chr(92) + chr(92) + '%s", outdir, pats[i]);'
MUT  = 'snprintf(pat, sizeof(pat), "%s' + chr(92)     + '%s", outdir, pats[i]);'

OUTDIR = os.path.join(os.environ.get("TEMP", "."), "idm_selftest_out")
ARCH = os.path.join(os.environ.get("TEMP", "."), "idm-probes", time.strftime("%Y-%m-%d"),
                    "loop-evidence", "sweep-fixture-" + time.strftime("%H%M%S"))
os.makedirs(ARCH, exist_ok=True)


def read():
    return io.open(SRC, encoding="utf-8", newline="").read()


def write(t):
    io.open(SRC, "w", encoding="utf-8", newline="").write(t)


def listeners():
    raw = subprocess.run("netstat -ano", shell=True, capture_output=True).stdout.decode("gbk", "replace")
    return sum(1 for l in raw.splitlines() if "127.0.0.1:%d" % PORT in l and "LISTENING" in l)


def build(tag):
    p = subprocess.run("cmake --build build --target engine_selftest", shell=True, cwd=ROOT,
                       capture_output=True)
    txt = (p.stdout + p.stderr).decode("utf-8", "replace")
    io.open(os.path.join(ARCH, tag + "-build.log"), "w", encoding="utf-8").write(txt)
    rebuilt = "Building C object" in txt and "engine_selftest.c.obj" in txt
    err = [l for l in txt.splitlines() if "error:" in l]
    warn = [l for l in txt.splitlines() if "unknown escape" in l]
    return p.returncode, rebuilt, err, warn


def plant():
    if os.path.isdir(OUTDIR):
        subprocess.run('rm -rf "%s"' % OUTDIR, shell=True)
    os.makedirs(OUTDIR)
    for c in CANARY:
        io.open(os.path.join(OUTDIR, c), "wb").write(b"x")


def survived():
    return [c for c in CANARY if os.path.exists(os.path.join(OUTDIR, c))]


def run(tag):
    if listeners() != 0:
        return None, "端口上已有监听 ⇒ 这一轮不记账"
    plant()
    slog = io.open(os.path.join(ARCH, tag + "-server.log"), "w", encoding="utf-8")
    srv = subprocess.Popen([sys.executable, SRV, str(PORT), SUSER, SPASS], cwd=ROOT,
                           stdout=slog, stderr=slog)
    op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for _ in range(60):
        try:
            if op.open("http://127.0.0.1:%d/_stats" % PORT, timeout=3).status == 200:
                break
        except Exception:
            pass
        if srv.poll() is not None:
            return None, "服务提前退出"
        time.sleep(1)
    env = dict(os.environ)
    env["IDM_SELFTEST_MAX_LIFE"] = "300"
    t0 = time.strftime("%H:%M:%S")
    try:
        p = subprocess.run([EXE, str(PORT), SUSER, SPASS], cwd=ROOT, capture_output=True,
                           env=env, timeout=600)
        txt = (p.stdout + p.stderr).decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        srv.terminate()
        return None, "自测 600s 没回"
    io.open(os.path.join(ARCH, tag + "-selftest.log"), "w", encoding="utf-8").write(txt)
    for _ in range(40):
        if srv.poll() is not None:
            break
        time.sleep(0.5)
    if srv.poll() is None:
        srv.terminate()
    summ = re.search(r"结果：\d+ 通过，\d+ 失败", txt)
    reds = [l.strip() for l in txt.splitlines() if l.strip().startswith("[FAIL]") and CHECK in l]
    info = "T3=%s→%s rc=%d ｜%s ｜自检红=%d ｜跑完 canary 存活=%s" % (
        t0, time.strftime("%H:%M:%S"), rc, summ.group(0) if summ else "(无结果行)",
        len(reds), survived() or "无")
    return (len(reds) > 0), info


results = []
orig = read()
if orig.count(GOOD) != 1:
    print("锚点命中 %d 次 ⇒ 夹具过期，先修夹具" % orig.count(GOOD))
    sys.exit(2)

# 基线（未变异）：canary 被删、自检不红
rc0, reb0, err0, warn0 = build("pre")
if rc0 or err0 or not (reb0 or True):
    print("基线前构建失败 rc=%d errors=%d" % (rc0, len(err0)))
    sys.exit(1)
red_base, info_base = run("baseline")
ok_base = (red_base is False) and (survived() == [])
results.append(("BASELINE 清扫有效（自检不红 + canary 被删）", ok_base, info_base))

# MUT-ESC：把转义写回坏的那一版，自检必须红，且 canary 必须还在
assert GOOD in read()
write(read().replace(GOOD, MUT, 1))
try:
    if read() == orig:
        results.append(("MUT-ESC", False, "空变异：改前后逐字相同"))
    else:
        rc1, reb1, err1, warn1 = build("mut-esc")
        if rc1 or err1 or not reb1:
            results.append(("MUT-ESC", False, "构建不干净 rc=%d rebuilt=%s errors=%d" % (rc1, reb1, len(err1))))
        else:
            red_mut, info_mut = run("mut-esc")
            ok_mut = (red_mut is True) and (survived() != [])
            results.append(("MUT-ESC 模式写坏 ⇒ 自检必须红 + canary 必须存活", ok_mut,
                            "%s ｜警告 unknown escape=%d" % (info_mut, len(warn1))))
finally:
    write(orig)

restored = read() == orig
rc2, reb2, err2, warn2 = build("final-restore")
dirty = subprocess.run("git status --porcelain tools/engine_selftest.c", shell=True, cwd=ROOT,
                       capture_output=True).stdout.decode("utf-8", "replace").strip()

print("归档：%s" % ARCH)
print("%-52s %-5s %s" % ("面", "判定", "读数"))
for n, ok, got in results:
    print("%-52s %-5s %s" % (n, "OK" if ok else "BAD", got))
print("源码还原=%s ｜git status(该文件)=%s ｜还原后重编 rc=%d errors=%d"
      % (restored, dirty or "干净", rc2, len(err2)))
bad = sum(1 for _, ok, _ in results if not ok) + (0 if (restored and not dirty and not err2) else 1)
print("mutation_fixture_sweep: %d/%d 按预期 ⇒ %s"
      % (len(results) - bad, len(results), "这条自检有牙" if bad == 0 else "未通过"))
sys.exit(0 if bad == 0 else 1)
