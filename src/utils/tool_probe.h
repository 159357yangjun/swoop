#ifndef IDM_UTILS_TOOL_PROBE_H
#define IDM_UTILS_TOOL_PROBE_H

#include <QString>
#include <QStringList>

// 外部工具（yt-dlp / ffmpeg / aria2c）可用性探测的记忆层。
//
// 为什么要有这一层：判"工具在不在"最可靠的办法是真的跑一次 `--version`，
// 但那是一次同步子进程 —— 实测 yt-dlp 1.34s、ffmpeg 0.08~0.47s、aria2c 0.06~0.12s
// （yt-dlp 是 PyInstaller 单文件包，每次启动都要自解压，所以特别慢）。
// 而调用点不止一处：MainWindow 构造里三个组件的自愈检查、新建任务对话框每次打开、
// 任务真正开始前、HLS 合成前……同一份 1.3 秒被反复付。
//
// 缓存键是 (路径, 参数, 大小, 修改时间)：组件被重新下载/替换时大小或 mtime 必然变，
// 于是自动失效重探，不需要谁记得去 clear。文件不存在时键是 (0, epoch)，
// 命中缓存直接返回 false（不 spawn），等它出现那天键就变了 —— 自愈路径不受影响。
// 参数进键是因为结果由「文件+参数」共同决定：今天每个工具只有一种探法，
// 少带这项不会出错，但同一个 exe 加一种探法时就会串味。
namespace ToolProbe {

// 跑 `exePath args...`，退出码 0 才算可用。结果按上面的键记忆。
bool available(const QString& exePath, const QStringList& args);

} // namespace ToolProbe

#endif // IDM_UTILS_TOOL_PROBE_H
