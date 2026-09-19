#include "site_explorer_dialog.h"
#include "plugin_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>

// IDM Site Grabber 风格的文件分类（与左侧树分类保持一致）
static const QStringList kCategories = {
    QStringLiteral("视频"), QStringLiteral("音频"), QStringLiteral("压缩文件"),
    QStringLiteral("文档"), QStringLiteral("图片"), QStringLiteral("程序"),
    QStringLiteral("其他")
};

SiteExplorerDialog::SiteExplorerDialog(const QList<QString>& queueNames,
                                       const QString& defaultSaveDir,
                                       QWidget* parent)
    : QDialog(parent), m_defaultDir(defaultSaveDir)
{
    setWindowTitle(QStringLiteral("站点抓取器"));
    resize(820, 620);
    setMinimumSize(720, 540);

    // ── 顶部：网址 + 抓取 / 停止 ──
    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QStringLiteral("输入网页地址，例如 https://example.com/video-page"));
    m_fetchBtn = new QPushButton(QStringLiteral("抓取"), this);
    m_stopBtn = new QPushButton(QStringLiteral("停止"), this);
    m_stopBtn->setEnabled(false);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->hide();

    m_statusLabel = new QLabel(QStringLiteral("等待抓取..."), this);
    m_catStatLabel = new QLabel(QStringLiteral("尚未发现链接"), this);
    m_catStatLabel->setStyleSheet(QStringLiteral("color:#666; font-size:12px;"));

    // ── 筛选栏：模板预设 + 扩展名 + 仅文件 ──
    m_templateCombo = new QComboBox(this);
    m_templateCombo->addItems({
        QStringLiteral("全部文件"), QStringLiteral("仅音视频"),
        QStringLiteral("仅压缩包"), QStringLiteral("仅文档图片"),
        QStringLiteral("自定义") });
    m_templateCombo->setCurrentIndex(0);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("扩展名过滤，如 mp4,mkv,zip（留空=全部）"));
    m_onlyFilesCheck = new QCheckBox(QStringLiteral("仅显示文件链接"), this);
    m_onlyFilesCheck->setChecked(true);

    // ── 分类复选框（IDM 风格按类型多选） ──
    QHBoxLayout* catRow = new QHBoxLayout;
    catRow->addWidget(new QLabel(QStringLiteral("文件类型:"), this));
    for (const QString& c : kCategories) {
        auto* cb = new QCheckBox(c, this);
        cb->setChecked(true);
        m_catChecks[c] = cb;
        m_catEnabled[c] = true;
        connect(cb, &QCheckBox::toggled, this, &SiteExplorerDialog::onCategoryToggled);
        catRow->addWidget(cb);
    }
    catRow->addStretch(1);

    // ── 递归整站选项 ──
    m_recursiveCheck = new QCheckBox(QStringLiteral("递归整站"), this);
    m_recursiveCheck->setChecked(false);
    m_depthSpin = new QSpinBox(this);
    m_depthSpin->setRange(1, 10);
    m_depthSpin->setValue(3);
    m_depthSpin->setEnabled(false);
    m_sameDomainCheck = new QCheckBox(QStringLiteral("仅同域"), this);
    m_sameDomainCheck->setChecked(true);

    QHBoxLayout* recRow = new QHBoxLayout;
    recRow->addWidget(m_recursiveCheck);
    recRow->addWidget(new QLabel(QStringLiteral("最大深度"), this));
    recRow->addWidget(m_depthSpin);
    recRow->addWidget(m_sameDomainCheck);
    recRow->addStretch(1);

    // ── 加入队列 + 归档 ──
    m_queueCombo = new QComboBox(this);
    m_queueCombo->addItem(QStringLiteral("不加入队列（立即下载）"), QString());
    for (const auto& q : queueNames)
        m_queueCombo->addItem(QStringLiteral("加入队列：%1").arg(q), q);

    m_archiveCheck = new QCheckBox(QStringLiteral("按类型归档到子目录"), this);
    m_archiveCheck->setChecked(true);
    m_archiveCheck->setToolTip(QStringLiteral("开启后文件归入 saveDir/视频、saveDir/压缩包 等子目录"));

    // ── 结果表格（列：选择/文件名/类型/链接） ──
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("选择"), QStringLiteral("文件名"),
        QStringLiteral("类型"), QStringLiteral("链接") });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);

    QPushButton* selectAllBtn = new QPushButton(QStringLiteral("全选"), this);
    QPushButton* clearBtn = new QPushButton(QStringLiteral("全不选"), this);
    QPushButton* invertBtn = new QPushButton(QStringLiteral("反选"), this);
    m_reloadPluginsBtn = new QPushButton(QStringLiteral("重新加载插件"), this);
    m_dlBtn = new QPushButton(QStringLiteral("下载选中"), this);
    m_dlBtn->setEnabled(false);

    QDialogButtonBox* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);

    // 布局
    QHBoxLayout* top = new QHBoxLayout;
    top->addWidget(m_urlEdit, 1);
    top->addWidget(m_fetchBtn);
    top->addWidget(m_stopBtn);

    QHBoxLayout* filter = new QHBoxLayout;
    filter->addWidget(new QLabel(QStringLiteral("模板:"), this));
    filter->addWidget(m_templateCombo);
    filter->addWidget(m_filterEdit, 1);
    filter->addWidget(m_onlyFilesCheck);

    QHBoxLayout* tail = new QHBoxLayout;
    tail->addWidget(new QLabel(QStringLiteral("加入:"), this));
    tail->addWidget(m_queueCombo, 1);
    tail->addWidget(m_archiveCheck);

    QHBoxLayout* actions = new QHBoxLayout;
    actions->addWidget(selectAllBtn);
    actions->addWidget(clearBtn);
    actions->addWidget(invertBtn);
    actions->addWidget(m_reloadPluginsBtn);
    actions->addStretch(1);
    actions->addWidget(m_dlBtn);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->addLayout(top);
    root->addLayout(filter);
    root->addLayout(catRow);
    root->addLayout(recRow);
    root->addWidget(m_progress);
    root->addWidget(m_statusLabel);
    root->addWidget(m_catStatLabel);
    root->addWidget(m_table, 1);
    root->addLayout(tail);
    root->addLayout(actions);
    root->addWidget(box);

    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &SiteExplorerDialog::onReplyFinished);

    // UI 刷新防抖定时器：多次快速回包合并为一次表格重建
    m_uiTimer = new QTimer(this);
    m_uiTimer->setSingleShot(true);
    m_uiTimer->setInterval(150);
    connect(m_uiTimer, &QTimer::timeout, this, &SiteExplorerDialog::applyFilter);
    connect(m_fetchBtn, &QPushButton::clicked, this, &SiteExplorerDialog::onFetch);
    connect(m_stopBtn, &QPushButton::clicked, this, &SiteExplorerDialog::onStop);
    connect(selectAllBtn, &QPushButton::clicked, this, [this](bool) { onSelectAll(true); });
    connect(clearBtn, &QPushButton::clicked, this, [this](bool) { onSelectAll(false); });
    connect(invertBtn, &QPushButton::clicked, this, &SiteExplorerDialog::onInvert);
    connect(m_reloadPluginsBtn, &QPushButton::clicked, this, &SiteExplorerDialog::onReloadPlugins);
    connect(m_dlBtn, &QPushButton::clicked, this, &SiteExplorerDialog::onDownloadSelected);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &SiteExplorerDialog::onFilterChanged);
    connect(m_onlyFilesCheck, &QCheckBox::toggled, this, &SiteExplorerDialog::onFilterChanged);
    connect(m_templateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SiteExplorerDialog::onTemplateChanged);
    connect(m_recursiveCheck, &QCheckBox::toggled, this, &SiteExplorerDialog::onRecursiveToggled);
    connect(m_table, &QTableWidget::itemChanged, this, &SiteExplorerDialog::onSelectionChanged);
}

