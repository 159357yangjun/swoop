#ifndef WEB_SERVER_CONTROLLER_H
#define WEB_SERVER_CONTROLLER_H

#include <QObject>
#include <QJsonArray>
#include <QString>
#include <functional>
#include <QSystemTrayIcon>

class WebServer;
class Settings;
class TaskListModel;

// L2 组件：内嵌 Web 管理界面的启动 / 停止 / 端口-令牌刷新。
// 自身不持有引擎或任务状态：任务列表经 TaskListModel 提供器注入、任务增删控经 sink 注入、
// 托盘通知经 sink 注入；仅负责按设置构造 WebServer 生命周期与请求处理回调。
class WebServerController : public QObject {
    Q_OBJECT
public:
    explicit WebServerController(QObject* parent = nullptr);

    void setSettings(Settings* s) { m_settings = s; }
    void setTaskModel(TaskListModel* model) { m_taskModel = model; }
    void setSpeedSink(std::function<qint64(int)> f) { m_speedSink = f; }
    void setAddTaskSink(std::function<int(const QString&, const QString&, const QString&, int)> f) { m_addSink = f; }
    void setControlSink(std::function<bool(int, const QString&)> f) { m_ctrlSink = f; }
    void setTraySink(std::function<void(const QString&, const QString&, QSystemTrayIcon::MessageIcon, int)> f) { m_traySink = f; }

    // 依据设置启动 / 停止 / 刷新 Web 管理界面（等价原 MainWindow::applyWebServer）
    void apply();
    bool isRunning() const;

private:
    void buildProviders();
    WebServer*      m_webServer = nullptr;
    Settings*       m_settings = nullptr;
    TaskListModel*  m_taskModel = nullptr;
    std::function<qint64(int)> m_speedSink;
    std::function<int(const QString&, const QString&, const QString&, int)> m_addSink;
    std::function<bool(int, const QString&)> m_ctrlSink;
    std::function<void(const QString&, const QString&, QSystemTrayIcon::MessageIcon, int)> m_traySink;
};

#endif // WEB_SERVER_CONTROLLER_H
