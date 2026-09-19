#include "plugin_manager.h"
#include "logger.h"
#include "app_paths.h"

#include <QFileSystemWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QCoreApplication>

PluginManager& PluginManager::instance()
{
    static PluginManager m;
    return m;
}

PluginManager::PluginManager()
{
    // 默认插件目录：便携 = exe 目录/plugins，否则 AppData/Local/idm-next/plugins
    QString local = AppPaths::dataDir();
    if (local.isEmpty())
        local = QCoreApplication::applicationDirPath();
    m_dir = local + QStringLiteral("/plugins");
    if (!QDir().mkpath(m_dir))
        m_dir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");

    installBundledPlugins();
    reload();
    ensureWatcher();
}

int PluginManager::enabledCount() const
{
    int n = 0;
    for (const auto& p : m_plugins)
        if (p.enabled) ++n;
    return n;
}

void PluginManager::setPluginDir(const QString& dir)
{
    if (dir.isEmpty() || dir == m_dir)
        return;
    m_dir = dir;
    QDir().mkpath(m_dir);
    if (m_watcher) {
        QStringList paths = m_watcher->directories();
        if (!paths.isEmpty())
            m_watcher->removePaths(paths);
        m_watcher->addPath(m_dir);
    }
    reload();
}

void PluginManager::ensureWatcher()
{
    if (m_watcher)
        return;
    m_watcher = new QFileSystemWatcher(this);
    m_watcher->addPath(m_dir);
    // 目录内任意文件增删改（含编辑器“另存替换”）都会触发 directoryChanged → 热重载
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString&) {
                Log::info(QStringLiteral("插件目录变更，热重载站点解析插件"));
                reload();
                emit pluginsReloaded();
            });
}

// 首次运行：把资源中的示例插件（:/plugins/*.json）复制到可写目录，供用户热编辑
void PluginManager::installBundledPlugins()
{
    QDir target(m_dir);
    if (!target.entryList(QStringList() << QStringLiteral("*.json")).isEmpty())
        return;  // 已有插件，不覆盖用户改动

    QDir res(QStringLiteral(":/plugins"));
    QStringList bundled = res.entryList(QStringList() << QStringLiteral("*.json"));
    for (const QString& f : bundled) {
        QFile src(QStringLiteral(":/plugins/") + f);
        if (!src.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = src.readAll();
        src.close();
        QFile dst(m_dir + QStringLiteral("/") + f);
        if (dst.open(QIODevice::WriteOnly) && dst.write(data) == data.size())
            Log::info(QStringLiteral("已安装示例插件: %1").arg(f));
    }
}

bool PluginManager::parsePlugin(const QString& path, Plugin& out, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开");
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (doc.isNull()) {
        if (err) *err = pe.errorString();
        return false;
    }
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("根必须是 JSON 对象");
        return false;
    }
    QJsonObject o = doc.object();
    out.name = o.value(QStringLiteral("name")).toString(path);
    out.description = o.value(QStringLiteral("description")).toString();
    out.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    for (const auto& v : o.value(QStringLiteral("hosts")).toArray())
        out.hosts.append(v.toString());

    for (const auto& rv : o.value(QStringLiteral("extract")).toArray()) {
        if (!rv.isObject())
            continue;
        QJsonObject ro = rv.toObject();
        QString pat = ro.value(QStringLiteral("pattern")).toString();
        if (pat.isEmpty())
            continue;
        QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption |
                                     QRegularExpression::DotMatchesEverythingOption);
        if (!re.isValid()) {
            if (err) *err = QStringLiteral("无效正则: %1").arg(pat);
            continue;
        }
        ExtractRule rule;
        rule.pattern = re;
        rule.group = ro.value(QStringLiteral("group")).toInt(0);
        rule.prefix = ro.value(QStringLiteral("prefix")).toString();
        out.rules.append(rule);
    }
    return true;
}

void PluginManager::reload()
{
    QList<Plugin> loaded;
    QDir dir(m_dir);
    QStringList files = dir.entryList(QStringList() << QStringLiteral("*.json"),
                                      QDir::Files, QDir::Name);
    for (const QString& f : files) {
        Plugin p;
        QString err;
        if (!parsePlugin(m_dir + QStringLiteral("/") + f, p, &err)) {
            Log::warn(QStringLiteral("插件加载失败 %1: %2").arg(f).arg(err));
            continue;
        }
        Log::debug(QStringLiteral("已加载插件: %1 (hosts=%2, rules=%3)")
                       .arg(p.name).arg(p.hosts.join(',')).arg(p.rules.size()));
        loaded.append(p);
    }
    m_plugins = loaded;
    Log::info(QStringLiteral("站点解析插件重载完成，共 %1 个（启用 %2）")
                  .arg(m_plugins.size()).arg(enabledCount()));
}

QStringList PluginManager::extractLinks(const QUrl& pageUrl, const QString& html) const
{
    QStringList out;
    QString host = pageUrl.host().toLower();

    auto addLink = [&](QString raw) {
        raw = raw.trimmed();
        if (raw.isEmpty())
            return;
        if (!raw.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
            !raw.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) &&
            !raw.startsWith(QStringLiteral("ftp://"), Qt::CaseInsensitive) &&
            !raw.startsWith(QStringLiteral("//"))) {
            // 相对链接：用页面 base 补全
            QUrl resolved = pageUrl.resolved(QUrl::fromUserInput(raw));
            if (!resolved.isValid())
                return;
            raw = resolved.toString();
        } else if (raw.startsWith(QStringLiteral("//"))) {
            raw = QStringLiteral("https:") + raw;
        }
        QUrl u(raw);
        QString scheme = u.scheme().toLower();
        if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https") &&
            scheme != QStringLiteral("ftp"))
            return;
        if (!out.contains(raw))
            out.append(raw);
    };

    for (const auto& p : m_plugins) {
        if (!p.enabled)
            continue;
        bool hostMatch = false;
        for (const QString& h : p.hosts) {
            QString hl = h.toLower();
            if (hl == QStringLiteral("*") || host.endsWith(hl) || hl.endsWith(host)) {
                hostMatch = true;
                break;
            }
        }
        if (!hostMatch)
            continue;
        for (const auto& rule : p.rules) {
            QRegularExpressionMatchIterator it = rule.pattern.globalMatch(html);
            while (it.hasNext()) {
                QRegularExpressionMatch m = it.next();
                QString captured = (rule.group > 0 && m.capturedTexts().size() > rule.group)
                                       ? m.captured(rule.group) : m.captured(0);
                if (!captured.isEmpty()) {
                    if (!rule.prefix.isEmpty())
                        captured = rule.prefix + captured;
                    addLink(captured);
                }
            }
        }
    }
    return out;
}
