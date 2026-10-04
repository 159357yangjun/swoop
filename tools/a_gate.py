#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A 面回填修复的验收门：fresh 面必须 0 缺失。

为什么单独写一个门而不是靠人看输出：这条判据要能被反复复算，
并且**必须在修复前红过**（否则它不知道自己在拦什么）。

用法：python tools/a_gate.py [rounds]        （默认 6）
退码：0 = fresh 面全命中；1 = 仍有缺失；2 = 实验未成立（SETUP-INVALID > 0 ⇒ 不能读成产品结论）
"""
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import a_profile_discriminate as D  # noqa: E402


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    work = pathlib.Path(os.environ.get("TEMP", "/tmp")) / f"a-gate-{time.strftime('%H%M%S')}"
    work.mkdir(parents=True, exist_ok=True)
    lines = []

    def say(s):
        print(s)
        lines.append(s)

    srv = subprocess.Popen([sys.executable, str(D.ROOT / "tools" / "disk_e2e_server.py"),
                            str(D.HTTP_PORT), "524288"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    miss = invalid = 0
    try:
        time.sleep(2)
        ext_id = D.derive_id_from_key()
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True).stdout.decode().strip()
        say(f"[A-GATE] 仓根={ROOT} HEAD={head} 面=fresh 轮数={rounds}")
        for i in range(rounds):
            prof = work / f"p{i}"
            p = D.launch(prof)
            ver = D.cdp_version()
            if not ver:
                say(f"  第{i+1:2d}遍 [FAIL] CDP 无响应")
                invalid += 1
                continue
            cdp = WsHolder(ver)
            ok, listed, note = cdp.run(ext_id)
            cdp.close()
            try:
                if p.poll() is None:
                    p.terminate(); p.wait(timeout=15)
            except Exception:
                pass
            if not ok:
                invalid += 1
                say(f"  第{i+1:2d}遍 [SETUP-INVALID] {note}")
            elif not listed:
                miss += 1
                say(f"  第{i+1:2d}遍 [缺失] {note}")
            else:
                say(f"  第{i+1:2d}遍 [命中] {note}")
    finally:
        try:
            if srv.poll() is None:
                srv.terminate(); srv.wait(timeout=10)
        except Exception:
            pass
        dst = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "a-gate-last.txt"
        dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
        shutil_rmtree(work)

    valid = rounds - invalid
    rc = 2 if invalid else (1 if miss else 0)
    print(f"[A-GATE] 有效轮={valid} A面缺失={miss} SETUP-INVALID={invalid} 退码={rc}")
    print(f"[读数档] {dst}")
    return rc


def shutil_rmtree(p):
    import shutil
    shutil.rmtree(p, ignore_errors=True)


class WsHolder:
    def __init__(self, ver):
        self.ws = D.Ws(ver["webSocketDebuggerUrl"])

    def run(self, ext_id):
        return D.one_round_a(self.ws, ext_id, D.HTTP_PORT)

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
