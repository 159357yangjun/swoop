#include "settings_dialog.h"
#include "settings.h"
#include "network.h"   /* network_set_http2_enabled：设置变更实时下发到 WinHTTP 引擎 */
#include <QUuid>

#include <QObject>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QScrollArea>
#include <QFrame>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QLabel>
#include <QDialog>
#include <QMessageBox>

// 站点登录编辑小对话框（不需要 Q_OBJECT，直接 exec 读值）
static bool editSiteLogin(QWidget* parent, SiteLogin& login, bool isNew)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? QStringLiteral("添加站点登录")
                             : QStringLiteral("编辑站点登录"));
    dlg.setMinimumWidth(360);

    auto* layout = new QFormLayout(&dlg);
    layout->setLabelAlignment(Qt::AlignRight);

    auto* urlEdit  = new QLineEdit(login.url, &dlg);
    auto* userEdit = new QLineEdit(login.username, &dlg);
    auto* passEdit = new QLineEdit(login.password, &dlg);
    passEdit->setEchoMode(QLineEdit::Password);

    urlEdit->setPlaceholderText(QStringLiteral("域名或 URL 前缀，如 example.com"));
    layout->addRow(QStringLiteral("站点 / 域名:"), urlEdit);
    layout->addRow(QStringLiteral("用户名:"), userEdit);
    layout->addRow(QStringLiteral("密码:"), passEdit);

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btns);

    if (dlg.exec() == QDialog::Accepted) {
        login.url      = urlEdit->text().trimmed();
        login.username = userEdit->text();
        login.password = passEdit->text();
        return !login.url.isEmpty();
    }
    return false;
}

// ───────────────────────────────────────────────
void SettingsDialog::selectTab(int index)
{
    if (auto* tab = findChild<QTabWidget*>()) {
        if (index >= 0 && index < tab->count())
            tab->setCurrentIndex(index);
    }
}

SettingsDialog::SettingsDialog(Settings& settings, QWidget* parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("选项"));
    setMinimumSize(560, 470);

    auto* tab = new QTabWidget(this);

    auto* general    = new QWidget;
    auto* fileTypes  = new QWidget;
    auto* downloads  = new QWidget;
    auto* connection = new QWidget;
    auto* proxy      = new QWidget;
    auto* sites      = new QWidget;
    auto* web        = new QWidget;

    buildGeneralTab(general);
    buildFileTypesTab(fileTypes);
    buildDownloadsTab(downloads);
    buildConnectionTab(connection);
    buildProxyTab(proxy);
    buildSitesTab(sites);
    buildWebTab(web);

    /* 每个标签页都套一层 QScrollArea。
     * 原因：设置项多的页（「下载」）自然高度接近 900px，而对话框默认高度只有 ~560。
     * 不套滚动区时，QTabWidget 会把 FormLayout 的行压到互相重叠——文字直接叠在一起
     * 看不清，用户只会以为界面坏了。套上之后窗口再小也只是出滚动条，排版永远不塌。 */
    auto addPage = [tab](QWidget* w, const QString& title) {
        auto* sc = new QScrollArea(tab);
        sc->setWidget(w);              // 会把 w 重新挂到 viewport 下
        sc->setWidgetResizable(true);  // 让页面随窗口宽度自适应
        sc->setFrameShape(QFrame::NoFrame);
        sc->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        tab->addTab(sc, title);
    };
    addPage(general,    QStringLiteral("常规"));
    addPage(fileTypes,  QStringLiteral("文件类型"));
    addPage(downloads,  QStringLiteral("下载"));
    addPage(connection, QStringLiteral("连接"));
    addPage(proxy,      QStringLiteral("代理"));
    addPage(sites,      QStringLiteral("站点登录"));
    addPage(web,        QStringLiteral("远程/Web"));

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* main = new QVBoxLayout(this);
    main->addWidget(tab);
    main->addWidget(buttons);
}

