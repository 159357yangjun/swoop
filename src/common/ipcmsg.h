#ifndef IDM_IPCMSG_H
#define IDM_IPCMSG_H

/* 主进程窗口类名：浏览器消息宿主/第二个实例靠它 FindWindow 定位常驻进程。 */
#define IDM_WINDOW_CLASS L"SwoopNativeWnd"

/* WM_COPYDATA 的 dwData 魔数（'SWP1'），用于识别「把 URL 交给主进程下载」的载荷。
   载荷为 UTF-8、按行分隔：line0=url，line1=filename(可空)，line2=referer(可空)。 */
#define IDM_COPYDATA_MAGIC 0x53575031u

#endif
