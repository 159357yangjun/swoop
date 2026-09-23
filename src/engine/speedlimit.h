#ifndef IDM_SPEEDLIMIT_H
#define IDM_SPEEDLIMIT_H

/* 全局下载限速：令牌桶，跨所有分片线程共享。单位字节/秒，0 = 不限。线程安全。 */
void      dl_speed_init(void);
void      dl_set_speed_limit(long long bps);
long long dl_get_speed_limit(void);

/* 下载线程每写入一块数据后调用：按令牌桶预算，超速则休眠。 */
void      dl_throttle(long long bytes);

#endif
