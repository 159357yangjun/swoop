#include "web_server_controller.h"
#include "web_server.h"
#include "task_list_model.h"
#include "settings.h"
#include "logger.h"

#include <QJsonObject>

WebServerController::WebServerController(QObject* parent)
    : QObject(parent) {}

void WebServerController::buildProviders()
{
    auto getTasks = [this]() -> QJsonArray {
        QJsonArray arr;
        if (!m_taskModel) return arr;
        const auto& tasks = m_taskModel->tasks();
        for (const auto& t : tasks) {
            QJsonObject o;
            o[QStringLiteral("id")]         = t.id;
            o[QStringLiteral("name")]       = t.fileName;
            o[QStringLiteral("url")]        = t.url;
            o[QStringLiteral("size")]       = t.fileSize;
            o[QStringLiteral("downloaded")] = t.downloaded;
            o[QStringLiteral("speed")]      = m_speedSink ? m_speedSink(t.id) : 0;
            o[QStringLiteral("state")]      = t.state;
            o[QStringLiteral("status")]     = t.statusText;
            o[QStringLiteral("queue")]      = t.queue;
            int pct = t.progressPct >= 0 ? t.progressPct
                : (t.fileSize > 0
                   ? static_cast<int>(t.downloaded * 100 / t.fileSize) : -1);
            o[QStringLiteral("progress")] = pct;
            arr.append(o);
        }
        return arr;
    };
    auto addTask = [this](const QString& url, const QString& dir, const QString& queue,
                          int threads) -> int {
        if (!m_settings || !m_addSink) return -1;
        QString saveDir = dir.isEmpty() ? m_settings->defaultSaveDir() : dir;
        int th = threads > 0 ? threads : m_settings->maxThreads();
        return m_addSink(url, saveDir, queue, th);
    };
    auto control = [this](int id, const QString& action) -> bool {
        if (!m_ctrlSink) return false;
        return m_ctrlSink(id, action);
    };
    m_webServer->setProviders(getTasks, addTask, control);
}


bool WebServerController::isRunning() const
{
    return m_webServer && m_webServer->isListening();
}
void WebServerController::apply()
{
    if (!m_settings) return;
    if (!m_settings->webEnabled()) {
        if (m_webServer && m_webServer->isListening()) {
            m_webServer->stopServer();
            Log::info(QStringLiteral("Web 管理界面已关闭"));
        }
        return;
    }
    if (!m_webServer) {
        m_webServer = new WebServer(this);
        buildProviders();
    }
    if (m_webServer->isListening())
        m_webServer->stopServer();
    if (m_webServer->startServer(static_cast<quint16>(m_settings->webPort()),
                                 m_settings->webToken())) {
        Log::info(QStringLiteral("Web 管理界面已启动: http://127.0.0.1:%1/")
                      .arg(m_settings->webPort()));
        if (m_traySink)
            m_traySink(QStringLiteral("Web 管理界面"),
                       QStringLiteral("已启动：http://127.0.0.1:%1/").arg(m_settings->webPort()),
                       QSystemTrayIcon::Information, 4000);
    } else {
        Log::warn(QStringLiteral("Web 管理界面启动失败: %1").arg(m_webServer->errorString()));
        if (m_traySink)
            m_traySink(QStringLiteral("Web 管理界面"),
                       QStringLiteral("启动失败：端口 %1 可能被占用").arg(m_settings->webPort()),
                       QSystemTrayIcon::Warning, 4000);
    }
}
