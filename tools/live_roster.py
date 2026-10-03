#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""「哪一份是活的」名单门：docs/ARCHITECTURE.md §1.3 表里列的**未编译集**，必须与现算的集合逐字相等。

要解决的陷阱（2026-09-30 用户实跑发现）：`src/core/legacy/network_winhttp.c` 874 行、和
`src/core/network_curl.c` 定义**同一批 9 个对外符号**（`network_init` 等），但它一行都不进编译。
只读代码的人会去改错的那一份 —— 而"哪份是活的"这件事，仓里此前只能靠反推 CMakeLists 得出，
没有登记，也没有门。所以这张表必须可复算、且漂移即红。

判据来自哪里（不是我新发明的规矩）：
  * 「编译集」= CMakeLists.txt 里被点名的 `src/**` / `tools/**` 源文件（注释行先去掉，理由见下）。
  * 「未编译 TU」= 磁盘上的 .c/.cpp − 编译集。
  * 表由 docs/ARCHITECTURE.md §1.3 那一节的第一张 markdown 表派生。

**两个输入都必须先去注释**，否则门会被注释骗过去 —— 这条是本仓已入库的规矩：
  1. CMakeLists 的 `#` 行：有人在注释里写一句"network_winhttp.c 已停用"，naive grep 就会把它
     当成"被 CMake 点名"⇒ 从未编译集里消失 ⇒ 表反而报 EXTRA、并且把真陷阱洗白。
  2. markdown 的 `<!-- -->`：把表整段注释掉（"暂时藏起来"）不应该让门变绿 —— 注释掉的行不计入，
     于是同一批文件会以 ROSTER-MISSING 报红。