// ── 常规 ────────────────────────────────────────
void SettingsDialog::buildGeneralTab(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight);

    m_dirEdit = new QLineEdit(m_settings.defaultSaveDir(), page);
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."), page);
    connect(browseBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseDir);
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(m_dirEdit);
    dirRow->addWidget(browseBtn);
    form->addRow(QStringLiteral("默认下载目录:"), dirRow);

    m_themeCombo = new QComboBox(page);
    m_themeCombo->addItem(QStringLiteral("浅色"), QStringLiteral("light"));
    m_themeCombo->addItem(QStringLiteral("深色"), QStringLiteral("dark"));
    int idx = m_themeCombo->findData(m_settings.theme());
    m_themeCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    form->addRow(QStringLiteral("主题:"), m_themeCombo);

    m_closeTrayCheck = new QCheckBox(
        QStringLiteral("关闭窗口时最小化到系统托盘（而非退出程序）"), page);
    m_closeTrayCheck->setChecked(m_settings.closeToTray());
    form->addRow(QString(), m_closeTrayCheck);

    m_clipboardCheck = new QCheckBox(
        QStringLiteral("自动捕获剪贴板中的下载链接"), page);
    m_clipboardCheck->setChecked(m_settings.clipboardMonitor());
    form->addRow(QString(), m_clipboardCheck);

    form->addRow(QString(),
                 new QLabel(QStringLiteral("提示：修改主题后点击“确定”即时生效。"), page));
}

void SettingsDialog::onBrowseDir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择默认下载目录"), m_dirEdit->text());
    if (!dir.isEmpty())
        m_dirEdit->setText(dir);
}

// ── 文件类型 ────────────────────────────────────
void SettingsDialog::buildFileTypesTab(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);

    auto* tip = new QLabel(QStringLiteral(
        "自动捕获以下扩展名的链接（用于剪贴板监听与浏览器嗅探）："), page);
    tip->setWordWrap(true);
    layout->addWidget(tip);

    m_extList = new QListWidget(page);
    m_extList->addItems(m_settings.capturedExtensions());
    m_extList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(m_extList);

    auto* row = new QHBoxLayout;
    m_extEdit = new QLineEdit(page);
    m_extEdit->setPlaceholderText(QStringLiteral("输入扩展名如 mp4，回车或点“添加”"));
    connect(m_extEdit, &QLineEdit::returnPressed, this, &SettingsDialog::onAddExt);
    auto* addBtn = new QPushButton(QStringLiteral("添加"), page);
    auto* delBtn = new QPushButton(QStringLiteral("删除选中"), page);
    connect(addBtn, &QPushButton::clicked, this, &SettingsDialog::onAddExt);
    connect(delBtn, &QPushButton::clicked, this, &SettingsDialog::onRemoveExt);
    row->addWidget(m_extEdit);
    row->addWidget(addBtn);
    row->addWidget(delBtn);
    layout->addLayout(row);
}

void SettingsDialog::onAddExt()
{
    QString t = m_extEdit->text().trimmed().toLower();
    if (t.isEmpty()) return;
    if (t.startsWith('.')) t.remove(0, 1);
    for (int i = 0; i < m_extList->count(); ++i)
        if (m_extList->item(i)->text() == t) { m_extEdit->clear(); return; }
    m_extList->addItem(t);
    m_extEdit->clear();
}

void SettingsDialog::onRemoveExt()
{
    qDeleteAll(m_extList->selectedItems());
}

