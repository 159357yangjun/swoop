// idm_next_host.cpp — IDM Next Native Messaging Host (C++ 原生实现)
//
// 浏览器扩展通过 Chrome Native Messaging 协议（stdin/stdout，每条消息前 4 字节
// 小端序长度前缀 + JSON 正文）与本进程通信。本进程将下载请求直接转发给运行中
// 的 IDM Next GUI（通过 QLocalServer "idm-next-ipc"），无需每下载拉起一个子进程，
// 也不再依赖 Python。
//
// 支持的操作：
//   - ping:        心跳检测（GUI 是否在运行）
//   - add_download: 添加单个下载任务
//   - batch_add:    批量添加多个下载任务
//   - get_status:   获取 IDM Next 运行状态
//
// 若 GUI 未运行，add_download/batch_add 会回退到 `idm-next --cli add`（无界面 CLI）。
//
// 编译：Qt6::Core（仅 Core，不依赖 Widgets）。Windows 下设为 Win32 子系统，
//       不弹控制台黑窗，stdio 句柄由浏览器继承。

#include <QCoreApplication>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QtEndian>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

// ── Native Messaging 基础 I/O ────────────────────────────────────────

#ifdef Q_OS_WIN
static bool readExact(HANDLE h, void* buf, qint64 n)
{
    char* p = static_cast<char*>(buf);
    qint64 remaining = n;
    while (remaining > 0) {
        DWORD r = 0;
        if (!ReadFile(h, p, static_cast<DWORD>(remaining), &r, nullptr) || r == 0)
            return false;
        p += r;
        remaining -= r;
    }
    return true;
}

static bool writeExact(HANDLE h, const void* buf, qint64 n)
{
    const char* p = static_cast<const char*>(buf);
    qint64 remaining = n;
    while (remaining > 0) {
        DWORD w = 0;
        if (!WriteFile(h, p, static_cast<DWORD>(remaining), &w, nullptr) || w == 0)
            return false;
        p += w;
        remaining -= w;
    }
    return true;
}
#else
static bool readExact(int fd, void* buf, qint64 n)
{
    char* p = static_cast<char*>(buf);
    qint64 remaining = n;
    while (remaining > 0) {
        ssize_t r = ::read(fd, p, static_cast<size_t>(remaining));
        if (r <= 0)
            return false;
        p += r;
        remaining -= r;
    }
    return true;
}

static bool writeExact(int fd, const void* buf, qint64 n)
{
    const char* p = static_cast<const char*>(buf);
    qint64 remaining = n;
    while (remaining > 0) {
        ssize_t w = ::write(fd, p, static_cast<size_t>(remaining));
        if (w <= 0)
            return false;
        p += w;
        remaining -= w;
    }
    return true;
}
#endif

// 从 stdin 读取一条 Native Messaging 消息（4 字节长度前缀 + JSON）
static QByteArray readNativeMessage()
{
#ifdef Q_OS_WIN
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    quint32 len = 0;
    if (!readExact(h, &len, 4))
        return QByteArray();
    len = qFromLittleEndian(len);
    if (len == 0 || len > 10 * 1024 * 1024)  // 最大 10MB
        return QByteArray();
    QByteArray body;
    body.resize(static_cast<int>(len));
    if (!readExact(h, body.data(), len))
        return QByteArray();
    return body;
#else
    int fd = STDIN_FILENO;
    quint32 len = 0;
    if (!readExact(fd, &len, 4))
        return QByteArray();
    len = qFromLittleEndian(len);
    if (len == 0 || len > 10 * 1024 * 1024)  // 最大 10MB
        return QByteArray();
    QByteArray body;
    body.resize(static_cast<int>(len));
    if (!readExact(fd, body.data(), len))
        return QByteArray();
    return body;
#endif
}

// 向 stdout 写入一条 Native Messaging 消息
static void writeNativeMessage(const QByteArray& json)
{
    quint32 len = qToLittleEndian(static_cast<quint32>(json.size()));
#ifdef Q_OS_WIN
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    writeExact(h, &len, 4);
    writeExact(h, json.constData(), json.size());
    FlushFileBuffers(h);
#else
    writeExact(STDOUT_FILENO, &len, 4);
    writeExact(STDOUT_FILENO, json.constData(), json.size());
#endif
}

// ── 定位 IDM Next 主程序 ─────────────────────────────────────────────