退出码：0 = 表与集合一致；1 = 不一致（逐条 ROSTER-* 打印）；2 = 工具前置不成立（缺文件/缺节）。
用法：python tools/live_roster.py
"""

import glob
import os
import re
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "ARCHITECTURE.md")
CML = os.path.join(ROOT, "CMakeLists.txt")
SRC = os.path.join(ROOT, "src")

ANCHOR = "### 1.3"
UNCOMPILED_CAT = "未编译"
CATS = {UNCOMPILED_CAT, "仅声明型死头", "开关才编译", "并存分派，不是两版"}

SRC_RE = re.compile(r"(src|tools)/[A-Za-z0-9_./*{}-]+\.(c|cpp|h)\b")

red = []
info = []


def fail(tag, msg):
    red.append("%s: %s" % (tag, msg))


# ── 1. 编译集（CMakeLists，注释行不参与匹配）──────────────────────────────
def cmake_text_no_comments():
    with open(CML, encoding="utf-8") as fh:
        lines = []
        in_block = False
        for line in fh:
            stripped = line.lstrip()
            if in_block:                       # #[[ ... ]] 块注释
                if "]]" in line:
                    in_block = False
                continue
            if stripped.startswith("#[[" ) or stripped.startswith("#[[\n") or "#[[" in line:
                if "]]" not in line:
                    in_block = True
                continue
            if stripped.startswith("#"):
                continue
            lines.append(line)
        return "".join(lines)


def disk_tus():
    out = []
    for base in ("src", "tools"):
        for ext in ("c", "cpp"):
            for path in glob.glob(os.path.join(ROOT, base, "**", "*." + ext), recursive=True):
                out.append(os.path.relpath(path, ROOT).replace("\\", "/"))
    return set(out)


def referenced():
    txt = cmake_text_no_comments()
    return {m.group(0) for m in SRC_RE.finditer(txt)}


# ── 2. 表（markdown，HTML 注释块不参与）───────────────────────────────────
def strip_html_comments(txt):
    return re.sub(r"<!--.*?-->", "", txt, flags=re.S)


def brace_expand(token):
    m = re.search(r"\{([^{}]*)\}", token)
    if not m:
        return [token]
    head, tail = token[:m.start()], token[m.end():]
    out = []
    for alt in m.group(1).split(","):
        out.extend(brace_expand(head + alt.strip() + tail))
    return out


def parse_table():
    """返回 (rows, err)；rows = [(paths, category, basis)]，paths 已展开为具体路径。"""
    with open(DOC, encoding="utf-8") as fh:
        raw = fh.read()
    txt = strip_html_comments(raw)
    lines = txt.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.startswith(ANCHOR):
            start = i
            break
    if start is None:
        return None, "文档里找不到 §%s 这一节（表被删了/被注释了/编号改了）" % ANCHOR.strip("# ").replace(".3", ".3")
    rows = []
    seen_table = False
    header_checked = False
    for line in lines[start + 1:]:
        if line.startswith("|"):
            seen_table = True
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) < 3:
                fail("ROSTER-MALFORMED", "表列数不足 3（文件/类别/依据）：%s" % line)
                continue
            if set(cells[0]) <= set("-: ") and cells[0]:
                continue                                   # 分隔行
            if not header_checked:
                # 第一行是表头，不是数据。列名是解析锚点：有人改列名（或改了列序），
                # 下面按 cells[1] 取类别就会静默取错列 —— 所以宁可红，不许猜。
                header_checked = True
                if "文件" not in cells[0] or cells[1] != "类别":
                    return None, ("§1.3 表头不是「文件 / 类别 / 依据」而是「%s / %s」⇒ 门的取列假设已"
                                  "失效，改表必须同批改门" % (cells[0], cells[1]))
                continue
            category, basis = cells[1], cells[2]
            paths = []
            for token in re.split(r"[、,]", cells[0].replace("`", "")):
                token = token.strip()
                if not token:
                    continue
                hit = brace_expand(token)
                expanded = []
                for one in hit:
                    if any(ch in one for ch in "*?"):
                        matches = glob.glob(os.path.join(ROOT, one.replace("/", os.sep)))
                        matches = [os.path.relpath(p, ROOT).replace("\\", "/") for p in matches]
                        if not matches:
                            fail("ROSTER-STALE", "表里的通配 %s 在盘上匹配 0 个文件" % one)
                        expanded.extend(sorted(matches))
                    else:
                        expanded.append(one)
                paths.extend(expanded)
            rows.append((paths, category, basis))
        elif seen_table:
            break                                            # 表结束
    if not rows:
        return None, "§%s 后面第一张表是空的" % ANCHOR
    return rows, None


# ── 3. 头文件可达性（从编译 TU 出发，按编译器顺序：同目录 → 全 src 同名）───
HEADER_INDEX = {}
for _dp, _dd, _ff in os.walk(SRC):
    for _f in _ff:
        if _f.endswith((".h", ".hpp")):
            HEADER_INDEX.setdefault(_f, []).append(
                os.path.relpath(os.path.join(_dp, _f), ROOT).replace("\\", "/"))


def header_reachable(header_rel):
    """header_rel 是否被某个编译 TU 直接或传递 include。返回 (bool, 首个 includer)。

    解析顺序照编译器：先「同目录/相对路径」精确命中，命不中再按 basename 在全 src 找
    （对应 CMake 那 9 个 target_include_directories）。同名头会让后者偏松 —— 这一条只做
    「进不进编译」的判断，偏松的方向是「少报红」，所以 §1.3 表里那行的依据要能人工复核。
    """
    compiled = sorted(compiled_tus)
    include_cache = {}

    def includes_of(rel):
        if rel in include_cache:
            return include_cache[rel]
        full = os.path.join(ROOT, rel.replace("/", os.sep))
        res = []
        if os.path.isfile(full):
            with open(full, encoding="utf-8", errors="replace") as fh:
                body = strip_c_comments(fh.read())
            for m in re.finditer(r'^\s*#\s*include\s*"([^"]+)"', body, re.M):
                res.append(m.group(1))
        include_cache[rel] = res
        return res

    def resolve(from_rel, name):
        base = name.split("/")[-1]
        # 同目录
        cand = os.path.normpath(os.path.join(os.path.dirname(from_rel), name)).replace("\\", "/")
        if os.path.isfile(os.path.join(ROOT, cand.replace("/", os.sep))):
            return cand
        # target_include_directories 的 9 个目录 + src 根，简化为「全 src 同名」
        cands = HEADER_INDEX.get(base, [])
        for rel in sorted(cands):
            if rel.startswith("src/"):
                return rel
        return None

    target = os.path.normpath(header_rel).replace("\\", "/")
    seen, queue = set(), list(compiled)
    while queue:
        cur = queue.pop()
        if cur in seen:
            continue
        seen.add(cur)
        for name in includes_of(cur):
            resolved = resolve(cur, name)
            if resolved == target:
                return True, cur
            if resolved:
                queue.append(resolved)
    return False, None


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text, flags=re.M)


# ── 3b. 僵尸 include：活着的编译单元 include 了「实现不编译」的头 ────────────
"""为什么要单独判这一族（2026-10-02 实测出来的洞）：
   `src/gui/main_window.h:10` 至今 `#include "legacy/glass_effect.h"`，而 glass_effect.cpp 一行不进编译。
   原表把这件事写成一句**散文**（§1.2 末「唯一还开着的项」），门只建模「头是否可达」——
   可达 ⇒ 报 info、永不红。于是"清掉了"和"没清"在退码上完全一样：**假绿**。
   两侧都得拦：① 死头被活 TU include（今天真实存在的那一侧）；② 表里登记了 include 点却已不存在（反向）。
   所以判据从**代码**现算 includer，登记表只作对照；两边集合必须逐字相等。"""


def all_headers():
    """src 下所有 .h/.hpp（相对路径）。"""
    out = []
    for dirpath, _d, files in os.walk(SRC):
        rel_dir = os.path.relpath(dirpath, ROOT).replace("\\", "/")
        if not rel_dir.startswith("src/"):
            continue
        for f in files:
            if f.endswith((".h", ".hpp")):
                out.append("%s/%s" % (rel_dir, f))
    return set(out)


def dead_headers():
    """有同名实现、但那个实现不在编译集里的 src 头。"""
    out = set()
    for hdr in all_headers():
        stem = hdr.rsplit(".", 1)[0]
        impl_cpp, impl_c = stem + ".cpp", stem + ".c"
        has_impl = (os.path.isfile(os.path.join(ROOT, impl_cpp.replace("/", os.sep)))
                    or os.path.isfile(os.path.join(ROOT, impl_c.replace("/", os.sep))))
        if has_impl and impl_cpp not in compiled_tus and impl_c not in compiled_tus:
            out.add(hdr)
    return out


def resolve_include(from_rel, name):
    """照编译器顺序解析一条 #include "..."：同目录 → 全 src 同名。返回相对路径或 None。"""
    cand = os.path.normpath(os.path.join(os.path.dirname(from_rel), name)).replace("\\", "/")
    if os.path.isfile(os.path.join(ROOT, cand.replace("/", os.sep))):
        return cand
    base = name.split("/")[-1]
    for rel in sorted(HEADER_INDEX.get(base, [])):
        if rel.startswith("src/"):
            return rel
    return None


