#pragma once

#include <QObject>
#include <functional>

/* 在一条临时工作线程上跑 job，跑完把 done 投递回 **context 所在线程**执行。
 *
 * 为什么要有它：组件自愈（下载 ffmpeg / aria2c 压缩包）以前整条都写在
 * QNetworkReply::finished 的 lambda 里，而那个 lambda 的接收者是 context
 * ⇒ 它跑在 GUI 线程上。于是「写 90MB 到磁盘 + tar 解压
 * （QProcess::waitForFinished，上限 60 秒）+ 复制文件」全在 GUI 线程里，
 * 首次安装开机就是一次数秒到数十秒的白屏冻结；同一段时间里 IPC 服务端也无法
 * 处理新连接（CI 第 13 步那个「连上了、3 秒没回话」就是这么来的）。
 *
 * 只依赖 QtCore：不引入 QtConcurrent，也不新增任何依赖。
 * ⚠️ job 里不得碰 QWidget / QNetworkReply / context 的其它成员 ——
 * 需要 UI 或需要线程亲和性的东西，一律放到 done 里。 */
void runOffThread(QObject* context, std::function<void()> job, std::function<void()> done);
