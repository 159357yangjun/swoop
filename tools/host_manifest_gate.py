#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""native-messaging host 的安装器卫生门（只读，退码非 0 = 违约）。

为什么需要它：`install_host.ps1` 过去把**仓库里受跟踪的** host manifest 当安装目标就地改写
（`Set-Content … $JsonPath`）。后果有两条，都不是"风格问题"：
  · 跑一次安装脚本就把开发树弄脏，而改写的产物与现状等价（path 本来就该指同目录的 exe）；
  · PowerShell 5.1 的 `Set-Content -Encoding UTF8` 会写 **BOM**，JSON 解析器遇 BOM 直接报错
    ⇒ 谁真执行过那三行，就会当场造出一份浏览器读不了的 host manifest（错误形态是
      "Native messaging host not found / 无法解析"，很难往回想到编码）。

判据（都是"从盘上重算"，两边都不写死第二份）：
  A  安装器不得对受跟踪文件做写入（正则识别 cmdlet，注释里的散文提及不算）。
  B  host manifest 的 `allowed_origins` 必须等于由扩展 `manifest.json` 的 `key`
     现算出来的 ID —— 这条是**判别式**：把 json 里任意一个字符改掉就必须红。
     它替代了"手写两份 ID"的需求：ID 的唯一真源是 `key`，json 只是它的记录。
  C  Windows 上相对 path 合法（官方契约），但必须能解析到真实文件，且指向的文件名唯一。
  D  两份 JSON 必须是严格 UTF-8（无 BOM、可解析）。