def direct_includers(header_rel):
    """所有直接 #include 该头的文件（编译 TU **加上全部头**，去注释后按编译器顺序解析）。
    ⚠ 必须含 .h：main_window.h 不是编译单元，但它 include 了死头——只扫 .c/.cpp 会漏掉今天这一条。"""
    hits = []
    for rel in sorted(compiled_tus | all_headers()):
        full = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            continue
        with open(full, encoding="utf-8", errors="replace") as fh:
            body = strip_c_comments(fh.read())
        for n, line in enumerate(body.split("\n"), 1):
            m = re.match(r'\s*#\s*include\s*"([^"]+)"', line)
            if m and resolve_include(rel, m.group(1)) == header_rel:
                hits.append((rel, n))
    return hits


# ── 4. 主判据 ─────────────────────────────────────────────────────────────
t0 = time.time()
for path, what in ((DOC, "docs/ARCHITECTURE.md"), (CML, "CMakeLists.txt"), (SRC, "src/")):
    if not os.path.exists(path):
        print("[RED] ROSTER-PRECONDITION: 找不到 %s（%s）" % (what, path))
        sys.exit(2)

all_tus = disk_tus()
refs = referenced()
compiled_tus = all_tus & refs
uncompiled_tus = all_tus - refs

# 表里的注释行被 strip 掉之后，"被 CMake 点名"这件事也必须只看非注释行 —— 上面已经这么做。
rows, err = parse_table()
if err:
    print("[RED] ROSTER-ANCHOR: %s" % err)
    print("[RED] ROSTER-ANCHOR: 未编译集现算 %d 条：%s"
          % (len(uncompiled_tus), ", ".join(sorted(uncompiled_tus))))
    print("[RED] ROSTER-ANCHOR 是**红**，不是跳过：本门退出码 1")
    print("live_roster EXIT=1 cost=%dms" % int((time.time() - t0) * 1000))
    sys.exit(1)