void SiteExplorerDialog::setAllCategories(bool on)
{
    m_internal = true;
    for (const QString& c : kCategories) {
        m_catChecks[c]->setChecked(on);
        m_catEnabled[c] = on;
    }
    m_internal = false;
}

void SiteExplorerDialog::onReloadPlugins()
{
    PluginManager::instance().reload();
    int total = PluginManager::instance().pluginCount();
    int enabled = PluginManager::instance().enabledCount();
    m_statusLabel->setText(QStringLiteral("已重新加载站点解析插件：共 %1 个（启用 %2 个），目录：%3")
                               .arg(total).arg(enabled)
                               .arg(PluginManager::instance().pluginDir()));
}

void SiteExplorerDialog::onTemplateChanged(int idx)
{
    // 模板预设：按 IDM 习惯锁定勾选状态；自定义则不改动
    m_internal = true;
    switch (idx) {
        case 0: setAllCategories(true);  break;                       // 全部文件
        case 1: {                                                       // 仅音视频
            for (const QString& c : kCategories)
                m_catChecks[c]->setChecked(
                    c == QStringLiteral("视频") || c == QStringLiteral("音频"));
            break;
        }
        case 2: {                                                       // 仅压缩包
            for (const QString& c : kCategories)
                m_catChecks[c]->setChecked(c == QStringLiteral("压缩文件"));
            break;
        }
        case 3: {                                                       // 仅文档图片
            for (const QString& c : kCategories)
                m_catChecks[c]->setChecked(
                    c == QStringLiteral("文档") || c == QStringLiteral("图片"));
            break;
        }
        default: m_internal = false; return;  // 自定义
    }
    m_internal = false;
    for (const QString& c : kCategories)
        m_catEnabled[c] = m_catChecks[c]->isChecked();
    applyFilter();
}

