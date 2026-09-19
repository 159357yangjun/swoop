#include "web_server.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QList>

WebServer::WebServer(QObject* parent) : QObject(parent) {}

void WebServer::setProviders(GetTasksFn getTasks, AddTaskFn addTask, ControlFn control)
{
    m_getTasks = std::move(getTasks);
    m_addTask  = std::move(addTask);
    m_control  = std::move(control);
}

bool WebServer::startServer(quint16 port, const QString& token)
{
    m_token = token;
    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &WebServer::onNewConnection);
    }
    if (m_server->isListening())
        m_server->close();
    // 仅监听本地回环地址：远程无法访问，安全性更好
    return m_server->listen(QHostAddress::LocalHost, port);
}

void WebServer::stopServer()
{
    if (m_server && m_server->isListening())
        m_server->close();
    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it)
        it.key()->close();
    m_buffers.clear();
}

bool WebServer::isListening() const
{
    return m_server && m_server->isListening();
}

QString WebServer::errorString() const
{
    return m_server ? m_server->errorString() : QStringLiteral("未初始化");
}

quint16 WebServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

void WebServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket* sock = m_server->nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead,    this, &WebServer::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, &WebServer::onDisconnected);
        m_buffers[sock] = QByteArray();
    }
}

void WebServer::onReadyRead()
{
    auto* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    m_buffers[sock].append(sock->readAll());

    // 防止异常大请求撑爆内存
    if (m_buffers[sock].size() > (1 << 20)) {
        sendText(sock, 413, QStringLiteral("请求过大"), "text/plain; charset=utf-8");
        m_buffers.remove(sock);
        sock->close();
        return;
    }

    QByteArray& buf = m_buffers[sock];
    int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;  // 头还没收完，等下一次

    // 解析请求行 + 头
    int lineEnd = buf.indexOf("\r\n");
    QByteArray firstLine = buf.left(lineEnd);
    QList<QByteArray> parts = firstLine.split(' ');
    if (parts.size() < 2) { sock->close(); m_buffers.remove(sock); return; }
    QString method = QString::fromLatin1(parts[0]);
    QString fullPath = QString::fromLatin1(parts[1]);

    QMap<QString, QString> headers;
    int pos = lineEnd + 2;
    while (pos < headerEnd) {
        int nl = buf.indexOf("\r\n", pos);
        if (nl < 0) break;
        QByteArray h = buf.mid(pos, nl - pos);
        int colon = h.indexOf(':');
        if (colon > 0)
            headers[QString::fromLatin1(h.left(colon)).toLower()] =
                QString::fromUtf8(h.mid(colon + 1).trimmed());
        pos = nl + 2;
    }

    int contentLength = headers.value(QStringLiteral("content-length")).toInt();
    int bodyStart = headerEnd + 4;
    if (method == QStringLiteral("POST") && buf.size() < bodyStart + contentLength)
        return;  // body 还没收完，等下一次

    QByteArray body = (method == QStringLiteral("POST"))
                      ? buf.mid(bodyStart, contentLength) : QByteArray();

    handleRequest(sock, method.toUtf8() + ' ' + fullPath.toUtf8() + ' ' + body, headers);
    m_buffers.remove(sock);  // 本连接一次性处理，随后关闭
    sock->flush();
    sock->disconnectFromHost();
}

void WebServer::onDisconnected()
{
    auto* sock = qobject_cast<QTcpSocket*>(sender());
    if (sock) {
        m_buffers.remove(sock);
        sock->deleteLater();  // 释放连接，避免长会话累积
    }
}

void WebServer::handleRequest(QTcpSocket* sock, const QByteArray& raw,
                               const QMap<QString, QString>& headers)
{
    // raw = "METHOD /path?query BODY"
    int sp1 = raw.indexOf(' ');
    int sp2 = raw.indexOf(' ', sp1 + 1);
    QString method = QString::fromLatin1(raw.left(sp1));
    QString fullPath = QString::fromLatin1(raw.mid(sp1 + 1, sp2 - sp1 - 1));
    QByteArray body = raw.mid(sp2 + 1);
    route(sock, method, fullPath, body, headers);
}

