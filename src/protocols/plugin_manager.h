#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QList>
#include <QRegularExpression>

// 站点解析插件管理器（JDownloader 式）：
//   - 插件以 JSON 规则文件描述，从 plugins 目录热加载（QFileSystemWatcher 监听，编辑即生效）
//   - 每个插件声明生效 host 列表 + 一组抽取规则（regex / linkify），用于从页面 HTML 中
//     精准提取下载直链（比通用 href/src 解析更针对特定站点）
//   - 首次运行自动从资源（:/plugins）播种示例插件到可写目录，用户可直接增删改并热重载
class PluginManager : public QObject {
    Q_OBJECT
public:
    struct ExtractRule {
        QRegularExpression pattern;
        int group = 0;            // 捕获组序号（0=整段匹配）
        QString prefix;           // 可选前缀（相对链接补全，一般留空用页面 base 解析）
    };

    struct Plugin {
        QString name;
        QString description;
        QStringList hosts;        // 生效域名；含 "*" 表示全部站点
        bool enabled = true;
        QList<ExtractRule> rules;
    };

    static PluginManager& instance();

    // 插件目录（可写，热加载来源）。默认 AppData/plugins，可在设置中覆盖。
    void setPluginDir(const QString& dir);
    QString pluginDir() const { return m_dir; }

    // 重新扫描目录并加载插件（热重载入口，文件变更时自动调用）
    void reload();

    // 依据页面 URL 与 HTML，套用匹配插件抽取下载直链（绝对、去重、仅 http(s)/ftp）
    QStringList extractLinks(const QUrl& pageUrl, const QString& html) const;

    QList<Plugin> plugins() const { return m_plugins; }
    int pluginCount() const { return m_plugins.size(); }
    int enabledCount() const;

signals:
    void pluginsReloaded();

private:
    PluginManager();
    void ensureWatcher();
    void installBundledPlugins();   // 首次运行从 :/plugins 播种
    static bool parsePlugin(const QString& path, Plugin& out, QString* err);

    QList<Plugin> m_plugins;
    QString       m_dir;
    class QFileSystemWatcher* m_watcher = nullptr;
};

#endif // PLUGIN_MANAGER_H
