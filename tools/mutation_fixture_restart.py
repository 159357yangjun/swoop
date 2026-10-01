#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[33] 两条新断言的变异夹具：证明它们**会红**，否则它们只是读数不是护栏。

要验的两条（2026-09-30 S2 交付时按用户要求先标「不占功」，跑过这个才算数）：
  A 「交出前父进程确实停了笔（状态=已暂停 且 尺寸 ≥2s 不增）」
     ⇒ 变异：把 [33] 里那句 dlmgr_pause 拿掉（父进程继续写盘）⇒ A 必须红。
  B 「面 B：不匹配是**真的**造出来了（.idmtmp 确实不存在，不是删除失败装了个假的）」
     ⇒ 变异：把 utf8_delete 的返回值当成功（deleted = 1）⇒ B 必须红。

纪律（都是本仓已经付过学费的）：
  * 变异前先 grep 锚点、变异后断言 `mut != orig`（防空变异）；
  * 必须重新构建并**要求输出里有 `Building C object .../engine_selftest.c.obj`**——
    没有这一行就是没重编，跑的是旧二进制，那一轮读数全部作废；
  * 每道变异跑完立刻按字节还原并复核；最后再编一次，让盘上的 exe 对应 HEAD 的源码；
  * 起服务前必须 0 个监听、起后恰好 1 个（两个服务同占 18080 会让统计读数读到别人家）；
  * 每一面的完整 stdout 落档成文件：口头"跑过了"不可复算。

用法：python tools/mutation_fixture_restart.py
退出码：0 = 基线绿 + 两道变异各自红 + 还原干净；1 = 有判据咬不动或过程不干净。
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
PORT, SUSER, SPASS = 18080, "idmuser", "idmpass"

DAY = time.strftime("%Y-%m-%d")
ARCH = os.path.join(os.environ.get("TEMP", "/tmp"), "idm-probes", DAY,
                    "loop-evidence", "restart-fixture-" + time.strftime("%H%M%S"))
os.makedirs(ARCH, exist_ok=True)

CHECK_A = "交出前父进程确实停了笔"
CHECK_B = "面 B：不匹配是**真的**造出来了"

MUT_PAUSE = ("            dlmgr_pause(id);\n            /* 交接前必须证明",
             "            /* MUT-1: dlmgr_pause(id); 被拿掉 —— 父进程继续写盘\n"
             "            /* 交接前必须证明")
MUT_DEL = ("                deleted = utf8_delete(tmpp);",
           "                deleted = 1; /* MUT-2: 把删除结果当成功（原本它可能返回 0）*/")


def read_src():
    return io.open(SRC, encoding="utf-8", newline="").read()


def write_src(txt):
    io.open(SRC, "w", encoding="utf-8", newline="").write(txt)


def listeners():
    """127.0.0.1:PORT 上 LISTENING 的条数。netstat 在中文 Windows 上是 GBK。"""
    out = subprocess.run("netstat -ano", shell=True, capture_output=True).stdout
    try:
        out = out.decode("gbk", "replace")
    except Exception:
        out = out.decode("utf-8", "replace")
    n = 0
    for line in out.splitlines():
        if "127.0.0.1:%d" % PORT in line and "LISTENING" in line:
            n += 1
    return n


def build(tag):
    """重新构建；**必须**看到 engine_selftest.c.obj 那一行，否则这一轮不可记账。"""
    p = subprocess.run("cmake --build build --target engine_selftest", shell=True,
                       cwd=ROOT, capture_output=True)
    txt = (p.stdout + p.stderr).decode("utf-8", "replace")
    io.open(os.path.join(ARCH, tag + "-build.log"), "w", encoding="utf-8").write(txt)
    rebuilt = "Building C object" in txt and "engine_selftest.c.obj" in txt
    errors = [l for l in txt.splitlines() if "error:" in l]
    return p.returncode, rebuilt, errors


def run_suite(tag):
    """起一个（且只有一个）服务，跑整套自测，返回 (退码, 完整 stdout)。"""
    if listeners() != 0:
        return None, "★端口上已有 %d 个监听 ⇒ 这一轮不记账（先确认归属再清）" % listeners(), None
    srvlog = io.open(os.path.join(ARCH, tag + "-server.log"), "w", encoding="utf-8")
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "engine_selftest_server.py"),
                            str(PORT), SUSER, SPASS], cwd=ROOT, stdout=srvlog, stderr=srvlog)
    op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    ready = False
    for _ in range(60):
        try:
            if op.open("http://127.0.0.1:%d/_stats" % PORT, timeout=3).status == 200:
                ready = True
                break
        except Exception:
            pass
        if srv.poll() is not None:
            break
        time.sleep(1)
    if not ready:
        srv.terminate()
        return None, "服务没起来（poll=%s）" % srv.poll(), None
    n_after = listeners()
    env = dict(os.environ)
    env["IDM_SELFTEST_MAX_LIFE"] = "300"
    env["IDM_SELFTEST_EVIDENCE_DIR"] = os.path.join(ARCH, tag)
    os.makedirs(env["IDM_SELFTEST_EVIDENCE_DIR"], exist_ok=True)
    out = io.open(os.path.join(ARCH, tag + "-selftest.log"), "w", encoding="utf-8")
    t0 = time.time()
    try:
        p = subprocess.run([EXE, str(PORT), SUSER, SPASS], cwd=ROOT, capture_output=True,
                           env=env, timeout=600)
        txt = (p.stdout + p.stderr).decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        srv.terminate()
        return None, "★自测 600s 没回（这一轮不记账）", None
    out.write(txt)
    out.close()
    for _ in range(40):
        if srv.poll() is not None:
            break
        time.sleep(0.5)
    if srv.poll() is None:
        srv.terminate()
    note = "T3=%s→%s 起后监听=%d 用时=%ds" % (
        time.strftime("%H:%M:%S", time.localtime(t0)), time.strftime("%H:%M:%S"), n_after,
        int(time.time() - t0))
    return rc, txt, note


