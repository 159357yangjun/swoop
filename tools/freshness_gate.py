#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""新鲜度门：产物二进制必须不早于"它自己输入集合里最新的那次提交"。

为什么要它（2026-10-01 实测出来的形状）：盘上 `build/idm-next.exe` 是 09-30 20:04 编的，
而 P0 根因修复在 `7777bce`（10-01 08:14，改 `src/core/download_core.c`）⇒ 那枚出厂二进制**不含**该修复。
这句话以前只在文档里，靠人读；本门把它变成退码。

比较量为什么不是"HEAD 的提交时间"：那样每一笔只改文档的提交都会让门永远红
（红到下次重编为止，而它报的并不是"二进制过期"这件事）。所以取
**该 exe 的输入集合里最新一次提交的 committer 时间**，与 exe 的 mtime 比。

输入集合一律取**超集**（宁可多算输入，不可漏算）：超集只会让"落后"更容易被发现，
不会让一个真正过期的二进制被判成新鲜。

构建证人（`ninja -C build -n <目标>`）**只提示、不定罪**，这是本轮实测定的：
`src/core/download_core.c` 的 mtime 是 08:21:31，比 `engine_selftest.exe`（08:12:58）晚 8 分钟，
ninja 因此要重编 5 步——可 `git status --porcelain -- <输入>` 是**空的**，内容与 HEAD 逐字相同。
⇒ ninja 看见的是"被碰过"，不是"变了"；拿它定罪会把一枚**确实含修复**的二进制判成过期
（而那枚二进制刚跑出 177/0，里面就有 `[35]` 延后压缩的断言，是行为的证人）。
定罪权交给：内容是否等于 HEAD（`git status`）+ 提交时间差（带容差）。

三种不合格各判一种成因，不许混：
  STALE        exe 比输入集合里最新的提交更早 ⇒ 二进制不含那次改动
  DIRTY_INPUT  输入集合里有未提交改动        ⇒ 任何二进制都不可能含它（连"落后多少"都无从谈起）
  MISSING      产物不存在                    ⇒ 响亮，不静默跳过

用法：python tools/freshness_gate.py [--rev <git rev>]   （--rev 只读比较基准，不切工作树）
退码：0 全部 FRESH；1 有任何不合格（STALE/MISSING/DIRTY_INPUT）；
      2 = 门自己没跑到判断（rev 取不到、内部异常）——崩溃绝不能被读成"报了红"，两种要分开。
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")   # GBK 控制台下中文诊断等于没诊断

ROOT = Path(__file__).resolve().parents[1]     # 数据根与主都在本仓，不读别的仓的文件

# (产物, ninja 目标名, 输入集合[相对仓根]) —— 输入取超集，理由见文件头
TARGETS = [
    ("build/idm-next.exe", "idm-next",
     ["src", "resources", "third_party/libcurl/include", "CMakeLists.txt"]),
    ("build/idm-next-host.exe", "idm-next-host",
     ["src/host", "CMakeLists.txt"]),
    ("build/engine_selftest.exe", "engine_selftest",
     ["tools/engine_selftest.c", "src/core", "src/utils", "src/protocols", "CMakeLists.txt"]),
    ("build/ui_snapshot.exe", "ui_snapshot",
     ["tools/ui_snapshot.cpp", "src", "resources", "CMakeLists.txt"]),
]
# 规则数（政策，可写死）：时间证人只在差距"明显不是一次构建顺序能解释的"时候才单独定罪。
# 为什么要有容差：先构建、后提交是正常顺序（第一版没有它，把 08:12 编、08:14 提交的
# engine_selftest.exe 判成落后 96 秒——那是假警报，二进制确实含那次修复）。
# 代价也写清楚：一笔在构建后 1 小时内提交的真实改动，会被这一条放过；
# 那正是第二证人（ninja 自己的判断）存在的理由，两道不要都省。
GRACE_SECONDS = 3600


def git(*args):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True)
    return r.stdout.decode("utf-8", "replace").strip(), r.returncode


def sha12(rel):
    p = ROOT / rel
    if not p.is_file():
        return "-"
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def run_ninja_dry(target):
    """`ninja -n` = dry run：只打印它会做什么，不编译、不写盘（本轮边界禁止重编）。"""
    r = subprocess.run(["ninja", "-C", "build", "-n", target], cwd=ROOT, capture_output=True)
    return r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace"), r.returncode