listed_uncompiled = set()
zombie_listed = {}          # 表里登记的「死头 ← 谁 include 它」：{死头: {includer, ...}}
for paths, category, basis in rows:
    label = ", ".join(paths)
    if category not in CATS:
        fail("ROSTER-UNKNOWN-CATEGORY",
             "类别 %r 不在登记的四类里（%s）⇒ 这条既不算通过也不算失败，必须收进表"
             % (category, "、".join(sorted(CATS))))
        continue

    if category == UNCOMPILED_CAT:
        for p in paths:
            if not os.path.isfile(os.path.join(ROOT, p.replace("/", os.sep))):
                fail("ROSTER-STALE", "表里的 %s 在盘上不存在（改名/删除后名单没跟上）" % p)
                continue
            if p.endswith((".h", ".hpp")):
                # 头文件不进 TU 恒等式：它是否"进编译"只看有没有编译 TU 走到它（直接或传递）。
                reach, includer = header_reachable(p)
                if reach:
                    fail("ROSTER-LIVENESS", "%s：标了未编译，但仍被 %s 那条链 include ⇒ 声明还在进编译"
                         % (p, includer))
                else:
                    info.append("    %s：无任何编译 TU 走到 ⇒ 整份不进编译（含声明）" % p)
                # 「未编译」这一类也要贡献僵尸 include 的对照集（判据不靠类别标签，见 §3b 注释）
                if re.search(r"\binclude\b", basis, re.I):
                    stem = p.rsplit(".", 1)[0]
                    for m in SRC_RE.finditer(basis):
                        cand = m.group(0)
                        if cand.endswith((".h", ".hpp")) and cand.rsplit(".", 1)[0] != stem:
                            zombie_listed.setdefault(p, set()).add(cand)
                continue
            if p not in all_tus:
                fail("ROSTER-STALE", "表里的 %s 在盘上不存在（改名/删除后名单没跟上）" % p)
            elif p in compiled_tus:
                fail("ROSTER-LIVENESS", "表标 %s 为未编译，但它现在在编译集里" % p)
            else:
                listed_uncompiled.add(p)
        continue

    if category == "开关才编译":
        opts = re.findall(r"IDM_BUILD_[A-Z0-9_]+", basis)
        if not opts:
            fail("ROSTER-BASIS", "%s 标了开关，但依据列没写出 option 名" % label)
        for tok in opts:
            m = re.search(r"option\(\s*" + tok + r"\s+\"[^\"]*\"\s+(ON|OFF|TRUE|FALSE)",
                          cmake_text_no_comments(), re.I)
            if not m:
                fail("ROSTER-OPTION-STALE", "%s：CMakeLists 里找不到 option %s（或写法变了）" % (label, tok))
            elif m.group(1).upper() not in ("OFF", "FALSE"):
                fail("ROSTER-OPTION-STALE", "%s：%s 默认已是 %s ⇒「开关才编译」不再成立"
                     % (label, tok, m.group(1)))

    if category == "仅声明型死头":
        # 依据列必须点出符号名（形如 `Glass::`），否则「调用点=0」这条无从复算 ——
        # 2026-09-30 实测过这个洞：把该行依据列里的 `Glass::` 全删，门照样绿（静默跳过）。
        syms = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*::", basis))
        if not syms:
            fail("ROSTER-BASIS", "%s：依据列没点出符号名（形如 `Glass::`）⇒ 调用点检查没有对象，"
                 "「无调用点」这句无法复算" % label)
        for p in paths:
            if not os.path.isfile(os.path.join(ROOT, p.replace("/", os.sep))):
                fail("ROSTER-STALE", "表里的 %s 在盘上不存在" % p)
                continue
            sib = re.sub(r"\.h$", ".cpp", p)
            sib2 = re.sub(r"\.h$", ".c", p)
            # ⚠ 「仅声明型死头」有两种，判据不能只认一种：
            #   ①有同名实现且实现不编译（glass_effect.h/.cpp 曾经那种）；
            #   ②**根本没有同名实现**（button_translator.h 这种 header-only）。
            #   第一版无条件要求实现落在未编译集 ⇒ ②被判成「仅声明不成立」而红——
            #   红的是判据写窄了，不是名单写错。header-only 时这一条不该发言。
            sib_exists = (os.path.isfile(os.path.join(ROOT, sib.replace("/", os.sep)))
                          or os.path.isfile(os.path.join(ROOT, sib2.replace("/", os.sep))))
            if sib_exists and sib not in uncompiled_tus and sib2 not in uncompiled_tus:
                fail("ROSTER-LIVENESS", "%s：同名实现（%s/%s）不在未编译集 ⇒「仅声明」不成立" % (p, sib, sib2))
            reach, includer = header_reachable(p)
            if not reach:
                fail("ROSTER-LIVENESS", "%s：没有任何编译 TU 走到这个头 ⇒ 它连声明都不进编译，"
                     "应改标为未编译/死头（表里的说法要跟着改）" % p)
            else:
                info.append("    %s 由 %s 引入；声明进编译、实现不编译" % (p, includer))
            # 依据列里点名的「还在 include 这个死头」的文件，进对照集。
            # ⚠ 锚点必须是字面 `include`：这条行本来就写着"被 … include ⇒"。若哪天这句话被改写、
            #   一个 include 都没点名，下面会报 ROSTER-BASIS（而不是静默拿到空集、把一侧的牙卸掉）。
            # 依据列里点名的「还在 include 这个死头」的文件，进对照集。
            # ⚠ 锚点必须是字面 `include`：这条行本来就写着"被 … include ⇒"。若哪天这句话被改写、
            #   一个 include 都没点名，下面会报 ROSTER-BASIS（而不是静默拿到空集、把一侧的牙卸掉）。
            if re.search(r"\binclude\b", basis, re.I):
                stem = p.rsplit(".", 1)[0]
                for m in SRC_RE.finditer(basis):
                    cand = m.group(0)
                    # 排除"死头自己那一行"：表里的路径**不带扩展名**（`src/gui/legacy/glass_effect.{h,cpp}`），
                    # 所以不能拿 cand != p 比——那样永远不等，把该排的排不掉、该收的又因 .h 结尾判断而收进来。
                    if cand.endswith((".h", ".hpp")) and cand.rsplit(".", 1)[0] != stem:
                        zombie_listed.setdefault(p, set()).add(cand)
        listed_syms = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*::", basis))
        if not listed_syms:
            fail("ROSTER-BASIS", "%s：依据列没点出符号名（形如 `Glass::`）⇒ 调用点检查没有对象，"
                 "「无调用点」这句无法复算" % label)
        # 判据**不拿表里的字符串当搜索词**（第一版就是 `for sym in basis` —— 把符号名删掉后循环直接变空，
        # 门照样绿 ⇒ ROSTER-BASIS 那条红是我"以为有"而没有）。
        # 真做法：从死头源码里抽它**自己声明**的限定名。⚠ 只认「定义点」而不是全文出现：
        #   button_translator.h 里 `QString::fromUtf8(...)` 是**使用**别处的名字，
        #   按全文抽会把 QString:: 当成它的导出符号（实测就是这样误红，且红得像是名单写错）。
        code_syms = set()
        for p in paths:
            full = os.path.join(ROOT, p.replace("/", os.sep))
            if not os.path.isfile(full):
                continue
            with open(full, encoding="utf-8", errors="replace") as fh:
                hsrc = strip_c_comments(fh.read())
            code_syms |= {m + "::" for m in
                          re.findall(r"\b(?:class|struct|enum|namespace)\s+([A-Za-z_][A-Za-z0-9_]*)", hsrc)}
            code_syms |= {m + "::" for m in
                          re.findall(r"^\s*namespace\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", hsrc, re.M)}
            # 头里定义的静态成员/自由函数不算限定名（`ZhCnButtonTranslator::translate` 这种在 .cpp 才有）
        if not code_syms:
            fail("ROSTER-BASIS", "%s：%s 里抽不出它自己声明的类/命名空间 ⇒「调用点=0」无从复算"
                 % (label, ", ".join(paths)))
        unknown = listed_syms - code_syms
        if unknown:
            fail("ROSTER-BASIS", "%s：表登记的符号 %s 不是该头声明的名字（该头声明的是 %s）⇒ "
                 "调用点检查会去数别人家的符号" % (label, ", ".join(sorted(unknown)),
                                                   ", ".join(sorted(code_syms)) or "(无)"))
        for sym in sorted(listed_syms & code_syms):
            hits = []
            for dirpath, _d, files in os.walk(os.path.join(ROOT, "src")):
                for f in files:
                    if not f.endswith((".c", ".cpp", ".h")):
                        continue
                    rel = os.path.relpath(os.path.join(dirpath, f), ROOT).replace("\\", "/")
                    if os.path.dirname(rel) == os.path.dirname(paths[0]):
                        continue                     # 自己的 legacy 目录不算调用点
                    with open(os.path.join(ROOT, rel.replace("/", os.sep)),
                              encoding="utf-8", errors="replace") as fh:
                        body = strip_c_comments(fh.read())
                    if re.search(r"(?<![\w:])" + re.escape(sym), body):
                        hits.append(rel)
            if hits:
                fail("ROSTER-LIVENESS", "%s 有调用点 %s ⇒ 一旦它的实现仍未编译就是链接错误，"
                     "名单必须改标" % (sym, ", ".join(hits[:4])))
            else:
                info.append("    %s 调用点=0（全 src 去注释后）" % sym)
        continue

    if category == "并存分派，不是两版":
        missing = [p for p in paths if p not in compiled_tus]
        if missing:
            fail("ROSTER-LIVENESS", "标了并存分派但有文件不在编译集：%s" % ", ".join(missing))
        dispatch = [m.group(0) for m in SRC_RE.finditer(basis)]
        if not dispatch:
            fail("ROSTER-BASIS", "%s：依据列没写分派点文件 ⇒「并存分派」无法复算" % label)
        else:
            dfile = dispatch[0]
            full = os.path.join(ROOT, dfile.replace("/", os.sep))
            if not os.path.isfile(full):
                fail("ROSTER-STALE", "分派点 %s 不存在" % dfile)
            else:
                with open(full, encoding="utf-8", errors="replace") as fh:
                    body = strip_c_comments(fh.read())
                named = {os.path.splitext(p.split("/")[-1])[0] + ".h" for p in paths}
                cnt = sum(1 for b in named if '#include "%s"' % b in body)
                if cnt < 2:
                    fail("ROSTER-LIVENESS", "%s 只 include 了本行 %d 个文件 ⇒ 不像分派点，"
                         "「并存分派」的依据失效" % (dfile, cnt))
                else:
                    info.append("    %s include 了本行 %d/%d 个" % (dfile, cnt, len(named)))

