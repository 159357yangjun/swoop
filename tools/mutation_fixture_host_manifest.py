#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""host_manifest_gate.py 的变异夹具：每条变异都必须让门红，且**红在具名判据块上**。

为什么需要它（上一轮踩过的坑）：修完安装器后 A 报「扫到 0 处写入调用」，
绿是因为正则什么都找不到了 —— 那不是"没违规"，是"量具瞎了"。
更糟的是 `$JsonPath` 这条规则一旦不再命中任何行，B/C/D 会被 `return False` 一起短路，
一把四判据的门退化成永绿。所以这里四面各配一条变异，外加一条阴性对照。

用法：python3 tools/mutation_fixture_host_manifest.py
"""
import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parent.parent
GATE = ROOT / "tools" / "host_manifest_gate.py"
NMH = ROOT / "browser-extension" / "native-messaging-host"
INSTALLER = NMH / "install_host.ps1"
HOST_JSON = NMH / "com.tencent.idm_next.json"
EXT_JSON = ROOT / "browser-extension" / "manifest.json"

NL = "\n"
results = []


def run_gate():
    p = subprocess.run([sys.executable, str(GATE)], capture_output=True)
    out = p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")
    return p.returncode, out


def fail_blocks(out):
    """返回红的判据块字母集合——只说「红了」不算证据，必须说红在哪一块。"""
    blocks = set()
    for line in out.splitlines():
        if "[FAIL]" in line:
            for b in "ABCD":
                if f"] {b} " in line or f"{b} 安装" in line or f"{b} allowed" in line \
                   or f"{b} path" in line or f"{b} manifest" in line or f"{b} 跳过" in line \
                   or f"{b} 扩展" in line or f"{b} host" in line:
                    blocks.add(b)
    return blocks


class Restore:
    """**字节级**备份。上一轮用 read_text/write_text 往返，把 LF 写成了 CRLF：
    盘上多 1 字节、`git status` 却报干净（autocrlf 的 smudge 看不见内容差异），
    等于夹具自己污染了受跟踪文件。所以这里只碰 bytes，且写入前不动任何换行。"""

    def __init__(self):
        self.originals = {}
        self.checked = {}

    def put(self, path, data):
        p = Path(path)
        if p not in self.originals:
            self.originals[p] = p.read_bytes()
            self.checked[p] = self.originals[p]
        blob = data if isinstance(data, bytes) else data.encode("utf-8")
        p.write_bytes(blob)

    def restore(self):
        for p, b in self.originals.items():
            p.write_bytes(b)
        self.originals.clear()


def case(name, expect_block, mutate, ctx):
    rc, out = run_gate()
    if rc != 0:
        results.append((name, "SKIP-基线已红", f"rc={rc}"))
        return
    ctx.put(*mutate())
    try:
        rc2, out2 = run_gate()
        blocks = fail_blocks(out2)
        ok = rc2 != 0 and expect_block in blocks
        results.append((name, "PASS" if ok else "FAIL",
                        f"rc={rc2} 红块={sorted(blocks)} 期望含={expect_block}"))
        if not ok:
            # 归因失败时把门的原话留下，否则只知道"没对上"而不知道它说了什么
            results.append((name + "/原始输出", "INFO",
                            " | ".join(l.strip() for l in out2.splitlines() if "[FAIL]" in l
                                       or "跳过" in l)[:300]))
    finally:
        ctx.restore()
        rc3, out3 = run_gate()
        if rc3 != 0:
            results.append((name + "/还原后", "FAIL", f"还原仍红 rc={rc3}"))
        # 还原必须逐字节回到原件——只看退码会漏掉 CRLF 那类污染
        for p, b in ctx.checked.items():
            now = Path(p).read_bytes()
            if now != b:
                results.append((name + "/字节对账", "FAIL",
                                f"{Path(p).name} 长度 {len(b)}→{len(now)}"))


def main():
    ctx = Restore()
    inst = INSTALLER.read_text(encoding="utf-8-sig")
    hj = HOST_JSON.read_bytes().decode("utf-8")

    # ── 阴性对照：写入发生在「生成目录的变量」上，模板一个字都不碰 ⇒ 必须绿 ──
    neg = inst.replace(
        "$ErrorActionPreference = \"Stop\"",
        "$ErrorActionPreference = \"Stop\"\n"
        "$GeneratedManifest = Join-Path $TargetDir \"generated.json\"\n"
        "# 下面这行落在生成物上，不是仓库模板：\n"
        "Set-Content -LiteralPath $GeneratedManifest -Value 'x'",
        1)
    ctx.put(INSTALLER, neg)
    try:
        rc, out = run_gate()
        results.append(("N 阴性对照（写临时生成文件）",
                        "PASS" if rc == 0 else "FAIL",
                        f"rc={rc}（期望 0；非 0 = 门会把正确的安装器也拦下）"))
    finally:
        ctx.restore()

    # ── M-A：改写受跟踪模板 ⇒ A 必须红 ──
    case("M-A 安装器改写仓库模板", "A",
         lambda: (INSTALLER, inst.replace(
             "$ErrorActionPreference = \"Stop\"",
             "$ErrorActionPreference = \"Stop\"\n"
             "$Json | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 $JsonPath", 1)),
         ctx)

    # ── M-B：allowed_origins 里改一个字符 ⇒ B 必须红（判别式）──
    bad_origin = hj.replace("fmnehlhldcoknecihnlbbcmhpaaapmbp", "fmnehlhldcoknecihnlbbcmhpaaapmbq", 1)
    assert bad_origin != hj, "夹具无效：替换没生效"
    case("M-B allowed_origins 与 key 不符", "B", lambda: (HOST_JSON, bad_origin), ctx)

    # ── M-C：path 指向不存在的 exe ⇒ C 必须红 ──
    bad_path = hj.replace("\"path\":\"idm-next-host.exe\"", "\"path\":\"no-such-host.exe\"", 1)
    assert bad_path != hj, "夹具无效：path 替换没生效"
    case("M-C path 解析不到文件", "C", lambda: (HOST_JSON, bad_path), ctx)

    # ── M-D：加 BOM ⇒ D 必须红（这正是旧安装器会造出的形态）──
    bom = "\ufeff" + hj
    case("M-D host manifest 带 BOM", "D", lambda: (HOST_JSON, bom), ctx)

    print(f"[FIXTURE] 夹具={len(results)} 条  仓根={ROOT}")
    bad = 0
    for name, verdict, detail in results:
        if verdict != "PASS":
            bad += 1
        print(f"    [{verdict}] {name} — {detail}")
    print(f"[FIXTURE] 结果：PASS={len(results)-bad} FAIL/SKIP={bad} 退码={1 if bad else 0}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
