#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""新鲜度门（构建闸门）：跑断言之前先证明"被测产物确实是这次构建出来的"。

要拦的缺陷（2026-10-02 主控点名，比偶发红更严）：同一轮里六个 rc=0 实际吃的是**旧二进制**
——编译 FAILED 我却在看上一条 PASS。红会被看到，**假绿直接进结论**。

两道判据，都必须会拦（不是日志）：
  J1 BUILD_RC —— 本脚本自己起一次 `ninja -C <build> <targets>`；非 0 ⇒ 立刻退 3，**不进入断言阶段**。
  J2 FRESH  —— 每个被测 exe 的 mtime 必须 ≥ 本次构建开始时间；否则退 4（产物比这轮构建还旧）。

反面夹具（--selftest）两面各留一份原始输出文件：
  A「编译失败但测试仍报绿」→ 必须被 J1 拦下（退 3、且断言阶段一行都不许出现）；
  B「exe 比源码旧」        → 必须被 J2 拦下（退 4）。

用法：python tools/build_gate.py [--selftest]
退出码：0=可以进断言；2=前置不成立（缺 ninja/仓根）；3=BUILD_RC≠0；4=产物不比构建新。
"""
import argparse
import os
import subprocess
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SELFDIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SELFDIR)
BUILD = os.path.join(ROOT, "build")


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd or ROOT, capture_output=True, shell=False)
    return p.returncode, (p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace"))


def sha12(path):
    import hashlib
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()[:12]


def pick_input_source(target):
    """挑一个该目标**真依赖**的仓内源文件（面 B 要推 mtime 的对象）。"""
    newest, path = newest_input_times(target)
    if path == "-" or not os.path.isfile(os.path.join(ROOT, path.replace("/", os.sep))):
        return None
    return os.path.join(ROOT, path.replace("/", os.sep))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--targets", default="ui_snapshot engine_selftest idm-next")
    ap.add_argument("--logdir", default="")
    args = ap.parse_args()

    if not os.path.isdir(BUILD):
        print("[BUILD_GATE] PRECONDITION build/ 不存在：%s" % BUILD)
        return 2
    logdir = args.logdir or os.path.join(
        os.environ.get("TEMP", "/tmp"), "idm-build-gate", time.strftime("%Y-%m-%d"))
    os.makedirs(logdir, exist_ok=True)

    if args.selftest:
        return selftest(args, logdir)

    targets = args.targets.split()
    t_start = int(time.time())          # 构建开始时刻（只作诊断，不当判据）
    rc, out = run(["ninja", "-C", "build"] + targets)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    with open(os.path.join(logdir, "build-%s.log" % stamp), "w", encoding="utf-8") as fh:
        fh.write(out)
    head = subprocess.run("git rev-parse --short HEAD", cwd=ROOT,
                          capture_output=True, shell=True).stdout.decode().strip()
    print("[repo] %s @ %s ｜[gate] ninja rc=%d 构建开始=%d 日志=%s"
          % (ROOT, head, rc, t_start, logdir))
    if rc != 0:
        print("[BUILD_GATE] RED J1 BUILD_RC=%d ⇒ **不进断言阶段**（编译没过的产物上任何 PASS 都是旧的）" % rc)
        tail = "\n".join(out.strip().splitlines()[-6:])
        print(tail)
        return 3

    # J2 的判据是「exe 不早于它的输入」，**不是**「exe 在这一次被重编」。
    # ⚠ 第一版写成 mtime >= build_start，实测立刻误红：ninja 是增量的，内容没变时它 no-op、
    #   不 touch 产物 ⇒ 合格产物被判死。真正要防的形状是"源码变了而 exe 还是旧的"，
    #   所以基准必须是**该目标输入的 mtime**（用 ninja -t deps 拿真依赖集，不猜目录）。
    bad = 0
    for t in targets:
        exe = os.path.join(BUILD, t + ".exe")
        if not os.path.isfile(exe):
            print("[BUILD_GATE] RED J2 产物缺失 %s" % exe)
            bad += 1
            continue
        mt = int(os.stat(exe).st_mtime)
        deps = newest_input_times(t)
        newest, newest_path = deps
        ok = mt >= newest
        if not ok:
            bad += 1
        print("[BUILD_GATE] %s exe=%s sha12=%s mtime=%d 最新输入=%s(%d, %s) 差=%ds 本轮是否重编=%s %s"
              % ("OK" if ok else "RED", t + ".exe", sha12(exe), mt,
                 newest_path, newest,
                 time.strftime("%H:%M:%S", time.localtime(newest)), mt - newest,
                 "是" if mt >= t_start else "否(ninja no-op)",
                 "" if ok else "⇒ 产物比它的输入旧 ⇒ 不含那次改动，整轮作废"))
    if bad:
        print("[BUILD_GATE] RED J2 %d/%d 个产物不合格 ⇒ 不进断言阶段" % (bad, len(targets)))
        return 4
    print("[BUILD_GATE] OK BUILD_RC=0 且 %d/%d 产物不早于其输入 ⇒ 允许进断言"
          % (len(targets), len(targets)))
    return 0


def newest_input_times(target):
    """返回 (最新输入 mtime, 路径)。

    ⚠ 第一版用 `ninja -t deps <target>`，实测直接吐「deps not found」⇒ 解析到 0 条输入 ⇒
      三个合格产物全被判死（**喊狼的门**）。deps 只对 .obj 节点有效、且要写完整节点名。
      现在走两步：`-t query <exe>` 拿到该目标的全部 .obj 节点，再逐个 `-t deps` 取编译器
      真写下的依赖 —— 输入集来自构建系统本身，不猜目录、不扫全盘。
      任何一步拿不到仓内输入时返回当前时刻，让调用方保守判不合格（宁红不误绿）。"""
    rc, out = run(["ninja", "-C", "build", "-t", "query", target + ".exe"])
    if rc != 0:
        return int(time.time()), "(ninja -t query 失败)"
    objs = []
    for line in out.splitlines():
        s = line.strip().lstrip("| ").replace("\\", "/")
        if s.endswith(".obj") or s.endswith(".a"):
            objs.append(s)
    repo_root_fwd = ROOT.replace("\\", "/")
    newest, path = 0, "-"
    for obj in objs:
        rc2, d2 = run(["ninja", "-C", "build", "-t", "deps", obj])
        if rc2 != 0:
            continue
        for cand in d2.splitlines():
            c = cand.strip().replace("\\", "/")
            if not c.startswith(repo_root_fwd):
                continue          # Qt/系统头不参与"输入是否更新"的判断
            rel = c[len(repo_root_fwd) + 1:]
            if not (rel.startswith("src/") or rel.startswith("tools/")
                    or rel.startswith("resources/") or rel == "CMakeLists.txt"):
                continue
            full = os.path.join(ROOT, rel.replace("/", os.sep))
            if os.path.isfile(full):
                m = int(os.stat(full).st_mtime)
                if m > newest:
                    newest, path = m, rel
    if newest == 0:
        return int(time.time()), "(没解析到任何仓内输入)"
    return newest, path


# ── 反面夹具 ───────────────────────────────────────────────────────────────
def selftest(args, logdir):
    """两面都要"按预期红"。所有改动在 finally 里逐字还原。"""
    results = []
    src = os.path.join(ROOT, "tools", "ui_snapshot.cpp")
    orig = open(src, encoding="utf-8", newline="").read()
    try:
        # 面 A：故意把源码改坏（编译必失败），然后照常跑门 ⇒ 期望 rc=3，且输出里没有断言阶段的字样
        broken = orig.replace("int main(int argc, char** argv) {",
                              "int main(int argc, char** argv) { this line is not valid c++ ;", 1)
        if broken == orig:
            results.append(("A 锚点", False, "main 签名没找到 ⇒ 夹具过期"))
        else:
            open(src, "w", encoding="utf-8", newline="").write(broken)
            rc, out = run([sys.executable, os.path.join(SELFDIR, "build_gate.py")])
            path = os.path.join(logdir, "fixture-A-build-fails.log")
            open(path, "w", encoding="utf-8").write("rc=%d\n%s" % (rc, out))
            no_assert = "允许进断言" not in out
            ok = (rc == 3) and no_assert
            results.append(("A 编译失败仍想跑测试", ok,
                            "rc=%d 未进断言=%s 输出=%s" % (rc, no_assert, path)))
    finally:
        open(src, "w", encoding="utf-8", newline="").write(orig)

    # 面 B：正常构建一次拿到合格产物，再把**该目标真依赖的那份源码** mtime 推到未来
    #       ⇒ exe 显得比输入旧。⚠ 第一版碰的是 tools/ui_snapshot.cpp，而 engine_selftest / idm-next
    #         根本不依赖它 ⇒ 只红一个、另外两个照样绿（拦不住"部分产物过期"）。现在逐个处理。
    rc0, _ = run(["ninja", "-C", "build"] + args.targets.split())
    if rc0 != 0:
        results.append(("B 前置构建", False, "ninja rc=%d ⇒ 面 B 不作数" % rc0))
    else:
        touched = []
        for t in args.targets.split():
            src_b = pick_input_source(t)
            if src_b is None:
                results.append(("B 选输入(%s)" % t, False, "找不到可推的仓内输入"))
                continue
            content = open(src_b, encoding="utf-8", newline="").read()
            if src_b not in [p for p, _ in touched]:
                touched.append((src_b, content))
            future = time.time() + 3600
            os.utime(src_b, (future, future))
        rc, out = run([sys.executable, os.path.join(SELFDIR, "build_gate.py")])
        path = os.path.join(logdir, "fixture-B-stale-exe.log")
        open(path, "w", encoding="utf-8").write("rc=%d\n%s" % (rc, out))
        stale_n = out.count("⇒ 产物比它的输入旧")
        expected_n = len(args.targets.split())
        ok = (rc == 4) and stale_n == expected_n
        results.append(("B exe 比源码旧", ok,
                        "rc=%d 不合格=%d/%d 输出=%s" % (rc, stale_n, expected_n, path)))
        now = int(time.time())
        for p, content in touched:
            open(p, "w", encoding="utf-8", newline="").write(content)
            os.utime(p, (now, now))

    bad = sum(1 for _, ok, _ in results if not ok)
    print("\n%-26s %-5s %s" % ("夹具面", "判定", "读数"))
    for name, ok, reading in results:
        print("%-26s %-5s %s" % (name, "OK" if ok else "BAD", reading))
    print("build_gate selftest: %d/%d 面按预期红 ｜logdir=%s"
          % (len(results) - bad, len(results), logdir))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
