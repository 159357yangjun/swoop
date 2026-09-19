#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QMap>
#include <functional>

class QTcpServer;
class QTcpSocket;

// 极简内嵌 HTTP 服务器：为 IDM Next 提供本地 Web 远程管理界面。
// 仅监听 127.0.0.1（默认），所有 /api 调用需携带 token（?token= 或 X-Token 头）。
// 通过回调与 MainWindow 解耦，自身不依赖任务模型/引擎。
class WebServer : public QObject {
    Q_OBJECT
public:
    explicit WebServer(QObject* parent = nullptr);

    using GetTasksFn = std::function<QJsonArray()>;
    using AddTaskFn  = std::function<int(const QString& url, const QString& dir,
                                         const QString& queue, int threads)>;
    using ControlFn  = std::function<bool(int id, const QString& action)>; // pause/resume/cancel/remove

    void setProviders(GetTasksFn getTasks, AddTaskFn addTask, ControlFn control);

    bool startServer(quint16 port, const QString& token);
    void stopServer();
    bool isListening() const;
    QString errorString() const;
    quint16 port() const;

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void handleRequest(QTcpSocket* sock, const QByteArray& raw,
                        const QMap<QString, QString>& headers);
    void route(QTcpSocket* sock, const QString& method, const QString& path,
               const QByteArray& body, const QMap<QString, QString>& headers);
    static QString extractToken(const QString& path,
                                const QMap<QString, QString>& headers);
    void sendJson(QTcpSocket* sock, int code, const QJsonObject& obj);
    void sendHtml(QTcpSocket* sock, const QString& html);
    void sendText(QTcpSocket* sock, int code, const QString& text,
                  const QByteArray& ctype);
    QString pageHtml(const QString& token) const;  // 内嵌单文件管理页

    QTcpServer*        m_server = nullptr;
    QString            m_token;
    GetTasksFn         m_getTasks;
    AddTaskFn          m_addTask;
    ControlFn          m_control;
    QHash<QTcpSocket*, QByteArray> m_buffers;  // 每个连接累积的接收缓冲
};

#endif // WEB_SERVER_H