static QString resolveCli()
{
    // 1) 与 host 同目录（打包/安装后的标准位置）
    QString dir = QCoreApplication::applicationDirPath();
    QString cand = QDir(dir).filePath(QStringLiteral("idm-next.exe"));
    if (QFile::exists(cand))
        return cand;
    // 2) 开发构建目录（向上找 build / build_check）
    for (const QString& sub : {QStringLiteral("build"), QStringLiteral("build_check")}) {
        QString p = QDir(dir).filePath(sub + QStringLiteral("/idm-next.exe"));
        if (QFile::exists(p))
            return p;
    }
    // 3) 常见安装路径
    QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString prog = QStringLiteral("C:/Program Files/IDMNext/idm-next.exe");
    for (const QString& p : {QDir(local).filePath(QStringLiteral("IDMNext/idm-next.exe")),
                             prog,
                             QStringLiteral("D:/IDMNext/idm-next.exe")}) {
        if (QFile::exists(p))
            return p;
    }
    // 4) PATH
    return QString();
}

// ── 与 GUI 通信 ──────────────────────────────────────────────────────

// 通过 QLocalServer 把请求转发给运行中的 GUI。
// *connected 返回是否已连上 GUI（false = GUI 未运行，调用方应回退到 CLI）。
static QJsonObject forwardToGui(const QJsonObject& req, bool* connected)
{
    *connected = false;
    QLocalSocket sock;
    sock.connectToServer(QStringLiteral("idm-next-ipc"));
    if (!sock.waitForConnected(800))
        return QJsonObject();   // GUI 未运行
    *connected = true;

    QByteArray data = QJsonDocument(req).toJson(QJsonDocument::Compact);
    sock.write(data);
    sock.flush();
    if (!sock.waitForBytesWritten(2000))
        return QJsonObject();

    QJsonObject response;
    if (sock.waitForReadyRead(3000)) {
        QByteArray resp = sock.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(resp);
        if (doc.isObject())
            response = doc.object();
    }
    sock.disconnectFromServer();
    if (response.isEmpty()) {
        response["success"] = false;
        response["message"] = QStringLiteral("GUI 未响应");
    }
    return response;
}

// GUI 未运行时的回退：直接拉起无界面 CLI 添加任务
static QJsonObject cliAdd(const QString& url, const QString& filename,
                          const QString& saveDir, const QString& format)
{
    QString cli = resolveCli();
    if (cli.isEmpty())
        return {{QStringLiteral("success"), false},
                {QStringLiteral("message"), QStringLiteral("IDM Next 未找到，请确认已安装")}};

    QStringList args = {QStringLiteral("--cli"), QStringLiteral("add"), url,
                        QStringLiteral("--no-wait")};
    if (!filename.isEmpty()) {
        args << QStringLiteral("--name") << filename;
    }
    if (!saveDir.isEmpty()) {
        args << QStringLiteral("--dir") << saveDir;
    }
    if (!format.isEmpty()) {
        args << QStringLiteral("--format") << format;
    }

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(cli, args);
    if (!p.waitForFinished(30000))
        return {{QStringLiteral("success"), false},
                {QStringLiteral("message"), QStringLiteral("IDM Next 响应超时")}};

    QJsonObject o;
    o["success"] = (p.exitCode() == 0);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    o["message"] = out.isEmpty() ? QStringLiteral("已添加下载") : out;
    return o;
}

// 定位 yt-dlp 可执行文件（与 GUI 的 VideoDownloader::bundledYtDlpPath 逻辑一致）。
// 1) exe 同目录 /yt-dlp/yt-dlp.exe（便携模式或打包后的标准位置）
// 2) 非便携：AppData/Local/IDM Next/yt-dlp/yt-dlp.exe
// 3) 回退到 PATH
static QString findYtDlp()
{
    QString dir = QCoreApplication::applicationDirPath();
    QString cand = QDir(dir).filePath(QStringLiteral("yt-dlp/yt-dlp.exe"));
    if (QFile::exists(cand))
        return cand;
    QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    cand = QDir(local).filePath(QStringLiteral("yt-dlp/yt-dlp.exe"));
    if (QFile::exists(cand))
        return cand;
    return QStringLiteral("yt-dlp");
}

