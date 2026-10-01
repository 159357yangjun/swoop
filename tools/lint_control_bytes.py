#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""源码/文档里不许出现**裸控制字节**（NUL、0x01、SOH/STX 之类）。

为什么需要它（2026-09-30 ~ 10-01 实测三例，全是同一个成因族）：
  1. `tools/engine_selftest.c` 里 `fn_seen[0] = '<NUL>';` —— 我用 python 注入时 `\0` 被解释成实体字节，
     GCC 只给一句 `null character(s) preserved in literal` 就编过了。危害的准确形状（本轮实测纠正，
     原先我写成"让 grep 静默失明"是过头的）：`grep -c` 照样数得到 3，但**任何含该字节的输出会让
     读数在管道/工具层整段变成不可见的"二进制"**——本会话真实踩过两次（分桶行"消失"、门读数显示为空）。
  2. `docs/visual-baseline-2026-09-30.md` 里 4 处：`btn_after\\01_main_empty_dark.png` 被写成
     `btn_after<0x01>_main_empty_dark.png` —— **`\01` 被当成八进制转义吃掉**，反斜杠和两个数字一起没了。
  3. 同族第三例：`sweep_leftovers` 的 `"%s\\%s"` 掉成一个反斜杠 ⇒ 清扫静默空转（那条已由
     `tools/mutation_fixture_sweep.py` 演过会红）。
⇒ 共同点：**注入工具吃掉反斜杠，产出的仍然是"能编译/能渲染"的文件**，只有字节级才看得见。

判据来自哪里：允许的控制字符只有 `\\t`(09) `\\n`(0A) `\\r`(0D)；其余 C0 (00–1F) 与 0x7F 一律算裸控制字节。
范围用 `git ls-files` 的**文本扩展名**子集当分母（每轮印出来）：不这么定的话，`.png/.a/.dll` 会让门
永远是红的，而"永远红的门"等于没有门。

用法：python tools/lint_control_bytes.py [--selftest]
  --selftest 跑两面（把已知坏字节种进临时副本必须红、干净副本必须绿），用来证明本门不是装饰。
退出码：0 = 无裸控制字节；1 = 有（逐条打印 文件:偏移:行 字节值与上下文）；2 = 前置不成立（不在 git 仓里等）。
"""

import os
import subprocess
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT_EXT = {".c", ".h", ".cpp", ".hpp", ".py", ".md", ".qss", ".txt", ".json",
            ".js", ".yml", ".yaml", ".ps1", ".bat", ".qrc", ".nsi", ".in", ".cmake"}
ALLOWED = {0x09, 0x0A, 0x0D}
NAMES = {0x00: "NUL", 0x01: "SOH(\\01 八进制转义被吃)", 0x02: "STX(\\02 同上)",
         0x07: "BEL", 0x08: "BS", 0x0B: "VT", 0x0C: "FF", 0x1B: "ESC", 0x7F: "DEL"}


def scan_bytes(data):
    out = []
    line = 1
    for i, byte in enumerate(data):
        if byte == 0x0A:
            line += 1
        if byte not in ALLOWED and 0x00 <= byte <= 0x1F or byte == 0x7F:
            out.append((i, line, byte))
    return out


def tracked_text_files():
    p = subprocess.run("git ls-files", shell=True, cwd=ROOT, capture_output=True)
    if p.returncode != 0:
        return None
    files = [f for f in p.stdout.decode("utf-8", "replace").split("\n") if f]
    return [f for f in files
            if os.path.splitext(f)[1].lower() in TEXT_EXT
            and os.path.isfile(os.path.join(ROOT, f.replace("/", os.sep)))]


def report(hits, rel):
    data = open(os.path.join(ROOT, rel.replace("/", os.sep)) if not os.path.isabs(rel) else rel, "rb").read()
    for off, line, byte in hits:
        ctx = data[max(0, off - 40):off + 40]
        print("    %-56s off=%-8d 行=%-5d 字节=0x%02X %s"
              % (rel, off, line, byte, NAMES.get(byte, "")))
        print("           上下文=%s" % ascii(ctx))


def main():
    t0 = time.time()
    if not os.path.isdir(os.path.join(ROOT, ".git")):
        print("[RED] CTRL-BYTE-PRECONDITION: 找不到 .git（%s）⇒ 分母取不到，不判" % ROOT)
        return 2
    files = tracked_text_files()
    if files is None:
        print("[RED] CTRL-BYTE-PRECONDITION: git ls-files 失败 ⇒ 分母取不到，不判")
        return 2

    if "--selftest" in sys.argv:
        return selftest(files)

    total_hits = 0
    for rel in files:
        data = open(os.path.join(ROOT, rel.replace("/", os.sep)), "rb").read()
        hits = scan_bytes(data)
        if hits:
            report(hits, rel)
            total_hits += len(hits)
    print("lint_control_bytes: 扫了 %d 个跟踪文本文件（分母=git ls-files ∩ 扩展名%s）"
          % (len(files), "…"))
    if total_hits:
        print("[RED] 裸控制字节 %d 处：含这些字节的读数会被工具层当成二进制整段丢掉（文件头有实测三条）"
              % total_hits)
        print("lint_control_bytes EXIT=1 成本=%dms" % int((time.time() - t0) * 1000))
        return 1
    print("lint_control_bytes OK: 0 处裸控制字节（允许 09/0A/0D）｜成本 %dms" % int((time.time() - t0) * 1000))
    return 0


def selftest(files):
    """两面：种一个已知坏字节必须红，干净副本必须绿。写临时文件，绝不碰仓内文件。"""
    import tempfile
    tmp = tempfile.mkdtemp(prefix="ctrl-byte-selftest-")
    clean = os.path.join(tmp, "clean.c")
    dirty = os.path.join(tmp, "dirty.c")
    body = b'#include <stdio.h>\nint main(void){printf("ok\\n"); return 0;}\n'
    open(clean, "wb").write(body)
    open(dirty, "wb").write(body.replace(b"return 0;", b"fn[0] = '\x01'; return 0;"))
    bad = 0
    for name, path, want in (("干净副本", clean, 0), ("种了 0x01 的副本", dirty, 1)):
        data = open(path, "rb").read()
        got = 1 if scan_bytes(data) else 0
        ok = (got == want)
        print("  [自检] %-16s 期望=%s 实得=%s ⇒ %s" % (name, "红" if want else "绿",
                                                     "红" if got else "绿", "OK" if ok else "BAD"))
        if not ok:
            bad += 1
    # 反向再核一次：如果扫描器对真实文件全瞎，上面两面也会全绿 —— 所以补一条"分母非空"
    if not files:
        print("  [自检] BAD 真实分母为 0 ⇒ 本门没有任何可扫对象，永远绿就是装饰")
        bad += 1
    else:
        print("  [自检] 真实分母非空 OK（%d 个文件）" % len(files))
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)
    print("selftest: %s" % ("两面 + 分母都过" if bad == 0 else "有面不对"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