void SiteExplorerDialog::onCategoryToggled()
{
    if (m_internal)
        return;
    for (const QString& c : kCategories)
        m_catEnabled[c] = m_catChecks[c]->isChecked();
    // 手动改动分类视为「自定义」
    m_templateCombo->setCurrentIndex(m_templateCombo->count() - 1);
    applyFilter();
}

void SiteExplorerDialog::onRecursiveToggled(bool on)
{
    m_depthSpin->setEnabled(on);
}

void SiteExplorerDialog::onFetch()
{
    if (m_crawling) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("整站抓取进行中，请先点「停止」"));
        return;
    }
    QUrl url(m_urlEdit->text().trimmed());
    if (!url.isValid() || url.scheme().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请输入有效的网页地址"));
        return;
    }

    // 重置状态
    m_table->setRowCount(0);
    m_links.clear();
    m_dlBtn->setEnabled(false);
    m_visited.clear();
    m_linkSeen.clear();
    m_replyLevel.clear();
    m_inflight.clear();
    m_pendingPages.clear();
    m_uiTimer->stop();
    m_pagesDone = 0;
    m_baseHost = url.host().toLower();

    m_progress->show();
    m_fetchBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);

    bool recursive = m_recursiveCheck->isChecked();
    m_crawling = recursive;   // 单页模式不进入 crawling 状态（一次 finished 即收尾）

    if (recursive) {
        m_statusLabel->setText(QStringLiteral("开始整站抓取 %1（最大深度 %2，%3）...")
                                   .arg(url.toString())
                                   .arg(m_depthSpin->value())
                                   .arg(m_sameDomainCheck->isChecked()
                                        ? QStringLiteral("仅同域") : QStringLiteral("不限域名")));
    } else {
        m_statusLabel->setText(QStringLiteral("正在抓取 %1 ...").arg(url.toString()));
    }

    enqueuePage(url, 1);   // level 1 = 起始页
}

void SiteExplorerDialog::enqueuePage(const QUrl& url, int level)
{
    QString u = url.toString();
    if (m_visited.contains(u))
        return;
    m_visited.insert(u);
    // 并发未达上限 → 直接发；否则进待抓队列，由 pumpQueue 在有空槽时补充
    if (m_inflight.size() < kMaxConcurrent)
        startGet(url, level);
    else
        m_pendingPages.enqueue(qMakePair(url, level));
}

