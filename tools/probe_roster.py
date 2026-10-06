#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""探针名单门：docs/ARCHITECTURE.md 里那张 PROBE-ROSTER 表，必须与**二进制里的模式集**、
以及 **.github/workflows/ci.yml 的闸门名单**逐字相等。

为什么要这道门（2026-10-02）：`IDM_SIZEWIRING_PROBE` 加进 `ui_snapshot.cpp` 和 CI 之后，
文档一处都没登记——"哪个模式存在、它是不是断言型、它进不进闸门"这三件事此前只存在于
"当前代码恰好遵守"，没有任何东西在它漂移时报红。人读不到名单就会以为套件是 18 个模式。

三个集合各自怎么取（都不许手写第二份名单）：
  A. 二进制声明的模式 = `tools/ui_snapshot.cpp` 里 `qEnvironmentVariableIsSet("IDM_..._PROBE")` 的实参。
     ⚠ 不用 `strings build/ui_snapshot.exe`：那要 exe 存在且新鲜，而本门必须在**未编译的工作树**上也能跑；
       源码扫描给的是"代码声明了哪些模式"，正是登记表要对的东西。
  B. CI 闸门名单 = ci.yml 里 `PROBES="..."` 那段续行（去掉换行与引号）。
  C. 文档表 = ARCHITECTURE.md 里 PROBE-ROSTER-BEGIN..END 之间的 markdown 行。

类型不由人写死：`人眼对照` 必须与"B 里没有它"一致——一个人肉标注和一个机器事实互相钉。

第三种类型 `待修型（刻意不进闸门名单）` 是为"红门不许进门、但也不许从名单蒸发"加的：
先红后绿的夹具（如 IDM_PATHGUARD_PROBE 的 G1/G2）在产品修好前必然红，把它塞进 CI 会让
CI 永远红、真回归被淹没；可它若干脆不登记，ROSTER-MISSING 又该点名它。所以给它一条**只收紧不放宽**
的规则：必须标 待修型 + 必须不在 CI 名单（三者互相钉），断言型/人眼对照的旧规则一字未动。

断言条数那一列**不比对**（它每轮都在动），只要求它是非负整数，并在读数行印出来供核对。

退出码：0 = 三集合一致；1 = 不一致（逐条 ROSTER-* 打印）；2 = 前置不成立（缺文件/缺锚点/解析不出）。
用法：python tools/probe_roster.py [--verbose]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "tools/ui_snapshot.cpp"
CI = ".github/workflows/ci.yml"
DOC = "docs/ARCHITECTURE.md"
BEGIN = "<!-- PROBE-ROSTER-BEGIN"
END = "<!-- PROBE-ROSTER-END -->"


def read(rel):
    p = os.path.join(ROOT, rel)
    if not os.path.isfile(p):
        print(f"[RED] PRECONDITION missing {rel}")
        sys.exit(2)
    with open(p, encoding="utf-8", errors="replace") as fp:
        return fp.read()


def declared():
    """A：源码里真实读取过的模式名（qEnvironmentVariableIsSet 的实参）。"""
    s = read(SRC)
    return set(re.findall(r'qEnvironmentVariableIsSet\("(IDM_[A-Z0-9_]*PROBE)"\)', s))


def ci_list():
    """B：ci.yml 的 PROBES="..." （可跨行续写）。"""
    s = read(CI)
    m = re.search(r'PROBES="((?:[^"\\]|\\.)*?)"', s)
    if not m:
        print("[RED] PRECONDITION ci.yml 里找不到 PROBES=\"...\"")
        sys.exit(2)
    return set(t for t in re.split(r"\s+", m.group(1)) if t)