void WebServer::route(QTcpSocket* sock, const QString& method,
                      const QString& fullPath, const QByteArray& body,
                      const QMap<QString, QString>& headers)
{
    QString path = fullPath.section('?', 0, 0);  // 去掉查询串用于路由

    // 管理页（localhost，免 token；token 已内嵌到页面 JS 中）
    if (method == QStringLiteral("GET") && (path == QStringLiteral("/") ||
        path == QStringLiteral("/index.html"))) {
        sendHtml(sock, pageHtml(m_token));
        return;
    }

    if (path == QStringLiteral("/favicon.ico")) {
        sendText(sock, 404, QStringLiteral(""), "text/plain");
        return;
    }

    // 其余均要求 token
    QString token = extractToken(fullPath, headers);
    if (token != m_token) {
        sendJson(sock, 401, QJsonObject{{QStringLiteral("error"),
                                         QStringLiteral("unauthorized")}});
        return;
    }

    if (path == QStringLiteral("/api/ping")) {
        sendJson(sock, 200, QJsonObject{{QStringLiteral("ok"), true}});
        return;
    }

    QStringList seg = path.split('/', Qt::SkipEmptyParts);
    bool isTasksBase = (seg.size() == 2 && seg[0] == QStringLiteral("api") &&
                        seg[1] == QStringLiteral("tasks"));

    if (method == QStringLiteral("GET") && isTasksBase) {
        QJsonObject resp;
        resp[QStringLiteral("tasks")] = m_getTasks ? m_getTasks() : QJsonArray();
        resp[QStringLiteral("serverTime")] =
            QDateTime::currentDateTime().toString(Qt::ISODate);
        sendJson(sock, 200, resp);
        return;
    }

    if (method == QStringLiteral("POST") && isTasksBase) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            sendJson(sock, 400, QJsonObject{{QStringLiteral("error"),
                QStringLiteral("invalid json")}});
            return;
        }
        QJsonObject o = doc.object();
        QString url = o.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty()) {
            sendJson(sock, 400, QJsonObject{{QStringLiteral("error"),
                QStringLiteral("url required")}});
            return;
        }
        QString dir   = o.value(QStringLiteral("dir")).toString();
        QString queue = o.value(QStringLiteral("queue")).toString();
        int threads   = o.value(QStringLiteral("threads")).toInt();
        int id = m_addTask ? m_addTask(url, dir, queue, threads) : -1;
        if (id <= 0) {
            sendJson(sock, 500, QJsonObject{{QStringLiteral("error"),
                QStringLiteral("add failed")}});
            return;
        }
        sendJson(sock, 200, QJsonObject{{QStringLiteral("id"), id}});
        return;
    }

    // 任务控制：/api/tasks/<id>/<action>
    if (seg.size() == 4 && seg[0] == QStringLiteral("api") &&
        seg[1] == QStringLiteral("tasks")) {
        bool ok = false;
        int id = seg[2].toInt(&ok);
        QString action = seg[3];
        if (!ok || !(action == QStringLiteral("pause") || action == QStringLiteral("resume") ||
                     action == QStringLiteral("cancel") || action == QStringLiteral("remove"))) {
            sendJson(sock, 404, QJsonObject{{QStringLiteral("error"),
                QStringLiteral("not found")}});
            return;
        }
        bool done = m_control ? m_control(id, action) : false;
        sendJson(sock, 200, QJsonObject{{QStringLiteral("ok"), done}});
        return;
    }

    sendJson(sock, 404, QJsonObject{{QStringLiteral("error"),
        QStringLiteral("not found")}});
}

