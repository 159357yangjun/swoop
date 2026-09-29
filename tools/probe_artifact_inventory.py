#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用户主目录根上的探针产物清点器（只读，绝不删除）。

为什么要有这个脚本：本仓库的离屏探针一度把默认输出目录硬编码成
`C:/Users/yyyy/idm_shots`，配合"每轮临时换一个新 IDM_SHOT_DIR / 顺手 tee 一份构建日志"
的写法，在用户工作区根上攒出几十个 `idm*` 条目（2026-09-29 实测 47 个 / 191.5MB）。
清理之前必须先知道**每一项是什么、还有没有人引用**，否则"看着像垃圾"就可能删掉
UI 迭代的唯一历史对比图。所以这个脚本只做三件事：

1. 列清单：绝对路径 / 最后修改时间 / 体积 / 内容类别（截图目录、日志、压缩包）
2. 查引用：在**本仓库**的全部文本文件里 grep 每一项的名字，命中就标「不可删」
   并打印命中的文件 —— 没查引用之前，任何"可以删"的结论都是猜
3. 同类目去重候选：对 `idm_ui_vN` 这类同族目录，比较文件集合与逐文件 SHA-1，
   指出哪些字节相同、哪些不同（相同=同一份东西被换了名字重复渲染过）

用法（不装任何东西，仓库自带 python 即可）：
    python tools/probe_artifact_inventory.py            # 全量清单 + 引用 + 去重比较
    python tools/probe_artifact_inventory.py --count    # 只要"还剩几个 / 共多少 MB"

