#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分层守门：扫 src/** 的引号 include，任何「低层反向依赖高层」即刻退出非 0。

层号取自 docs/ARCHITECTURE.md §2 的图与 §2.1 的表（不是本工具新发明的规矩）：
    L0 core | L1 storage · web · utils · model | L2 app | L3 protocols | L4 gui
    L5 入口（src/main.cpp · src/cli · src/host）
铁律：只允许「大号层 → 小号层」。

三处判断需要说明依据，改这个表的人必须同时改文档：
1. `src/model/` 在 §2 的图里**没有列**（那是阶段 1 才从 gui/ 搬出来的目录）。本工具按
   §1.2/§3.2 的归位意图把它记成 L1：它只做 Qt ItemView 的数据映射，不含 widget，
   因此只准依赖 L0/L1，**禁止**依赖 gui/app。这条就是本轮要修的 `task_list_model.cpp`。
2. 反向依赖里有两对是**文档明文许可**的，登记在 SANCTIONED：app(L2)→gui(L4)
   （§2 括号）与 app(L2)→protocols(L3)（§2.1「允许依赖 L3（类型）」）。
   纯按「大号→小号」的字面判会把 app→protocols 的 5 条也报成违规，那是我的数字比较
   比文档更严，不是真越界 —— 许可对会被 --count-allowed 逐条打印，不静默吞掉。
3. 入口层记 L5（最高），§2.1 只说它们「不互相依赖」，那是另一条规矩，本工具不判。

include 的解析顺序照编译器：同目录 → CMake 的 9 个 target_include_directories
（src/core、src/storage、src/gui、src/model、src/web、src/app、src/protocols、
src/cli、src/utils）→ src/ 根。解析不到就当外部头（Qt/系统/vendored）跳过。

用法：python tools/lint_layering.py [--count-allowed]
退出码：0 = 无越界；1 = 有越界（逐条打印）；2 = 工具自己的前置不成立（src/ 找不到等）。
"""

import os
import re
import sys

# 根目录从 __file__ 推，不靠 cwd：cwd 跑错地方时，相对路径版本会「扫到 0 个文件」而退 0，
# 那是一条永远绿的假门（本会话已经在别处踩过「零命中当成没做/没坏」的同类亏）。
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

LAYERS = {
    "core": 0,
    "storage": 1,
    "web": 1,
    "utils": 1,
    "model": 1,
    "app": 2,
    "protocols": 3,
    "gui": 4,
    "cli": 5,
    "host": 5,
}

# (源目录, 目标目录) → 文档依据。命中时不报违规，但会在 --count-allowed 里逐条打出来，
# 新增一条时数字会变 —— 读数的人看得见，别让它变成无人认领的豁免名单。
SANCTIONED = {
    ("app", "gui"): "docs/ARCHITECTURE.md §2 括号：app 层允许持有 gui 引用",
    ("app", "protocols"): "docs/ARCHITECTURE.md §2.1 L2 行：允许依赖 L3（类型）",
}
# 许可对必须有上限，否则 --count-allowed 只是给人看的清单、没有牙：
# 今天新增第 6 条 app→gui，门照样绿，而读者不会去数。上限取**当前实测条数**，
# 超出即红 —— 要么那条是真越界（按 (a) 那样改方向），要么得同时改 §2/§2.1 并说明理由。
MAX_ALLOWED = {("app", "gui"): 5, ("app", "protocols"): 5}
# ⚠ 文档张力（本工具按上表放行，但必须留字）：§2.1 的 L2 行同时写着「禁止继承
# QWidget/持有 UI 树」，而实际 5 条 app→gui 里 app/appearance_controller.cpp:12 依赖的
# gui/sidebar_panel.h 就是 QWidget 子类。两句不一致时本工具取了更宽的那句，
# 这条张力属已知未修项，不是本轮改动引入的。

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def force_utf8():
    """Git Bash / pwsh 的控制台默认不是 UTF-8，中文输出会变成乱码——乱码的日志没法当证据贴。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def die(msg, code=2):
    sys.stderr.write("[layer-gate] 前置错误：%s\n" % msg)
    sys.exit(code)


def layer_of(rel_path):
    """相对 src/ 的路径：<dir>/x.cpp → <dir>；main.cpp 这种根文件记成入口层。"""
    parts = rel_path.replace("\\", "/").split("/")
    if len(parts) < 2:
        return "entry", 5
    d = parts[0]
    if d not in LAYERS:
        return d, None
    return d, LAYERS[d]


def collect_headers():
    """返回 {相对 src/ 的路径: True}，以及 {头文件名: [路径…]} 用于同目录/包含目录解析。"""
    by_name = {}
    all_paths = set()
    for root, dirs, files in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in ("build", "build_check")]
        for fn in files:
            if not fn.endswith((".h", ".hpp")):
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, SRC).replace("\\", "/")
            all_paths.add(rel)
            by_name.setdefault(fn, []).append(rel)
    return all_paths, by_name


def resolve(inc, includer_rel, all_paths, by_name, include_dirs):
    """照编译器顺序解析成相对 src/ 的路径；解析不到返回 None。"""
    includer_dir = os.path.dirname(includer_rel)
    cand = []
    if includer_dir:
        cand.append(os.path.normpath(os.path.join(includer_dir, inc)).replace("\\", "/"))
    for d in include_dirs:
        cand.append(os.path.normpath(os.path.join(d, inc)).replace("\\", "/"))
    cand.append(os.path.normpath(inc).replace("\\", "/"))
    for c in cand:
        if c in all_paths:
            return c
    # 最后一档：按唯一文件名匹配（本项目里有 "app_icons.h" 这种裸名 include）
    base = os.path.basename(inc)
    hits = by_name.get(base, [])
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        return ("AMBIGUOUS", hits)
    return None