QString WebServer::extractToken(const QString& path, const QMap<QString, QString>& headers)
{
    // 优先从查询串 ?token=xxx 提取（页内 JS 与 API 调用均走查询参数）
    int q = path.indexOf('?');
    if (q >= 0) {
        QUrlQuery qry(path.mid(q + 1));
        QString t = qry.queryItemValue(QStringLiteral("token"));
        if (!t.isEmpty()) return t;
    }
    // 其次支持 X-Token 头
    return headers.value(QStringLiteral("x-token"));
}

void WebServer::sendJson(QTcpSocket* sock, int code, const QJsonObject& obj)
{
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sendText(sock, code, QString::fromUtf8(body), "application/json; charset=utf-8");
}

void WebServer::sendHtml(QTcpSocket* sock, const QString& html)
{
    sendText(sock, 200, html, "text/html; charset=utf-8");
}

void WebServer::sendText(QTcpSocket* sock, int code, const QString& text,
                         const QByteArray& ctype)
{
    QByteArray body = text.toUtf8();
    QByteArray resp;
    resp.append(QStringLiteral("HTTP/1.1 %1 %2\r\n")
                .arg(code).arg(code == 200 ? QStringLiteral("OK")
                                : code == 400 ? QStringLiteral("Bad Request")
                                : code == 401 ? QStringLiteral("Unauthorized")
                                : code == 404 ? QStringLiteral("Not Found")
                                : code == 413 ? QStringLiteral("Payload Too Large")
                                : QStringLiteral("Error")).toUtf8());
    resp.append("Content-Type: " + ctype + "\r\n");
    resp.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    resp.append("Connection: close\r\n");
    resp.append("Access-Control-Allow-Origin: *\r\n");
    resp.append("\r\n");
    resp.append(body);
    sock->write(resp);
}