退出码：0 = 清点完成（不代表"可以删"）；2 = 主目录不存在或不可读。
"""
import hashlib
import io
import os
import sys

PREFIX = "idm"


def default_tmp_dir():
    import datetime
    return os.path.join(os.environ.get("TEMP", home_root()), "idm-next-probes",
                        datetime.date.today().isoformat())
TEXT_EXTS = (".md", ".py", ".cpp", ".h", ".c", ".ps1", ".sh", ".yml", ".nsi",
             ".txt", ".json", ".ini", ".html", ".js", ".bat", ".cmake")
SKIP_PARTS = (".git", "build", "installer", "third_party", "dist")
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def home_root():
    return os.path.expanduser("~")


def entry_size_mb(path):
    if os.path.isfile(path):
        return os.path.getsize(path) / 1048576.0
    total = 0
    for _r, _d, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(_r, f))
            except OSError:
                pass
    return total / 1048576.0


def classify(path):
    if os.path.isfile(path):
        low = path.lower()
        if low.endswith(".zip"):
            return "压缩包"
        if low.endswith(".txt") or low.endswith(".log"):
            return "日志"
        return "其它文件"
    png = 0
    other = 0
    for _r, _d, files in os.walk(path):
        for f in files:
            if f.lower().endswith(".png"):
                png += 1
            else:
                other += 1
    if png and not other:
        return "截图目录(%d 张)" % png
    if png:
        return "混合目录(%d 图/%d 其它)" % (png, other)
    return "其它目录"


def repo_texts():
    for root, dirs, files in os.walk(REPO_ROOT):
        rel_parts = set(os.path.relpath(root, REPO_ROOT).split(os.sep))
        dirs[:] = [d for d in dirs if d not in rel_parts or True]
        if any(p in rel_parts for p in SKIP_PARTS):
            continue
        for f in files:
            if not f.endswith(TEXT_EXTS):
                continue
            p = os.path.join(root, f)
            try:
                yield p, io.open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue


def reference_map(names):
    hits = dict((n, []) for n in names)
    scanned = 0
    texts = list(repo_texts())
    for path, body in texts:
        scanned += 1
        rel = os.path.relpath(path, REPO_ROOT)
        # 清点器与它的输出文档本身就会写出这些名字（那正是它们在说的东西）。
        # 把这种自引用算成依赖，就会永远把每个条目都标成"不可删"，等于查了个寂寞。
        low = os.path.normpath(rel).lower()
        if low.endswith("probe_artifact_inventory.py") or "probe-artifact-inventory" in low:
            continue
        for n in names:
            if n in body:
                hits[n].append(rel)
    return hits, scanned


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def fingerprint_dir(path):
    """目录内容指纹：文件名 -> 该文件 SHA-1。忽略绝对路径，便于跨目录比较。"""
    out = {}
    for root, _d, files in os.walk(path):
        for f in files:
            fp = os.path.join(root, f)
            try:
                out[f] = sha1_of(fp)
            except OSError:
                out[f] = "unreadable"
    return out


def dedup_group(entries):
    """按名字族（idm_ui_vN / idm_shots*）分组并比较内容。"""
    groups = {}
    for name in entries:
        if not os.path.isdir(os.path.join(home_root(), name)):
            continue                      # 日志文件不参与"同族内容比较"
        if name.startswith("idm_ui"):
            key = "idm_ui*"
        elif name.startswith("idm_shots"):
            key = "idm_shots*"
        else:
            continue
        groups.setdefault(key, []).append(name)
    for key, members in sorted(groups.items()):
        print("\n### 同族比较 %s （%d 个目录）" % (key, len(members)))
        fps = {}
        for m in members:
            fps[m] = fingerprint_dir(os.path.join(home_root(), m))
        for m in sorted(members, key=lambda x: -len(fps[x])):
            names = set(fps[m])
            print("  %-16s 文件 %2d 个，唯一名 %2d 个" % (m, len(names), len(names)))
        identical = []
        for i in range(len(members)):
            for j in range(i + 1, len(members)):
                a, b = members[i], members[j]
                if fps[a] and fps[a] == fps[b]:
                    identical.append((a, b))
        if identical:
            print("  逐文件 SHA-1 完全相同的目录对：")
            for a, b in identical:
                print("    %s == %s" % (a, b))
        else:
            print("  没有任何两个目录是逐字节相同的（各自内容不同，不能按名字判重复）")


def match_against(target_dir):
    """把"当前代码刚渲染出来的那一套"与主目录根上每个同族目录比：
       共有多少同名文件、其中多少逐字节相同。用来判断哪一套对应现在的界面。"""
    if not os.path.isdir(target_dir):
        print("不是目录：%s" % target_dir)
        return 2
    t = fingerprint_dir(target_dir)
    print("对照源：%s（%d 个文件）" % (target_dir, len(t)))
    home = home_root()
    rows = []
    for n in sorted(x for x in os.listdir(home)
                    if x.lower().startswith(PREFIX) and os.path.isdir(os.path.join(home, x))):
        f = fingerprint_dir(os.path.join(home, n))
        if not f:
            continue
        common = set(t) & set(f)
        same = sum(1 for k in common if t[k] == f[k])
        rows.append((len(common), same, n, len(f)))
    print("\n| 目录 | 文件数 | 与对照同名 | 其中逐字节相同 |")
    print("| --- | --- | --- | --- |")
    for common, same, n, total in sorted(rows, reverse=True):
        print("| %s | %d | %d | %d |" % (n, total, common, same))
    print("\n⚠️ 同名但字节不同不等于界面不同：本仓库的渲染对照早就记过，"
          "同一份代码连跑两次，部分面板也会有噪声差异。")
    return 0


def main():
    home = home_root()
    if not os.path.isdir(home):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print("读不到主目录：%s" % home)
        return 2
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        entries = sorted(n for n in os.listdir(home) if n.lower().startswith(PREFIX))
    except OSError:
        print("读不到主目录内容")
        return 2

    total = 0.0
    if "--match" in sys.argv:
        i = sys.argv.index("--match")
        return match_against(sys.argv[i + 1] if i + 1 < len(sys.argv) else default_tmp_dir())
    if "--count" in sys.argv:
        for n in entries:
            total += entry_size_mb(os.path.join(home, n))
        print("COUNT=%d TOTAL_MB=%.1f HOME=%s" % (len(entries), total, home))
        return 0

    print("主目录：%s" % home)
    print("`%s*` 条目 %d 个\n" % (PREFIX, len(entries)))

    refs, scanned = reference_map(entries)
    print("仓库内扫描文本文件 %d 个，其中被引用到的条目 %d 个\n"
          % (scanned, sum(1 for v in refs.values() if v)))

    print("| 条目 | 最后修改 | 体积MB | 类别 | 仓库引用 |")
    print("| --- | --- | --- | --- | --- |")
    for n in entries:
        p = os.path.join(home, n)
        import datetime
        mt = datetime.datetime.fromtimestamp(os.path.getmtime(p)).strftime("%Y-%m-%d %H:%M")
        sz = entry_size_mb(p)
        total += sz
        r = sorted(set(refs.get(n) or []))
        if not r:
            verdict = "无引用"
        else:
            # 区分"删了会坏事"和"某句话会变旧"：只有脚本/配置里的引用才是依赖
            hard = [x for x in r if x.lower().endswith(
                (".py", ".ps1", ".sh", ".yml", ".cmake", ".nsi", ".js", ".json", ".bat"))]
            if hard:
                verdict = "**不可删：被脚本使用**（" + ", ".join(hard[:3]) + "）"
            else:
                verdict = "仅文档/注释提及（删了不崩，但那句话会失效；" + ", ".join(r[:2]) + "）"
        print("| %s | %s | %.1f | %s | %s |" % (n, mt, sz, classify(p), verdict))
    print("\n合计 %.1f MB" % total)

    dedup_group(entries)
    print("\n本脚本不删除任何文件；删除需要用户明确点头。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