# 恒等式（多桶分类必须印，否则某一桶既不算通过也不算失败）
print("TU 恒等式：磁盘 %d = 编译 %d + 未编译 %d  [%s]"
      % (len(all_tus), len(compiled_tus), len(uncompiled_tus),
         "OK" if len(all_tus) == len(compiled_tus) + len(uncompiled_tus) else "RED"))
if len(all_tus) != len(compiled_tus) + len(uncompiled_tus):
    fail("ROSTER-IDENTITY", "桶数不闭合")

# 双向比对
for p in sorted(uncompiled_tus - listed_uncompiled):
    fail("ROSTER-MISSING", "现算未编译但表里没有：%s" % p)
extra = sorted(listed_uncompiled - uncompiled_tus)
if extra:
    fail("ROSTER-EXTRA", "表里标未编译但实际不在现算集合：%s" % ", ".join(extra))

# 僵尸 include：实现不编译的死头，仍被谁 #include。两侧都要拦（见 §3b 的来历）。
# ⚠ **不依赖表里的类别标签**：判据从代码现算"谁 include 了死头"，再从**任意行**的依据列收集
#   点名的 includer。第一版只在 `category == "仅声明型死头"` 分支里收集，于是
#   「未编译」行依据列里那句"仍被 src/gui/main_window.h:10 include"根本进不了对照集 ⇒
#   反向那侧（清掉 include 却忘改表）静默绿。M10 变异把这点暴露出来了。
zombie_actual = {}
for hdr in sorted(dead_headers()):
    inc = direct_includers(hdr)
    if inc:
        zombie_actual[hdr] = {rel for rel, _ln in inc}
        info.append("    [僵尸 include] %s ← %s"
                    % (hdr, ", ".join("%s:%d" % (r, n) for r, n in inc)))