void SiteExplorerDialog::startGet(const QUrl& url, int level)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                 "AppleWebKit/537.36 (KHTML, like Gecko) "
                                 "Chrome/120.0 Safari/537.36"));
    QNetworkReply* reply = m_nam->get(req);
    m_replyLevel[reply] = level;
    m_inflight.insert(reply);
}

// 有空槽时从待抓队列补充页面（生产者/消费者模式的消费者侧）
void SiteExplorerDialog::pumpQueue()
{
    while (!m_pendingPages.isEmpty() && m_inflight.size() < kMaxConcurrent) {
        auto p = m_pendingPages.dequeue();
        startGet(p.first, p.second);
    }
}

void SiteExplorerDialog::onReplyFinished(QNetworkReply* reply)
{
    int level = m_replyLevel.value(reply, 1);
    m_replyLevel.remove(reply);
    m_inflight.remove(reply);

    if (reply->error() != QNetworkReply::NoError) {
        // 单页模式：直接报错；递归模式：跳过该页继续
        if (!m_crawling) {
            m_progress->hide();
            m_fetchBtn->setEnabled(true);
            m_stopBtn->setEnabled(false);
            m_statusLabel->setText(QStringLiteral("抓取失败：%1").arg(reply->errorString()));
            reply->deleteLater();
            return;
        }
    } else {
        QUrl base = reply->url();
        QString html = QString::fromUtf8(reply->readAll());
        // 解析本页文件链接 + 插件直链
        parseHtml(html, base);
        addPluginLinks(base, html);
        // 递归：抽取同域子页入队（级 +1，受最大深度限制）
        if (m_crawling)
            crawlLinksFrom(html, base, level);
    }

    ++m_pagesDone;

    if (m_crawling) {
        pumpQueue();   // 有空槽则补充待抓页面
        if (m_inflight.isEmpty() && m_pendingPages.isEmpty())
            maybeFinishCrawl();   // 无在途请求且无待抓 → 收尾
        else {
            m_statusLabel->setText(QStringLiteral("整站抓取中… 已抓 %1 页，发现 %2 个链接，在途 %3 页")
                                       .arg(m_pagesDone).arg(m_links.size()).arg(m_inflight.size()));
            scheduleUiRefresh();   // 防抖合并表格刷新
        }
    } else {
        m_progress->hide();
        m_fetchBtn->setEnabled(true);
        m_stopBtn->setEnabled(false);
        applyFilter();
        int pluginHits = PluginManager::instance().enabledCount();
        m_statusLabel->setText(QStringLiteral("抓取完成，共发现 %1 个链接（启用插件 %2 个），过滤后显示 %3 个")
                                   .arg(m_links.size()).arg(pluginHits).arg(m_table->rowCount()));
    }
    reply->deleteLater();
}

void SiteExplorerDialog::crawlLinksFrom(const QString& html, const QUrl& base, int level)
{
    int maxDepth = m_depthSpin->value();
    if (level >= maxDepth)   // 已达最大深度，不再深挖
        return;

    static const QRegularExpression re(
        "(?:href|src|data-src)\\s*=\\s*[\"']([^\"']+)[\"']",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = re.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        QString raw = m.captured(1).trimmed();
        if (raw.isEmpty() || raw.startsWith(QStringLiteral("javascript:")) ||
            raw.startsWith(QStringLiteral("mailto:")) || raw.startsWith(QLatin1Char('#')) ||
            raw.startsWith(QStringLiteral("data:")) ||
            raw.startsWith(QStringLiteral("ftp:")))   // 子页仅走 http/https
            continue;
        QUrl resolved = base.resolved(QUrl::fromUserInput(raw));
        if (!resolved.isValid())
            continue;
        QString scheme = resolved.scheme().toLower();
        if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
            continue;
        if (m_sameDomainCheck->isChecked()) {
            QString host = resolved.host().toLower();
            // 允许 www. 与非 www. 视为同域
            QString a = m_baseHost, b = host;
            if (a.startsWith(QStringLiteral("www."))) a = a.mid(4);
            if (b.startsWith(QStringLiteral("www."))) b = b.mid(4);
            if (a != b)
                continue;
        }
        // 仅把「像网页」的链接当作子页继续抓取；图片/CSS/JS/音视频等文件
        // 链接仍由 parseHtml 收集为可下载项，但不再作为子页抓取，避免无谓请求
        if (!looksLikePage(resolved))
            continue;
        enqueuePage(resolved, level + 1);   // 内部已做 visited 去重
    }
}