QString WebServer::pageHtml(const QString& token) const
{
    // 单文件管理页（玻璃拟态，粉蓝配色）。token 内嵌到脚本中供 fetch 使用。
    QString js = QStringLiteral(
        "const TOKEN = \"%1\";\n"
        "const api = (p, opt) => fetch(p + (p.includes('?') ? '&' : '?') + 'token=' + TOKEN, opt);\n"
        "function fmtSize(b){ if(b<0) return '-'; const u=['B','KB','MB','GB','TB']; let i=0; let n=b; while(n>=1024&&i<u.length-1){n/=1024;i++;} return (i?n.toFixed(2):n)+' '+u[i]; }\n"
        "function fmtSpeed(b){ return b>0 ? (fmtSize(b)+'/s') : '-'; }\n"
        "function stateText(s){ return ['等待中','下载中','已暂停','已完成','失败','已取消'][s] || s; }\n"
        "function loadTasks(){\n"
        "  api('/api/tasks').then(r=>r.json()).then(d=>{\n"
        "    const tb=document.getElementById('tasks'); tb.innerHTML='';\n"
        "    (d.tasks||[]).forEach(t=>{\n"
        "      const pct = t.progress>=0 ? t.progress : (t.size>0 ? Math.floor(t.downloaded*100/t.size) : -1);\n"
        "      const tr=document.createElement('tr');\n"
        "      tr.innerHTML = `<td>${escapeHtml(t.name||t.url)}</td>`+\n"
        "        `<td>${stateText(t.state)}</td>`+\n"
        "        `<td>${pct>=0 ? pct+'%' : '-'}</td>`+\n"
        "        `<td>${fmtSize(t.downloaded)} / ${fmtSize(t.size)}</td>`+\n"
        "        `<td>${fmtSpeed(t.speed)}</td>`+\n"
        "        `<td class='acts'>`+\n"
        "          `<button onclick=\"ctrl(${t.id},'pause')\">暂停</button>`+\n"
        "          `<button onclick=\"ctrl(${t.id},'resume')\">继续</button>`+\n"
        "          `<button onclick=\"ctrl(${t.id},'cancel')\">取消</button>`+\n"
        "          `<button onclick=\"ctrl(${t.id},'remove')\">移除</button>`+\n"
        "        `</td>`;\n"
        "      tb.appendChild(tr);\n"
        "    });\n"
        "  }).catch(e=>console.error(e));\n"
        "}\n"
        "function ctrl(id,a){ api('/api/tasks/'+id+'/'+a,{method:'POST'}).then(loadTasks); }\n"
        "function addTask(){\n"
        "  const url=document.getElementById('url').value.trim();\n"
        "  if(!url) return;\n"
        "  const dir=document.getElementById('dir').value.trim();\n"
        "  const queue=document.getElementById('queue').value.trim();\n"
        "  const threads=parseInt(document.getElementById('threads').value)||0;\n"
        "  api('/api/tasks',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url,dir,queue,threads})})\n"
        "    .then(r=>r.json()).then(d=>{ if(d.id>0){document.getElementById('url').value=''; loadTasks();} else alert('添加失败'); });\n"
        "}\n"
        "function escapeHtml(s){ return String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c])); }\n"
        "setInterval(loadTasks, 2000); loadTasks();\n"
    ).arg(token);

    return QStringLiteral(
        "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>IDM Next · 远程管理</title>"
        "<style>"
        "  *{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,'Segoe UI',Roboto,'Microsoft YaHei',sans-serif;}"
        "  body{min-height:100vh;background:linear-gradient(135deg,#ffd6e8 0%,#e6f0ff 45%,#d9e8ff 100%);padding:32px;color:#2b2b4a;}"
        "  .card{background:rgba(255,255,255,0.55);backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px);"
        "        border:1px solid rgba(255,255,255,0.7);border-radius:18px;padding:26px;max-width:980px;margin:0 auto;"
        "        box-shadow:0 12px 40px rgba(120,120,200,0.18);}"
        "  h1{font-size:22px;margin-bottom:4px;background:linear-gradient(90deg,#ff5fa2,#5b8cff);-webkit-background-clip:text;background-clip:text;color:transparent;}"
        "  .sub{font-size:13px;color:#7a7a9a;margin-bottom:20px;}"
        "  .form{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:20px;}"
        "  .form input{flex:1 1 240px;min-width:160px;padding:10px 12px;border:1px solid rgba(150,150,200,0.3);border-radius:10px;background:rgba(255,255,255,0.7);font-size:14px;}"
        "  .form button{padding:10px 18px;border:0;border-radius:10px;color:#fff;font-size:14px;cursor:pointer;"
        "        background:linear-gradient(90deg,#ff5fa2,#5b8cff);box-shadow:0 6px 16px rgba(120,120,220,0.28);}"
        "  .form button:hover{filter:brightness(1.05);}"
        "  table{width:100%;border-collapse:collapse;font-size:13px;}"
        "  th,td{text-align:left;padding:9px 10px;border-bottom:1px solid rgba(150,150,200,0.16);}"
        "  th{color:#6a6a8a;font-weight:600;}"
        "  .acts button{margin-right:5px;padding:5px 10px;border:0;border-radius:8px;cursor:pointer;font-size:12px;color:#fff;background:linear-gradient(90deg,#ff7eb3,#7aa8ff);}"
        "  .acts button:hover{filter:brightness(1.06);}"
        "</style></head><body><div class='card'>"
        "<h1>IDM Next · 远程管理</h1>"
        "<div class='sub'>本地 Web 控制台（仅 127.0.0.1 可访问）</div>"
        "<div class='form'>"
        "  <input id='url' placeholder='下载链接 URL（支持 http/https、.m3u8、YouTube 等）'>"
        "  <input id='dir' placeholder='保存目录（可选，留空用默认）'>"
        "  <input id='queue' placeholder='队列（可选）'>"
        "  <input id='threads' placeholder='线程数（可选）' style='flex:0 0 110px'>"
        "  <button onclick='addTask()'>添加下载</button>"
        "</div>"
        "<table><thead><tr><th>文件名</th><th>状态</th><th>进度</th><th>大小</th><th>速度</th><th>操作</th></tr></thead>"
        "<tbody id='tasks'></tbody></table>"
        "</div><script>") + js + QStringLiteral("</script></body></html>");
}
