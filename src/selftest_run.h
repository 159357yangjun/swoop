#ifndef IDM_SELFTEST_RUN_H
#define IDM_SELFTEST_RUN_H

/* 无界面自测：起一个本地支持 Range 的 HTTP 服务，验证
   ① 多线程分片整段下载；② 暂停后续传。全部通过返回 0。 */
int run_selftest(void);
/* 定义在 selftest.c（那边调的是**真实** task_create；本文件里的 task_create
   被 Makefile 的 -D 映射成了持久化夹具，测不到预留与改名）。
   必须由 run_selftest() 调用：它依赖自测本地 HTTP 服务已经起来了。 */
int test_race_at_start(void);

#endif
