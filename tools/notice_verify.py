#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核对 NOTICE.md 里每一个"算出来的数"——从盘上/git 重算，不一致就 exit 1。

设计约束（都是踩过才写的）：
  · 本工具只读：不写 NOTICE.md、不改任何文件、不联网。要联网才能核的那两行（徽章、公开性）
    在 NOTICE §2 里明确标了"⚠️ 要联网"，不进这里，因此这里核不到的一切仍然挂在"未验"里。
  · 数据根 = 本文件所在仓（SELFDIR 与 ROOT 同一个主），不引用别的仓的文件；
    NOTICE §2 的 L7 那行指向 `D:/visual studio/lianxi/swoop/idm`，它是**记录**，本工具不读它。
  · 期望值一律从 NOTICE.md 里解析，实测值一律从 git/盘上算——两边都不许写死第二份。
  · 每个判据块用自己的局部计数，`bad += ` 放到最后；一块红不许把别的块说成嫌疑。
用法：python3 tools/notice_verify.py            （= --verify）
"""
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")  # GBK 控制台下中文诊断等于没诊断

SELFDIR = Path(__file__).resolve().parent
ROOT = SELFDIR.parent
NOTICE = ROOT / "NOTICE.md"
LIC_NAME = re.compile(r"(^|/)(LICENSE|LICENCE|COPYING|UNLICENSE)(\.[A-Za-z]+)?$", re.I)

bad_total = 0
lines_out = []


def git(*args):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.stdout.strip(), r.returncode


def norm(s):
    return " ".join(s.split())


def say(ok, name, detail):
    global bad_total
    bad_total += 0 if ok else 1
    lines_out.append(f'{"[OK]  " if ok else "[FAIL]"} {name}｜{detail}')


def tracked_paths():
    out, rc = git("ls-files", "-z")
    if rc != 0:
        return None
    return [p for p in out.split("\0") if p]


def size_of(rel):
    p = ROOT / rel
    return p.stat().st_size if p.is_file() else None


def block(name, checks):
    """checks: list of (ok, label, detail) —— 本块自己数自己的红。"""
    global bad_total
    b = 0
    lines_out.append(f"--- {name} ---")
    for ok, label, detail in checks:
        b += 0 if ok else 1
        lines_out.append(f'{"[OK]  " if ok else "[FAIL]"} {label}｜{detail}')
    bad_total += b
    return b


# ───────────────────────── 1. §1 范围表：逐目录文件数与字节数 ─────────────────────────
doc = NOTICE.read_text(encoding="utf-8")
paths = tracked_paths()
ck = []
row_re = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*(\d+)\s*\|\s*([\d,]+)\s*\|", re.M)
dir_rows = row_re.findall(doc)
claimed_root = None
m = re.search(r"^\|\s*仓根散装文件\s*\|\s*(\d+)\s*\|", doc, re.M)
if m:
    claimed_root = int(m.group(1))
mc = re.search(r"合计[^|]*\|\s*\*\*(\d+)\*\*", doc)
claimed_total = int(mc.group(1)) if mc else None

sum_files = sum_bytes = 0
for rel, cfiles, cbytes in dir_rows:
    rel = rel.rstrip("/")
    sel = [p for p in paths if p == rel or p.startswith(rel + "/")]
    n = len(sel)
    by = sum(size_of(p) or 0 for p in sel)
    sum_files += n
    sum_bytes += by
    ck.append((str(n) == cfiles.replace(",", ""), f"{rel}/ 文件数", f"盘上={n} 文中={cfiles}"))
    ck.append((str(by) == cbytes.replace(",", ""), f"{rel}/ 字节数", f"盘上={by} 文中={cbytes}"))

root_files = [p for p in paths if "/" not in p]
ck.append((claimed_root is not None and len(root_files) == claimed_root,
           "仓根散装文件数", f"盘上={len(root_files)} 文中={claimed_root}"))
ck.append((claimed_total == len(paths), "合计文件数", f"git ls-files={len(paths)} 文中={claimed_total}"))
ck.append((sum_files + len(root_files) == len(paths),
           "恒等式 Σ各目录+仓根==总数", f"{sum_files}+{len(root_files)}=={len(paths)}"))

# 表里列的目录 vs 盘上真实存在的顶层目录：对称差必须为空
dirs_doc = {rel.rstrip("/") for rel, _, _ in dir_rows}
dirs_disk = {p.split("/")[0] for p in paths if "/" in p}
theirs = sorted(dirs_disk - dirs_doc)
mine = sorted(dirs_doc - dirs_disk)
ck.append((not theirs and not mine, "目录覆盖对称差",
           f"盘上有而文中无={len(theirs)}{theirs or ''} 文中独有={len(mine)}{mine or ''}"))
block("§1 范围表", ck)

# ───────────────────────── 2. 全文 `路径:行 含「原文」` 证据锚点 ─────────────────────────
tok_re = re.compile(r"([^\s`;，,|；、」]+?):(\d+) 含「([^」]+)」")
tokens = tok_re.findall(doc)
ck = []
missing_anchor = 0
for rel, ln, needle in tokens:
    p = ROOT / rel
    if not p.is_file():
        ck.append((False, rel, f"文件不存在（第 {ln} 行的引用悬空）"))
        missing_anchor += 1
        continue
    ls = p.read_text(encoding="utf-8", errors="replace").splitlines()
    i = int(ln) - 1
    if not (0 <= i < len(ls)):
        ck.append((False, f"{rel}:{ln}", f"该文件只有 {len(ls)} 行"))
        continue
    ok = norm(needle) in norm(ls[i])
    ck.append((ok, f"{rel}:{ln}",
               (f"含「{needle[:48]}」" if ok else f"第 {ln} 行实际是「{norm(ls[i])[:70]}」")))
ck.append((len(tokens) >= 12, "证据锚点条数下限",
           f"本轮解析到 {len(tokens)} 条（下限 12 是规则，不是实测）"))
block("§2/§3/§4 证据锚点", ck)

# ───────────────────────── 3. §3 第三方表：版本必须有本行证据撑 ─────────────────────────
sec3 = doc.split("## 3.")[1].split("## 4.")[0] if "## 3." in doc else ""
row3 = re.compile(r"^\|\s*([^|]*?)\s*\|\s*([0-9][\w.\-]*)\s*\|([^|]*)\|", re.M)
ck = []
triplet = []
for comp, ver, evid in row3.findall(sec3):
    toks = tok_re.findall(evid)
    if not toks:
        ck.append((False, comp, f"版本 {ver} 没有任何『路径:行 含「」』证据"))
        continue
    needles = [n for _, _, n in toks]
    if any(ver in n for n in needles):
        ck.append((True, comp, f"版本 {ver} 出现在本行证据原文里（{len(toks)} 条锚点）"))
    elif len(toks) == 3 and all(re.search(r"(\d+)$", n.strip()) for n in needles):
        joined = ".".join(re.search(r"(\d+)$", n.strip()).group(1) for n in needles)
        ck.append((joined == ver, comp, f"三段合成 {joined} == 文中 {ver}？（走的是拆分证据这条支路，具名登记）"))
        triplet.append(comp)
    else:
        ck.append((False, comp, f"版本 {ver} 在本行 {len(toks)} 条证据原文里都找不到"))
ck.append((len(triplet) <= 1, "拆分证据行数上限",
           f"本轮 {len(triplet)} 行走拆分支路：{triplet or '无'}（只许 zstd 这一类，多出来就是要改文档）"))
block("§3 第三方版本对账", ck)

# ───────────────────────── 4. §2 许可证事实（离线可核的那几条） ─────────────────────────
ck = []
lic_tracked = [p for p in paths if LIC_NAME.search(p)]
ck.append((not lic_tracked, "L1 master 许可证文件计数",
           f"命中 {len(lic_tracked)} 个{lic_tracked or '（文中声明 =0）'}"))

sha_main, rc = git("rev-parse", "origin/main:LICENSE")
ck.append((rc == 0 and sha_main in doc, "L3 origin/main:LICENSE 的 blob sha",
           f"实测={sha_main if rc == 0 else '(取不到)'} 文中是否含此串={sha_main in doc if rc == 0 else False}"))
sz_main, rc = git("cat-file", "-s", "origin/main:LICENSE")
ck.append((rc == 0 and sz_main in doc, "L3 该文件大小", f"实测={sz_main} 文中是否含此数={sz_main in doc}"))
body, rc = git("show", "origin/main:LICENSE")
ls1 = body.splitlines()
need1 = len(ls1) >= 3 and ls1[0].strip() == "MIT License" and ls1[2].strip() in doc
ck.append((need1, "L4 首行与第 3 行",
           f"第1行={ls1[0].strip() if ls1 else '(空)'} 第3行={ls1[2].strip() if len(ls1) > 2 else '(缺)'}（第3行须原样出现在文中）"))
head, rc = git("symbolic-ref", "refs/remotes/origin/HEAD")
ck.append((rc == 0 and head.endswith("/main") and "origin/main" in doc,
           "L5 徽章归因于默认分支", f"symbolic-ref={head}"))
n_lic_hist, _ = git("log", "--all", "--format=%h", "--", "LICENSE")
lic_hist = n_lic_hist.split()
m6 = re.search(r"^\| L6 \|.*$", doc, re.M)
six_ref = re.findall(r"`([0-9a-f]{7,}) ", m6.group(0)) if m6 else []
ck.append((len(lic_hist) == 1 and bool(six_ref) and lic_hist[0].startswith(six_ref[0][:7]),
           "L6 历史改过根 LICENSE 的提交数与所指提交",
           f"实测={len(lic_hist)} 条 {lic_hist}；文中该行点名={six_ref or '(无)'}"))
first_add, rc = git("log", "--diff-filter=A", "--format=%h %ad", "--date=short", "--", "browser-extension")
fa = first_add.splitlines()[-1] if first_add else ""
ck.append((bool(fa) and fa.split()[0] in doc and fa.split()[1] in doc,
           "L9 browser-extension 首次入库", f"{fa}（短 sha 与日期都要在文中）"))
block("§2 许可证事实（离线）", ck)

# ───────────────────────── 5. 被忽略的上游文档目录 / 扩展 DLL / 运行时后端 ─────────────────────────
ck = []
docs_dir = ROOT / "third_party/libcurl/docs"
rec_files = sorted(p for p in docs_dir.rglob("*") if p.is_file()) if docs_dir.is_dir() else []
top_files = sorted(p for p in docs_dir.iterdir() if p.is_file()) if docs_dir.is_dir() else []
top_dirs = sorted(p for p in docs_dir.iterdir() if p.is_dir()) if docs_dir.is_dir() else []
mrec = re.search(r"递归有 \*\*(\d+)\*\* 个文件（顶层是 (\d+) 个文件 \+ (\d+) 个子目录", doc)
shape = mrec.groups() if mrec else "(文中解析不到)"
ck.append((mrec is not None
           and int(mrec.group(1)) == len(rec_files)
           and int(mrec.group(2)) == len(top_files)
           and int(mrec.group(3)) == len(top_dirs),
           "上游 docs 文件数（递归 + 顶层拆分）",
           f"盘上 递归={len(rec_files)} 顶层文件={len(top_files)} 子目录={len(top_dirs)}｜文中={shape}"))
lic_any = [str(p.relative_to(ROOT)).replace("\\", "/")
           for p in (ROOT / "third_party/libcurl").rglob("*")
           if p.is_file() and LIC_NAME.search(p.name)]
_, rc = git("check-ignore", "-q", "third_party/libcurl/docs")
ck.append((not lic_any and rc == 0,
           "third_party/libcurl 递归内的许可证全文 + docs 被忽略",
           f"许可名文件={len(lic_any)} 个{lic_any or ''}；check-ignore rc={rc}"))

ext_dlls = [p for p in paths if p.startswith("browser-extension/") and p.endswith(".dll")]
ck.append((str(len(ext_dlls)) in doc,
           "扩展目录被跟踪的 DLL 数", f"实测={len(ext_dlls)} 个：{[os.path.basename(x) for x in ext_dlls]}"))
be = [p for p in paths if p.startswith("browser-extension/")]
be_bytes = sum(size_of(p) or 0 for p in be)
mb = re.search(r"入库数 (\d+) 字节", doc)
ck.append((mb is not None and int(mb.group(1)) == be_bytes,
           "扩展入库字节数（与磁盘数分开）", f"入库={be_bytes}（{len(be)} 个文件）文中={mb.group(1) if mb else '(解析不到)'}"))
disk_bytes = sum(size_of(os.path.relpath(f, ROOT)) for f in (ROOT / "browser-extension").rglob("*") if f.is_file())
ck.append((str(disk_bytes) in doc, "扩展磁盘字节数", f"实测={disk_bytes}（文中须出现这个精确数，禁止用『约 16MB』冒充）"))

hits = [p for p in paths if re.search(r"aria2|yt-dlp|yt_dlp|ffmpeg", p, re.I)]
bin_hits = [p for p in hits if re.search(r"\.(exe|dll|a|lib|so|dylib)$", p, re.I)]
ck.append((len(hits) == 4 and all(h.startswith("src/protocols/") for h in hits) and not bin_hits,
           "§5 运行时后端", f"命中 {len(hits)} 个且全在 src/protocols/；可执行体 {len(bin_hits)} 个；"
           f"yt-dlp 命中 {len([h for h in hits if 'yt' in h.lower()])} 个"))
block("§2/§3/§5 盘上对账", ck)

# ───────────────────────── 汇总 ─────────────────────────
print("\n".join(lines_out))
print("--- 汇总 ---")
tool_sha = hashlib.sha256((SELFDIR / "notice_verify.py").read_bytes()).hexdigest()[:12]
doc_sha = hashlib.sha256(NOTICE.read_bytes()).hexdigest()[:12]
print(f"[NOTICE_VERIFY] rows§1={len(dir_rows)} anchors={len(tokens)} "
      f"3p_rows={len(sec3.splitlines()) and sum(1 for _ in row3.finditer(sec3))} "
      f"FAIL={bad_total} tool={tool_sha}/{sum(1 for _ in open(__file__, encoding='utf-8'))}行 "
      f"notice_sha12={doc_sha}")
print("未由本工具核（不得当成已验）：GitHub 徽章/公开性（要联网）、NOTICE §2 的 L7（本仓之外的工作树）、")
print("                      徽章所反映的**当下**默认分支内容（本地 origin/main 可能过期：git fetch 到 github.com:443 超时）。")
sys.exit(1 if bad_total else 0)