// 用 `yt-dlp -J` 获取视频格式列表并解析为结构化数组。
// 每个格式项：{ id, label, type, ext, filesize, height, fmt }
//   type: "combined" | "audio" | "video"
//   fmt : 直接传给 yt-dlp -f 的选择串（video-only 会补 +bestaudio）
static QJsonArray listYtDlpFormats(const QString& url)
{
    QJsonArray result;
    QString ydl = findYtDlp();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(ydl, QStringList() << QStringLiteral("-J")
                               << QStringLiteral("--no-warnings")
                               << QStringLiteral("--no-playlist")
                               << url);
    if (!p.waitForStarted(5000))
        return result;
    if (!p.waitForFinished(45000))
        return result;

    QByteArray out = p.readAllStandardOutput();
    QJsonDocument doc = QJsonDocument::fromJson(out);
    if (!doc.isObject())
        return result;

    QJsonArray fmts = doc.object().value(QStringLiteral("formats")).toArray();
    for (const QJsonValue& v : fmts) {
        QJsonObject f = v.toObject();
        QString fid     = f.value(QStringLiteral("format_id")).toString();
        QString ext     = f.value(QStringLiteral("ext")).toString();
        QString vcodec  = f.value(QStringLiteral("vcodec")).toString();
        QString acodec  = f.value(QStringLiteral("acodec")).toString();
        int     w   = f.value(QStringLiteral("width")).toInt(0);
        int     h   = f.value(QStringLiteral("height")).toInt(0);
        double  fps = f.value(QStringLiteral("fps")).toDouble(0);
        double  fs  = f.value(QStringLiteral("filesize")).toDouble(-1);
        if (fs < 0)
            fs = f.value(QStringLiteral("filesize_approx")).toDouble(-1);
        QString note = f.value(QStringLiteral("format_note")).toString();
        QString abr  = f.value(QStringLiteral("abr")).toString();

        bool hasVideo = (vcodec != QStringLiteral("none") && !vcodec.isEmpty());
        bool hasAudio = (acodec != QStringLiteral("none") && !acodec.isEmpty());

        QString type;
        QString label;
        QString fmt;
        if (!hasVideo && hasAudio) {
            type = QStringLiteral("audio");
            label = QStringLiteral("仅音频");
            if (!abr.isEmpty())
                label += QStringLiteral(" %1kbps").arg(abr);
            fmt = fid;
        } else if (hasVideo && hasAudio) {
            type = QStringLiteral("combined");
            label = QStringLiteral("视频+音频");
            fmt = fid;
        } else if (hasVideo && !hasAudio) {
            type = QStringLiteral("video");
            label = QStringLiteral("仅视频");
            fmt = fid + QStringLiteral("+bestaudio");
        } else {
            continue; // 既无视频也无音频，跳过
        }

        if (w > 0 && h > 0)
            label += QStringLiteral(" %1x%2").arg(w).arg(h);
        else if (h > 0)
            label += QStringLiteral(" %1p").arg(h);
        if (fps > 0)
            label += QStringLiteral(" %1fps").arg(QString::number(fps, 'f', 0));
        if (!ext.isEmpty())
            label += QStringLiteral(" (%1)").arg(ext);
        if (!note.isEmpty())
            label += QStringLiteral(" · %1").arg(note);

        QJsonObject item;
        item["id"]       = fid;
        item["label"]    = label;
        item["type"]     = type;
        item["ext"]      = ext;
        item["filesize"] = fs > 0 ? fs : -1;
        item["height"]   = h;
        item["fmt"]      = fmt;
        result.append(item);
    }
    return result;
}

// 处理 list_formats：查询某视频 URL 的可选画质/格式列表
static QJsonObject handleListFormats(const QString& url)
{
    QJsonObject o;
    if (url.isEmpty()) {
        o["success"] = false;
        o["message"] = QStringLiteral("URL 不能为空");
        return o;
    }
    if (!url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
        !url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        o["success"] = false;
        o["message"] = QStringLiteral("仅支持 http(s) 视频链接");
        return o;
    }
    QJsonArray formats = listYtDlpFormats(url);
    if (formats.isEmpty()) {
        o["success"] = false;
        o["message"] = QStringLiteral("无法获取格式列表（yt-dlp 未安装或网络失败）");
        return o;
    }
    o["success"] = true;
    o["formats"] = formats;
    return o;
}

// ── 各类操作处理 ─────────────────────────────────────────────────────

static QJsonObject handlePing()
{
    QLocalSocket sock;
    sock.connectToServer(QStringLiteral("idm-next-ipc"));
    bool up = sock.waitForConnected(800);
    sock.disconnectFromServer();
    QJsonObject o;
    o["success"] = up;
    o["version"] = QStringLiteral("0.3.0");
    o["message"] = up ? QStringLiteral("pong") : QStringLiteral("IDM Next 未在运行");
    return o;
}