def source_files():
    out = []
    for root, dirs, files in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in ("build", "build_check")]
        for fn in files:
            if fn.endswith((".c", ".cpp", ".h", ".hpp")):
                out.append(os.path.join(root, fn))
    if not out:
        die("在 %s 下找不到任何源文件（工具根路径推错了？）" % SRC)
    return out


def summarize_pairs(per_pair_hits):
    return " · ".join("%s→%s ×%d/%s" % (p[0], p[1], len(sorted(set(h))),
                                        MAX_ALLOWED.get(p, "未登记"))
                      for p, h in sorted(per_pair_hits.items())) or "无"


def main(argv):
    force_utf8()
    count_allowed = "--count-allowed" in argv
    if not os.path.isdir(SRC):
        die("没有 %s 目录（脚本应当留在 <仓库根>/tools/ 下）" % SRC)

    all_paths, by_name = collect_headers()
    include_dirs = sorted({os.path.dirname(p) for p in all_paths})
    # CMake 只把这 9 个目录加进 include path，多余的目录名会让解析比编译器更宽
    cmake_dirs = [d for d in include_dirs if d in LAYERS]

    violations = []
    allowed = []
    scanned = 0
    for path in source_files():
        rel = os.path.relpath(path, SRC).replace("\\", "/")
        src_dir, src_layer = layer_of(rel)
        if src_layer is None:
            die("目录 src/%s/ 没有层号，请先在 LAYERS 里补（并同步 docs/ARCHITECTURE.md）" % src_dir)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError as exc:
            die("读不了 %s：%s" % (path, exc))
        for no, line in enumerate(lines, 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            scanned += 1
            tgt = resolve(m.group(1), rel, all_paths, by_name, cmake_dirs)
            if isinstance(tgt, tuple):
                die("头文件名有歧义 %s：%s —— 解析规则不能再靠裸文件名，请修工具"
                    % (m.group(1), tgt[1]))
            if not tgt:
                continue
            tgt_dir, tgt_layer = layer_of(tgt)
            if tgt_layer is None:
                die("src/%s/ 没有层号（被 %s 引用）" % (tgt_dir, rel))
            if tgt_layer <= src_layer:
                continue
            pair = (src_dir, tgt_dir)
            if pair in SANCTIONED:
                allowed.append((rel, no, m.group(1), tgt, src_layer, tgt_layer))
                continue
            violations.append((rel, no, m.group(1), tgt, src_dir, src_layer, tgt_dir, tgt_layer))

    # 许可对的条数与上限核对（这一步让 --count-allowed 从"给人看的清单"变成"有牙的判据"）
    per_pair_hits = {}
    for rel, no, inc, tgt, sl, tl in allowed:
        # 键用目录名而不是层号：SANCTIONED / MAX_ALLOWED 都是按 (源目录, 目标目录) 登记的，
        # 用层号查会查不到（第一版就错在这里，把 5 条许可全报成 OVERCAP）。
        pair = (os.path.dirname(rel).replace("\\", "/"), os.path.dirname(tgt).replace("\\", "/"))
        per_pair_hits.setdefault(pair, []).append("%s:%d" % (rel, no))
    over = []
    for pair, hits in sorted(per_pair_hits.items()):
        uniq = sorted(set(hits))
        cap = MAX_ALLOWED.get((pair[0], pair[1]))
        if cap is None:
            over.append((pair, len(uniq), 0))
        elif len(uniq) > cap:
            over.append((pair, len(uniq), cap))

    if count_allowed and allowed:
        print("[layer-gate] 文档许可的反向引用（不判违规，但受上限约束）：")
        for pair, hits in sorted(per_pair_hits.items()):
            uniq = sorted(set(hits))
            src_d, tgt_d = pair
            print("  %s → %s：%d 条（去重后 %d 条，上限 %s）依据：%s"
                  % (src_d, tgt_d, len(hits), len(uniq),
                     MAX_ALLOWED.get(pair, "未登记"),
                     SANCTIONED.get(pair, "⚠ 文档里找不到依据")))
            for h in uniq:
                print("      %s" % h)

    if not violations and not over:
        print("[layer-gate] 通过：根=%s 扫了 %d 条引号 include，反向依赖 0 条；"
              "文档许可的反向引用 %d 条（%s；明细见 --count-allowed）。"
              % (ROOT.replace("\\", "/"), scanned, len(allowed), summarize_pairs(per_pair_hits)))
        return 0

    if violations:
        print("[layer-gate] 发现 %d 条「低层反向依赖高层」：" % len(violations))
        for rel, no, inc, tgt, sd, sl, td, tl in violations:
            print("  VIOLATION %s:%d  include \"%s\" → %s  (L%d %s → L%d %s)"
                  % (rel, no, inc, tgt, sl, sd, tl, td))
    for pair, n, cap in over:
        print("  OVERCAP %s → %s：%d 条，上限 %d 条 —— 许可对被扩大了。"
              "要么这条是真越界（照 model→gui 那样改方向：低层只发数据），"
              "要么先改 docs/ARCHITECTURE.md §2/§2.1 并说明为什么。"
              % (pair[0], pair[1], n, cap))
    print("[layer-gate] 修法：低层只发数据（键/枚举），高层查表换实现 —— "
          "别把高层的头往下搬来让门闭嘴。")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
