#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用**项目自己的真实 flags** 收全仓 warning 清单（只读，不写 build 目录）。

为什么要它（2026-10-01，主控否掉了我前两版扫描器之后定下的做法）：
  * 我手搓 flags 的 `-fsyntax-only` 版在 59 个 TU 里只解析成功 6 个 ⇒ 那个"0 命中"是瞎出来的；
  * 我自己的正则版把合法的 `"%s\\\\%s"` 报成坏 ⇒ 4 个控件错 2 个 ⇒ 也不可信。
  ⇒ 唯一站得住的办法：**命令由构建系统自己交出来** —— `ninja -t commands <target>` 给出每个 TU 的
  真实编译命令，我把它们改成 `-fsyntax-only`（去掉 `-o`，其余一字不动）再逐条跑。

产出是**可复算的**：`tools/warning_inventory.py --verify <清单文件>` 会重算一遍并与文件比对，
不一致就 exit 1（清单里不含耗时等易变字段，所以可比）。

用法：python tools/warning_inventory.py                 # 打印清单到 stdout
      python tools/warning_inventory.py > docs/warnings-<日期>.txt
      python tools/warning_inventory.py --verify docs/warnings-<日期>.txt
退出码：0 = 正常（--verify 时 = 清单与现算一致）；1 = 不一致或有 TU 编译报错；2 = 前置不成立。

这个 0 只能盖到哪里（2026-10-01 主控追问后补；两句都是"给一个 0 标清射程"）：
  1. **项目自己一条 -W 都没开**：从 `ninja -t commands` 的四个目标里抽 `-W*`，只有链接器用的
     `-Wl,`；`CMakeLists.txt` 里也没有 `add_compile_options`。⇒ "0 warning" 的准确读法是
     「在只要求编译器默认诊断的前提下 0」，不是"代码干净"。清单也不是死的：零 -W 条件下
     它照样抓到了种进去的未知转义（那属默认诊断）⇒ 上限=默认开的那部分。
  2. **`-fsyntax-only` 不走 codegen** ⇒ 只在生成阶段报的那类（典型如 -Wmaybe-uninitialized）
     不在射程内。要覆盖那一类得真写到目标文件，本工具故意不写产物。
  3. 抬上限不需要改构建：`--extra-w "-Wall -Wextra"` 只往**本工具自己拼的命令**上加开关，
     build/ 与 CMakeLists.txt 一个字不动；默认不开（不开才与历史清单可比）。