def names_failed(txt):
    return [l.strip() for l in txt.splitlines() if l.strip().startswith("[FAIL]")]


def case(name, want_red, mutate):
    """跑一道：应用变异 → 证明落地 → 重编（要见到 obj 行）→ 跑 → 看目标那条是否真红 → 还原。"""
    orig = read_src()
    old, new = mutate
    if orig.count(old) != 1:
        results.append((name, False, "锚点命中 %d 次（必须恰好 1）⇒ 夹具过期" % orig.count(old)))
        return
    mut = orig.replace(old, new, 1)
    if mut == orig:
        results.append((name, False, "空变异：改前后逐字相同"))
        return
    write_src(mut)
    try:
        brc, rebuilt, errs = build(name)
        if brc != 0 or errs or not rebuilt:
            results.append((name, False, "构建不干净 rc=%d rebuilt=%s errors=%d ⇒ 这一轮不作数"
                            % (brc, rebuilt, len(errs))))
            return
        rc, txt, note = run_suite(name)
        if txt is None:
            results.append((name, False, "跑不成：%s" % note))
            return
        hit = any(want_red in l for l in names_failed(txt))
        summ = re.search(r"结果：\d+ 通过，\d+ 失败", txt)
        results.append((name, hit, "%s ｜目标那条红=%s ｜%s｜rc=%d"
                        % (note, hit, summ.group(0) if summ else "(没抓到结果行)", rc)))
        io.open(os.path.join(ARCH, name + "-verdict.txt"), "w", encoding="utf-8").write(
            "\n".join(names_failed(txt)) + "\n")
    finally:
        write_src(orig)
        if read_src() != orig:
            results.append((name + " 还原", False, "还原失败：字节不一致"))


results = []

# 基线：先确认 exe 对应的就是盘上源码（要么刚重编、要么 ninja 说 no work to do），
# 否则下面"两条目标都绿"这个前提本身不可信。
brc0, rebuilt0, errs0 = build("pre-baseline")
p0 = io.open(os.path.join(ARCH, "pre-baseline-build.log"), encoding="utf-8").read()
if brc0 or errs0 or not (rebuilt0 or "no work to do" in p0):
    print("基线之前构建不干净：rc=%d errors=%d" % (brc0, len(errs0)))
    sys.exit(1)
print("基线构建前提：%s" % ("真重编（有 obj 行）" if rebuilt0 else "ninja: no work to do ⇒ exe 就是盘上源码"))

rc, txt, note = run_suite("baseline")
if txt is None:
    print("基线跑不成：%s" % note)
    sys.exit(1)
b_fail = names_failed(txt)
base_ok = (not any(CHECK_A in l for l in b_fail)) and (not any(CHECK_B in l for l in b_fail))
summ = re.search(r"结果：\d+ 通过，\d+ 失败", txt)
results.append(("BASELINE（两条目标都必须绿）", base_ok,
                "%s ｜%s ｜现有红项 %d 条（P0 三面 + 前提抖动，已知）"
                % (note, summ.group(0) if summ else "(无结果行)", len(b_fail))))

case("MUT-1 让父进程继续写盘", CHECK_A, MUT_PAUSE)
case("MUT-2 把删除失败当成功", CHECK_B, MUT_DEL)

# 全部还原后再编一次，让盘上的 exe 对应 HEAD 源码（别把变异体留成"当前二进制"）
brc, rebuilt, errs = build("final-restore")
dirty = subprocess.run("git status --porcelain tools/engine_selftest.c", shell=True, cwd=ROOT,
                       capture_output=True).stdout.decode("utf-8", "replace").strip()

print("归档目录：%s" % ARCH)
print("")
print("%-34s %-6s %s" % ("项", "判定", "读数"))
for name, ok, got in results:
    print("%-34s %-6s %s" % (name, "OK" if ok else "BAD", got))
print("")
print("还原核对：git status（tools/engine_selftest.c）= %s" % (dirty if dirty else "干净（与 HEAD 逐字一致）"))
print("还原后重编：rc=%d 有 obj 行=%s 错误=%d" % (brc, rebuilt, len(errs)))
bad = sum(1 for _, ok, _ in results if not ok) + (0 if not dirty else 1)
print("mutation_fixture_restart: %d/%d 按预期 ⇒ %s"
      % (len(results) - bad, len(results), "两条新断言都有牙" if bad == 0 else "有判据咬不动或树没还原"))
sys.exit(0 if bad == 0 else 1)