def doc_rows():
    """C：文档表的行 -> {mode: (kind, in_ci_flag, count)}。"""
    s = read(DOC)
    i = s.find(BEGIN)
    j = s.find(END)
    if i < 0 or j < 0 or j < i:
        print("[RED] PRECONDITION %s 缺少 PROBE-ROSTER 锚点（或顺序颠倒）" % DOC)
        sys.exit(2)
    rows = {}
    for line in s[i:j].splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4 or not cells[0].startswith("IDM_"):
            continue
        mode, kind, inc, cnt = cells
        if not re.fullmatch(r"\d+", cnt):
            print(f"[RED] ROSTER-COUNT {mode} 的断言条数不是非负整数：{cnt!r}")
            sys.exit(1)
        if kind not in ("断言型", "人眼对照", "待修型（刻意不进闸门名单）"):
            print(f"[RED] ROSTER-KIND {mode} 的类型只能是 断言型/人眼对照/待修型（刻意不进闸门名单），实得 {kind!r}")
            sys.exit(1)
        if inc not in ("是", "否"):
            print(f"[RED] ROSTER-INCI {mode} 的「在 CI 闸门名单」只能 是/否，实得 {inc!r}")
            sys.exit(1)
        rows[mode] = (kind, inc == "是", int(cnt))
    if not rows:
        print("[RED] PRECONDITION 锚点之间没解析出任何数据行")
        sys.exit(2)
    return rows


def main():
    verbose = "--verbose" in sys.argv
    a = declared()
    b = ci_list()
    rows = doc_rows()
    c = set(rows)
    bad = 0

    def fail(code, msg):
        nonlocal bad
        bad += 1
        print(f"[RED] {code} {msg}")

    for m in sorted(a - c):
        fail("ROSTER-MISSING", f"{m} 代码里有、文档表里没有")
    for m in sorted(c - a):
        fail("ROSTER-EXTRA", f"{m} 文档表里有、代码里没有这个模式")
    for m in sorted(a ^ b):
        side = "在代码不在 CI" if m in a else "在 CI 不在代码"
        # 人眼对照表 / 待修型（红门刻意不进门）：只有当文档把它标成 断言型 时才算违规
        if m in a and m not in b and rows.get(m, ("", False))[1] is False \
                and rows.get(m, ("x",))[0] in ("人眼对照", "待修型（刻意不进闸门名单）"):
            continue
        fail("ROSTER-CI", f"{m} {side}")
    for m, (kind, inc, _n) in sorted(rows.items()):
        if m in a:
            truly_in_ci = m in b
            if inc != truly_in_ci:
                fail("ROSTER-CI-FLAG",
                     f"{m} 表里写「{'是' if inc else '否'}」，ci.yml 实测{'在' if truly_in_ci else '不在'}名单")
            if kind == "人眼对照" and truly_in_ci:
                fail("ROSTER-KIND", f"{m} 标为人眼对照却出现在 CI 闸门名单里")
            if kind == "断言型" and not truly_in_ci:
                fail("ROSTER-KIND", f"{m} 标为断言型却没进 CI 闸门名单")
            # 待修型的钉子：它**必须**不在闸门名单（否则 CI 常红），且必须标 否。
            # 与"断言型⇔在名单"合起来 ⇒ 类型列不能拿"待修型"当永久豁免的逃生口：
            # 想留在名单里就必须先让它绿并改标断言型。
            if kind.startswith("待修型") and truly_in_ci:
                fail("ROSTER-KIND", f"{m} 标为待修型却出现在 CI 闸门名单里（红门不许进门）")

    print(f"[probe_roster] 代码声明={len(a)} CI名单={len(b)} 文档表={len(c)} "
          f"人眼对照={sum(1 for k, _, _ in rows.values() if k == '人眼对照')} "
          f"待修型={sum(1 for k, _, _ in rows.values() if k.startswith('待修型'))} "
          f"断言型={sum(1 for k, _, _ in rows.values() if k == '断言型')} 违规={bad}")
    if verbose:
        for m, (kind, inc, n) in sorted(rows.items()):
            print(f"    {m:24s} {kind} in_ci={'是' if inc else '否'} asserts={n}")
    if bad:
        print("[PROBE_ROSTER] FAIL")
        return 1
    print("[PROBE_ROSTER] OK: 三集合一致（模式名 / 类型 / CI 名单互钉）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