"""

import os
import re
import subprocess
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TARGETS = ["idm-next", "idm-next-host", "ui_snapshot", "engine_selftest"]
CMD_RE = re.compile(r"(?:^|\s)(?:[A-Za-z]:\\[^\s]*\\)?(gcc|g\+\+)(\.exe)?\s")


def commands_for(target):
    p = subprocess.run(["ninja", "-C", "build", "-t", "commands", target],
                       cwd=ROOT, capture_output=True)
    if p.returncode != 0:
        return []
    out = []
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        if CMD_RE.search(line) and " -c " in line:
            out.append(line)
    return out


def split_cmd(line):
    """按双引号切词（ninja 输出里带空格的路径都用引号包住）。
    ⚠️ 必须处理**转义引号**：真命令里有 -DIDM_NEXT_VERSION=\"0.1.0\"，把它当引号开关丢掉会让
    定义值变成反斜杠包住的 0.1.0 ⇒ g++ 报 "stray backslash" 与 "too many decimal points"，
    而这两条看起来像源码问题、其实是我的拆词器造出来的（本仓 src/main.cpp 就是这么"未覆盖"的）。"""
    toks, cur, q, i = [], "", False, 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and line[i + 1] == '"':
            cur += '"'                      # 转义引号 ⇒ 一个字面引号，不改引号状态
            i += 2
            continue
        if ch == '"':
            q = not q
            i += 1
            continue
        if ch == " " and not q:
            if cur:
                toks.append(cur)
                cur = ""
            i += 1
            continue
        cur += ch
        i += 1
    if cur:
        toks.append(cur)
    return toks


def to_syntax_only(toks):
    """改成语法检查：去掉 -o（不写产物），并去掉 -MD/-MT/-MF（依赖文件写盘会先 fatal error，
    实测这样第一条命令就死在 "opening dependency file ..." 上 ⇒ 覆盖率会全灭）。"""
    out, skip = [], False
    for t in toks:
        if skip:
            skip = False
            continue
        if t in ("-o", "-MT", "-MF"):
            skip = True
            continue
        if t in ("-MD", "-MMS", "-MM"):
            continue
        out.append(t)
    if "-fsyntax-only" not in out:
        out.append("-fsyntax-only")
    return out


def source_of(toks):
    for i, t in enumerate(toks):
        if t == "-c" and i + 1 < len(toks):
            return toks[i + 1]
    for t in toks:
        if t.endswith((".c", ".cpp", ".cxx", ".cc")):
            return t
    return "?"


def classify(msg):
    m = re.search(r"warning: (.*?) at |warning: (.*?)\s*$", msg)
    txt = (m.group(1) or m.group(2)) if m else msg
    return re.sub(r"['\u2018\u2019`\u00b4]", "'", txt)[:64]


def build_inventory(extra=None):
    seen, rows = {}, []
    for tgt in TARGETS:
        for line in commands_for(tgt):
            toks = split_cmd(line)
            src = source_of(toks)
            rel = os.path.relpath(src, ROOT).replace("\\", "/") if os.path.isabs(src) else src
            key = (rel, tgt)
            if rel in seen:                      # 同一 TU 在多个 target 里出现，取第一次的命令
                continue
            seen[rel] = True
            argv = to_syntax_only(toks)
            if extra:
                argv = argv + extra
            p = subprocess.run(argv, cwd=ROOT, capture_output=True)
            txt = (p.stdout + p.stderr).decode("utf-8", "replace")
            wl = [l.strip() for l in txt.splitlines() if re.search(r"\bwarning:", l)]
            el = [l.strip() for l in txt.splitlines() if re.search(r"\berror:", l)]
            rows.append((tgt, rel, p.returncode, wl, el))
    return rows


def extra_flags():
    if "--extra-w" in sys.argv:
        try:
            return sys.argv[sys.argv.index("--extra-w") + 1].split()
        except IndexError:
            return []
    return []


def render(rows):
    out = []
    out.append("# 全仓 warning 清单（由 tools/warning_inventory.py 生成；命令取自 ninja -t commands）")
    out.append("# 复算：python tools/warning_inventory.py > 本文件   核对：--verify 本文件")
    # "未覆盖"=命令本身没被拆词器吃下（诊断指向 <command-line> 而不是某个源文件）。
    # 这种 TU 不能记进"干净通过"，否则覆盖率会说谎（本轮实测：1/64 条命令报 stray '' in program）。
    def not_covered(r):
        return any("<command-line>" in l for l in r[4])
    uncovered = sum(1 for r in rows if not_covered(r))
    ok = sum(1 for r in rows if r[2] == 0 and not r[4])
    err = sum(1 for r in rows if (r[2] != 0 or r[4]) and not not_covered(r))
    warns = sum(len(r[3]) for r in rows)
    out.append("warning 开关=项目自身: none（唯一的 -W* 是链接器 -Wl,）｜额外: %s" % (" ".join(extra_flags()) if any(k == "--extra-w" for k in sys.argv) else "无"))
    out.append("适用域=解析+AST（-fsyntax-only；需 codegen 的 warning 类不在射程）")
    out.append("扫描 TU 数=%d ｜干净通过=%d ｜源码里报 error=%d ｜未覆盖(命令没吃下)=%d ｜warning 行数=%d"
               % (len(rows), ok, err, uncovered, warns))
    cats = {}
    for _t, _rel, _rc, wl, _el in rows:
        for l in wl:
            m = re.search(r"warning: (.*)", l)
            k = classify(m.group(1) if m else l)
            cats[k] = cats.get(k, 0) + 1
    out.append("")
    out.append("## 按类别（次数）")
    for k in sorted(cats, key=lambda x: (-cats[x], x)):
        out.append("  %4d  %s" % (cats[k], k))
    out.append("")
    out.append("## 逐条")
    for tgt, rel, rc, wl, el in sorted(rows, key=lambda r: r[1]):
        for l in wl + el:
            out.append("  [%s] %s" % (tgt, l))
    for tgt, rel, rc, wl, el in sorted(rows, key=lambda r: r[1]):
        if rc != 0 and el:
            out.append("  [RC 非零] %s (%s) rc=%d" % (rel, tgt, rc))
    return "\n".join(out) + "\n"


def main():
    t0 = time.time()
    if not os.path.isdir(BUILD):
        print("[RED] 找不到 build/（%s）⇒ 取不到真实编译命令，不判" % BUILD)
        return 2
    extra = []
    if "--extra-w" in sys.argv:
        extra = sys.argv[sys.argv.index("--extra-w") + 1].split()
    rows = build_inventory(extra)
    if not rows:
        print("[RED] 一个编译命令都没取到 ⇒ 本次扫描没有对象，不能报'0 warning'")
        return 2
    txt = render(rows)
    if "--verify" in sys.argv:
        try:
            path = sys.argv[sys.argv.index("--verify") + 1]
        except Exception:
            print("[RED] --verify 需要一个清单文件路径")
            return 2
        if not os.path.isfile(path):
            print("[RED] 清单文件不存在：%s" % path)
            return 2
        want = open(path, encoding="utf-8", errors="replace").read()
        # 不参与比对的行：注释里的复算指引，以及两条**静态射程说明**（它们不来自测量，
        # 改了它们不该逼一次 130s 的全量重跑；真正要比的是计数行、类别直方图与逐条清单）。
        SKIP = ("# 复算", "warning 开关=", "适用域=")
        a = [l for l in want.splitlines() if not l.startswith(SKIP)]
        b2 = [l for l in txt.splitlines() if not l.startswith(SKIP)]
        if a == b2:
            print("warning_inventory 一致：%d 行逐字相同（成本 %dms）" % (len(b), int((time.time() - t0) * 1000)))
            return 0
        import difflib
        d = list(difflib.unified_diff(a, b2, "清单文件", "现算", lineterm="", n=1))
        print("[RED] 清单与现算不一致（前 40 行差异）：")
        for l in d[:40]:
            print("   " + l)
        print("warning_inventory EXIT=1 成本=%dms" % int((time.time() - t0) * 1000))
        return 1
    sys.stdout.write(txt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