void SiteExplorerDialog::maybeFinishCrawl()
{
    m_crawling = false;
    m_progress->hide();
    m_fetchBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_uiTimer->stop();          // 取消挂起的防抖刷新
    applyFilter();              // 末次全量刷新
    m_statusLabel->setText(QStringLiteral("整站抓取完成：共抓 %1 页，发现 %2 个链接，过滤后显示 %3 个")
                               .arg(m_pagesDone).arg(m_links.size()).arg(m_table->rowCount()));
}

void SiteExplorerDialog::onStop()
{
    if (!m_crawling && m_inflight.isEmpty())
        return;
    // 终止在途请求与待抓队列
    for (QNetworkReply* r : m_inflight)
        r->abort();
    m_inflight.clear();
    m_replyLevel.clear();
    m_pendingPages.clear();
    m_crawling = false;
    m_progress->hide();
    m_fetchBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_uiTimer->stop();
    applyFilter();
    m_statusLabel->setText(QStringLiteral("已停止抓取：共抓 %1 页，发现 %2 个链接，过滤后显示 %3 个")
                               .arg(m_pagesDone).arg(m_links.size()).arg(m_table->rowCount()));
}

// 防抖刷新：多次快速回包合并为一次表格重建，避免每页一次 O(n) 全量重绘
void SiteExplorerDialog::scheduleUiRefresh()
{
    if (!m_uiTimer->isActive())
        m_uiTimer->start(150);
}

void SiteExplorerDialog::parseHtml(const QString& html, const QUrl& base)
{
    static const QRegularExpression re(
        "(?:href|src|data-src)\\s*=\\s*[\"']([^\"']+)[\"']",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = re.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        QString raw = m.captured(1).trimmed();
        if (raw.isEmpty() || raw.startsWith(QStringLiteral("javascript:")) ||
            raw.startsWith(QStringLiteral("mailto:")) || raw.startsWith(QLatin1Char('#')) ||
            raw.startsWith(QStringLiteral("data:")))
            continue;
        QUrl resolved = base.resolved(QUrl::fromUserInput(raw));
        if (!resolved.isValid())
            continue;
        QString scheme = resolved.scheme().toLower();
        if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https") &&
            scheme != QStringLiteral("ftp"))
            continue;
        QString u = resolved.toString();
        if (m_linkSeen.contains(u))   // O(1) 哈希去重，避免 O(n²) 线性扫描
            continue;
        m_linkSeen.insert(u);

        LinkItem item;
        item.url = u;
        item.fileName = QFileInfo(resolved.path()).fileName();
        item.type = classifyType(item.fileName);
        m_links.append(item);
    }
}

// 套用站点解析插件抽取专属直链，并入 m_links（去重、按扩展名归类）
void SiteExplorerDialog::addPluginLinks(const QUrl& base, const QString& html)
{
    QStringList links = PluginManager::instance().extractLinks(base, html);
    for (const QString& u : links) {
        if (m_linkSeen.contains(u))   // O(1) 哈希去重
            continue;
        m_linkSeen.insert(u);

        QUrl resolved = QUrl::fromUserInput(u);
        LinkItem item;
        item.url = u;
        item.fileName = QFileInfo(resolved.path()).fileName();
        item.type = classifyType(item.fileName);
        m_links.append(item);
    }
}