static QJsonObject handleAddDownload(const QString& url, const QString& filename,
                                     const QString& saveDir, const QString& queue,
                                     const QString& format)
{
    if (url.isEmpty())
        return {{QStringLiteral("success"), false},
                {QStringLiteral("message"), QStringLiteral("URL 不能为空")}};

    QJsonObject req;
    req["command"] = QStringLiteral("add");
    req["url"] = url;
    req["filename"] = filename;
    if (!saveDir.isEmpty())
        req["dir"] = saveDir;          // 浏览器端“另存到…”选择的目录
    if (!queue.isEmpty())
        req["queue"] = queue;          // 浏览器端“加入队列”选择
    if (!format.isEmpty())
        req["format"] = format;        // 浏览器端选择的视频画质（yt-dlp -f 选择串）

    bool connected = false;
    QJsonObject r = forwardToGui(req, &connected);
    if (!connected)
        r = cliAdd(url, filename, saveDir, format);
    return r;
}

static QJsonObject handleBatchAdd(const QJsonArray& items)
{
    int added = 0, failed = 0;
    QStringList errors;
    for (const QJsonValue& v : items) {
        QJsonObject it = v.toObject();
        QString url = it.value(QStringLiteral("url")).toString();
        QString fn = it.value(QStringLiteral("filename")).toString();
        QString dir = it.value(QStringLiteral("dir")).toString();
        QString q = it.value(QStringLiteral("queue")).toString();
        QString fmt = it.value(QStringLiteral("format")).toString();
        if (url.isEmpty()) {
            ++failed;
            continue;
        }
        QJsonObject r = handleAddDownload(url, fn, dir, q, fmt);
        if (r.value(QStringLiteral("success")).toBool())
            ++added;
        else {
            ++failed;
            errors << r.value(QStringLiteral("message")).toString();
        }
    }
    QJsonObject o;
    o["success"] = added > 0;
    o["added"] = added;
    o["failed"] = failed;
    QString msg = QStringLiteral("已添加 %1 个任务").arg(added);
    if (failed > 0)
        msg += QStringLiteral("，%1 个失败").arg(failed);
    o["message"] = msg;
    return o;
}

static QJsonObject handleGetStatus()
{
    QLocalSocket sock;
    sock.connectToServer(QStringLiteral("idm-next-ipc"));
    if (!sock.waitForConnected(800))
        return {{QStringLiteral("success"), false},
                {QStringLiteral("running"), false},
                {QStringLiteral("message"), QStringLiteral("IDM Next 未在运行")}};

    QJsonObject req;
    req["command"] = QStringLiteral("list");
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.flush();
    sock.waitForBytesWritten(2000);

    QJsonObject response;
    if (sock.waitForReadyRead(3000)) {
        QJsonObject r = QJsonDocument::fromJson(sock.readAll()).object();
        response["success"] = true;
        response["running"] = true;
        response["message"] = r.value(QStringLiteral("message")).toString();
    } else {
        response["success"] = false;
        response["running"] = false;
        response["message"] = QStringLiteral("IDM Next 未响应");
    }
    sock.disconnectFromServer();
    return response;
}

// ── 主消息循环 ───────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    // 与 GUI 主程序保持一致的应用名，确保 AppPaths::dataDir()（AppData/Local/IDM Next）
    // 在 host 进程中计算出的路径一致，从而能找到 GUI 已下载/捆绑的 yt-dlp。
    app.setApplicationName(QStringLiteral("IDM Next"));

    while (true) {
        QByteArray raw = readNativeMessage();
        if (raw.isEmpty())
            break;   // stdin EOF（浏览器关闭 host）

        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject())
            continue;

        QJsonObject msg = doc.object();
        QString action = msg.value(QStringLiteral("action")).toString();
        QJsonObject response;

        if (action == QStringLiteral("ping")) {
            response = handlePing();
        } else if (action == QStringLiteral("add_download")) {
            response = handleAddDownload(
                msg.value(QStringLiteral("url")).toString(),
                msg.value(QStringLiteral("filename")).toString(),
                msg.value(QStringLiteral("saveDir")).toString(),
                msg.value(QStringLiteral("queue")).toString(),
                msg.value(QStringLiteral("format")).toString());
        } else if (action == QStringLiteral("list_formats")) {
            response = handleListFormats(msg.value(QStringLiteral("url")).toString());
        } else if (action == QStringLiteral("batch_add")) {
            response = handleBatchAdd(msg.value(QStringLiteral("items")).toArray());
        } else if (action == QStringLiteral("get_status")) {
            response = handleGetStatus();
        } else {
            response["success"] = false;
            response["message"] = QStringLiteral("未知操作: ") + action;
        }

        writeNativeMessage(QJsonDocument(response).toJson(QJsonDocument::Compact));
    }

    return 0;
}
