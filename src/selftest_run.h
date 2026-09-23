#ifndef IDM_SELFTEST_RUN_H
#define IDM_SELFTEST_RUN_H

/* 无界面自测：起一个本地支持 Range 的 HTTP 服务，验证
   ① 多线程分片整段下载；② 暂停后续传。全部通过返回 0。 */
int run_selftest(void);

#endif