// 是否像网页：无扩展名，或扩展名属于常见页面/动态脚本后缀。
// 图片/CSS/JS/音视频/压缩包等直接作为可下载文件收集，不再当作子页递归抓取。
bool SiteExplorerDialog::looksLikePage(const QUrl& url)
{
    QString path = url.path();
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext.isEmpty())
        return true;   // 无扩展名（如 /about）→ 视为页面
    static const QSet<QString> pageExts = {
        QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("shtml"),
        QStringLiteral("xhtml"), QStringLiteral("php"), QStringLiteral("php3"),
        QStringLiteral("phtml"), QStringLiteral("asp"), QStringLiteral("aspx"),
        QStringLiteral("jsp"), QStringLiteral("do"), QStringLiteral("cgi"),
        QStringLiteral("pl"), QStringLiteral("py"), QStringLiteral("cfm")
    };
    return pageExts.contains(ext);
}

QString SiteExplorerDialog::classifyType(const QString& fileName)
{
    QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext == QStringLiteral("mp4") || ext == QStringLiteral("mkv") ||
        ext == QStringLiteral("webm") || ext == QStringLiteral("avi") ||
        ext == QStringLiteral("mov") || ext == QStringLiteral("flv") ||
        ext == QStringLiteral("wmv") || ext == QStringLiteral("m3u8"))
        return QStringLiteral("视频");
    if (ext == QStringLiteral("mp3") || ext == QStringLiteral("flac") ||
        ext == QStringLiteral("wav") || ext == QStringLiteral("aac") ||
        ext == QStringLiteral("ogg") || ext == QStringLiteral("m4a") ||
        ext == QStringLiteral("wma"))
        return QStringLiteral("音频");
    if (ext == QStringLiteral("zip") || ext == QStringLiteral("rar") ||
        ext == QStringLiteral("7z") || ext == QStringLiteral("tar") ||
        ext == QStringLiteral("gz") || ext == QStringLiteral("bz2") ||
        ext == QStringLiteral("iso"))
        return QStringLiteral("压缩文件");
    if (ext == QStringLiteral("pdf") || ext == QStringLiteral("doc") ||
        ext == QStringLiteral("docx") || ext == QStringLiteral("xls") ||
        ext == QStringLiteral("xlsx") || ext == QStringLiteral("ppt") ||
        ext == QStringLiteral("pptx") || ext == QStringLiteral("txt") ||
        ext == QStringLiteral("epub") || ext == QStringLiteral("mobi"))
        return QStringLiteral("文档");
    if (ext == QStringLiteral("jpg") || ext == QStringLiteral("jpeg") ||
        ext == QStringLiteral("png") || ext == QStringLiteral("gif") ||
        ext == QStringLiteral("webp") || ext == QStringLiteral("bmp") ||
        ext == QStringLiteral("svg"))
        return QStringLiteral("图片");
    if (ext == QStringLiteral("exe") || ext == QStringLiteral("msi") ||
        ext == QStringLiteral("apk") || ext == QStringLiteral("dmg") ||
        ext == QStringLiteral("deb") || ext == QStringLiteral("bin"))
        return QStringLiteral("程序");
    return QStringLiteral("其他");
}

// 文件类型 → 归档子目录名（与 MainWindow 全局归档保持一致）
QString SiteExplorerDialog::folderForType(const QString& type)
{
    if (type == QStringLiteral("视频"))     return QStringLiteral("视频");
    if (type == QStringLiteral("音频"))     return QStringLiteral("音乐");
    if (type == QStringLiteral("压缩文件")) return QStringLiteral("压缩包");
    if (type == QStringLiteral("文档"))     return QStringLiteral("文档");
    if (type == QStringLiteral("图片"))     return QStringLiteral("图片");
    if (type == QStringLiteral("程序"))     return QStringLiteral("程序");
    return QStringLiteral("其他");
}

// 实时统计各类型发现数（基于 m_links 全集，与当前筛选无关），刷新 m_catStatLabel
void SiteExplorerDialog::updateCategoryStats()
{
    if (m_links.isEmpty()) {
        m_catStatLabel->setText(QStringLiteral("尚未发现链接"));
        return;
    }
    QMap<QString, int> cnt;
    for (const auto& it : m_links)
        cnt[it.type]++;
    QStringList parts;
    for (const QString& c : kCategories)
        if (cnt.value(c, 0) > 0)
            parts << QStringLiteral("%1 %2").arg(c).arg(cnt.value(c));
    m_catStatLabel->setText(QStringLiteral("发现 %1 个链接：").arg(m_links.size())
                                + parts.join(QStringLiteral("  ·  ")));
}

