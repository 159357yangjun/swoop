#!/usr/bin/env python3
"""静默运行控制台子系统程序，避免弹黑窗。

用法:
    python tools/quiet_run.py <exe> [args...]
环境变量原样透传，返回子进程退出码。

为什么需要这个: ui_snapshot.exe / engine_selftest.exe 都是 console 子系统，
从 Windows shell 启动会各自弹一个控制台窗口；批量跑几十次会让用户以为
"AI 开了常驻命令行不关"。creationflags=CREATE_NO_WINDOW 从根上避免。

另: 需要看输出时它仍然捕获 stdout/stderr 并原样打印, 所以可以直接当 exe 的替身用。
"""
import os
import subprocess
import sys

CREATE_NO_WINDOW = 0x08000000


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe, args = sys.argv[1], sys.argv[2:]
    if not os.path.isabs(exe):
        exe = os.path.abspath(exe)
    if not os.path.exists(exe):
        print(f"[quiet_run] 找不到: {exe}", file=sys.stderr)
        return 127

    p = subprocess.run(
        [exe, *args],
        capture_output=True,
        text=True,
        errors="replace",
        creationflags=CREATE_NO_WINDOW,
    )
    if p.stdout:
        sys.stdout.write(p.stdout)
    if p.stderr:
        sys.stderr.write(p.stderr)
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
