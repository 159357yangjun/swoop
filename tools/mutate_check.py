"""把本轮修复逐个注掉（突变测试），每个替换都必须真的命中，否则报错退出。

注掉的是「修复」本身，保留调用点/函数签名，这样测出来的红是修复缺失导致的，
而不是编译错误导致的。用法：
    python tools/mutate_check.py mutate    # 注掉 5 处，编译并跑自测
    ...   restore 由外部 cp 覆盖回原文件
"""
import sys, re, pathlib

P = pathlib.Path("src/core/download_core.c")
s = P.read_text(encoding="utf-8")

MUT = [
    # (说明, 原文, 替换)
    ("[8] pause 恢复成只认 RUNNING",
     "if (t->status == TASK_PENDING || t->status == TASK_RUNNING)\n            t->status = TASK_PAUSED;",
     "if (t->status == TASK_RUNNING)\n            t->status = TASK_PAUSED;"),
    ("[8] need_size 恢复成按状态判断",
     "int need_size = (t->chunk_count == 0) ? 1 : 0;",
     "int need_size = (t->status == TASK_PENDING) ? 1 : 0;"),
    ("[8] restart 不再清零 chunk_count",
     "    t->chunk_count = 0;\n    LeaveCriticalSection(&g_lock);\n\n    /* dlmgr_start 检测到「尚未分段」",
     "    LeaveCriticalSection(&g_lock);\n\n    /* dlmgr_start 检测到「尚未分段」"),
    ("[9] 写入不做转义",
     'static void json_write_escaped(FILE *fp, const char *s) {\n    if (!s) return;',
     'static void json_write_escaped(FILE *fp, const char *s) {\n    if (!s) return;\n    fputs(s, fp); return;'),
    ("[9] 读取不做还原",
     "    if (escaped) {\n        json_read_string(fld, out, cap);\n    } else {",
     "    if (0) {\n        json_read_string(fld, out, cap);\n    } else {"),
    ("[10] 加载后不排序",
     "    for (int i = 1; i < g_task_count; i++) {\n        DownloadTask tmp = g_tasks[i];",
     "    for (int i = 1; i < 0; i++) {\n        DownloadTask tmp = g_tasks[i];"),
    ("[11] 载入前不清空既有任务（恢复成「往数组尾部追加」）",
     "    reset_all_tasks();\n\n    /* 读取整个 JSON 文件 */",
     "    /* reset_all_tasks(); */\n\n    /* 读取整个 JSON 文件 */"),
]

for desc, old, new in MUT:
    if old not in s:
        print(f"!! 突变未命中（原文找不到）: {desc}")
        sys.exit(2)
    s = s.replace(old, new, 1)
    print(f"   已注掉: {desc}")

P.write_text(s, encoding="utf-8")
print("突变写入完成")