void SiteExplorerDialog::applyFilter()
{
    m_internal = true;   // 抑制 addRow 触发的 itemChanged → onSelectionChanged
    m_table->setRowCount(0);
    QString filter = m_filterEdit->text().trimmed().toLower();
    QStringList exts;
    if (!filter.isEmpty()) {
        for (const QString& s : filter.split(QStringLiteral(","), Qt::SkipEmptyParts))
            exts << s.trimmed();
    }
    bool onlyFiles = m_onlyFilesCheck->isChecked();
    for (const auto& item : m_links) {
        if (!m_catEnabled.value(item.type, true))
            continue;                       // 该分类未勾选
        if (onlyFiles && item.type == QStringLiteral("其他"))
            continue;                       // 仅文件链接：排除页面/其他
        if (!exts.isEmpty()) {
            QString ext = QFileInfo(item.fileName).suffix().toLower();
            if (!exts.contains(ext))
                continue;
        }
        addRow(item);
    }
    m_internal = false;
    m_dlBtn->setEnabled(m_table->rowCount() > 0);
    updateCategoryStats();
}

void SiteExplorerDialog::addRow(const LinkItem& item)
{
    int r = m_table->rowCount();
    m_table->insertRow(r);

    auto* chk = new QTableWidgetItem();
    chk->setCheckState(Qt::Unchecked);
    chk->setFlags(chk->flags() | Qt::ItemIsUserCheckable);
    m_table->setItem(r, 0, chk);

    m_table->setItem(r, 1, new QTableWidgetItem(
                         item.fileName.isEmpty() ? item.url : item.fileName));
    m_table->setItem(r, 2, new QTableWidgetItem(item.type));
    m_table->setItem(r, 3, new QTableWidgetItem(item.url));
}

void SiteExplorerDialog::onSelectAll(bool checked)
{
    m_internal = true;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* chk = m_table->item(r, 0);
        if (chk)
            chk->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    m_internal = false;
    onSelectionChanged();
}

void SiteExplorerDialog::onInvert()
{
    m_internal = true;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* chk = m_table->item(r, 0);
        if (chk)
            chk->setCheckState(chk->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
    m_internal = false;
    onSelectionChanged();
}

// 勾选变化 → 刷新「下载选中(N)」按钮文案（itemChanged 在 m_internal 期间被抑制）
void SiteExplorerDialog::onSelectionChanged()
{
    if (m_internal)
        return;
    int n = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* chk = m_table->item(r, 0);
        if (chk && chk->checkState() == Qt::Checked)
            ++n;
    }
    if (n > 0) {
        m_dlBtn->setText(QStringLiteral("下载选中 (%1)").arg(n));
        m_dlBtn->setEnabled(true);
    } else {
        m_dlBtn->setText(QStringLiteral("下载选中"));
        m_dlBtn->setEnabled(m_table->rowCount() > 0);
    }
}

void SiteExplorerDialog::onFilterChanged()
{
    applyFilter();
}

void SiteExplorerDialog::onDownloadSelected()
{
    QString queue = m_queueCombo->currentData().toString();
    bool archive = m_archiveCheck->isChecked();
    int added = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* chk = m_table->item(r, 0);
        if (!chk || chk->checkState() != Qt::Checked)
            continue;
        QString url = m_table->item(r, 3)->text();
        QString type = m_table->item(r, 2)->text();
        if (archive) {
            QString dir = m_defaultDir + QStringLiteral("/") + folderForType(type);
            emit requestAddUrlWithDir(url, dir, queue);
        } else if (queue.isEmpty()) {
            emit requestAddUrl(url);
        } else {
            emit requestAddUrlToQueue(url, queue);
        }
        ++added;
    }
    if (added == 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先勾选要下载的链接"));
        return;
    }
    m_statusLabel->setText(QStringLiteral("已发送 %1 个下载请求%2")
                               .arg(added)
                               .arg(archive ? QStringLiteral("（按类型归档）") : QStringLiteral("")));
}
