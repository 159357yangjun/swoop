#ifndef IDM_APP_IPC_SERVER_H
#define IDM_APP_IPC_SERVER_H

#include <QObject>
#include <QString>
#include <functional>

class QLocalServer;
class QLocalSocket;

// ── L2 应用编排组件：IPC 单实例通信 ───────────────────────────────
// 从 MainWindow 抽取。接收浏览器扩展 / CLI 经 native-messaging-host 转发来的
// 下载请求（QLocalServer "idm-next-ipc"），解析 JSON 命令后转发给上层注入的回调；
// 对 MainWindow 内部状态（m_taskModel 任务数）以「提供器」回调解耦。
//
// 行为契约（与原 MainWindow::startIpcServer / onIpcConnection 一致）：
//  - 启动时移除残留旧服务端再 listen；失败仅 Log::warn 不崩溃。
//  - "add" 命令 → 调用注入的 addTask 回调（通常为 MainWindow::addTaskFromUrl）；
//  - "list" 命令 → 用注入的 taskCount 提供器应答当前任务数；
//  - 其余命令 → 应答「不支持的操作」。
//  - 每个连接都回写 JSON 响应并断开。
class IpcServer : public QObject {
    Q_OBJECT
public:
    explicit IpcServer(QObject* parent = nullptr);

    // 启动监听；返回是否成功（失败原因已内部 Log::warn）
    bool start();

    // "add" 命令 → 转发到上层任务添加（通常为 MainWindow::addTaskFromUrl）
    void setAddTaskCallback(std::function<void(const QString& url, const QString& filename,
                                               const QString& dir, int threads,
                                               const QString& queue, const QString& format)> cb);
    // "list" 命令响应所需的当前任务数（由 MainWindow 注入：m_taskModel->rowCount()）
    void setTaskCountProvider(std::function<int()> cb);

    /* ── 测具读数（不参与任何行为；IDM_ACTIVATE_PROBE D 段用它做归因）──
     * 一次 onNewConnection() 会「取尽」若干条连接，每条最多在 handleClient 里付 ~200ms 宽限。
     * D 段原先只印「事件循环单轮最长」，那个数**混进了同轮里的重绘/定时器**，
     * 所以它涨到 2500ms 时无法判断是不是这条串行宽限路径造成的。
     * 这三个数把「取尽几条 / 取尽本身花了多久 / 历史上最久一次」分开，
     * 于是「单轮最长 ≈ 取尽耗时」⇒ IPC 串行；「单轮最长 ≫ 取尽耗时」⇒ 别的东西占大头。
     * 只增计数、不改任何控制流。 */
    struct DrainStat {
        int   connections = 0;   // 最近一次取尽的连接数 N
        qint64 lastMs = 0;       // 最近一次取尽本身耗时
        qint64 maxMs = 0;        // 本进程内取尽耗时的历史最大
    };
    static DrainStat drainStat();

private slots:
    void onNewConnection();

private:
    void handleClient(QLocalSocket* client);

    QLocalServer* m_server = nullptr;
    std::function<void(const QString&, const QString&, const QString&, int, const QString&, const QString&)> m_addTask;
    std::function<int()> m_taskCount;

    static DrainStat s_drainStat;   // 单进程内只跑一个 IpcServer，测具用静态量最省事
};

#endif // IDM_APP_IPC_SERVER_H
