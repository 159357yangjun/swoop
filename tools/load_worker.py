#!/usr/bin/env python3
"""load_diff_run.sh 的负载发生器：纯 CPU 忙等，到点自己退。

为什么单独一个文件而不是 `python -c "…"`：上一轮我用 `python -c "多行" >/dev/null 2>&1 &`，
worker 的 traceback 被自己重定向进了 /dev/null ⇒ 16 只里有几只当场死掉都看不出来，
"负载面"的读数于是等同于空闲面（实测 calib 492–573ms = 空闲值），那一面整面作废。
现在 stderr 落盘、由跑批器检查非空即响亮报错。

用法：python tools/load_worker.py <Unix 截止时刻>   （到 time() >= 截止 就退出）
"""
import sys
import time

deadline = float(sys.argv[1])
x = 0
while time.time() < deadline:
    x += 1