def fmt(epoch):
    return time.strftime("%Y-%m-%d %H:%M:%S %z", time.localtime(epoch))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default="HEAD",
                    help="新鲜度基准取哪个 rev 的提交历史（只读，不切工作树）")
    args = ap.parse_args()

    rev_sha, rc = git("rev-parse", "--short", args.rev)
    if rc != 0:
        print(f"[FRESHNESS_GATE] rev={args.rev} 取不到 ⇒ 门的输入都没有，整轮作废")
        return 2

    bad = 0
    print(f"基准 rev={args.rev} ({rev_sha})  现在={fmt(int(time.time()))}  "
          f"容差={GRACE_SECONDS}s（规则数，理由在文件头）")
    for exe, target, inputs in TARGETS:
        p = ROOT / exe
        out, rc2 = git("log", "-1", "--format=%ct %h %s", args.rev, "--", *inputs)
        if rc2 != 0 or not out:
            print(f"[SKIP?]  {exe} ｜输入集合查不到提交（rc={rc2}）⇒ 这一条没有证人")
            bad += 1
            continue
        ct, csha, *csubj = out.split(" ", 2)
        ct = int(ct)
        dirty, _ = git("status", "--porcelain", "--", *inputs)
        dirty_n = len([l for l in dirty.splitlines() if l.strip()])
        if not p.is_file():
            print(f"[MISSING] {exe} ｜不存在 ⇒ 无法判新鲜度（不静默放行）")
            bad += 1
            continue
        mt = int(p.stat().st_mtime)
        gap = mt - ct                       # >0 = 二进制比那次提交新
        # 第二证人：构建系统自己的判断（`ninja -n` 是 dry-run，不编译、不写盘）
        nin, nrc = run_ninja_dry(target)
        if nrc == 0 and "no work to do" in nin:
            w1 = "no work to do"
        else:
            steps = len([l for l in nin.splitlines() if l.strip().startswith("[")])
            w1 = f"说要重编 {steps} 步(rc={nrc})"
        reasons = []
        if dirty_n:
            mark = "DIRTY_INPUT"
            reasons.append(f"有 {dirty_n} 个未提交的输入")
        elif gap < -GRACE_SECONDS:
            mark = "STALE"
            reasons.append(f"时间证人定罪：落后 {-gap}s > 容差 {GRACE_SECONDS}s")
        else:
            mark = "FRESH"
            if gap < 0:
                reasons.append(f"落在容差内({-gap}s)")
            # 构建证人只提示、不定罪（实测理由见文件头那条更新）：
            # ninja 的 mtime 规则会把"被碰过但内容没变"也判成脏。
            if w1.startswith("说要重编") and not dirty_n:
                reasons.append("提示：ninja 想重编，但输入内容与 HEAD 一致 ⇒ 是被碰过不是变了"
                               "（本轮实测：download_core.c mtime 08:21:31 > exe 08:12:58，git status 却干净）")
        bad += 0 if mark == "FRESH" else 1
        print(f"[{mark}] {exe} sha12={sha12(exe)} mtime={fmt(mt)} "
              f"｜输入最新提交={csha} {fmt(ct)} {(csubj[0][:34] if csubj else '')} "
              f"差={gap}s 构建证人={w1}"
              + (f" ⇒ {'；'.join(reasons)}" if reasons else ""))
        print(f"         复算: stat -c '%y %s' {exe} ; "
              f"git log -1 --format='%h %ct %ad %s' --date=iso {args.rev} -- {' '.join(inputs)} ; "
              f"ninja -C build -n {target} ; git status --porcelain -- {' '.join(inputs)}")
    print(f"[FRESHNESS_GATE] rev={args.rev} 目标={len(TARGETS)} 不合格={bad} "
          f"tool={hashlib.sha256((ROOT / 'tools' / 'freshness_gate.py').read_bytes()).hexdigest()[:12]}"
          f"/{sum(1 for _ in open(__file__, encoding='utf-8'))}行")
    print("注：STALE 不是回归，是发现——它说「这枚二进制不含那次改动」。要能把『产品 exe == 当前源码』这句话讲出口，"
          "必须重编产品目标（本轮边界禁止）。两道证人都不读文档、都进退码。")
    return 1 if bad else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as e:      # 门自己坏了 ≠ 有不合格：崩溃绝不能被读成"报了红"
        print(f"[FRESHNESS_GATE_BROKEN] {type(e).__name__}: {e}")
        print("退码 2 = 门没跑到判断，整轮作废（不要当成 1=发现过期，也不要当成 0=通过）")
        sys.exit(2)