// ── 下载 ────────────────────────────────────────
void SettingsDialog::buildDownloadsTab(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight);

    m_threadsSpin = new QSpinBox(page);
    m_threadsSpin->setRange(1, 32);
    m_threadsSpin->setValue(m_settings.maxThreads());
    form->addRow(QStringLiteral("每任务最大线程数:"), m_threadsSpin);

    m_concurrentSpin = new QSpinBox(page);
    m_concurrentSpin->setRange(1, 16);
    m_concurrentSpin->setValue(m_settings.maxConcurrent());
    form->addRow(QStringLiteral("同时下载的任务数:"), m_concurrentSpin);

    m_limitSpin = new QSpinBox(page);
    m_limitSpin->setRange(0, 102400);
    m_limitSpin->setSuffix(QStringLiteral(" KB/s"));
    m_limitSpin->setSpecialValueText(QStringLiteral("不限速"));
    m_limitSpin->setValue(m_settings.speedLimitKBps());
    /* 记下打开对话框时的限速值：save() 用它判断用户到底有没有手动改过限速。
     * 不记这个值，就只能无条件把档位标记成「自定义」（见 save() 里的说明）。 */
    m_limitAtOpen = m_limitSpin->value();
    form->addRow(QStringLiteral("全局限速:"), m_limitSpin);

    m_connSpin = new QSpinBox(page);
    m_connSpin->setRange(1, 32);
    m_connSpin->setValue(m_settings.connectionsPerServer());
    form->addRow(QStringLiteral("每服务器连接数:"), m_connSpin);

    m_retrySpin = new QSpinBox(page);
    m_retrySpin->setRange(0, 20);
    m_retrySpin->setValue(m_settings.retryCount());
    form->addRow(QStringLiteral("下载失败重试次数:"), m_retrySpin);

    m_uaEdit = new QLineEdit(m_settings.userAgent(), page);
    m_uaEdit->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("用户代理 (UA):"), m_uaEdit);

    m_autoArchiveCheck = new QCheckBox(
        QStringLiteral("按文件类型自动归档（视频/音乐/压缩包/文档/程序 分目录保存）"), page);
    m_autoArchiveCheck->setChecked(m_settings.autoArchiveByType());
    form->addRow(QString(), m_autoArchiveCheck);

    m_autoYtDlpCheck = new QCheckBox(
        QStringLiteral("缺失时自动下载 yt-dlp（开箱即用，无需手动安装）"), page);
    m_autoYtDlpCheck->setChecked(m_settings.autoDownloadYtDlp());
    form->addRow(QString(), m_autoYtDlpCheck);

    m_ytDlpPathEdit = new QLineEdit(m_settings.ytDlpCustomPath(), page);
    m_ytDlpPathEdit->setPlaceholderText(QStringLiteral("留空=自动查找/下载；亦可指定 yt-dlp 路径"));
    m_ytDlpBtn = new QPushButton(QStringLiteral("立即下载 yt-dlp"), page);
    connect(m_ytDlpBtn, &QPushButton::clicked, this, &SettingsDialog::onDownloadYtDlpClicked);
    auto* ytRow = new QHBoxLayout;
    ytRow->addWidget(m_ytDlpPathEdit, 1);
    ytRow->addWidget(m_ytDlpBtn);
    form->addRow(QStringLiteral("yt-dlp 路径:"), ytRow);

    m_ffmpegPathEdit = new QLineEdit(m_settings.ffmpegCustomPath(), page);
    m_ffmpegPathEdit->setPlaceholderText(QStringLiteral("留空=自动查找 PATH 或捆绑目录 ffmpeg/ffmpeg.exe"));
    m_ffmpegBtn = new QPushButton(QStringLiteral("浏览…"), page);
    connect(m_ffmpegBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseFfmpegClicked);
    auto* ffRow = new QHBoxLayout;
    ffRow->addWidget(m_ffmpegPathEdit, 1);
    ffRow->addWidget(m_ffmpegBtn);
    form->addRow(QStringLiteral("ffmpeg 路径:"), ffRow);

    m_autoFfmpegCheck = new QCheckBox(
        QStringLiteral("缺失时自动下载 ffmpeg（开箱即用，HLS 转 MP4 免手动安装）"), page);
    m_autoFfmpegCheck->setChecked(m_settings.autoDownloadFfmpeg());
    form->addRow(QString(), m_autoFfmpegCheck);

    // ── BT/磁力（aria2 式）──
    m_autoAria2Check = new QCheckBox(
        QStringLiteral("缺失时自动下载 aria2c（开箱即用，BT/磁力免手动安装）"), page);
    m_autoAria2Check->setChecked(m_settings.autoDownloadAria2());
    form->addRow(QString(), m_autoAria2Check);

    m_aria2PathEdit = new QLineEdit(m_settings.aria2CustomPath(), page);
    m_aria2PathEdit->setPlaceholderText(QStringLiteral("留空=自动查找/下载；亦可指定 aria2c 路径"));
    m_aria2Btn = new QPushButton(QStringLiteral("立即下载 aria2"), page);
    connect(m_aria2Btn, &QPushButton::clicked, this, &SettingsDialog::onDownloadAria2Clicked);
    auto* a2Row = new QHBoxLayout;
    a2Row->addWidget(m_aria2PathEdit, 1);
    a2Row->addWidget(m_aria2Btn);
    form->addRow(QStringLiteral("aria2c 路径:"), a2Row);

    // 这里的措辞必须与引擎/调度器的真实行为一致：写「后续版本生效」这种空头承诺
    // 等于界面上撒谎（本项目踩过多次），宁可写清楚作用范围与生效时机。
    auto* note = new QLabel(QStringLiteral(
        "说明：\n"
        "· 每任务最大线程数 / 每服务器连接数 —— 取两者中较小值作为单个任务的并发连接数，"
        "对保存之后新建的任务生效。\n"
        "· 同时下载的任务数 —— 超出该数量的新任务会自动排队，等有任务结束再自动开始；"
        "手动点「开始 / 重新开始」可立即启动，不受此限制。\n"
        "· 其余各项（站点登录、代理、限速、重试、UA、分类目录等）保存后立即生效。"), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    form->addRow(QString(), note);

    // ── 下载完成自动关机/休眠 ──
    m_powerCombo = new QComboBox(page);
    m_powerCombo->addItem(QStringLiteral("下载完成后不操作"), QStringLiteral("none"));
    m_powerCombo->addItem(QStringLiteral("下载完成后关机"), QStringLiteral("shutdown"));
    m_powerCombo->addItem(QStringLiteral("下载完成后休眠"), QStringLiteral("sleep"));
    int pidx = m_powerCombo->findData(m_settings.shutdownAction());
    m_powerCombo->setCurrentIndex(pidx < 0 ? 0 : pidx);
    form->addRow(QStringLiteral("下载完成后:"), m_powerCombo);

    m_graceSpin = new QSpinBox(page);
    m_graceSpin->setRange(10, 600);
    m_graceSpin->setSuffix(QStringLiteral(" 秒"));
    m_graceSpin->setValue(m_settings.shutdownGraceSec());
    form->addRow(QStringLiteral("关机/休眠前倒计时:"), m_graceSpin);

    auto* pnote = new QLabel(QStringLiteral(
        "开启后，当所有下载任务完成（含失败）且无新任务时，倒计时结束自动关机或休眠；倒计时内可取消。"), page);
    pnote->setWordWrap(true);
    pnote->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    form->addRow(QString(), pnote);
}

