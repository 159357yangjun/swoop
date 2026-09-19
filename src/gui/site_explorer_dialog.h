#ifndef SITE_EXPLORER_DIALOG_H
#define SITE_EXPLORER_DIALOG_H

#include <QDialog>
#include <QNetworkAccessManager>
#include <QList>
#include <QMap>
#include <QSet>
#include <QHash>
#include <QQueue>
#include <QPair>
#include <QTimer>

class QLineEdit;
class QTableWidget;
class QPushButton;
class QLabel;
class QProgressBar;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QNetworkReply;

// 站点抓取器（对齐 IDM Site Grabber）：输入网页 URL，抓取并解析页面内所有链接，
// 按文件类型分类多选筛选后，勾选批量加入下载（可指定队列）。
// 增强：递归整站抓取（最大深度 / 仅同域）+ 按文件类型自动归档到子目录。
class SiteExplorerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SiteExplorerDialog(const QList<QString>& queueNames,
                                const QString& defaultSaveDir,
                                QWidget* parent = nullptr);

signals:
    void requestAddUrl(const QString& url);
    void requestAddUrlToQueue(const QString& url, const QString& queue);
    // 按类型归档：调用方已拼好 saveDir/<分类>/ 子目录，后端不再二次归档
    void requestAddUrlWithDir(const QString& url, const QString& dir, const QString& queue);

private slots:
    void onFetch();
    void onReplyFinished(QNetworkReply* reply);
    void onStop();
    void onSelectAll(bool checked);
    void onInvert();
    void onDownloadSelected();
    void onFilterChanged();
    void onCategoryToggled();
    void onTemplateChanged(int idx);
    void onReloadPlugins();
    void onRecursiveToggled(bool on);
    void onSelectionChanged();             // 勾选变化 → 刷新「下载选中(N)」

private:
    struct LinkItem { QString url; QString type; QString fileName; };

    void parseHtml(const QString& html, const QUrl& base);
    void addPluginLinks(const QUrl& base, const QString& html);
    void applyFilter();
    void addRow(const LinkItem& item);
    void setAllCategories(bool on);

    // 按扩展名归类（与左侧树分类一致），供 parseHtml 与插件链接复用
    static QString classifyType(const QString& fileName);
    // 文件类型 → 归档子目录名（与 MainWindow 全局归档保持一致：视频/音乐/压缩包/文档/图片/程序/其他）
    static QString folderForType(const QString& type);
    // 该链接是否「像网页」（决定递归时是否继续作为子页抓取）
    static bool looksLikePage(const QUrl& url);
    // 实时统计各类型发现数，刷新 m_catStatLabel
    void updateCategoryStats();

    // ── 递归整站抓取 ──
    void enqueuePage(const QUrl& url, int level);   // 记录 visited，按并发上限入队/直发
    void startGet(const QUrl& url, int level);      // 真正发起一次 GET
    void crawlLinksFrom(const QString& html, const QUrl& base, int level); // 抽取同域子页入队
    void pumpQueue();                               // 有空槽时从待抓队列补充
    void maybeFinishCrawl();                        // 无在途请求且无待抓 → 收尾
    void scheduleUiRefresh();                       // 防抖合并 UI 刷新

    QLineEdit*     m_urlEdit;
    QTableWidget*  m_table;
    QPushButton*   m_fetchBtn;
    QPushButton*   m_stopBtn;
    QPushButton*   m_dlBtn;
    QPushButton*   m_reloadPluginsBtn;
    QLabel*        m_statusLabel;
    QLabel*        m_catStatLabel;          // 分类统计（实时显示各类型发现数）
    QProgressBar*  m_progress;
    QLineEdit*     m_filterEdit;
    QCheckBox*     m_onlyFilesCheck;
    QComboBox*     m_queueCombo;
    QComboBox*     m_templateCombo;          // 模板预设
    QMap<QString, QCheckBox*> m_catChecks;   // 分类复选框（视频/音频/...）
    QMap<QString, bool>       m_catEnabled;  // 各分类是否勾选

    // ── 递归 / 归档控件 ──
    QCheckBox*     m_recursiveCheck;         // 递归整站
    QSpinBox*      m_depthSpin;              // 最大深度（页级）
    QCheckBox*     m_sameDomainCheck;        // 仅同域
    QCheckBox*     m_archiveCheck;           // 按类型归档到子目录

    QNetworkAccessManager* m_nam = nullptr;
    QList<LinkItem> m_links;
    QString m_defaultDir;
    bool m_internal = false;   // 程序化改复选框时抑制 onCategoryToggled

    // ── 递归抓取状态 ──
    static const int kMaxConcurrent = 6;     // 并发页面抓取上限（生产者/消费者信号量）
    bool        m_crawling = false;          // 是否处于整站递归中
    QString     m_baseHost;                  // 起始页主机（仅同域判定）
    QSet<QString> m_visited;                 // 已入队/已抓页面 URL（去重）
    QSet<QString> m_linkSeen;                // 已收集文件链接 URL（O(1) 去重）
    QQueue<QPair<QUrl, int>> m_pendingPages; // 待抓子页（并发达上限时排队）
    QHash<QNetworkReply*, int> m_replyLevel; // 在途请求 → 页级
    QSet<QNetworkReply*> m_inflight;         // 在途请求（停止时 abort）
    int m_pagesDone = 0;                     // 已完成页面数
    QTimer*      m_uiTimer = nullptr;        // UI 刷新防抖定时器
};

#endif // SITE_EXPLORER_DIALOG_H