用法：python3 tools/host_manifest_gate.py
"""
import base64
import hashlib
import json
import os
import subprocess
import re
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

SELFDIR = Path(__file__).resolve().parent
ROOT = SELFDIR.parent
EXT = ROOT / "browser-extension"
NMH = EXT / "native-messaging-host"
MANIFEST_JSON = EXT / "manifest.json"
HOST_JSON = NMH / "com.tencent.idm_next.json"
INSTALLER = NMH / "install_host.ps1"

# PowerShell 里会造成"写入这个文件"的 cmdlet / API。刻意要求它们带目标参数在同一行出现，
# 这样 README/注释里那句"运行 install_host.ps1 会自动改为…"不会被误判成写入。
WRITE_RE = re.compile(
    r"(Set-Content|Out-File|Add-Content|\bNew-ItemProperty\b|"
    r"\[System\.IO\.File\]::(WriteAllText|AppendAllText))",
    re.I,
)

failures = []
notes = []


def fail(line):
    failures.append(line)


def derive_extension_id(key_b64):
    """Chrome/Edge：ID = sha256(DER) 前 32 个 hex 位，逐位 0-f → a-p。"""
    der = base64.b64decode(key_b64)
    digest = hashlib.sha256(der).hexdigest()[:32]
    return "".join(chr(ord("a") + int(c, 16)) for c in digest)


def read_json_strict(path):
    """按**浏览器口径**读：UTF-8 且不吃 BOM。

    上一轮踩过的坑：这里用 `utf-8-sig` 解码，等于门自己把 BOM 吞掉 ——
    夹具往盘上写一份带 BOM 的 manifest，门却报绿，那条变异等于没测。
    真实故障形态正是"PowerShell 写出 BOM ⇒ 浏览器说 host not found"，
    所以门必须比浏览器更严格地失败，而不是更宽容。
    """
    raw = Path(path).read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raise ValueError("文件以 BOM(ef bb bf) 开头")
    return json.loads(raw.decode("utf-8"))


# ── A：安装器不得改写受跟踪的 manifest ─────────────────────────────
def check_installer_does_not_write_tracked():
    if not INSTALLER.exists():
        fail(f"A 跳过：找不到安装器 {INSTALLER}")
        return False
    text = INSTALLER.read_text(encoding="utf-8-sig")
    text = INSTALLER.read_text(encoding="utf-8-sig")
    hits = []
    for i, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        m = WRITE_RE.search(line)
        if not m:
            continue
        # 落笔目标可能在 cmdlet **之后任意位置**：真实写法是
        #   `$Json | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 $JsonPath`
        # 目标被 `-Encoding UTF8` 隔在后面。早先要求"紧邻 cmdlet"⇒ 扫到 0 处、门自证失明。
        # 因此这里对整行找目标（管道左侧的源不算目标）。
        if not re.search(r"(-Path\b|-LiteralPath\b|-Value\b|\$[A-Za-z][A-Za-z0-9_]*(Path|File)\b)",
                         line, re.I):
            continue
        hits.append((i, stripped))
    tracked_names = {"com.tencent.idm_next.json", "manifest.json"}
    # 写成临时文件/安装目录（不落这两个名字）是允许的：那正是"生成而非改写"的形态。
    bad = [h for h in hits if any(n in h[1] for n in tracked_names)
           or re.search(r"\$JsonPath\b|\$ManifestPath\b|\$HostJson\b", h[1])]
    if bad:
        for no, src in bad:
            fail(f"A 安装器在改写受跟踪 manifest：{INSTALLER.name}:{no}: {src[:120]}")
    else:
        notes.append(f"A 安装器不对受跟踪文件落笔（扫到 {len(hits)} 处写入调用："
                     f"{'; '.join('L%d' % n for n, _ in hits) or '无'}）")
    return True


def is_tracked(path):
    """git 是否跟踪这个文件——用相对路径查，避免绝对路径/斜杠差异导致静默 false。"""
    try:
        rel = path.relative_to(ROOT).as_posix()
        out = os.popen(f'cd "{ROOT}" && git ls-files --error-unmatch "{rel}" 2>nul').read()
        return rel in out.replace("\\", "/")
    except Exception:
        return False


# ── B：allowed_origins 必须由 key 现算对上 ─────────────────────────
def check_origin_matches_key():
    if not (MANIFEST_JSON.exists() and HOST_JSON.exists()):
        fail(f"B 跳过：缺 {MANIFEST_JSON.name} 或 {HOST_JSON.name}")
        return False
    try:
        ext = read_json_strict(MANIFEST_JSON)
        host = read_json_strict(HOST_JSON)
    except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as e:
        fail(f"B 跳过：JSON 读取失败（{e}）")
        return False
    key = ext.get("key")
    if not key:
        fail("B 扩展 manifest 没有 `key` ⇒ 装载后 ID 由 Chrome 随机分配，"
             "host 侧无法预填 allowed_origins（要么固定 key，要么安装时回填）")
        return True
    derived = derive_extension_id(key)
    origins = host.get("allowed_origins") or []
    want = f"chrome-extension://{derived}/"
    if not isinstance(origins, list) or not origins:
        fail(f"B allowed_origins 为空/类型错：{origins!r}")
    elif origins != [want]:
        fail(f"B allowed_origins 与 key 派生 ID 不符：表里={origins} 应为=[{want}]")
    else:
        notes.append(f"B allowed_origins == key 现算 ID（{derived}）")
    return True


# ── C：path 必须解析得到真实文件 ───────────────────────────────────
def check_path_resolves():
    if not HOST_JSON.exists():
        fail("C 跳过：缺 host manifest")
        return False
    try:
        host = read_json_strict(HOST_JSON)
    except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as e:
        fail(f"C 跳过：host manifest 读取失败（{e}）")
        return False
    p = host.get("path")
    if not p:
        fail("C host manifest 缺 path")
        return True
    resolved = Path(p) if os.path.isabs(p) else (NMH / p)
    if not resolved.exists():
        fail(f"C path 解析不到文件：{p} → {resolved}（Windows 允许相对 manifest 所在目录，"
             f"但目标必须在）")
        return True
    siblings = sorted(x.name for x in NMH.glob("*host*.exe"))
    if len(siblings) != 1:
        fail(f"C 宿主 exe 名字不唯一，装哪个看运气：{siblings}")
    else:
        notes.append(f"C path={p} → {resolved.name}（同级 *host*.exe 计数={len(siblings)}）")
    return True


# ── D：严格 UTF-8、无 BOM、可解析 ─────────────────────────────────
def check_strict_utf8():
    for f in (MANIFEST_JSON, HOST_JSON):
        if not f.exists():
            fail(f"D 跳过：缺 {f.name}")
            continue
        raw = f.read_bytes()
        if raw.startswith(b"\xef\xbb\xbf"):
            fail(f"D {f.name} 带 BOM（首 3 字节 ef bb bf）⇒ 浏览器按 JSON 读会报 Unexpected "
                 f"token，典型来源是 PowerShell 5.1 的 Set-Content -Encoding UTF8")
            continue
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as e:
            fail(f"D {f.name} 不是严格 UTF-8：{e}")
            continue
        try:
            json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as e:
            fail(f"D {f.name} UTF-8 但不是合法 JSON：{e}")
            continue
        notes.append(f"D {f.name} 无 BOM、UTF-8+JSON 可读（{len(raw)} 字节）")
    return True


def is_tracked(path):
    """git 是否跟踪这个文件——用相对路径查，避免绝对路径/斜杠差异导致静默 false。"""
    try:
        rel = path.relative_to(ROOT).as_posix()
        out = os.popen(f'cd "{ROOT}" && git ls-files --error-unmatch "{rel}" 2>nul').read()
        return rel in out.replace("\\", "/")
    except Exception:
        return False


# ── E：安装器必须真能被 Windows PowerShell 解析 ───────────────────
# 这条是 E2E 第一次真跑（2026-10-03 09:1x）撞出来的，不是想出来的：
# `install_host.ps1` 是 UTF-8 **无 BOM** + 中文注释 ⇒ PS 5.1 按 ANSI(GBK) 解码，
# 中文字节被切成假引号，字符串跨行、整个脚本 ParserError（TOKEN_ERRS=2），
# 后果是「用户照着 README 跑 install_host.ps1 = 直接失败」。
# D 判据管的是 manifest 别带 BOM；这里方向相反：**ps1 必须带 BOM**，两者不能混为一谈。
def check_installer_parses():
    if not INSTALLER.exists():
        fail("E 跳过：找不到安装器")
        return False
    raw = INSTALLER.read_bytes()
    has_bom = raw.startswith(b"\xef\xbb\xbf")
    non_ascii = sum(1 for c in raw.decode("utf-8", "replace") if ord(c) > 0x7f)
    if non_ascii and not has_bom:
        fail(f"E 安装器含 {non_ascii} 个非 ASCII 字符却无 BOM ⇒ PowerShell 5.1 按 GBK 解码"
             f"会当场 ParserError（实测 TOKEN_ERRS=2），README 教的这一步根本跑不起来")
        return True
    # 有 BOM 或纯 ASCII：调 PSParser 复核，别只信字节头
    ps = ("$t = Get-Content -Raw -LiteralPath '" + str(INSTALLER) + "'; $e=$null;"
          " [void][System.Management.Automation.PSParser]::Tokenize($t,[ref]$e);"
          " \"TOKEN_ERRS=\" + $e.Count")
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                            "-Command", ps], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        fail(f"E 跳过：无法调用 powershell 复核语法（{e}）")
        return False
    out = p.stdout.decode("gbk", errors="replace").strip()
    m = re.search(r"TOKEN_ERRS=(\d+)", out)
    if not m:
        fail(f"E 跳过：powershell 没回话（stdout={out[:120]!r} stderr={p.stderr.decode('gbk','replace')[:120]!r}）")
        return False
    n = int(m.group(1))
    if n != 0:
        fail(f"E PowerShell 解析安装器失败：TOKEN_ERRS={n}（BOM={has_bom}）")
    else:
        notes.append(f"E 安装器可被 PowerShell 解析（TOKEN_ERRS=0 BOM={int(has_bom)} "
                     f"非ASCII={non_ascii}）")
    return True


def main():
    checks = [check_installer_does_not_write_tracked,
              check_origin_matches_key,
              check_path_resolves,
              check_strict_utf8,
              check_installer_parses]
    ran = [c() for c in checks]
    skipped = len(checks) - sum(1 for r in ran if r)

    print(f"[HOST-MANIFEST-GATE] 仓根={ROOT} 断言块={len(checks)} "
          f"实际执行={sum(1 for r in ran if r)} 跳过={skipped}")
    for n in notes:
        print("    [OK]   " + n)
    for m in failures:
        print("    [FAIL] " + m)
    # 「跳过」不是通过：判据没跑起来就别给绿灯（上一轮 A 扫到 0 处就是这个形状）
    rc = 1 if (failures or skipped) else 0
    print(f"[HOST-MANIFEST-GATE] 结果：违规={len(failures)} 跳过={skipped} 退码={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