// ── 连接 ────────────────────────────────────────
void SettingsDialog::buildConnectionTab(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight);

    m_timeoutSpin = new QSpinBox(page);
    m_timeoutSpin->setRange(5, 120);
    m_timeoutSpin->setSuffix(QStringLiteral(" 秒"));
    m_timeoutSpin->setValue(m_settings.connectTimeoutSec());
    form->addRow(QStringLiteral("连接超时:"), m_timeoutSpin);

    auto* note = new QLabel(QStringLiteral(
        "重试次数在“下载”页设置。连接超时为全局网络参数，保存后对新任务生效。"), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    form->addRow(QString(), note);

    m_http2Check = new QCheckBox(
        QStringLiteral("启用 HTTP/2 协商（HTTPS 经 ALPN 自动提速；关闭可兼容老代理/服务器）"), page);
    m_http2Check->setChecked(m_settings.http2Enabled());
    form->addRow(QString(), m_http2Check);
}

// ── 代理 ────────────────────────────────────────
void SettingsDialog::buildProxyTab(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight);

    m_proxyTypeCombo = new QComboBox(page);
    m_proxyTypeCombo->addItem(QStringLiteral("不使用代理"), QStringLiteral("none"));
    m_proxyTypeCombo->addItem(QStringLiteral("HTTP 代理"),  QStringLiteral("http"));
    m_proxyTypeCombo->addItem(QStringLiteral("SOCKS 代理"), QStringLiteral("socks"));
    int pidx = m_proxyTypeCombo->findData(m_settings.proxyType());
    m_proxyTypeCombo->setCurrentIndex(pidx < 0 ? 0 : pidx);
    form->addRow(QStringLiteral("代理类型:"), m_proxyTypeCombo);

    m_proxyHostEdit = new QLineEdit(m_settings.proxyHost(), page);
    m_proxyHostEdit->setPlaceholderText(QStringLiteral("如 127.0.0.1"));
    form->addRow(QStringLiteral("代理服务器:"), m_proxyHostEdit);

    m_proxyPortSpin = new QSpinBox(page);
    m_proxyPortSpin->setRange(0, 65535);
    m_proxyPortSpin->setValue(m_settings.proxyPort());
    form->addRow(QStringLiteral("端口:"), m_proxyPortSpin);

    m_proxyUserEdit = new QLineEdit(m_settings.proxyUser(), page);
    form->addRow(QStringLiteral("用户名:"), m_proxyUserEdit);

    m_proxyPassEdit = new QLineEdit(m_settings.proxyPass(), page);
    m_proxyPassEdit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("密码:"), m_proxyPassEdit);

    auto* note = new QLabel(QStringLiteral(
        "代理凭据将安全保存于本机配置。下载引擎的代理通道将在后续版本中接入，当前仅保存配置。"), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    form->addRow(QString(), note);
}