if not zombie_actual and not zombie_listed:
    info.append("    [僵尸 include] 现算 0 条、表里也没登记 ⇒ 这一族今天不存在（两面夹具见 mutation_fixture_live_roster.py）")
for hdr in sorted(zombie_actual):
    if hdr not in zombie_listed:
        fail("ROSTER-ZOMBIE", "%s 的实现不编译，却仍被 %s include；§1.3 表没有登记这条 ⇒ "
             "「已完全断开」这类说法随时会重新变成假的" % (hdr, ", ".join(sorted(zombie_actual[hdr]))))
    else:
        missing = zombie_actual[hdr] - zombie_listed[hdr]
        extra = zombie_listed[hdr] - zombie_actual[hdr]
        if missing:
            fail("ROSTER-ZOMBIE", "%s 还被 %s include，但表里没点名这几个"
                 % (hdr, ", ".join(sorted(missing))))
        if extra:
            fail("ROSTER-ZOMBIE-STALE", "表里说 %s 被 %s include，实测已经没有这条 include 了 ⇒ "
                 "要么清掉登记，要么把该文件重新纳入名单" % (hdr, ", ".join(sorted(extra))))
for hdr in sorted(zombie_listed):
    if hdr not in zombie_actual:
        fail("ROSTER-ZOMBIE-STALE", "表里给 %s 登记了 include 点（%s），实测没有任何文件 include 它"
             % (hdr, ", ".join(sorted(zombie_listed[hdr]))))

for line in info:
    print(line)

print("表行数=%d（未编译桶 %d）｜现算未编译=%d：%s"
      % (len(rows), len(listed_uncompiled), len(uncompiled_tus),
         ", ".join(sorted(uncompiled_tus))))

if red:
    for r in red:
        print("[RED] " + r)
    print("live_roster: %d 条红｜成本 %dms" % (len(red), int((time.time() - t0) * 1000)))
    sys.exit(1)

print("live_roster OK: 表与复算集合一致（未编译 %d 条全部登记，四个类别全部建模）｜成本 %dms"
      % (len(uncompiled_tus), int((time.time() - t0) * 1000)))
sys.exit(0)
