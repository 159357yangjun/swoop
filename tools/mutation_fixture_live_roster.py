#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""live_roster 门的变异夹具：每道判据都要在「已知坏」的产物上真的红一次。

为什么要有这个文件（2026-09-30 本会话的教训）：`tools/live_roster.py` 的每条 `[RED] ROSTER-*`
如果没人把它逼出来过，它就只是装饰 —— 本仓已经吃过两次这种亏（`assert mut != mut` 的空变异、
以及 M6 那个「依据列删掉符号名、调用点检查静默跳过」的洞：**是先跑出绿、才补的判据**）。

复跑：python tools/mutation_fixture_live_roster.py
    退出码 0 = 九道全按预期红/绿；1 = 有判据咬不动（或还原不干净）。

安全：所有变异都在内存里留原始字节，跑完逐字还原并 `cmp` 式核对（不靠 git，因此未提交的改动也不会丢）。
日志落在 %TEMP%\\idm-probes\\<日期>\\live_roster\\ 下，路径印在最后一行 —— 红的那次原文必须可重取。
"""

import io
import os
import subprocess
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "ARCHITECTURE.md")
CML = os.path.join(ROOT, "CMakeLists.txt")
GATE = [sys.executable, os.path.join(ROOT, "tools", "live_roster.py")]

DAY = time.strftime("%Y-%m-%d")
LOGDIR = os.path.join(os.environ.get("TEMP", "/tmp"), "idm-probes", DAY, "live_roster")
if not os.path.isdir(LOGDIR):
    os.makedirs(LOGDIR)
LOG = os.path.join(LOGDIR, "mutation-%s.log" % time.strftime("%Y%m%dT%H%M%SZ"))

ORIG = {}


def read(path):
    with io.open(path, encoding="utf-8", newline="") as fh:
        return fh.read()


def snap(*paths):
    for p in paths:
        ORIG[p] = read(p)


def put(path, text):
    # ⚠ 写之前先把原件登记进 ORIG：上一版 m6 自己抛异常时 restore() 根本没跑，
    #   task_detail_dialog.h 与 ARCHITECTURE.md 的变异留在盘上（git status 才被抓出来），
    #   后面 M7~M11 全从残骸起跑 ⇒ 三条 SKIPPED-ANCHOR + 一条假 BAD。
    #   登记放在 put 里而不是各 case 里，是为了让"忘了 snap"这件事结构上不可能发生。
    if path not in ORIG:
        ORIG[path] = read(path)
    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    return True          # case() 用「返回值 is None」判「锚点没找到」，所以这里必须返回真值


EXTRA_FILES = {}          # 代码类变异额外碰到的文件（同样逐字还原）


def restore():
    for p, text in list(ORIG.items()) + list(EXTRA_FILES.items()):
        put(p, text)
    bad = [p for p, text in list(ORIG.items()) + list(EXTRA_FILES.items()) if read(p) != text]
    return bad


def gate():
    proc = subprocess.run(GATE, cwd=ROOT, capture_output=True)
    out = proc.stdout.decode("utf-8", "replace") + proc.stderr.decode("utf-8", "replace")
    return proc.returncode, out


def row_of(prefix):
    for line in read(DOC).split("\n"):
        if line.startswith("| " + prefix):
            return line
    return None


results = []
EXTRA = {}
NL = "\n"


def case(name, expect_tag, expect_rc, mutate, note):
    """跑一道变异：先断言锚点在、再断言改动真的落地（非空变异），然后跑门。"""
    text = mutate()
    if text is None:
        results.append((name, False, "SKIPPED-ANCHOR", "锚点串不在当前文件里 ⇒ 夹具已过期，先修夹具"))
        return
    rc, out = gate()
    hit = expect_tag in out if expect_tag else None
    ok = (rc == expect_rc) and (hit if expect_rc == 1 else not _any_red(out))
    ok = ok and EXTRA.get(name, True)          # 两面夹具的第二面（M5 的 naive 对照）
    reading = "rc=%d tag_found=%s" % (rc, hit) if expect_tag else "rc=%d naive洗白=%s" % (rc, EXTRA.get(name))
    results.append((name, ok, reading, note))
    with io.open(LOG, "a", encoding="utf-8") as fh:
        fh.write("=== %s ｜期望 %s rc=%d ===\n%s\n" % (name, expect_tag or "绿", expect_rc, out))
    # 每道变异跑完立刻还原：不还原的话，下一条是从上一条的残骸上起跑的 ——
    # 2026-09-30 第一版就是这么错的（M1 删了行 ⇒ M2/M3/M7 报「锚点不在」，M5 的红其实来自 M4）。
    still = restore()
    if still:
        results.append((name + " 还原", False, "restore-failed", "还原不干净会让后面所有读数作废"))


def _any_red(out):
    return "[RED]" in out


# ── 变异定义 ──────────────────────────────────────────────────────────────
WINHDR = "`src/core/legacy/network_winhttp.c`"
GLSHDR = "`src/gui/legacy/glass_effect.h`"
GLSCPP_PATH = "src/gui/legacy/glass_effect.h"
GLSCPP = "`src/gui/legacy/glass_effect.cpp`"
MAINH = os.path.join(ROOT, "src", "gui", "main_window.h")


def m1():
    """删掉未编译行 ⇒ 现算有、表里没有 ⇒ ROSTER-MISSING。"""
    s = read(DOC)
    row = row_of(WINHDR)
    if row is None:
        return None
    return put(DOC, s.replace(row + "\n", "", 1))


def m2():
    """把未编译行的文件写成一份**真在编译**的 TU ⇒ ROSTER-LIVENESS。"""
    s = read(DOC)
    row = row_of(WINHDR)
    if row is None:
        return None
    mut = row.replace("src/core/legacy/network_winhttp.c", "src/core/network_curl.c")
    if mut == row:
        return None
    return put(DOC, s.replace(row, mut, 1))


def m3():
    """文件名多一个字母（改名后名单没跟上）⇒ ROSTER-STALE。"""
    s = read(DOC)
    row = row_of(WINHDR)
    if row is None:
        return None
    mut = row.replace("network_winhttp.c", "network_winhttpX.c", 1)
    if mut == row:
        return None
    return put(DOC, s.replace(row, mut, 1))


def m4():
    """类别改成没登记的值 ⇒ 既不算通过也不算失败 ⇒ ROSTER-UNKNOWN-CATEGORY。"""
    s = read(DOC)
    row = row_of(GLSCPP)
    if row is None or "| 未编译 |" not in row:
        return None
    mut = row.replace("| 未编译 |", "| 已废弃 |", 1)
    return put(DOC, s.replace(row, mut, 1))


def m5():
    """两面夹具：往 CMakeLists 加一行**注释**点名未编译文件。
    第一面：门必须仍绿（去注释后它依然不在编译集）。
    第二面：naive 版命令（不去注释）必须把它当成"被 CMake 点名"而从未编译集里蒸发 ——
    这一面才证明 `grep -vE '^[[:space:]]*#'` 是承重的，不是装饰。"""
    s = read(CML)
    mut = s + "\n# 只是注释：src/core/legacy/network_winhttp.c 已停用，留档备查\n"
    put(CML, mut)
    import re
    naive = {m.group(0) for m in
             re.finditer(r"(src|tools)/[A-Za-z0-9_./*{}-]+\.(c|cpp)\b", read(CML))}
    washed = "src/core/legacy/network_winhttp.c" in naive      # True = naive 被注释洗白（预期）
    EXTRA["M5 CMake 注释点名死文件"] = washed
    with io.open(LOG, "a", encoding="utf-8") as fh:
        fh.write("=== M5 naive 对照：不去注释的 grep 把死文件当成被点名（洗白）= %s ===\n" % washed)
    return True


def m6a():
    """面 A：登记一条**盘上真成立**的「仅声明型死头」（button_translator.h 无实现、声明经 src/main.cpp 进编译）
    ⇒ 门必须绿。这一面是 M6 的前置：它若红，说明分支走不通，M6 的红就可能是别的原因蒙上的。"""
    st = os.path.join(ROOT, "src", "gui", "task_detail_dialog.h")
    body = read(st)
    put(st, body.replace("#include", '#include "button_translator.h"' + NL + "#include", 1))
    row_new = ("| `%s` | 仅声明型死头 | 被 `src/main.cpp` include ⇒ **声明进编译、实现不进**；"
               "`ZhCnButtonTranslator::` 在去注释后的全 `src/` 调用点 = 0 |" + NL) % "src/gui/button_translator.h"
    doc = read(DOC)
    anchor_row = row_of(WINHDR)
    if anchor_row is None:
        return None
    put(DOC, doc.replace(anchor_row + NL, anchor_row + NL + row_new, 1))
    return True


def m6():
    """两面夹具：现场造一个「仅声明型死头」条目，验 ROSTER-BASIS 有牙、且还原后仍绿。

    ⚠ 为什么不再用 glass_effect.h 那一行：2026-10-02 清掉最后一条 include 之后，那行的类别
      从「仅声明型死头」变成「未编译」⇒ 直接改它只会命中别的判据（实测 M6 变 rc=0）。
      判据本身没坏，是**夹具的锚点跟着产品漂了**。所以这里自己造两侧：
        面 A：代码里真加一条 include + 表里登记同一条 ⇒ 门必须绿（分支走得通）；
              然后删掉依据列的符号名 ⇒ ROSTER-BASIS 必须红。
        面 B（同一 case 的第二读数）：面 A 若红，说明分支根本没走通，M6 不作数。
    """
    # ⚠ 夹具标的必须是**盘上真成立**的事实，否则面 A 拿到的红来自我自己编的依据列（第一、二版都这样错：
    #   glass_effect.h 有未编译的 .cpp ⇒ 命中 ROSTER-LIVENESS；logger.h 的 `Log::` 真有调用点 ⇒ 命中
    #   ROSTER-LIVENESS「有调用点」）。现成的真事实：`src/gui/button_translator.h` 无同名实现、
    #   `ZhCnButtonTranslator::` 全 src 零命中、且已被编译 TU（src/main.cpp）include。
    hdr_rel = "src/gui/button_translator.h"
    if os.path.isfile(os.path.join(ROOT, "src", "gui", "button_translator.cpp")):
        return None                      # 前提变了就别造第二把半坏的门
    st = os.path.join(ROOT, "src", "gui", "task_detail_dialog.h")
    body = read(st)
    put(st, body.replace("#include", '#include "button_translator.h"' + NL + "#include", 1))

    row_new = ("| `%s` | 仅声明型死头 | 被 `src/main.cpp` include ⇒ **声明进编译、实现不进**；"
               "`ZhCnButtonTranslator::` 在去注释后的全 `src/` 调用点 = 0 |" + NL) % hdr_rel
    doc = read(DOC)
    anchor_row = row_of(WINHDR)
    if anchor_row is None:
        return None
    put(DOC, doc.replace(anchor_row + NL, anchor_row + NL + row_new, 1))

    rc_ok, out_ok = gate()
    with io.open(LOG, "a", encoding="utf-8") as fh:
        fh.write(("=== M6 面A（登记既有事实：button_translator.h 无实现、声明进编译）期望绿：rc=%d ===" + NL + "%s" + NL) % (rc_ok, out_ok))
    # ⚠ 面 A 的红**不能塞进 EXTRA**：EXTRA 会一直挂到本 case 结束，而"面 A 红就提前返回"意味着
    #   根本没做面 B —— 拿一个没做完的对照去判成败，等于把夹具自己的短路写进门的结果里。
    #   所以面 A 单独成一道读数（下面 main 里的 M6a），M6 只负责"删符号名必须红"这一件事。
    EXTRA["M6 依据列删符号名"] = True
    if rc_ok != 0:
        return True

    d2 = read(DOC)
    put(DOC, d2.replace(row_new, row_new.replace("ZhCnButtonTranslator::", ""), 1))
    return True


def m7():
    """整张表被 HTML 注释藏起来 ⇒ 表为空 ⇒ ROSTER-ANCHOR（不许当跳过）。"""
    s = read(DOC)
    row = row_of(WINHDR)
    lines = s.split("\n")
    head = None
    for i, line in enumerate(lines):
        # 必须用「| 文件 | 类别 |」整串定位：§1.2 的表头也是 `| 文件 | 现在位置 | …`，
        # 只认 `| 文件` 会把注释插到 §1.2 上（第一版就这么错过了一条红，见 §7 台账）。
        if line.startswith("| 文件 | 类别 |"):
            head = i
            break
    if row is None or head is None:
        return None
    lines.insert(head, "<!--")
    k = head + 1                                   # <!-- 之后就是原表头
    while k < len(lines) and lines[k].startswith("|"):
        k += 1
    if k == head + 1:
        return None                                # 表只有一行 ⇒ 夹具假设失效
    lines.insert(k, "-->")
    return put(DOC, "\n".join(lines))


def m8():
    """开关默认值从 OFF 翻成 ON ⇒「开关才编译」不再成立 ⇒ ROSTER-OPTION-STALE。"""
    s = read(CML)
    old = 'option(IDM_BUILD_UI_SNAPSHOT "Build offscreen UI snapshot tool" OFF)'
    if old not in s:
        return None
    return put(CML, s.replace(old, old[:-4] + 'ON)', 1))


def m9():
    """表头列名被改 ⇒ 门取列假设失效 ⇒ ROSTER-ANCHOR。"""
    s = read(DOC)
    if "\n| 文件 | 类别 |" not in s:
        return None
    return put(DOC, s.replace("\n| 文件 | 类别 |", "\n| 路径 | 活死 |", 1))



def m10():
    """表里把 include 点改成**真没有**的一条 ⇒ ROSTER-ZOMBIE-STALE。
    今天真实状态是「main_window.h 确实 include 了死头」，所以这一面测的是反向那侧的牙。"""
    s = read(DOC)
    row = row_of(GLSHDR)
    if row is None or "include" not in row:
        return None
    # ⚠ 不能"把 main_window.h 换成别的"：清掉 include 后那一行的依据列已经不含任何
    #   src/**.h 路径（SRC_RE 也吃不下 `{h,cpp}` 这种花括号 token），replace 等于空操作。
    #   这里反过来：**塞一条代码里并不存在的 include 点进依据列**，门必须认出它是瞎写的。
    mut = row.replace("清掉了原先那条 include（曾写作 `main_window.h:10`）",
                      "清掉了原先那条 include（曾写作 `main_window.h:10`），但仍被 `src/gui/settings_dialog.h` include")
    if mut == row:
        return None
    return put(DOC, s.replace(row, mut, 1))


def m11():
    """代码侧新增一条僵尸 include（task_detail_dialog.h include 死头），表里没登记 ⇒ ROSTER-ZOMBIE。
    ⚠ 这是**源码**变异不是文档变异：证明判据真在扫代码，而不是只比对文档自说自话。"""
    st = os.path.join(ROOT, "src", "gui", "task_detail_dialog.h")
    if st not in ORIG:
        snap(st)
    body = read(st)
    if "legacy/glass_effect.h" in body:
        return None                       # 已经存在 ⇒ 这条夹具的锚点没了，别做空变异
    mutated = body.replace("#include", '#include "legacy/glass_effect.h"' + chr(10) + "#include", 1)
    if mutated == body:
        return None
    return put(st, mutated)


def main():
    snap(DOC, CML)
    rc, out = gate()
    with io.open(LOG, "w", encoding="utf-8") as fh:
        fh.write("=== 基线（未变异，必须绿）===\n%s\n" % out)
    results.append(("BASELINE", rc == 0, "rc=%d" % rc,
                    "未变异必须绿，否则后面每条红的都不作数"))

    case("M1 删未编译行", "ROSTER-MISSING", 1, m1, "表漏登记 ⇒ 红")
    case("M2 未编译行写成活文件", "ROSTER-LIVENESS", 1, m2, "标错死活 ⇒ 红")
    case("M3 文件名不存在", "ROSTER-STALE", 1, m3, "名单过期 ⇒ 红")
    case("M4 类别没登记", "ROSTER-UNKNOWN-CATEGORY", 1, m4, "第三桶不能既不算过也不算败 ⇒ 红")
    case("M5 CMake 注释点名死文件", None, 0, m5, "两面：门仍绿 + naive 命令洗白该文件（对照印在日志）")
    case("M6a 登记真事实（前置）", None, 0, m6a,
         "面A：表+代码都成立 ⇒ 门必须绿；它若红，M6 的红就是蒙上的，不作数")
    case("M6 依据列删符号名", "ROSTER-BASIS", 1, m6,
         "调用点检查失去对象 ⇒ 红（第一版判据拿表里的字符串当搜索词，删掉后循环为空、照样绿）")
    case("M7 整表被 HTML 注释藏起", "ROSTER-ANCHOR", 1, m7, "藏表 ≠ 通过 ⇒ 红")
    case("M8 开关默认变 ON", "ROSTER-OPTION-STALE", 1, m8, "「开关才编译」失效 ⇒ 红")
    case("M9 表头列名被改", "ROSTER-ANCHOR", 1, m9, "取列假设失效 ⇒ 红")
    case("M10 表里 include 点写成没有的", "ROSTER-ZOMBIE-STALE", 1, m10,
         "登记与代码反向不符 ⇒ 红（清掉 include 后若忘了改表，就是这条拦）")
    case("M11 代码新增僵尸 include", "ROSTER-ZOMBIE", 1, m11,
         "源码变异：门真的在扫代码，不是只比文档")

    dirty = restore()
    rc_after, out_after = gate()

    bad = sum(1 for name, ok, got, note in results if not ok)
    if dirty or rc_after != 0:
        bad += 1
    lines = []
    lines.append("")
    lines.append("%-30s %-6s %-24s %s" % ("变异", "判定", "读数", "这道判据在防什么"))
    for name, ok, got, note in results:
        lines.append("%-30s %-6s %-24s %s" % (name, "OK" if ok else "BAD", got, note))
    lines.append("")
    lines.append("还原核对：%s" % ("干净（逐字一致）" if not dirty else "失败：%s" % dirty))
    lines.append("还原后再跑门：rc=%d ｜%s" % (rc_after, out_after.strip().splitlines()[-1]))
    lines.append("mutation_fixture_live_roster: %d/%d 道按预期（红/绿）%s"
                 % (len(results) - bad, len(results), "" if bad == 0 else "，有判据咬不动"))
    text = "\n".join(lines)
    print(text)
    print("日志（每次跑的完整 stdout）：%s" % LOG)
    with io.open(LOG, "a", encoding="utf-8") as fh:
        fh.write(text + "\n")
    sys.exit(0 if bad == 0 else 1)


main()