// ── 站点登录 ────────────────────────────────────
void SettingsDialog::buildSitesTab(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);

    auto* tip = new QLabel(QStringLiteral(
        "为需要登录的站点（如网盘、论坛）保存账号，下载该类链接时自动携带凭据。"), page);
    tip->setWordWrap(true);
    layout->addWidget(tip);

    m_siteTable = new QTableWidget(0, 3, page);
    m_siteTable->setHorizontalHeaderLabels({
        QStringLiteral("站点 / 域名"), QStringLiteral("用户名"), QStringLiteral("密码") });
    m_siteTable->horizontalHeader()->setStretchLastSection(true);
    m_siteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_siteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_siteTable);

    auto* row = new QHBoxLayout;
    auto* addBtn  = new QPushButton(QStringLiteral("添加"), page);
    auto* editBtn = new QPushButton(QStringLiteral("编辑"), page);
    auto* delBtn  = new QPushButton(QStringLiteral("删除"), page);
    connect(addBtn,  &QPushButton::clicked, this, &SettingsDialog::onAddSite);
    connect(editBtn, &QPushButton::clicked, this, &SettingsDialog::onEditSite);
    connect(delBtn,  &QPushButton::clicked, this, &SettingsDialog::onRemoveSite);
    row->addWidget(addBtn);
    row->addWidget(editBtn);
    row->addWidget(delBtn);
    row->addStretch();
    layout->addLayout(row);

    auto* note = new QLabel(QStringLiteral(
        "注：凭据保存在本机配置中，命中匹配规则的下载任务会自动带上 HTTP 认证"
        "（由服务器决定 Basic/Digest 等方式）。匹配规则可填域名（含子域）或 URL 前缀。"), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    layout->addWidget(note);

    refreshSiteTable();
}

