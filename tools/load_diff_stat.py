#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 load_diff_run.sh 的日志里算分布（min/median/max），不手写读数、不看单次。

退出码只表"量具有没有跑到东西"：任何一面解析到的轮数 < 期望轮数 ⇒ 1（整轮作废）。
**负载 vs 空闲谁高谁低、500ms 那条有没有超——这些一律只印进报告行，绝不进退码。**
"""
import re
import statistics
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

D = Path(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 5
FACES = ["idle", "load", "idle_after"]

RE_DUI = re.compile(r"\[act\] D 段归因：排空静默=(\S+) peakN=(\d+) 取尽自身峰值=(\d+)ms "
                    r"单轮最长=(\d+)ms IPC占比=(\d+)%")
RE_A = re.compile(r"D 段 事件循环单轮最长=(\d+)ms")
RE_B = re.compile(r"B 段放弃后往返=(-?\d+)ms")
RE_C = re.compile(r"C 段 ack (\d+)/(\d+)")
RE_ATTR = re.compile(r"有定时器=(\d+)ms → 停定时器=(\d+)ms → 再有定时器=(\d+)ms")
RE_ATTR_SKIP = re.compile(r"本轮未复现（D 段单轮最长=(\d+)ms")
RE_CONC = re.compile(r"⇒ (.+)$", re.M)

rows = {}
for face in FACES:
    runs = []
    for i in range(1, N + 1):
        p = D / f"{face}-{i}.log"
        if not p.is_file():
            continue
        t = p.read_text(encoding="utf-8", errors="replace")
        d = re.findall(r"\[PASS\]", t), re.findall(r"\[FAIL\]", t)
        r = {"log": p.name, "pass": len(d[0]), "fail": len(d[1])}
        m = RE_DUI.search(t)
        if m:
            r.update(quiet=m.group(1), peakN=int(m.group(2)), drain=int(m.group(3)),
                     worst=int(m.group(4)), share=int(m.group(5)))
            a = RE_A.search(t)
            r["worst_second_witness"] = int(a.group(1)) if a else None
            r["witness_agree"] = (a is not None and a.group(1) == m.group(4))
        b = RE_B.search(t)
        r["btrip"] = int(b.group(1)) if b else None
        c = RE_C.search(t)
        r["acks"] = f"{c.group(1)}/{c.group(2)}" if c else "?"
        at = RE_ATTR.search(t)
        if at:
            r["onA"], r["offB"], r["onC"] = (int(x) for x in at.groups())
        else:
            sk = RE_ATTR_SKIP.search(t)
            r["offB"] = None
            if sk:
                r["skip_note"] = sk.group(1)
        for line in t.splitlines():
            if line.startswith("[attr] 同一测具") or line.startswith("[attr] 本轮未复现"):
                tail = line.rsplit("⇒", 1)
                if len(tail) == 2:
                    r["conclusion"] = tail[1].strip()[:40]
        runs.append(r)
    rows[face] = runs


def dist(vals):
    v = [x for x in vals if isinstance(x, (int, float))]
    if not v:
        return "n=0 —"
    return (f"n={len(v)} min={min(v)} median={int(statistics.median(v))} max={max(v)}"
            + (f" 全部={v}" if len(v) <= 6 else ""))


print("面        轮数  单轮最长(=那条<500断言的实测)      B 段往返(轻对照，产品可见)     C 段 ack   PASS/FAIL")
for face in FACES:
    rs = rows[face]
    print(f"{face:11s} {len(rs):<4d} {dist([r.get('worst') for r in rs]):<38s} "
          f"{dist([r.get('btrip') for r in rs]):<34s} "
          f"{','.join(r['acks'] for r in rs):<10s} "
          f"{sum(r['pass'] for r in rs)}/{sum(r['fail'] for r in rs)}")

print("\n每轮逐条（单轮最长 / peakN / 取尽自身峰值 / IPC占比 / 排空静默 / 消融 on→off→on / 归因结论）")
for face in FACES:
    for r in rows[face]:
        attr = (f"on={r['onA']} off={r['offB']} on={r['onC']}"
                if "offB" in r and r.get("offB") else f"消融未跑(未复现,阈值前={r.get('skip_note','?')})")
        agree = "" if r.get("witness_agree", True) else " ⚠两个证人不等!"
        print(f"  {face:11s} {r['log']:20s} worst={r.get('worst')}ms peakN={r.get('peakN')} "
              f"drain={r.get('drain')}ms share={r.get('share')}% quiet={r.get('quiet','?')}{agree} "
              f"| {attr} | {r.get('conclusion', '-')}{'' if r['fail'] == 0 else ' | FAIL=' + str(r['fail'])}")

# 规则数（政策，可以写死）：负载比阈值 1.5 与"至少 3 轮"。实测中位不在这里写死，从空闲面自己算。
RATIO_MIN = 1.5
MIN_VALID_LOAD_ROUNDS = 3

# 机器面标定：同量功的忙等，证明负载面真的把机器压慢了（没有这条，"负载面"三个字没有内容）
# calib.txt 列：面 轮 pre post 墙钟秒 worker存活 负载比
cl = Path(D) / "calib.txt"
by = {}
if cl.is_file():
    print("\n机器面标定（固定功量忙等 ms；负载比 = 本轮 pre / 本轮空闲面中位；"
          f"规则阈值 {RATIO_MIN} —— 低于它的面不算负载面，实测中位不写死在代码里）")
    by = {}
    for line in cl.read_text(encoding="utf-8").splitlines():
        f = line.split()
        if len(f) >= 7:
            by.setdefault(f[0], []).append({"pre": int(f[2]), "post": int(f[3]),
                                            "dur": int(f[4]), "alive": int(f[5]),
                                            "ratio": float(f[6])})  # -1 = 空闲面，负载比不适用
    for face in FACES:
        if face in by:
            rs = by[face]
            ok = [r for r in rs if r["ratio"] >= RATIO_MIN]
            na = sum(1 for r in rs if r["ratio"] < 0)
            print(f"  {face:11s} pre={dist([r['pre'] for r in rs])}")
            print(f"  {'':11s} 墙钟秒={dist([r['dur'] for r in rs])} worker存活={dist([r['alive'] for r in rs])}")
            print(f"  {'':11s} 负载比={dist([r['ratio'] for r in rs if r['ratio'] >= 0])}"
                  f"（不适用/空闲={na}）满足阈值的轮={len(ok)}/{len(rs)}"
                  + ("  ← 负载面成立" if face != "load" or ok else ""))
            if face == "load" and len(ok) < len(rs):
                print(f"  {'':11s} ⚠负载面有 {len(rs)-len(ok)} 轮不在负载下（见上面的逐轮比值），"
                      f"下面的'单轮最长'分布只对满足的 {len(ok)} 轮负责")
    idle_pre = [r["pre"] for r in by.get("idle", [])]
    if idle_pre:
        med = statistics.median(idle_pre)
        print(f"  空闲面标定中位={int(med)}ms（负载比的分母，本轮实测）")

# 负载面只对"满足阈值的那些轮"取分布——否则我把空载轮当成负载轮，正是上一轮作废的那件事
def worst_for(face):
    rs = rows[face]
    if face != "load" or not cl.is_file():
        return [r.get("worst") for r in rs], [r.get("btrip") for r in rs], len(rs)
    ok_idx = {i + 1 for i, r in enumerate(by.get("load", [])) if r["ratio"] >= RATIO_MIN}
    sel = [r for j, r in enumerate(rs, 1) if j in ok_idx]
    return [r.get("worst") for r in sel], [r.get("btrip") for r in sel], len(sel)

print("\n【只对满足负载阈值的轮取分布】面 / 单轮最长 / B 段往返（轻对照）")
for face in FACES:
    w, b, n = worst_for(face)
    print(f"  {face:11s} n={n} 单轮最长={dist(w)}  B段往返={dist(b)}")

bad = [f for f in FACES if len(rows[f]) < N]
load_ok = True
if cl.is_file() and by.get("load"):
    load_ok = sum(1 for r in by["load"] if r["ratio"] >= RATIO_MIN) >= MIN_VALID_LOAD_ROUNDS
print(f"\n[LOAD_DIFF_STAT] 期望每面 {N} 轮；解析不足的面={bad or '无'}；"
      f"负载面满足阈值的轮数={'-' if not by.get('load') else sum(1 for r in by['load'] if r['ratio'] >= RATIO_MIN)}"
      f"（规则：≥3 轮才认为负载面成立）")
print("退码只表量具是否跑到东西（解析不足 / 负载面没真负载起来）；"
      "负载与空闲谁高、500ms 有没有超，全部只进上面的报告行，绝不进退码。")
sys.exit(1 if (bad or not load_ok) else 0)