void SettingsDialog::refreshSiteTable()
{
    const auto& logins = m_settings.siteLogins();
    m_siteTable->setRowCount(logins.size());
    for (int i = 0; i < logins.size(); ++i) {
        m_siteTable->setItem(i, 0, new QTableWidgetItem(logins[i].url));
        m_siteTable->setItem(i, 1, new QTableWidgetItem(logins[i].username));
        // 密码不回显明文（表格只作展示，编辑走 onEditSite 直接取 Settings 里的真值）
        auto* passItem = new QTableWidgetItem(
            logins[i].password.isEmpty() ? QString()
                                         : QStringLiteral("••••••••"));
        passItem->setToolTip(QStringLiteral("已保存密码（点「编辑」可查看/修改）"));
        m_siteTable->setItem(i, 2, passItem);
    }
}

void SettingsDialog::onAddSite()
{
    SiteLogin login;
    if (editSiteLogin(this, login, true)) {
        auto logins = m_settings.siteLogins();
        logins << login;
        m_settings.setSiteLogins(logins);
        refreshSiteTable();
    }
}

void SettingsDialog::onEditSite()
{
    int r = m_siteTable->currentRow();
    if (r < 0) return;
    auto logins = m_settings.siteLogins();
    if (r >= logins.size()) return;
    if (editSiteLogin(this, logins[r], false)) {
        m_settings.setSiteLogins(logins);
        refreshSiteTable();
    }
}

void SettingsDialog::onRemoveSite()
{
    int r = m_siteTable->currentRow();
    if (r < 0) return;
    auto logins = m_settings.siteLogins();
    if (r >= logins.size()) return;
    logins.removeAt(r);
    m_settings.setSiteLogins(logins);
    refreshSiteTable();
}

void SettingsDialog::onDownloadYtDlpClicked()
{
    emit downloadYtDlpRequested();
}

void SettingsDialog::onDownloadAria2Clicked()
{
    emit downloadAria2Requested();
}

void SettingsDialog::onBrowseFfmpegClicked()
{
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 ffmpeg 可执行文件"),
        m_ffmpegPathEdit->text(),
#ifdef Q_OS_WIN
        QStringLiteral("可执行文件 (*.exe);;所有文件 (*.*)")
#else
        QStringLiteral("所有文件 (*)")
#endif
    );
    if (!path.isEmpty())
        m_ffmpegPathEdit->setText(path);
}

// ── 确定：写回所有字段 ──────────────────────────
void SettingsDialog::buildWebTab(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_webCheck = new QCheckBox(QStringLiteral("启用 Web 远程管理界面（本地）"));
    m_webCheck->setChecked(m_settings.webEnabled());
    form->addRow(QString(), m_webCheck);

    m_webPortSpin = new QSpinBox(page);
    m_webPortSpin->setRange(1024, 65535);
    m_webPortSpin->setValue(m_settings.webPort());
    form->addRow(QStringLiteral("监听端口:"), m_webPortSpin);

    auto* tokenRow = new QHBoxLayout;
    m_webTokenEdit = new QLineEdit(m_settings.webToken(), page);
    m_webTokenEdit->setReadOnly(true);
    m_webTokenBtn = new QPushButton(QStringLiteral("重新生成"), page);
    connect(m_webTokenBtn, &QPushButton::clicked,
            this, &SettingsDialog::onRegenerateTokenClicked);
    tokenRow->addWidget(m_webTokenEdit, 1);
    tokenRow->addWidget(m_webTokenBtn);
    form->addRow(QStringLiteral("访问令牌:"), tokenRow);

    auto* note = new QLabel(QStringLiteral(
        "仅监听 127.0.0.1（本机），远程无法访问，安全性较好。启用后可在浏览器打开 "
        "http://127.0.0.1:<端口>/ 远程添加与管理下载任务；所有 /api 调用均需携带该令牌。"),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    form->addRow(QString(), note);
}

void SettingsDialog::onRegenerateTokenClicked()
{
    m_webTokenEdit->setText(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void SettingsDialog::accept()
{
    // 常规
    m_settings.setDefaultSaveDir(m_dirEdit->text());
    m_settings.setTheme(m_themeCombo->currentData().toString());
    m_settings.setCloseToTray(m_closeTrayCheck->isChecked());
    m_settings.setClipboardMonitor(m_clipboardCheck->isChecked());

    // 文件类型
    QStringList exts;
    for (int i = 0; i < m_extList->count(); ++i)
        exts << m_extList->item(i)->text();
    m_settings.setCapturedExtensions(exts);

    // 下载
    m_settings.setMaxThreads(m_threadsSpin->value());
    m_settings.setMaxConcurrent(m_concurrentSpin->value());
    m_settings.setSpeedLimitKBps(m_limitSpin->value());
    /* 只有限速数值真的被改动过，才把流量档位记为「自定义」。
     * 原实现是无条件 setTrafficMode(-1)：用户在状态栏选了「轻量」，只是打开设置
     * 换了个主题、点确定，档位就被悄悄改成「自定义」——工具栏/托盘/列表列三处
     * 的显示一起变，用户会以为自己选错了档。
     * 没改动就完全不碰 trafficMode，原档位（自动/轻量/中等/重量）原样保留。 */
    if (m_limitSpin->value() != m_limitAtOpen)
        m_settings.setTrafficMode(-1);
    m_settings.setConnectionsPerServer(m_connSpin->value());
    m_settings.setRetryCount(m_retrySpin->value());
    m_settings.setUserAgent(m_uaEdit->text().trimmed());
    m_settings.setAutoArchiveByType(m_autoArchiveCheck->isChecked());
    m_settings.setAutoDownloadYtDlp(m_autoYtDlpCheck->isChecked());
    m_settings.setYtDlpCustomPath(m_ytDlpPathEdit->text().trimmed());
    m_settings.setFfmpegCustomPath(m_ffmpegPathEdit->text().trimmed());
    m_settings.setAutoDownloadAria2(m_autoAria2Check->isChecked());
    m_settings.setAutoDownloadFfmpeg(m_autoFfmpegCheck->isChecked());
    m_settings.setAria2CustomPath(m_aria2PathEdit->text().trimmed());
    m_settings.setShutdownAction(m_powerCombo->currentData().toString());
    m_settings.setShutdownGraceSec(m_graceSpin->value());

    // 连接
    m_settings.setConnectTimeoutSec(m_timeoutSpin->value());
    m_settings.setHttp2Enabled(m_http2Check->isChecked());
    network_set_http2_enabled(m_http2Check->isChecked());   // 实时应用：后续新建连接按新策略协商

    // 代理
    m_settings.setProxyType(m_proxyTypeCombo->currentData().toString());
    m_settings.setProxyHost(m_proxyHostEdit->text().trimmed());
    m_settings.setProxyPort(m_proxyPortSpin->value());
    m_settings.setProxyUser(m_proxyUserEdit->text().trimmed());
    m_settings.setProxyPass(m_proxyPassEdit->text());

    // 远程 / Web
    m_settings.setWebEnabled(m_webCheck->isChecked());
    m_settings.setWebPort(m_webPortSpin->value());
    m_settings.setWebToken(m_webTokenEdit->text().trimmed());

    // 站点登录（从表格兜底读回）
    QList<SiteLogin> logins;
    for (int r = 0; r < m_siteTable->rowCount(); ++r) {
        SiteLogin l;
        l.url      = m_siteTable->item(r, 0) ? m_siteTable->item(r, 0)->text() : QString();
        l.username = m_siteTable->item(r, 1) ? m_siteTable->item(r, 1)->text() : QString();
        l.password = m_siteTable->item(r, 2) ? m_siteTable->item(r, 2)->text() : QString();
        if (!l.url.isEmpty()) logins << l;
    }
    m_settings.setSiteLogins(logins);

    m_settings.save();
    m_settings.applyToEngine();
    QDialog::accept();
}
