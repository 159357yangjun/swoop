#include "main_window.h"
#include "app_paths.h"
#include "download_manager.h"
#include "task_list_model.h"
#include "new_task_dialog.h"
#include "settings_dialog.h"
#include "category_filter_proxy.h"
#include "task_group_proxy.h"
#include "task_detail_dialog.h"
#include "batch_import_dialog.h"
#include "schedule_dialog.h"
#include "queue_manager.h"
#include "queue_manager_dialog.h"
#include "site_explorer_dialog.h"
#include "history_dialog.h"
#include "progress_delegate.h"
#include "sidebar_panel.h"
#include "app_icons.h"
#include "video_downloader.h"
#include "video_backend.h"
#include "web_server.h"
#include <QJsonArray>
#include <QJsonObject>
#include "hls_downloader.h"
#include "torrent_downloader.h"
#include "torrent_backend.h"
#include "ffmpeg_backend.h"
#include "download_core.h"
#include "network.h"   /* network_set_http2_enabled：启动时把持久设置注入 WinHTTP 引擎 */
#include "history_store.h"
#include "logger.h"

#include <QStandardPaths>
#include <QScreen>
#include <QGuiApplication>
#include <QScrollBar>
#include <QSet>
#include <QHash>
#include <QVariant>
#include <QDir>
#include <QFile>
#include <functional>
#include <algorithm>

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QToolBar>
#include <QToolButton>
#include <QComboBox>
#include <QActionGroup>
#include <QIcon>
#include <QApplication>
#include <QMessageBox>
#include <QSplitter>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QTreeView>
#include <QTableView>
#include <QStatusBar>
#include <QLabel>
#include <QUrl>
#include <QDir>
#include <QCloseEvent>
#include <QSystemTrayIcon>
#include <QClipboard>
#include <QPushButton>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>
#include <QLocalServer>
#include <QDialog>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QLibrary>
#include <QProcess>
#include <functional>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QNetworkProxy>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QEvent>
#include <QResizeEvent>
#include <QVector>
#include <QDataStream>
#include <QShowEvent>
#include <cmath>

// 完成提示音（Windows：内存合成 WAV + PlaySound）
#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <mmsystem.h>
#endif

// ── Windows 电源控制（关机 / 休眠）──────────────────────────────
#ifdef Q_OS_WIN
namespace {
// 请求 SE_SHUTDOWN_NAME 特权，否则普通进程调用 ExitWindowsEx 会失败
bool enableShutdownPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tkp;
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValue(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }
    AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
    bool ok = (GetLastError() == ERROR_SUCCESS);
    CloseHandle(hToken);
    return ok;
}

void windowsShutdown()
{
    enableShutdownPrivilege();
    // EWX_SHUTDOWN：执行完整关机流程；EWX_FORCE：强关无响应的应用
    ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, 0);
}

void windowsHibernate()
{
    // SetSuspendState 位于 PowrProf.dll，运行时动态解析以兼容不同工具链
    // 签名：BOOLEAN SetSuspendState(BOOLEAN Hibernate, BOOLEAN ForceCritical, BOOLEAN DisableWakeEvent)
    typedef BOOL (WINAPI *PFN_SetSuspendState)(BOOL, BOOL, BOOL);
    QLibrary powr(QStringLiteral("PowrProf"));
    if (powr.load()) {
        auto fn = reinterpret_cast<PFN_SetSuspendState>(powr.resolve("SetSuspendState"));
        if (fn) {
            fn(TRUE, TRUE, FALSE);  // Hibernate=true, Force=true
            return;
        }
    }
    // 回退：调用 rundll32 触发休眠
    QProcess::execute(QStringLiteral("rundll32.exe"),
                      QStringList() << QStringLiteral("powrprof.dll,SetSuspendState")
                                    << QStringLiteral("1") << QStringLiteral("0")
                                    << QStringLiteral("0"));
}
} // namespace
#endif

// ── 空状态插画（无任务时覆盖在任务列表视口上的引导层）──────────────
// 鼠标透明，不拦截拖放/点击；随视口尺寸自适应，由 updateEmptyState 控制显隐。
class EmptyStateOverlay : public QWidget {
public:
    explicit EmptyStateOverlay(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    void setTitle(const QString& t)    { if (m_title != t)    { m_title = t;    update(); } }
    void setSubtitle(const QString& s) { if (m_subtitle != s){ m_subtitle = s; update(); } }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRect r = rect();
        const int cx = r.center().x();
        const int cy = r.center().y();

        // 主题感知配色
        const QPalette pal = palette();
        const QColor accent = QColor(59, 130, 246);   // #3b82f6，明暗主题均清晰
        const QColor titleCol = pal.color(QPalette::WindowText);
        const QColor subCol = pal.color(QPalette::Disabled, QPalette::WindowText);

        // 干净的实心圆 + 白色下载箭头
        const int rad = 36;
        const QPointF c(cx, cy - 60);
        QRectF circ(c.x() - rad, c.y() - rad, rad * 2, rad * 2);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(circ);

        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        QPolygonF arrow;
        arrow << QPointF(cx, c.y() + 13)
              << QPointF(c.x() - 14, c.y() - 7)
              << QPointF(c.x() + 14, c.y() - 7);
        p.drawPolygon(arrow);
        p.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(cx - 16, c.y() + 15, cx + 16, c.y() + 15);

        // 标题
        p.setPen(titleCol);
        QFont ft = font(); ft.setPointSize(15); ft.setBold(true);
        p.setFont(ft);
        p.drawText(QRect(0, cy + 4, r.width(), 30), Qt::AlignCenter, m_title);

        // 副标题
        p.setPen(subCol);
        QFont fs = font(); fs.setPointSize(11);
        p.setFont(fs);
        p.drawText(QRect(0, cy + 38, r.width(), 24), Qt::AlignCenter, m_subtitle);
    }

private:
    QString m_title;
    QString m_subtitle;
};

// ── 下载完成自动关机/休眠：可取消倒计时对话框（非模态）──────────────
// 倒计时结束触发 onTrigger 回调（执行关机/休眠）；点击“取消”触发 onCancel。
// 不使用 Q_OBJECT（信号用 std::function 回调替代），避免 AUTOMOC 依赖。
class AutoPowerDialog : public QDialog {
public:
    using Callback = std::function<void()>;

    AutoPowerDialog(const QString& actionText, int seconds,
                    Callback onTrigger, Callback onCancel, QWidget* parent = nullptr)
        : QDialog(parent)
        , m_actionText(actionText)
        , m_secs(seconds)
        , m_onTrigger(std::move(onTrigger))
        , m_onCancel(std::move(onCancel))
    {
        setWindowTitle(QStringLiteral("下载完成 - 即将%1").arg(actionText));
        setFixedSize(360, 150);
        setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

        auto* v = new QVBoxLayout(this);
        m_label = new QLabel(this);
        m_label->setWordWrap(true);
        m_label->setAlignment(Qt::AlignCenter);
        v->addWidget(m_label);

        auto* btn = new QPushButton(QStringLiteral("取消"), this);
        v->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, &AutoPowerDialog::doCancel);

        updateLabel();

        m_timer = new QTimer(this);
        m_timer->setInterval(1000);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            --m_secs;
            if (m_secs <= 0) {
                m_timer->stop();
                if (m_onTrigger) m_onTrigger();
                close();
            } else {
                updateLabel();
            }
        });
        m_timer->start();
    }

    ~AutoPowerDialog() override { if (m_timer) m_timer->stop(); }

    void doCancel() {
        if (m_timer) m_timer->stop();
        if (m_onCancel) m_onCancel();
        close();
    }

protected:
    void closeEvent(QCloseEvent* e) override {
        if (m_timer) m_timer->stop();
        QDialog::closeEvent(e);
    }

private:
    void updateLabel() {
        m_label->setText(QStringLiteral("所有下载已完成，将在 %1 秒后%2。\n点击“取消”可中止。")
                             .arg(m_secs).arg(m_actionText));
    }

    QLabel*   m_label = nullptr;
    QTimer*   m_timer = nullptr;
    QString   m_actionText;
    int       m_secs = 0;
    Callback  m_onTrigger;
    Callback  m_onCancel;
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_manager(new DownloadManager(this))
    , m_taskModel(new TaskListModel(this))
    , m_proxy(new CategoryFilterProxy(this))
{
    setupUi();
    setupMenus();
    setupToolBar();
    setupStatusBar();

    // 下载队列管理：先创建并加载队列，供左侧分类树重建队列节点
    m_queueMgr = new QueueManager(this);
    m_queueMgr->load();
    connect(m_queueMgr, &QueueManager::queuesChanged, this, &MainWindow::rebuildQueueTree);

    setupCategoryTree();
    setupTrayIcon();

    // 队列调度器：每 1 秒检查各队列并发数，启动排队中的任务
    m_queueTimer = new QTimer(this);
    connect(m_queueTimer, &QTimer::timeout, this, &MainWindow::onQueueScheduler);
    m_queueTimer->start(1000);

    // 启动 IPC server（让浏览器扩展/CLI 转发下载请求到正在运行的 GUI）
    startIpcServer();

    // 加载全局设置（默认目录/线程数/限速/主题）
    m_settings.load();
    // 把持久化的 HTTP/2 开关注入 WinHTTP 引擎（network_init 已用默认开启，这里按用户设置校正）
    network_set_http2_enabled(m_settings.http2Enabled());
    // 把持久化的流量档位/限速状态同步到任务列表「限速」列
    updateTrafficIndicator();
    // 恢复上次的任务列表分组模式。必须放在 m_settings.load() 之后：
    // 以前这行写在 setupToolBar() 里，那时设置还没读盘，取到的永远是默认值 0，
    // 「按类型/按队列」的选择存了盘却从来恢复不了。
    applyGroupMode(m_settings.groupMode());

    // 视频组件：应用用户自定义路径，缺失且开启时自动下载独立版（开箱即用）
    if (!m_settings.ytDlpCustomPath().isEmpty())
        VideoDownloader::setYtDlpPath(m_settings.ytDlpCustomPath());
    if (!m_settings.ffmpegCustomPath().isEmpty())
        HlsDownloader::setFfmpegPath(m_settings.ffmpegCustomPath());
    if (m_settings.autoDownloadFfmpeg())
        FfmpegBackend::ensureAvailable(this, [](bool ok) {
            if (ok) Log::info(QStringLiteral("HLS 转码组件 ffmpeg 已就绪（开箱即用）"));
        }, false);
    if (m_settings.autoDownloadYtDlp())
        VideoBackend::ensureAvailable(this, [](bool ok) {
            if (ok) Log::info(QStringLiteral("视频组件 yt-dlp 已就绪（开箱即用）"));
        }, false);

    // BT/磁力组件（aria2 式）：应用用户自定义路径，缺失且开启时自动下载 aria2c
    if (!m_settings.aria2CustomPath().isEmpty())
        TorrentDownloader::setAria2Path(m_settings.aria2CustomPath());
    if (m_settings.autoDownloadAria2())
        TorrentBackend::ensureAvailable(this, [](bool ok) {
            if (ok) Log::info(QStringLiteral("BT/磁力组件 aria2c 已就绪（开箱即用）"));
        }, false);

    applyTheme();  // 根据设置应用 QSS 主题样式
    applyZoom();   // 应用初始视图缩放（字体 + 工具栏图标）
    applyNetworkProxy();  // 应用代理（站点抓取器等 Qt 网络）

    applyWebServer();  // 依据设置启动 Web 管理界面（默认关闭）

    // 引擎回调在 worker 线程触发，Qt 自动以 QueuedConnection 投递到 GUI 线程，安全
    connect(m_manager, &DownloadManager::taskProgress,     this, &MainWindow::onTaskProgress);
    connect(m_manager, &DownloadManager::taskCompleted,    this, &MainWindow::onTaskCompleted);
    connect(m_manager, &DownloadManager::taskStateChanged, this, &MainWindow::onTaskStateChanged);

    // 状态栏刷新与高频进度信号解耦：固定 4Hz 节拍批量刷新，
    // 状态变更/完成等低频事件仍即时 updateStatusBar()，总速度数字更稳定
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(250);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    m_statusTimer->start();

    Log::info(QStringLiteral("主窗口初始化完成"));
    setWindowTitle(QStringLiteral("IDM Next - 全协议下载平台"));
    // 默认尺寸按屏幕可用区域自适应：任务表固定列合计 ~800px，1100×700 时文件名列只剩
    // 100 多像素（「curl-8.22…」）。这里取屏幕 90% 并夹在合理区间，小屏也不会超出工作区。
    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        const QRect avail = scr->availableGeometry();
        resize(qBound(1180, int(avail.width()  * 0.88), 1680),
               qBound(720,  int(avail.height() * 0.88), 1060));
    } else {
        resize(1280, 780);
    }
    setAcceptDrops(true);  // 启用拖放：拖 URL 到窗口添加任务

    // 简洁实心背景（不再使用毛玻璃透明，避免界面发飘/圆角别扭）
    setAttribute(Qt::WA_TranslucentBackground, false);
    // 给主面板留出呼吸边距
    if (auto* splitter = centralWidget()) {
        splitter->setContentsMargins(12, 12, 12, 12);
    }

    // 持久化恢复：若存在上次保存的任务队列，恢复并刷新列表
    // dataDir 走 AppPaths（便携模式下为 exe 目录，否则 AppData/Local）
    QString dataDir = AppPaths::dataDir();
    QDir().mkpath(dataDir);
    m_statePath = dataDir + QStringLiteral("/tasks.json");
    // 下载历史持久化（SQLite）：#46 —— 与引擎 tasks.json 互不干扰，跨会话保留历史
    HistoryStore::instance().init(dataDir + QStringLiteral("/history.db"));
    if (QFile::exists(m_statePath)) {
        if (m_manager->loadState(m_statePath))
            refreshFromEngine();
    }
    // 定时/重复设置：必须在任务列表恢复之后再读——要按任务 ID 校验有效性，
    // 列表还没填的话所有记录都会被当成失效而丢掉。
    loadSchedules();

    // 剪贴板监听：检测到下载链接时提示用户
    if (m_settings.clipboardMonitor()) {
        connect(QApplication::clipboard(), &QClipboard::dataChanged,
                this, &MainWindow::onClipboardChanged);
    }

    // 定时下载检查器：每 5 秒检查一次到期任务
    m_scheduleTimer = new QTimer(this);
    connect(m_scheduleTimer, &QTimer::timeout, this, &MainWindow::onCheckScheduledTasks);
    m_scheduleTimer->start(5000);
}

MainWindow::~MainWindow()
{
    // MainWindow 析构早于子对象 m_manager（dlmgr_destroy），此处保存安全
    if (!m_statePath.isEmpty())
        m_manager->saveState(m_statePath);
}

void MainWindow::setupUi()
{
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    m_categoryTree = new QTreeView(this);
    m_categoryTree->setMinimumWidth(158);
    m_categoryTree->setHeaderHidden(true);
    // 数量徽标：计数由 refreshSidebarCounts() 算好写进 CountRole，委托只负责画，
    // 这样树节点增删（如动态队列）不需要改动委托。
    m_categoryTree->setItemDelegate(new CategoryBadgeDelegate(CountRole, m_categoryTree));
    m_categoryTree->setUniformRowHeights(true);   // 行高统一，长树滚动更省
    connect(m_categoryTree, &QTreeView::clicked, this, &MainWindow::onCategoryClicked);

    m_proxy->setSourceModel(m_taskModel);

    // 分组折叠代理插在过滤代理与视图之间：TaskListModel → CategoryFilterProxy → TaskGroupProxy → 视图
    m_groupProxy = new TaskGroupProxy(this);
    m_groupProxy->setSourceModel(m_proxy);

    m_taskList = new QTableView(this);
    m_taskList->setModel(m_groupProxy);
    m_taskList->horizontalHeader()->setStretchLastSection(false);
    m_taskList->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    // 拉伸列必须是「文件名」而不是「描述」：描述列多数时候是空的，
    // 让它吃掉全部剩余宽度会把文件名挤成「curl-8.22...」，用户认不出是哪个文件。
    m_taskList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    // 其余列按内容给足宽度，避免出现「2026-09-14 ...」这类被截断到不可读的文本。
    // 固定宽度合计刻意压到 ~800px，好让文件名列在 1300px 窗口下仍有 230px 以上。
    m_taskList->setColumnWidth(1, 88);   // 大小
    m_taskList->setColumnWidth(2, 112);  // 状态
    m_taskList->setColumnWidth(3, 96);   // 剩余时间
    m_taskList->setColumnWidth(4, 100);  // 传输速度
    m_taskList->setColumnWidth(5, 116);  // 最后连接（只显示 MM-dd HH:mm，完整时间见 tooltip）
    m_taskList->setColumnWidth(6, 128);  // 描述（域名）
    m_taskList->setColumnWidth(7, 62);   // 协议（HTTP/1.1、HTTP/2）
    m_taskList->setColumnWidth(8, 88);   // 限速（如「中等 2048」「自定义 2048」）
    m_taskList->setMinimumWidth(760);    // 低于此宽度时优先出现横向滚动条，而不是把每列都压扁
    // 文件名从中间省略：尾部是扩展名，「curl-8.22…mingw.zip」比「curl-8.22…」信息量大得多
    m_taskList->setTextElideMode(Qt::ElideMiddle);
    m_taskList->setWordWrap(false);
    m_taskList->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_taskList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_taskList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_taskList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_taskList->setAlternatingRowColors(true);
    // 隐藏行号竖表头：TaskListModel 的 headerData 只提供水平表头（竖直方向返回空），
    // 于是留出一条 30 多像素宽、内容全空的白槽——既白吃文件名列的宽度，
    // 暗色主题下它的左上角（QTableCornerButton）还会露成一块白块。
    // 历史/站点抓取两个表都已这么做，任务列表是漏了。
    m_taskList->verticalHeader()->setVisible(false);

    // 状态列（列2）玻璃拟态进度条委托
    m_taskList->setItemDelegateForColumn(2, new ProgressDelegate(this));

    // 双击任务行 → 弹出详情对话框
    connect(m_taskList, &QTableView::doubleClicked, this, &MainWindow::onTaskDoubleClicked);

    // 单击分组头行 → 折叠/展开该分组；modelReset（增删/折叠/切换模式）后重排分组头跨列合并
    connect(m_taskList, &QTableView::clicked, this, &MainWindow::onTaskListClicked);
    connect(m_groupProxy, &QAbstractItemModel::modelReset, this, &MainWindow::applyGroupSpans);

    // 右键菜单（开始/暂停/取消/重开/删除 + 移动到队列）
    m_taskList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_taskList, &QTableView::customContextMenuRequested,
            this, &MainWindow::onTaskContextMenu);

    // 空状态插画（无任务时列表区引导提示；鼠标透明，不挡拖放）
    m_emptyOverlay = new EmptyStateOverlay(m_taskList->viewport());
    m_emptyOverlay->hide();
    m_taskList->viewport()->installEventFilter(this);
    connect(m_proxy, &QAbstractItemModel::rowsInserted,  this, &MainWindow::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved,   this, &MainWindow::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::modelReset,    this, &MainWindow::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::layoutChanged,  this, &MainWindow::updateEmptyState);

    // 左侧导航面板 = 一行计数状态条 + 分类树。树的模型/过滤逻辑完全不变，
    // 这里只是把树装进一个带呼吸间距的容器，上方放一行文字统计。
    m_sidebar = new SidebarPanel(m_categoryTree, this);
    m_sidebar->setMinimumWidth(178);
    m_sidebar->setMaximumWidth(300);

    splitter->addWidget(m_sidebar);
    splitter->addWidget(m_taskList);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    setCentralWidget(splitter);
    // 侧栏只要放得下「下载中 已完成 失败」一行，多余宽度全给任务列表——
    // 文件名列是唯一会拉伸的列，宽度直接决定用户能不能认出文件名。
    splitter->setSizes({196, 1084});
}

void MainWindow::setupMenus()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(QStringLiteral("新建任务(&N)..."), QKeySequence::New, this, &MainWindow::onNewTask);
    fileMenu->addAction(QStringLiteral("批量导入(&B)..."), this, &MainWindow::onBatchImport);
    fileMenu->addAction(QStringLiteral("站点抓取器(&E)..."), this, &MainWindow::onSiteExplorer);
    fileMenu->addAction(QStringLiteral("下载队列管理(&Q)..."), this, &MainWindow::onManageQueues);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("设置(&S)..."), this, &MainWindow::onSettings);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("下载历史(&H)..."), this, &MainWindow::onShowHistory);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出(&X)"), QKeySequence::Quit, this, &QWidget::close);

    auto* taskMenu = menuBar()->addMenu(QStringLiteral("任务(&T)"));
    taskMenu->addAction(QStringLiteral("开始(&S)"), this, &MainWindow::onStartSelected);
    taskMenu->addAction(QStringLiteral("暂停(&P)"), this, &MainWindow::onPauseSelected);
    taskMenu->addAction(QStringLiteral("继续(&R)"), this, &MainWindow::onResumeSelected);
    taskMenu->addAction(QStringLiteral("取消(&C)"), this, &MainWindow::onCancelSelected);
    taskMenu->addAction(QStringLiteral("重新开始(&E)"), this, &MainWindow::onRestartSelected);
    taskMenu->addAction(QStringLiteral("删除(&D)"), this, &MainWindow::onRemoveSelected);
    taskMenu->addSeparator();
    taskMenu->addAction(QStringLiteral("定时下载..."), this, &MainWindow::onScheduleDownload);
    taskMenu->addSeparator();
    taskMenu->addAction(QStringLiteral("全部开始"), this, &MainWindow::onStartAll);
    taskMenu->addAction(QStringLiteral("全部暂停"), this, &MainWindow::onPauseAll);
    taskMenu->addAction(QStringLiteral("删除全部"), this, &MainWindow::onRemoveAll);
    taskMenu->addAction(QStringLiteral("清除已完成"), this, &MainWindow::onRemoveCompleted);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu->addAction(QStringLiteral("放大(&I)"), QKeySequence::ZoomIn, this, &MainWindow::onZoomIn);
    viewMenu->addAction(QStringLiteral("缩小(&O)"), QKeySequence::ZoomOut, this, &MainWindow::onZoomOut);
    viewMenu->addAction(QStringLiteral("重置缩放(&R)"), QKeySequence(Qt::CTRL | Qt::Key_0), this, &MainWindow::onZoomReset);
    viewMenu->addSeparator();

    // 任务列表分组：从工具栏下拉框搬进菜单（工具栏已经不再堆控件）。
    // 用互斥动作组而不是 QComboBox，菜单里勾选态是原生语义，也不需要额外的「分组:」标签。
    auto* groupMenu = viewMenu->addMenu(QStringLiteral("列表分组(&G)"));
    m_groupActions = new QActionGroup(groupMenu);
    m_groupActions->setExclusive(true);
    static const struct { const char* name; int mode; const char* hint; } kGroupModes[] = {
        {"不分组", 0, "所有任务混在一起按加入时间排列"},
        {"按类型(&T)", 1, "按文件类型分组，点击分组头可折叠"},
        {"按队列(&Q)", 2, "按所属下载队列分组，点击分组头可折叠"},
    };
    for (const auto& g : kGroupModes) {
        QAction* a = groupMenu->addAction(QString::fromUtf8(g.name));
        a->setCheckable(true);
        a->setData(g.mode);
        a->setStatusTip(QString::fromUtf8(g.hint));
        m_groupActions->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = g.mode] { applyGroupMode(mode); });
    }
    syncGroupMenu();

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(QStringLiteral("关于(&A)..."), this, &MainWindow::onAbout);
}

// 流量档位预设 → 限速值（KB/s）；对标 FDM 工具栏流量使用模式
namespace {
int trafficKbpsForMode(int mode) {
    switch (mode) {
        case 1:  return 512;    // 轻量：严格保留浏览带宽
        case 2:  return 2048;   // 中等：平衡
        case 3:  return 4096;   // 重量：接近全速，仍留余量
        default: return 0;      // 自动/未知 → 不限速
    }
}
QString trafficModeName(int mode) {
    switch (mode) {
        case 1:  return QStringLiteral("轻量");
        case 2:  return QStringLiteral("中等");
        case 3:  return QStringLiteral("重量");
        case -1: return QStringLiteral("自定义");
        default: return QStringLiteral("自动");
    }
}

// 状态栏总速度文本。单位阈值与任务表 formatSpeed 保持一致（KB 一位、MB 两位小数），
// 否则同一窗口里两处速度的精度不一样，看起来像两个不同的数。
QString formatSpeedText(qint64 bps) {
    if (bps <= 0)              return QStringLiteral("0 B/s");
    if (bps < 1024)            return QStringLiteral("%1 B/s").arg(bps);
    if (bps < 1024 * 1024)     return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB/s").arg(bps / 1048576.0, 0, 'f', 2);
}
}

void MainWindow::setupToolBar()
{
    m_toolBar = addToolBar(QStringLiteral("主工具栏"));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);   // 一个按钮，横排更省高度
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setIconSize(QSize(18, 18));

    // 工具栏只留一个主操作。其余动作全都有菜单栏/右键菜单入口，工具栏再铺一遍
    // 只是把「哪个是重点」这件事说没了——曾经这里有 13 个控件，等于没有主次。
    // 图标一律用自绘单色集（见 app_icons.h：混用系统位图会让风格拼凑）。
    QAction* newAct = m_toolBar->addAction(AppIcons::icon(AppIcons::Glyph::NewTask),
                                           QStringLiteral("新建任务"),
                                           this, &MainWindow::onNewTask);
    newAct->setToolTip(QStringLiteral("新建下载任务 (Ctrl+N)"));
    if (QWidget* w = m_toolBar->widgetForAction(newAct))
        w->setObjectName(QStringLiteral("primaryToolBtn"));

    // 「更多」溢出菜单：任务控制 + 功能入口集中在一处，一次点击就能看到全部，
    // 而不是在一条横排里找。这里刻意不放流量档位和分组——它们不是「操作」，
    // 已分别移到状态栏（流量）和视图菜单（分组），放在动作菜单里会混淆语义。
    auto* more = new QToolButton(this);
    more->setText(QStringLiteral("更多"));
    more->setIcon(AppIcons::icon(AppIcons::Glyph::More));
    more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more->setPopupMode(QToolButton::InstantPopup);
    more->setToolTip(QStringLiteral("更多操作"));
    more->setAutoRaise(true);

    QMenu* moreMenu = new QMenu(more);
    auto add = [this, moreMenu](AppIcons::Glyph g, const QString& text, auto slot) {
        moreMenu->addAction(AppIcons::icon(g), text, this, slot);
    };
    add(AppIcons::Glyph::Start,    QStringLiteral("开始"),       &MainWindow::onStartSelected);
    add(AppIcons::Glyph::Pause,    QStringLiteral("暂停"),       &MainWindow::onPauseSelected);
    add(AppIcons::Glyph::Cancel,   QStringLiteral("取消"),       &MainWindow::onCancelSelected);
    add(AppIcons::Glyph::Remove,   QStringLiteral("删除"),       &MainWindow::onRemoveSelected);
    moreMenu->addSeparator();
    add(AppIcons::Glyph::Queue,    QStringLiteral("下载队列管理"), &MainWindow::onManageQueues);
    add(AppIcons::Glyph::Site,     QStringLiteral("站点抓取器"),   &MainWindow::onSiteExplorer);
    add(AppIcons::Glyph::History,  QStringLiteral("下载历史"),     &MainWindow::onShowHistory);
    moreMenu->addSeparator();
    add(AppIcons::Glyph::Settings, QStringLiteral("设置"),         &MainWindow::onSettings);

    more->setMenu(moreMenu);
    m_toolBar->addWidget(more);
}

void MainWindow::onTrafficModeChanged(int index)
{
    if (!m_trafficCombo || index < 0) return;
    int mode = m_trafficCombo->itemData(index).toInt();
    // “自定义”项仅作显示态，不允许被选中（手动限速请在设置里改）
    if (mode == -1) {
        syncTrafficCombo();   // 复位回当前实际档位
        return;
    }
    applyTrafficMode(mode);
}

// 统一应用流量档位：写设置 + 实时下发纯 C 引擎与 aria2 + 持久化 + 同步两处 UI
void MainWindow::applyTrafficMode(int mode)
{
    int kbps = trafficKbpsForMode(mode);
    m_settings.setTrafficMode(mode);
    m_settings.setSpeedLimitKBps(kbps);
    m_manager->setMaxSpeed(kbps * 1024);
    TorrentDownloader::setGlobalSpeedLimit(kbps);
    m_settings.save();
    Log::info(QStringLiteral("流量档位切换为「%1」(%2 KB/s)")
                  .arg(trafficModeName(mode)).arg(kbps));
    syncTrafficCombo();
    syncTrafficMenu();
    updateTrafficIndicator();   // 同步任务列表「限速」列
}

void MainWindow::syncTrafficCombo()
{
    if (!m_trafficCombo) return;
    int idx = m_trafficCombo->findData(m_settings.trafficMode());
    if (idx < 0) idx = m_trafficCombo->findData(-1);  // 落到“自定义”
    m_trafficCombo->setCurrentIndex(idx);
}

void MainWindow::syncTrafficMenu()
{
    if (!m_trafficMenu) return;
    int cur = m_settings.trafficMode();
    for (QAction* a : m_trafficMenu->actions())
        a->setChecked(a->data().toInt() == cur);
}

// 把全局流量档位/限速状态文本同步到任务列表「限速」列（所有行共用该值）
void MainWindow::updateTrafficIndicator()
{
    if (!m_taskModel) return;
    int mode = m_settings.trafficMode();
    QString text;
    if (mode == 0)
        text = QStringLiteral("自动");          // 自动 = 不限速
    else if (mode == -1)
        text = QStringLiteral("自定义 %1").arg(m_settings.speedLimitKBps());
    else
        text = QStringLiteral("%1 %2").arg(trafficModeName(mode)).arg(trafficKbpsForMode(mode));
    m_taskModel->setGlobalTraffic(text);
    // 状态栏下拉也要跟着走：它现在承担原来「限速胶囊」的职责——让人一眼看出
    // 当前是不是在限速、限到哪一档。档位名与列的文本同源，不会说两套话。
    syncTrafficCombo();
}

void MainWindow::setupStatusBar()
{
    m_statusBar = statusBar();

    // 总速度：这是全局唯一的「现在有多快」，放状态栏最右一组里最显眼的位置。
    m_speedPill = new QLabel(QStringLiteral("0 B/s"), this);
    m_speedPill->setObjectName(QStringLiteral("statusPill"));
    m_speedPill->setToolTip(QStringLiteral("所有下载中任务的速度之和"));

    // 文案格式（「任务: N  下载中: M」）被 IDM_CONCURRENCY_PROBE 自测直接断言，改动需同步改自测
    m_taskCountLabel = new QLabel(QStringLiteral("任务: 0  下载中: 0"), this);
    m_taskCountLabel->setObjectName(QStringLiteral("statusPill"));

    // 流量档位：从工具栏搬来。它是「当前状态 + 偶尔改一下」，不是动作，
    // 放状态栏右端符合直觉，也把工具栏腾出来只放真正要反复点的东西。
    QLabel* trafficLabel = new QLabel(QStringLiteral("流量"), this);
    trafficLabel->setObjectName(QStringLiteral("statusPillHint"));
    m_trafficCombo = new QComboBox(this);
    m_trafficCombo->addItem(QStringLiteral("自动"), 0);
    m_trafficCombo->addItem(QStringLiteral("轻量"), 1);
    m_trafficCombo->addItem(QStringLiteral("中等"), 2);
    m_trafficCombo->addItem(QStringLiteral("重量"), 3);
    m_trafficCombo->addItem(QStringLiteral("自定义"), -1);
    m_trafficCombo->setToolTip(QStringLiteral(
        "流量使用模式：自动=不限速；轻量/中等/重量=限制下载速度以保留网络带宽；自定义=在设置中手动指定"));
    syncTrafficCombo();
    connect(m_trafficCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTrafficModeChanged);

    m_statusBar->addPermanentWidget(m_speedPill);
    m_statusBar->addPermanentWidget(m_taskCountLabel);
    m_statusBar->addPermanentWidget(trafficLabel);
    m_statusBar->addPermanentWidget(m_trafficCombo);
    m_statusBar->showMessage(QStringLiteral("就绪"));
}

void MainWindow::setupCategoryTree()
{
    m_categoryModel = new QStandardItemModel(this);

    // 全部使用自绘单色图标集（见 app_icons.h 注释），与工具栏保持同一套视觉语言。
    // 文件类型带语义色（压缩=琥珀 文档=靛蓝 音乐=紫 程序=绿 视频=玫红 其他=中性），
    // 让整棵树可以「扫色」而不是逐字读；其余界面仍是中性灰 + 单一强调色。
    struct FtDef { QString name; FileType ft; AppIcons::Glyph icon; AppIcons::Tint tint; };
    QList<FtDef> fileTypes = {
        {QStringLiteral("压缩文件"), FileType::Compressed, AppIcons::Glyph::Compressed, AppIcons::Tint::Amber},
        {QStringLiteral("文档"),     FileType::Document,   AppIcons::Glyph::Document,   AppIcons::Tint::Indigo},
        {QStringLiteral("音乐"),     FileType::Music,      AppIcons::Glyph::Music,      AppIcons::Tint::Purple},
        {QStringLiteral("程序"),     FileType::Program,    AppIcons::Glyph::Program,    AppIcons::Tint::Green},
        {QStringLiteral("视频"),     FileType::Video,      AppIcons::Glyph::Video,      AppIcons::Tint::Rose},
        // 「其他」不是装饰：没有它，父节点的数量徽标（未完成=8）会大于子节点之和（=1），
        // 用户会以为漏了任务；而且无扩展名/未识别类型的任务原本在树里根本无法单独筛出来。
        {QStringLiteral("其他"),     FileType::Other,      AppIcons::Glyph::OtherFile,  AppIcons::Tint::Slate},
    };

    auto makeCatItem = [this](const QString& name, Category cat,
                              FileType ft, AppIcons::Glyph g, AppIcons::Tint tint) -> QStandardItem* {
        auto* it = new QStandardItem(AppIcons::icon(g, tint), name);
        it->setData(static_cast<int>(TreeItemType::Category), TypeRole);
        it->setData(static_cast<int>(cat), CategoryRole);
        it->setData(static_cast<int>(ft), FileTypeRole);
        return it;
    };

    // ── 分类（按文件类型过滤所有任务） ──
    auto* allParent = new QStandardItem(AppIcons::icon(AppIcons::Glyph::Category, AppIcons::Tint::Slate),
                                        QStringLiteral("分类"));
    allParent->setData(static_cast<int>(TreeItemType::Category), TypeRole);
    allParent->setData(static_cast<int>(Category::All), CategoryRole);
    allParent->setData(static_cast<int>(FileType::All), FileTypeRole);
    for (const auto& f : fileTypes)
        allParent->appendRow(makeCatItem(f.name, Category::All, f.ft, f.icon, f.tint));
    m_categoryModel->appendRow(allParent);

    // ── 未完成（待定/下载中/暂停 + 文件类型） ──
    auto* dlParent = new QStandardItem(AppIcons::icon(AppIcons::Glyph::Pending, AppIcons::Tint::Accent),
                                       QStringLiteral("未完成"));
    dlParent->setData(static_cast<int>(TreeItemType::Category), TypeRole);
    dlParent->setData(static_cast<int>(Category::Downloading), CategoryRole);
    dlParent->setData(static_cast<int>(FileType::All), FileTypeRole);
    for (const auto& f : fileTypes)
        dlParent->appendRow(makeCatItem(f.name, Category::Downloading, f.ft, f.icon, f.tint));
    m_categoryModel->appendRow(dlParent);

    // ── 已完成（+ 文件类型） ──
    auto* doneParent = new QStandardItem(AppIcons::icon(AppIcons::Glyph::Completed, AppIcons::Tint::Green),
                                         QStringLiteral("已完成"));
    doneParent->setData(static_cast<int>(TreeItemType::Category), TypeRole);
    doneParent->setData(static_cast<int>(Category::Completed), CategoryRole);
    doneParent->setData(static_cast<int>(FileType::All), FileTypeRole);
    for (const auto& f : fileTypes)
        doneParent->appendRow(makeCatItem(f.name, Category::Completed, f.ft, f.icon, f.tint));
    m_categoryModel->appendRow(doneParent);

    // ── 失败 / 已取消（平级） ──
    m_categoryModel->appendRow(makeCatItem(QStringLiteral("失败"),   Category::Failed,
                                           FileType::All, AppIcons::Glyph::Failed, AppIcons::Tint::Red));
    m_categoryModel->appendRow(makeCatItem(QStringLiteral("已取消"), Category::Cancelled,
                                           FileType::All, AppIcons::Glyph::Cancelled, AppIcons::Tint::Slate));

    // ── 站点抓取方案 ──
    auto* siteItem = new QStandardItem(AppIcons::icon(AppIcons::Glyph::Site, AppIcons::Tint::Accent),
                                       QStringLiteral("站点抓取方案"));
    siteItem->setData(static_cast<int>(TreeItemType::Feature), TypeRole);
    siteItem->setData(QStringLiteral("site-explorer"), FeatureRole);
    m_categoryModel->appendRow(siteItem);

    // ── 队列（动态） ──
    m_queueParentItem = new QStandardItem(AppIcons::icon(AppIcons::Glyph::Queue, AppIcons::Tint::Slate),
                                          QStringLiteral("队列"));
    m_queueParentItem->setData(static_cast<int>(TreeItemType::QueueParent), TypeRole);
    m_categoryModel->appendRow(m_queueParentItem);

    m_categoryTree->setModel(m_categoryModel);
    m_categoryTree->expand(allParent->index());
    m_categoryTree->expand(dlParent->index());
    m_categoryTree->expand(doneParent->index());
    m_categoryTree->setCurrentIndex(m_categoryModel->index(0, 0));

    rebuildQueueTree();
    refreshSidebarCounts();   // 首次把各分类的数量算出来，否则树上一排空徽标
}

void MainWindow::rebuildQueueTree()
{
    if (!m_queueParentItem)
        return;
    while (m_queueParentItem->rowCount() > 0)
        m_queueParentItem->removeRow(0);
    for (const auto& q : m_queueMgr->queues()) {
        auto* it = new QStandardItem(q.name);
        it->setData(static_cast<int>(TreeItemType::Queue), TypeRole);
        it->setData(q.name, QueueNameRole);
        m_queueParentItem->appendRow(it);
    }
}

void MainWindow::setupTrayIcon()
{
    m_trayIcon = new QSystemTrayIcon(this);
    // 使用系统默认图标（后续可替换为自定义图标）
    m_trayIcon->setIcon(QIcon::fromTheme(QStringLiteral("download"),
                          QApplication::windowIcon()));
    m_trayIcon->setToolTip(QStringLiteral("IDM Next - 下载平台"));

    // 右键菜单
    auto* trayMenu = new QMenu(this);
    trayMenu->addAction(QStringLiteral("显示主窗口"), this, &QWidget::showNormal);
    trayMenu->addAction(QStringLiteral("新建任务..."), this, &MainWindow::onNewTask);
    trayMenu->addSeparator();
    trayMenu->addAction(QStringLiteral("全部开始"), this, &MainWindow::onStartAll);
    trayMenu->addAction(QStringLiteral("全部暂停"), this, &MainWindow::onPauseAll);
    trayMenu->addSeparator();

    // 流量档位子菜单（对标 FDM 托盘流量使用模式）
    auto* trafficMenu = trayMenu->addMenu(QStringLiteral("流量档位"));
    auto* tg = new QActionGroup(trafficMenu);
    tg->setExclusive(true);
    static const QList<QPair<QString, int>> kTrafficModes {
        {QStringLiteral("自动"), 0}, {QStringLiteral("轻量"), 1},
        {QStringLiteral("中等"), 2}, {QStringLiteral("重量"), 3}
    };
    for (const auto& m : kTrafficModes) {
        QAction* a = trafficMenu->addAction(m.first);
        a->setData(m.second);
        a->setCheckable(true);
        tg->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = m.second] { applyTrafficMode(mode); });
    }
    m_trafficMenu = trafficMenu;
    syncTrafficMenu();

    trayMenu->addSeparator();
    trayMenu->addAction(QStringLiteral("退出"), qApp, &QApplication::quit);
    m_trayIcon->setContextMenu(trayMenu);

    // 双击托盘图标恢复窗口
    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick)
            onTrayActivated();
    });

    m_trayIcon->show();
}

void MainWindow::onTrayActivated()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 关闭窗口时：若开启“关闭到托盘”，则隐藏到托盘而非退出；否则正常退出
    if (m_settings.closeToTray() && m_trayIcon && m_trayIcon->isVisible()) {
        hide();
        m_trayIcon->showMessage(QStringLiteral("IDM Next"),
                                QStringLiteral("程序已最小化到系统托盘，双击图标恢复"),
                                QSystemTrayIcon::Information, 3000);
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::onClipboardChanged()
{
    if (!m_settings.clipboardMonitor())
        return;

    QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty() || text.contains(' ') || text.contains('\n'))
        return;  // 只处理单行纯链接

    // 判断是否为下载链接
    bool isDownloadLink = text.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
                       || text.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
                       || text.startsWith(QStringLiteral("ftp://"), Qt::CaseInsensitive)
                       || text.startsWith(QStringLiteral("magnet:"));
    if (!isDownloadLink)
        return;

    // 文件类型过滤：仅自动捕获用户设置的扩展名
    // （无扩展名的链接或 magnet 链接放行，避免误伤短链 / BT 种子）
    QString lowerUrl = text.toLower();
    int lastDot = lowerUrl.lastIndexOf(QLatin1Char('.'));
    QString ext = (lastDot >= 0) ? lowerUrl.mid(lastDot + 1) : QString();
    bool extOk = m_settings.capturedExtensions().contains(ext)
                 || ext.isEmpty()
                 || lowerUrl.startsWith(QStringLiteral("magnet:"));
    if (!extOk)
        return;

    // 避免重复提示同一个链接
    static QString lastUrl;
    if (text == lastUrl)
        return;
    lastUrl = text;

    // 弹出询问对话框
    auto ret = QMessageBox::question(
        this,
        QStringLiteral("检测到下载链接"),
        QStringLiteral("剪贴板中检测到下载链接：\n\n%1\n\n是否添加下载任务？").arg(text),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (ret == QMessageBox::Yes) {
        // 直接用链接创建任务
        int id = internalAddTask(text, m_settings.defaultSaveDir(), QString(),
                                 m_settings.maxThreads(), QString());
        if (id > 0)
            Log::info(QStringLiteral("剪贴板捕获链接 → 任务 #%1").arg(id));
    }
}

void MainWindow::onScheduleDownload()
{
    int id = selectedTaskId();
    if (id <= 0) {
        QMessageBox::information(this, QStringLiteral("定时下载"),
                                 QStringLiteral("请先在任务列表中选择一个任务"));
        return;
    }

    ScheduleDialog dlg(this);
    // 如果已有定时设置，预填
    if (m_scheduledTasks.contains(id)) {
        dlg.setEnabled(true);
        dlg.setScheduledTime(m_scheduledTasks.value(id));
        dlg.setRecurring(m_recurringTasks.contains(id));
    }

    if (dlg.exec() != QDialog::Accepted)
        return;

    if (dlg.isEnabled()) {
        m_scheduledTasks[id] = dlg.scheduledTime();
        // 「每日重复」必须真的被记住并参与调度：对话框一直有这个勾选项，
        // 但此前没人读 isRecurring()，勾了等于没勾（典型的 UI 有、行为无）。
        if (dlg.isRecurring()) m_recurringTasks.insert(id);
        else                   m_recurringTasks.remove(id);
        saveSchedules();   // 立刻落盘：用户期望「设了就记住」，而不是关掉程序就丢
        const QString when = dlg.scheduledTime().toString(QStringLiteral("MM-dd HH:mm"));
        Log::info(QStringLiteral("任务 #%1 定时于 %2 开始%3")
                     .arg(id).arg(dlg.scheduledTime().toString("yyyy-MM-dd HH:mm:ss"))
                     .arg(dlg.isRecurring() ? QStringLiteral("（每日重复）") : QString()));
        m_statusBar->showMessage(
            dlg.isRecurring()
                ? QStringLiteral("任务 #%1 已设定为每日 %2 自动开始").arg(id).arg(when)
                : QStringLiteral("任务 #%1 已设定于 %2 自动开始").arg(id).arg(when), 5000);
    } else {
        m_scheduledTasks.remove(id);
        m_recurringTasks.remove(id);
        saveSchedules();
        m_statusBar->showMessage(QStringLiteral("任务 #%1 定时已取消").arg(id), 3000);
    }
}

void MainWindow::onCheckScheduledTasks()
{
    if (m_scheduledTasks.isEmpty())
        return;

    QDateTime now = QDateTime::currentDateTime();
    QList<int> toStart;
    bool changed = false;   // 有递推或删除就要回写配置（重复任务的下次时间得记住）

    for (auto it = m_scheduledTasks.begin(); it != m_scheduledTasks.end(); ) {
        if (it.value() <= now) {
            toStart << it.key();
            if (m_recurringTasks.contains(it.key())) {
                /* 每日重复：递推到下一个未来时刻，而不是简单 +24 小时。
                 * 按天递推能对齐到「每天的同一时刻」，即使程序当天没开、
                 * 连续错过好几天，重开后也只会立刻补跑一次并跳到下一个未来时刻，
                 * 不会攒出一串过期时间把任务反复拉起。 */
                QDateTime next = it.value();
                do { next = next.addDays(1); } while (next <= now);
                it.value() = next;
                Log::info(QStringLiteral("任务 #%1 每日重复，下次 %2")
                              .arg(it.key()).arg(next.toString("yyyy-MM-dd HH:mm:ss")));
                changed = true;
                ++it;
            } else {
                it = m_scheduledTasks.erase(it);
                changed = true;
            }
        } else {
            ++it;
        }
    }
    if (changed)
        saveSchedules();

    for (int id : toStart) {
        startTaskById(id);
        Log::info(QStringLiteral("定时任务 #%1 已自动开始").arg(id));

        if (m_trayIcon && m_trayIcon->isVisible())
            m_trayIcon->showMessage(QStringLiteral("定时下载"),
                                    QStringLiteral("任务 #%1 已自动开始下载").arg(id),
                                    QSystemTrayIcon::Information, 3000);
    }

    if (!toStart.isEmpty())
        syncStatus();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    // 接受包含文本/URL 的拖放
    if (event->mimeData()->hasText() || event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    QString url;
    const QMimeData* mime = event->mimeData();
    if (mime->hasUrls()) {
        // 优先取第一个 URL
        QList<QUrl> urls = mime->urls();
        if (!urls.isEmpty())
            url = urls.first().toString();
    } else if (mime->hasText()) {
        url = mime->text().trimmed();
    }

    if (url.isEmpty())
        return;

    // 弹出新建任务对话框预填链接
    NewTaskDialog dlg(this, m_settings.defaultSaveDir(), m_settings.maxThreads());
    dlg.setUrl(url);
    if (dlg.exec() == QDialog::Accepted) {
        NewTaskInput inp = dlg.result();
        int id = internalAddTask(inp.url, inp.saveDir, inp.fileName,
                                 inp.threadCount > 0 ? inp.threadCount : m_settings.maxThreads(),
                                 QString());
        if (id > 0)
            Log::info(QStringLiteral("拖放添加任务 #%1: %2").arg(id).arg(inp.url));
    }
    event->acceptProposedAction();
}

void MainWindow::applyTheme()
{
    // 从 Qt 资源系统加载 QSS 样式表（light/dark），应用到整个应用
    QString qssPath = QStringLiteral(":/qss/%1.qss").arg(m_settings.theme());
    QFile f(qssPath);
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
        Log::info(QStringLiteral("已加载主题: %1").arg(m_settings.theme()));
    } else {
        // 回退到亮色主题
        QFile fallback(QStringLiteral(":/qss/light.qss"));
        if (fallback.open(QFile::ReadOnly | QFile::Text))
            qApp->setStyleSheet(QString::fromUtf8(fallback.readAll()));
        Log::warn(QStringLiteral("主题 %1 加载失败，回退到 light").arg(m_settings.theme()));
    }

    // 状态列委托是自绘的，QSS 管不到它：必须把主题色调同步过去，
    // 否则暗色主题下进度条百分比/状态徽章文字对比度不足（旧版渐变即因此不可读）。
    const bool dark = (m_settings.theme() == QStringLiteral("dark"));
    ProgressDelegate::setDarkTheme(dark);
    // 侧栏状态行与数量徽标同样是自绘的（文字色/底色/圆点色 QSS 表达不了），
    // 它们的颜色在 paintEvent 里实时解析，所以这里只需改标记位再让它们重绘。
    // 注意要直接 update 状态行：它是子控件且自己铺满底色，父容器的 update 不会重绘它。
    SidebarPanel::setDarkTheme(dark);
    if (m_sidebar) {
        m_sidebar->update();
        if (m_sidebar->stats())
            m_sidebar->stats()->update();
    }
    if (m_categoryTree)
        m_categoryTree->viewport()->update();   // 徽标是委托自绘的，显式重绘一次更稳妥
    // 自绘图标集同理：QIconEngine 在每次 paint 时实时取色，所以这里只需更新标记位，
    // 图标本身不用重建（工具栏按钮、分类树、右键菜单都会在下次重绘时自动换色）。
    AppIcons::setDarkTheme(dark);
}

void MainWindow::applyZoom()
{
    // 全局字体缩放：每级 ±1pt；同时调整工具栏图标大小
    int pt = BASE_FONT_PT + m_zoomLevel;
    if (pt < 8) pt = 8;
    if (pt > 24) pt = 24;

    QFont font = qApp->font();
    font.setPointSize(pt);
    qApp->setFont(font);

    int iconSz = BASE_ICON_SZ + m_zoomLevel * 2;
    if (iconSz < 16) iconSz = 16;
    if (iconSz > 48) iconSz = 48;
    m_toolBar->setIconSize(QSize(iconSz, iconSz));
}

void MainWindow::onZoomIn()
{
    if (m_zoomLevel < 6) {
        ++m_zoomLevel;
        applyZoom();
        Log::info(QStringLiteral("视图放大: %1pt").arg(BASE_FONT_PT + m_zoomLevel));
    }
}

void MainWindow::onZoomOut()
{
    if (m_zoomLevel > -4) {
        --m_zoomLevel;
        applyZoom();
        Log::info(QStringLiteral("视图缩小: %1pt").arg(BASE_FONT_PT + m_zoomLevel));
    }
}

void MainWindow::onZoomReset()
{
    m_zoomLevel = 0;
    applyZoom();
    Log::info(QStringLiteral("视图缩放重置"));
}

void MainWindow::applyNetworkProxy()
{
    // 根据设置应用 Qt 应用级代理（仅站点抓取器等 Qt 网络使用）。
    // 设置 "none" 时沿用系统代理，与 C 下载引擎默认行为一致。
    const QString type = m_settings.proxyType();
    if (type != QStringLiteral("http") && type != QStringLiteral("socks")) {
        // "none"：不做任何覆盖，保持 Qt 默认（系统代理）
        return;
    }

    QNetworkProxy p(type == QStringLiteral("http") ? QNetworkProxy::HttpProxy
                                                   : QNetworkProxy::Socks5Proxy,
                    m_settings.proxyHost(), (quint16)m_settings.proxyPort());
    /* 凭据必须一起下发。原来只设了 host/port，于是 proxyUser/proxyPass 只对 C 引擎
     * （libcurl）生效，走 Qt 网络栈的站点抓取器一旦碰到需要认证的代理就是必然 407 ——
     * 设置页填的用户名密码在那一半功能里等于不存在。 */
    const QString user = m_settings.proxyUser();
    if (!user.isEmpty()) {
        p.setUser(user);
        p.setPassword(m_settings.proxyPass());
    }
    QNetworkProxy::setApplicationProxy(p);
}

void MainWindow::paintEvent(QPaintEvent* event)
{
    // 实心背景由 QSS 控制（background-color），无需自绘渐变
    QMainWindow::paintEvent(event);
}

int MainWindow::selectedTaskId() const
{
    QModelIndex proxyIdx = m_taskList->currentIndex();
    if (!proxyIdx.isValid())
        return -1;
    // 视图 → 分组代理 → 过滤代理 → 源模型（分组头行在分组代理处即被过滤掉）
    QModelIndex gIdx = m_groupProxy ? m_groupProxy->mapToSource(proxyIdx) : proxyIdx;
    if (!gIdx.isValid())
        return -1;
    QModelIndex srcIdx = m_proxy->mapToSource(gIdx);
    return m_taskModel->taskIdForRow(srcIdx.row());
}

void MainWindow::onCategoryClicked(const QModelIndex& index)
{
    auto* item = m_categoryModel->itemFromIndex(index);
    if (!item)
        return;
    TreeItemType type = static_cast<TreeItemType>(item->data(TypeRole).toInt());
    auto* proxy = qobject_cast<CategoryFilterProxy*>(m_proxy);
    if (!proxy)
        return;

    // 切换分类会触发过滤代理 layoutChanged → 分组代理 reset，视图滚动位置会跳顶；
    // 此处先记下、切换后还原，保持列表滚动连贯（不分组模式下也属于常见操作）
    int savedScroll = m_taskList->verticalScrollBar()->value();

    if (type == TreeItemType::Category) {
        Category cat = static_cast<Category>(item->data(CategoryRole).toInt());
        FileType ft  = static_cast<FileType>(item->data(FileTypeRole).toInt());
        proxy->setCategory(cat);
        proxy->setFileType(ft);
        proxy->setQueueFilter(QString());
    } else if (type == TreeItemType::Queue) {
        QString qname = item->data(QueueNameRole).toString();
        proxy->setCategory(Category::All);
        proxy->setFileType(FileType::All);
        proxy->setQueueFilter(qname);
    } else if (type == TreeItemType::Feature) {
        QString feature = item->data(FeatureRole).toString();
        if (feature == QStringLiteral("site-explorer"))
            onSiteExplorer();
        return;  // Feature 节点不改变过滤器
    }
    // QueueParent：仅展开，不切换过滤

    // 还原列表滚动位置（抵消上面过滤切换引发的 reset 跳顶）
    m_taskList->verticalScrollBar()->setValue(savedScroll);
}

void MainWindow::onTaskDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    // 分组头行：双击也触发折叠/展开，不打开详情
    if (m_groupProxy && index.data(TaskListModel::GroupHeaderRole).toBool()) {
        m_groupProxy->toggleGroup(index.row());
        return;
    }
    QModelIndex gIdx = m_groupProxy ? m_groupProxy->mapToSource(index) : index;
    if (!gIdx.isValid()) return;
    QModelIndex srcIdx = m_proxy->mapToSource(gIdx);
    int taskId = m_taskModel->taskIdForRow(srcIdx.row());
    if (taskId <= 0) return;

    // 非模态对话框：允许同时查看详情和操作主窗口
    auto* dlg = new TaskDetailDialog(taskId, m_manager, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

// 统一应用分组模式：视图菜单单选动作与启动恢复共用同一条路径。
// 之所以要「统一」，是因为以前工具栏下拉框自己 setGroupMode，启动恢复又走另一条路，
// 结果恢复读的是 load() 之前的默认值（恒为 0）——设置存了盘却读不回来。
void MainWindow::applyGroupMode(int mode)
{
    if (!m_groupProxy) return;
    m_groupProxy->setGroupMode(static_cast<TaskGroupProxy::GroupMode>(mode));
    if (m_settings.groupMode() != mode) {
        m_settings.setGroupMode(mode);   // 持久化，重启后恢复
        m_settings.save();
    }
    syncGroupMenu();
}

void MainWindow::syncGroupMenu()
{
    if (!m_groupActions) return;
    const int cur = m_settings.groupMode();
    for (QAction* a : m_groupActions->actions())
        a->setChecked(a->data().toInt() == cur);
}

void MainWindow::onTaskListClicked(const QModelIndex& index)
{
    if (!index.isValid() || !m_groupProxy) return;
    if (index.data(TaskListModel::GroupHeaderRole).toBool()) {
        // 折叠/展开会改变分组结构并触发 modelReset；先记下滚动位置再还原，避免视图跳顶
        int v = m_taskList->verticalScrollBar()->value();
        m_groupProxy->toggleGroup(index.row());
        m_taskList->verticalScrollBar()->setValue(v);
    }
}

void MainWindow::applyGroupSpans()
{
    if (!m_taskList || !m_groupProxy) return;
    m_taskList->clearSpans();
    if (m_groupProxy->groupMode() == TaskGroupProxy::None) return;
    const int cols = m_groupProxy->columnCount();
    for (int r = 0; r < m_groupProxy->rowCount(); ++r) {
        QModelIndex idx = m_groupProxy->index(r, 0);
        if (idx.data(TaskGroupProxy::GroupHeaderRole).toBool())
            m_taskList->setSpan(r, 0, 1, cols);   // 分组头跨列合并，仅锚点列(0)绘制文本
    }
}

void MainWindow::onNewTask()
{
    NewTaskDialog dlg(this, m_settings.defaultSaveDir(), m_settings.maxThreads());
    if (dlg.exec() != QDialog::Accepted)
        return;

    NewTaskInput inp = dlg.result();
    int id = internalAddTask(inp.url, inp.saveDir, inp.fileName,
                             inp.threadCount > 0 ? inp.threadCount : m_settings.maxThreads(),
                             QString(), inp.format);
    if (id <= 0)
        QMessageBox::warning(this, QStringLiteral("失败"),
                             QStringLiteral("无法添加任务，链接可能无效或引擎繁忙"));
}

void MainWindow::onBatchImport()
{
    BatchImportDialog dlg(this, m_settings.defaultSaveDir(), m_settings.maxThreads());
    if (dlg.exec() != QDialog::Accepted)
        return;

    BatchImportResult r = dlg.result();
    int added = 0;
    for (const QString& url : r.urls) {
        int id = internalAddTask(url, r.saveDir, QString(),
                                 r.threadCount > 0 ? r.threadCount : m_settings.maxThreads(),
                                 QString());
        if (id > 0)
            ++added;
    }
    syncStatus();
    Log::info(QStringLiteral("批量导入完成: %1/%2 个任务已添加").arg(added).arg(r.urls.size()));
    QMessageBox::information(this, QStringLiteral("批量导入"),
                             QStringLiteral("成功添加 %1 个下载任务").arg(added));
}

void MainWindow::onStartSelected()
{
    int id = selectedTaskId();
    if (id > 0) startTaskById(id);
}

void MainWindow::onPauseSelected()
{
    int id = selectedTaskId();
    if (id > 0) pauseTaskById(id);
}

void MainWindow::onResumeSelected()
{
    int id = selectedTaskId();
    if (id > 0) resumeTaskById(id);
}

void MainWindow::onCancelSelected()
{
    int id = selectedTaskId();
    if (id > 0) cancelTaskById(id);
}

void MainWindow::onRestartSelected()
{
    int id = selectedTaskId();
    if (id <= 0) return;
    restartTaskById(id);
}

void MainWindow::onRemoveSelected()
{
    int id = selectedTaskId();
    if (id <= 0) return;
    removeTaskById(id);
}

void MainWindow::onRemoveAll()
{
    auto ret = QMessageBox::question(
        this,
        QStringLiteral("删除全部任务"),
        QStringLiteral("确定要删除任务列表中的所有任务吗？\n（运行中的任务会先取消）"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;

    // 拷贝一份 id，避免边删边遍历
    QList<int> ids = m_states.keys();
    for (int id : ids)
        removeTaskById(id);
    m_speeds.clear();
    m_states.clear();
    syncStatus();
    Log::info(QStringLiteral("已删除全部任务"));
}

void MainWindow::onSettings()
{
    SettingsDialog dlg(m_settings, this);
    connect(&dlg, &SettingsDialog::downloadYtDlpRequested, this, [this]() {
        VideoBackend::ensureAvailable(this, [this](bool ok) {
            QMessageBox::information(this, QStringLiteral("yt-dlp"),
                ok ? QStringLiteral("视频组件 yt-dlp 已准备就绪。")
                   : QStringLiteral("yt-dlp 下载失败，请检查网络或手动指定路径。"));
        }, true);
    });
    connect(&dlg, &SettingsDialog::downloadAria2Requested, this, [this]() {
        TorrentBackend::ensureAvailable(this, [this](bool ok) {
            QMessageBox::information(this, QStringLiteral("aria2"),
                ok ? QStringLiteral("BT/磁力组件 aria2c 已准备就绪。")
                   : QStringLiteral("aria2c 下载失败，请检查网络或手动指定路径。"));
        }, true);
    });
    if (dlg.exec() == QDialog::Accepted) {
        // SettingsDialog::accept() 内部已 save + applyToEngine，这里只需刷新主题
        applyTheme();
        applyNetworkProxy();   // 代理可能在设置中修改，同步到 Qt 网络栈
        applyWebServer();      // 依据新设置启动/停止/更新 Web 管理界面

        // 重新连接/断开剪贴板监听
        auto* clip = QApplication::clipboard();
        disconnect(clip, &QClipboard::dataChanged, this, &MainWindow::onClipboardChanged);
        if (m_settings.clipboardMonitor())
            connect(clip, &QClipboard::dataChanged, this, &MainWindow::onClipboardChanged);

        // #50：全局限速变更实时推送给正在运行的 BT/磁力/FTP（aria2）任务
        TorrentDownloader::setGlobalSpeedLimit(m_settings.speedLimitKBps());
        syncTrafficCombo();   // 设置里改了限速值 → 同步工具栏档位下拉（显示“自定义”）
        syncTrafficMenu();    // 同步托盘流量子菜单勾选（手动值 → 4 项全不勾）
        updateTrafficIndicator();  // 同步任务列表「限速」列
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, QStringLiteral("关于 IDM Next"),
                       QStringLiteral("IDM Next v0.1.0\n\n"
                                      "全协议下载平台\n"
                                      "Qt 6 + (libcurl 待接入)\n\n"
                                      "下载引擎复用自原 MFC 项目纯 C 代码"));
}

void MainWindow::onTaskProgress(int taskId, qint64 downloaded, qint64 total, int speedBps)
{
    // 增量维护总速度缓存：仅用旧值与新值之差修正，避免 4Hz/逐信号全量 O(n) 遍历
    qint64 oldS = m_speeds.value(taskId, 0);
    qint64 ns   = (speedBps > 0) ? static_cast<qint64>(speedBps) : 0;
    m_totalSpeed += ns - (oldS > 0 ? oldS : 0);
    if (m_totalSpeed < 0) m_totalSpeed = 0;   // 防御增量漂移
    m_speeds[taskId] = speedBps;
    // HLS 后端以 (百分比, 100) 上报进度；视频/引擎以真实字节上报
    if (isStreamTask(taskId) && total == 100)
        m_taskModel->setProgressPct(taskId, static_cast<int>(downloaded));
    else
        m_taskModel->updateProgress(taskId, downloaded, speedBps);
    // 状态栏刷新交由独立低频定时器（4Hz）批量处理，避免逐信号 path 重绘
}

void MainWindow::onTaskCompleted(int taskId, bool success, const QString& error)
{
    Log::info(QStringLiteral("任务 %1 %2").arg(taskId).arg(success ? "完成" : "失败"));
    m_speeds[taskId] = 0;
    // 记录终态（完成=3 / 失败=4），保证“全部完成”判定不依赖 stateChanged 到达顺序
    m_states[taskId] = success ? 3 : 4;
    m_taskModel->updateStatus(taskId, m_states[taskId]);
    syncStatus();

    if (!success) {
        m_lastError = error;
        // 失败原因必须同时落到任务行上，不能只在托盘气泡里说一次就没了：
        // 用户过一会儿回来翻列表，只看到「失败」却查不到为什么，等于没有诊断信息。
        // 媒体后端（yt-dlp / aria2 / ffmpeg）的原因由各自 completed 信号带过来；
        // 引擎任务的原因由 4Hz 循环从 error_msg 回填，这里不碰，免得把更精确的
        // 引擎原因冲成泛化提示。
        if (isStreamTask(taskId)) {
            QString why = error.trimmed();
            if (why.isEmpty()) {
                if (isVideoTask(taskId))
                    why = QStringLiteral("yt-dlp 执行失败（请确认已安装 yt-dlp，或在设置中指定路径）");
                else if (isHlsTask(taskId))
                    why = QStringLiteral("ffmpeg/HLS 处理失败（请确认已安装 ffmpeg，或在设置中指定路径）");
                else
                    why = QStringLiteral("aria2 后端失败（请确认 aria2c 可用，并检查磁力/种子链接）");
            }
            m_taskModel->setErrorMsg(taskId, why);
        }
        // 视频/HLS 任务失败：给出可操作的提示
        if (isVideoTask(taskId) && m_trayIcon) {
            m_trayIcon->showMessage(
                QStringLiteral("视频下载失败"),
                error.isEmpty()
                    ? QStringLiteral("请确认已安装 yt-dlp（pip install yt-dlp 或放入 PATH）")
                    : error,
                QSystemTrayIcon::Warning, 5000);
        } else if (isHlsTask(taskId) && m_trayIcon) {
            m_trayIcon->showMessage(
                QStringLiteral("HLS 下载失败"),
                error.isEmpty()
                    ? QStringLiteral("请确认已安装 ffmpeg（放入 PATH 或设置 ffmpeg 路径）")
                    : error,
                QSystemTrayIcon::Warning, 5000);
        }
    } else if (isStreamTask(taskId)) {
        // 重试成功要把上一次的失败原因清掉，否则任务显示「已完成」却挂着一条旧错误
        m_taskModel->setErrorMsg(taskId, QString());
    }

    // 聚合通知：累计完成/失败，由 m_notifyTimer 合并为单条气泡 + 单次提示音，避免刷屏
    QString name;
    QString url;
    qint64 size = -1;
    const auto& tasks = m_taskModel->tasks();
    for (const auto& t : tasks) {
        if (t.id == taskId) {
            name = t.fileName;
            url  = t.url;
            size = t.fileSize;
            break;
        }
    }
    if (name.isEmpty())
        name = QStringLiteral("任务 #%1").arg(taskId);

    // 下载历史持久化（SQLite）：#46 —— 记录终态（完成/失败）
    HistoryStore::instance().recordFinished(url, name, size, success);

    if (success) {
        m_doneCount++;
        if (m_doneNames.size() < 4)   // 仅保留前几个名字用于提示
            m_doneNames.append(name);
    } else {
        m_failCount++;
    }

    if (!m_notifyTimer) {
        m_notifyTimer = new QTimer(this);
        m_notifyTimer->setSingleShot(true);
        connect(m_notifyTimer, &QTimer::timeout, this, &MainWindow::onNotifyTimer);
    }
    m_notifyTimer->start(750);  // 750ms 内的连续完成合并为一条

    // 全部下载完成 → 检查是否需要自动关机/休眠
    maybeAutoPowerAction();
}

void MainWindow::onNotifyTimer()
{
    if (!m_trayIcon) {
        m_doneCount = 0; m_failCount = 0; m_doneNames.clear();
        return;
    }

    QString title, body;
    if (m_doneCount > 0 && m_failCount == 0) {
        title = QStringLiteral("下载完成");
        body  = (m_doneCount == 1)
                    ? m_doneNames.first()
                    : QStringLiteral("%1 个下载已完成").arg(m_doneCount);
    } else if (m_doneCount == 0 && m_failCount > 0) {
        title = QStringLiteral("下载失败");
        body  = QStringLiteral("%1 个下载失败").arg(m_failCount);
        if (!m_lastError.isEmpty())
            body += QStringLiteral("：%1").arg(m_lastError);
    } else {
        title = QStringLiteral("下载完成");
        body  = QStringLiteral("完成 %1 个，失败 %2 个").arg(m_doneCount).arg(m_failCount);
    }

    m_trayIcon->showMessage(title, body,
                            QSystemTrayIcon::Information, 3500);

    // 仅在有成功项时播放完成提示音（每次聚合最多一次）
    if (m_doneCount > 0)
        playCompletionChime();

    m_doneCount = 0; m_failCount = 0; m_doneNames.clear(); m_lastError.clear();
}

void MainWindow::onTaskStateChanged(int taskId, int state)
{
    m_taskModel->updateStatus(taskId, state);
    m_states[taskId] = state;
    if (state == 3 || state == 4 || state == 5)
        m_speeds[taskId] = 0;  // 终态清零速度
    if (state == 1)
        cancelPendingAutoPower();   // 新下载开始 → 中止待定的关机/休眠
    else
        maybeAutoPowerAction();     // 终态变化后检查是否全部完成
    syncStatus();
}

void MainWindow::refreshFromEngine()
{
    int ids[128];
    int n = dlmgr_list(ids, 128);

    // 批量拉取所有任务实时详情：单次持锁、按 g_tasks[] 顺序填充，与上面 ids[] 索引对齐。
    // 取代原先对每个任务单独 dlmgr_get_task_info（每 tick 加锁 n 次 + 二分查找 n 次），
    // 把 4Hz 热路径上的锁竞争降到 2 次（list + infos），不与下载线程的进度回调抢锁。
    TaskInfo infos[128];
    int m = dlmgr_get_task_infos(infos, 128);
    int total = n < m ? n : m;

    // 仅当存在尚未同步进模型（恢复/历史）的任务时，才去拉取 URL 摘要——
    // 避免 4Hz 循环每 tick 都做 O(n) 的引擎摘要与字符串拼接（已同步后纯浪费）
    QMap<int, QString> urlMap;
    bool needUrlMap = false;
    for (int i = 0; i < total; ++i) {
        if (!m_taskModel->contains(ids[i])) { needUrlMap = true; break; }
    }
    if (needUrlMap) {
        TaskSummary summaries[128];
        int sumCount = dlmgr_get_task_summary(summaries, 128);
        for (int i = 0; i < sumCount; ++i)
            urlMap[summaries[i].task_id] = QString::fromUtf8(summaries[i].url);
    }

    for (int i = 0; i < total; ++i) {
        const TaskInfo &info = infos[i];

        if (m_taskModel->contains(ids[i])) {
            // 已存在于列表：仅当引擎值相对模型当前值有变化时才回写。
            // 活动任务由 onTaskProgress 实时同步、其余任务多已静止，4Hz 循环直接
            // 对每任务调用三次模型更新（含 m_rowOf 哈希查找与字段比较）属冗余；
            // 差量跳过未变任务，避免对「已完成/暂停/空闲」占多数的列表每 tick 空转。
            QString proto = QString::fromUtf8(info.http_version);
            const int prevState = m_states.value(ids[i], -1);
            if (m_taskModel->isStale(ids[i], info.downloaded, (int)info.speed_bps, info.status, proto)) {
                m_taskModel->updateStatus(ids[i], info.status);
                m_taskModel->updateProgress(ids[i], info.downloaded, (int)info.speed_bps);
                m_taskModel->setProtocol(ids[i], proto);
            }
            // 失败原因只在「状态跃迁进出失败态」时同步一次：
            // 放进上面 4Hz 的差量更新里会让每 tick 都做一次字符串比较，没必要。
            if (info.status == 4 && prevState != 4)
                m_taskModel->setErrorMsg(ids[i], QString::fromUtf8(info.error_msg).trimmed());
            else if (info.status != 4 && prevState == 4)
                m_taskModel->setErrorMsg(ids[i], QString());
            m_states[ids[i]] = info.status;
            m_speeds[ids[i]] = (info.status == 1) ? (int)info.speed_bps : 0;
            continue;
        }

        // 恢复的任务没有 Qt 回调，重新绑定以便实时刷新
        m_manager->rebindTask(ids[i]);

        TaskRow row;
        row.id         = ids[i];
        row.fileName   = QString::fromUtf8(info.filename);
        if (row.fileName.isEmpty())
            row.fileName = QStringLiteral("任务 #%1").arg(ids[i]);
        row.url            = urlMap.value(ids[i]);
        row.fileSize       = info.file_size;
        row.downloaded     = info.downloaded;
        row.speedBps       = (int)info.speed_bps;
        row.lastConnection = QDateTime::currentDateTime();
        row.state          = info.status;
        row.statusText     = TaskListModel::stateText(info.status);
        row.protocol       = QString::fromUtf8(info.http_version);
        m_taskModel->addTask(row);

        m_speeds[ids[i]] = 0;
        m_states[ids[i]] = info.status;
    }
    syncStatus();
}

void MainWindow::onStartAll()
{
    dlmgr_start_all();
    refreshFromEngine();
    syncStatus();
}

void MainWindow::onPauseAll()
{
    dlmgr_stop_all();
    refreshFromEngine();
    syncStatus();
}

void MainWindow::onRemoveCompleted()
{
    dlmgr_remove_completed();
    // 同步模型：移除已不在引擎队列中的任务行
    int ids[128];
    int n = dlmgr_list(ids, 128);
    QSet<int> liveIds;
    for (int i = 0; i < n; ++i)
        liveIds.insert(ids[i]);
    for (int row = m_taskModel->rowCount() - 1; row >= 0; --row) {
        int tid = m_taskModel->taskIdForRow(row);
        if (!liveIds.contains(tid)) {   // O(1) 哈希判定，避免对每个任务线性扫 ids[]
            m_taskModel->removeTask(tid);
            m_speeds.remove(tid);
            m_states.remove(tid);
        }
    }
    syncStatus();
}

void MainWindow::recomputeStatusAggregates()
{
    // 由 m_speeds/m_states 权威重算缓存（O(n)）。仅在结构/状态变更时调用，
    // 4Hz 显示路径使用缓存值，避免热路径重复全量遍历。
    qint64 s = 0;
    int a = 0;
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        if (it.value() == 1) {                 // 仅“下载中”计入活动数与总速度
            ++a;
            qint64 sp = m_speeds.value(it.key(), 0);
            if (sp > 0) s += sp;
        }
    }
    m_totalSpeed  = s;
    m_activeCount = a;
}

void MainWindow::syncStatus()
{
    recomputeStatusAggregates();
    updateStatusBar();
}

void MainWindow::updateStatusBar()
{
    // O(1)：直接读取缓存聚合值（m_totalSpeed / m_activeCount），不再遍历两张 Map
    qint64 totalSpeed = m_totalSpeed;
    int active = m_activeCount, total = m_taskModel->rowCount();

    // 状态栏总速度轻度 EMA 平滑（α=0.3），与引擎速度平滑一致，避免数字疯狂跳动
    if (totalSpeed <= 0)
        m_speedEma = 0;                       // 全部停止时立即归零，不做拖尾衰减
    else if (m_speedEma <= 0)
        m_speedEma = totalSpeed;
    else
        m_speedEma = (m_speedEma * 7 + totalSpeed * 3) / 10;
    qint64 dispSpeed = (totalSpeed <= 0) ? 0 : m_speedEma;

    // 总速度跟 4Hz 节拍照常刷——它本来就是「现在有多快」，慢一拍就失去意义。
    // 侧栏计数是聚合统计，降频到 1Hz 就够：每帧重算会让整棵树每秒重绘四次。
    if (m_speedPill)
        m_speedPill->setText(formatSpeedText(dispSpeed));
    if (m_sidebar && ++m_sidebarTick >= 4) {
        m_sidebarTick = 0;
        refreshSidebarCounts();   // 内部顺带把三个计数刷到侧栏状态行
    }

    m_taskCountLabel->setText(QStringLiteral("任务: %1  下载中: %2").arg(total).arg(active));
}

namespace {

// 左侧树的计数汇总：一次遍历任务表，按「类别 × 文件类型」和「队列」分桶。
// 单独成结构体是为了让 refreshSidebarCounts 只遍历任务表一遍（O(任务数)），
// 而不是每个树节点都去全量扫一次（O(节点数 × 任务数)）。
struct SidebarTally {
    struct Bucket { int all = 0, dl = 0, done = 0, fail = 0, cancel = 0; };

    Bucket m_all;                     // 全部任务的总计（文件类型 = All 时用）
    QHash<int, Bucket> byType;        // 键 = FileType 枚举值
    QHash<QString, int> byQueue;      // 键 = 队列名

    int categoryCount(int cat, int ft) const
    {
        const Bucket& b = (ft == int(FileType::All)) ? m_all : byType.value(ft);
        switch (static_cast<Category>(cat)) {
            case Category::All:         return b.all;
            case Category::Downloading: return b.dl;
            case Category::Completed:   return b.done;
            case Category::Failed:      return b.fail;
            case Category::Cancelled:   return b.cancel;
        }
        return 0;
    }
};

} // namespace

// 重算左侧树各节点的任务数并写进 CountRole。返回 -1 的节点（功能入口）不显示徽标。
// 只在数值真的变化时才 setData：本函数由 4Hz 状态栏节拍降频到 1Hz 调用，
// 无脑 setData 会让整棵树每秒重绘一次，既费电又会让滚动条闪烁。
void MainWindow::refreshSidebarCounts()
{
    if (!m_categoryModel || !m_taskModel)
        return;

    SidebarTally t;
    for (const TaskRow& r : m_taskModel->tasks()) {
        const int ft = static_cast<int>(CategoryFilterProxy::detectFileType(r.fileName));
        SidebarTally::Bucket& b = t.byType[ft];

        ++b.all; ++t.m_all.all;
        if (r.state == 0 || r.state == 1 || r.state == 2) { ++b.dl;     ++t.m_all.dl;     }
        else if (r.state == 3)                            { ++b.done;   ++t.m_all.done;   }
        else if (r.state == 4)                            { ++b.fail;   ++t.m_all.fail;   }
        else if (r.state == 5)                            { ++b.cancel; ++t.m_all.cancel; }

        if (!r.queue.isEmpty())
            ++t.byQueue[r.queue];
    }

    auto countOf = [&t](QStandardItem* it) -> int {
        switch (static_cast<TreeItemType>(it->data(TypeRole).toInt())) {
            case TreeItemType::Category:
                return t.categoryCount(it->data(CategoryRole).toInt(),
                                       it->data(FileTypeRole).toInt());
            case TreeItemType::Queue:
                return t.byQueue.value(it->data(QueueNameRole).toString(), 0);
            case TreeItemType::QueueParent: {
                int sum = 0;
                for (int i = 0; i < it->rowCount(); ++i)
                    sum += t.byQueue.value(it->child(i)->data(QueueNameRole).toString(), 0);
                return sum;
            }
            default:
                return -1;   // 站点抓取方案等功能入口：没有「数量」这个概念
        }
    };

    // 递归深度实测只有 2 层（分类 → 文件类型、队列 → 队列项），不存在爆栈风险
    std::function<void(QStandardItem*)> walk = [&](QStandardItem* it) {
        const QVariant cur = it->data(CountRole);
        const int c = countOf(it);
        if (!cur.isValid() || cur.toInt() != c)
            it->setData(c, CountRole);
        for (int i = 0; i < it->rowCount(); ++i)
            walk(it->child(i));
    };
    for (int i = 0; i < m_categoryModel->rowCount(); ++i)
        walk(m_categoryModel->item(i));

    // 侧栏状态行与树共用同一次遍历的结果，不额外扫一遍任务表
    if (m_sidebar && m_sidebar->stats())
        m_sidebar->stats()->setCounts(t.m_all.dl, t.m_all.done, t.m_all.fail);
}

// ── IPC 单实例通信 ──────────────────────────────

void MainWindow::startIpcServer()
{
    m_ipcServer = new QLocalServer(this);

    // 如果有残留的旧服务端（上次崩溃），先移除
    QLocalServer::removeServer(QStringLiteral("idm-next-ipc"));

    if (!m_ipcServer->listen(QStringLiteral("idm-next-ipc"))) {
        Log::warn(QStringLiteral("IPC server 启动失败: %1").arg(m_ipcServer->errorString()));
        return;
    }

    connect(m_ipcServer, &QLocalServer::newConnection,
            this, &MainWindow::onIpcConnection);

    Log::info(QStringLiteral("IPC server 已启动，监听 idm-next-ipc"));
}

void MainWindow::applyWebServer()
{
    if (!m_settings.webEnabled()) {
        if (m_webServer && m_webServer->isListening()) {
            m_webServer->stopServer();
            Log::info(QStringLiteral("Web 管理界面已关闭"));
        }
        return;
    }

    if (!m_webServer) {
        m_webServer = new WebServer(this);
        m_webServer->setProviders(
            [this]() -> QJsonArray {
                QJsonArray arr;
                const auto& tasks = m_taskModel->tasks();
                for (const auto& t : tasks) {
                    QJsonObject o;
                    o[QStringLiteral("id")]         = t.id;
                    o[QStringLiteral("name")]       = t.fileName;
                    o[QStringLiteral("url")]        = t.url;
                    o[QStringLiteral("size")]       = t.fileSize;
                    o[QStringLiteral("downloaded")] = t.downloaded;
                    o[QStringLiteral("speed")]      = m_speeds.value(t.id, 0);
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
            },
            [this](const QString& url, const QString& dir, const QString& queue,
                   int threads) -> int {
                QString saveDir = dir.isEmpty() ? m_settings.defaultSaveDir() : dir;
                int th = threads > 0 ? threads : m_settings.maxThreads();
                return internalAddTask(url, saveDir, QString(), th, queue, QString());
            },
            [this](int id, const QString& action) -> bool {
                if (action == QStringLiteral("pause"))        pauseTaskById(id);
                else if (action == QStringLiteral("resume"))  resumeTaskById(id);
                else if (action == QStringLiteral("cancel"))  cancelTaskById(id);
                else if (action == QStringLiteral("remove"))  removeTaskById(id);
                else return false;
                return true;
            });
    }

    // 已监听则先停后启，以套用新的端口 / 令牌
    if (m_webServer->isListening())
        m_webServer->stopServer();

    if (m_webServer->startServer(static_cast<quint16>(m_settings.webPort()),
                                 m_settings.webToken())) {
        Log::info(QStringLiteral("Web 管理界面已启动: http://127.0.0.1:%1/")
                      .arg(m_settings.webPort()));
        if (m_trayIcon && m_trayIcon->isVisible())
            m_trayIcon->showMessage(QStringLiteral("Web 管理界面"),
                QStringLiteral("已启动：http://127.0.0.1:%1/").arg(m_settings.webPort()),
                QSystemTrayIcon::Information, 4000);
    } else {
        Log::warn(QStringLiteral("Web 管理界面启动失败: %1").arg(m_webServer->errorString()));
        if (m_trayIcon && m_trayIcon->isVisible())
            m_trayIcon->showMessage(QStringLiteral("Web 管理界面"),
                QStringLiteral("启动失败：端口 %1 可能被占用").arg(m_settings.webPort()),
                QSystemTrayIcon::Warning, 4000);
    }
}

void MainWindow::onIpcConnection()
{
    QLocalSocket* client = m_ipcServer->nextPendingConnection();
    if (!client) return;

    // 读取客户端发来的 JSON 命令：原生消息以单次写入整体送达，但可能跨多次
    // readyRead 分片到达；此处用有界循环累积，直到得到可解析的完整 JSON 对象，
    // 避免单次 readAll 截断导致命令解析失败（原实现会退化成「不支持的操作」）。
    QByteArray data;
    for (int i = 0; i < 10; ++i) {
        if (client->bytesAvailable() == 0)
            client->waitForReadyRead(200);   // 最多 ~2s，正常情况首轮即有数据
        QByteArray chunk = client->readAll();
        if (!chunk.isEmpty())
            data.append(chunk);
        if (!data.isEmpty() && QJsonDocument::fromJson(data).isObject())
            break;   // 已收到完整命令
        if (client->state() != QLocalSocket::ConnectedState)
            break;   // 对端已断开
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject msg = doc.object();

    QString command = msg.value("command").toString();
    QJsonObject response;

    if (command == "add") {
        QString url = msg.value("url").toString();
        QString filename = msg.value("filename").toString();
        QString dir = msg.value("dir").toString();
        QString queue = msg.value("queue").toString();   // 非空=加入队列
        QString format = msg.value("format").toString(); // 视频画质（yt-dlp -f 选择串）
        int threads = msg.value("threads").toInt();

        if (url.isEmpty()) {
            response["success"] = false;
            response["message"] = QStringLiteral("URL 为空");
        } else {
            addTaskFromUrl(url, filename, dir, threads, queue, format);
            response["success"] = true;
            response["message"] = QStringLiteral("已添加下载: %1").arg(filename.isEmpty() ? url : filename);
        }
    } else if (command == "list") {
        // 简化：返回任务数量
        response["success"] = true;
        response["message"] = QStringLiteral("当前 %1 个任务").arg(m_taskModel->rowCount());
    } else {
        response["success"] = false;
        response["message"] = QStringLiteral("不支持的操作: %1").arg(command);
    }

    // 发送响应
    QByteArray respData = QJsonDocument(response).toJson(QJsonDocument::Compact);
    client->write(respData);
    client->flush();
    client->waitForBytesWritten(2000);
    client->disconnectFromServer();

    Log::info(QStringLiteral("IPC 请求: %1 → %2")
                 .arg(command, response.value("success").toBool() ? "成功" : "失败"));
}

void MainWindow::addTaskFromUrl(const QString& url, const QString& filename,
                                 const QString& dir, int threads,
                                 const QString& queue, const QString& format)
{
    QString saveDir = dir.isEmpty() ? m_settings.defaultSaveDir() : dir;
    int threadCount = threads > 0 ? threads : m_settings.maxThreads();

    int id = internalAddTask(url, saveDir, filename, threadCount, queue, format);
    if (id <= 0) {
        Log::warn(QStringLiteral("IPC 添加任务失败: %1").arg(url));
        return;
    }

    QString name = filename.isEmpty() ? QUrl(url).fileName() : filename;
    if (name.isEmpty())
        name = url;

    // 如果窗口最小化到托盘，弹出通知
    if (m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(
            QStringLiteral("新下载任务"),
            QStringLiteral("来自浏览器扩展: %1").arg(name),
            QSystemTrayIcon::Information, 3000);
    }

    // 恢复窗口到前台（可选：如果用户正在使用电脑）
    showNormal();
    raise();
    activateWindow();

    Log::info(QStringLiteral("IPC 添加任务 #%1: %2").arg(id).arg(url));
}

// ── 外部入口：浏览器协议关联 / 命令行位置参数 ──────────
void MainWindow::enqueueUrl(const QString& url)
{
    QString u = url.trimmed();
    if (u.isEmpty())
        return;
    int id = internalAddTask(u, m_settings.defaultSaveDir(), QString(),
                             m_settings.maxThreads(), QString());
    if (id > 0)
        Log::info(QStringLiteral("外部 URL 加入任务 #%1: %2").arg(id).arg(u));
    // 置前窗口，提示用户
    showNormal();
    raise();
    activateWindow();
}

// ── 统一的新增任务入口 ──────────────────────────────
// queue 为空：立即开始下载；否则加入指定队列，由 onQueueScheduler 调度启动
int MainWindow::internalAddTask(const QString& url, const QString& dir, const QString& name,
                                int threads, const QString& queue, const QString& format,
                                bool archiveByType)
{
    // 浏览器扩展等外部来源可能传入一个尚未存在的队列名：自动创建，
    // 否则该队列的 HTTP 任务会永远停在 state 0 等待调度（卡死）。
    if (!queue.isEmpty() && m_queueMgr && !m_queueMgr->contains(queue))
        m_queueMgr->addQueue(queue, 2);

    // 按文件类型自动归档（IDM 风格）：开启时把文件归入 saveDir/<分类>/ 子目录。
    // archiveByType=false 表示调用方已自行拼好归档子目录（如站点抓取器），不再二次归档。
    QString finalDir = dir;
    if (archiveByType && m_settings.autoArchiveByType() && !dir.isEmpty()) {
        FileType ft = FileType::Other;
        if (VideoDownloader::isVideoUrl(url)
                || url.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)
                || url.endsWith(QStringLiteral(".mpd"),  Qt::CaseInsensitive)) {
            ft = FileType::Video;  // 视频站点 URL 无扩展名，单独判定
        } else {
            QString fname = name.isEmpty() ? QUrl(url).fileName() : name;
            if (fname.isEmpty())
                fname = url;
            ft = CategoryFilterProxy::detectFileType(fname);
        }
        finalDir = dir + QStringLiteral("/") + CategoryFilterProxy::fileTypeFolderName(ft);
    }
    QDir().mkpath(finalDir);

    // ── BT/磁力/FTP（magnet / .torrent / ftp://）：交给 aria2 后端（aria2 式 RPC 集成）
    if (TorrentDownloader::isAria2Url(url)) {
        int id = m_videoIdSeq++;
        auto* td = new TorrentDownloader(this);
        td->setTaskId(id);

        connect(td, &TorrentDownloader::progressChanged, this, &MainWindow::onTaskProgress);
        connect(td, &TorrentDownloader::completed,      this, &MainWindow::onTaskCompleted);
        connect(td, &TorrentDownloader::stateChanged,   this, &MainWindow::onTaskStateChanged);

        TaskRow row;
        row.id = id;
        row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
        if (row.fileName.isEmpty())
            row.fileName = url;
        row.url       = url;
        row.fileSize  = -1;
        row.downloaded = 0;
        row.speedBps  = 0;
        row.lastConnection = QDateTime::currentDateTime();
        row.queue     = queue;
        // 无队列才立即开始；且无队列同样受「同时下载的任务数」全局上限约束——
        // 满额时留在 state 0，由 onQueueScheduler 有空位再自动拉起（与有队列的任务一致）。
        const bool startNow = queue.isEmpty() && hasFreeDownloadSlot();
        row.state     = startNow ? 1 : 0;
        row.statusText = TaskListModel::stateText(row.state);
        m_taskModel->addTask(row);

        m_speeds[id] = 0;
        m_states[id] = row.state;
        m_torrentTasks[id] = td;

        DownloadRequest req;
        req.url       = url;
        req.savePath  = finalDir;
        req.fileName  = name;
        req.threadCount = threads > 0 ? threads : m_settings.maxThreads();
        req.isMagnet  = url.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive);
        if (startNow) {
            td->start(req);
            cancelPendingAutoPower();
        } else {
            m_pendingStream[id] = req;   // 交给 onQueueScheduler 延迟启动
            if (queue.isEmpty()) m_deferredByCap.insert(id);   // 等全局并发空位
        }

        syncStatus();
        HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
        Log::info(QStringLiteral("添加 aria2 任务 #%1: %2").arg(id).arg(url));
        return id;
    }

    // ── HLS（M3U8）：交给原生 HlsDownloader（自建解析 + ffmpeg 合并），不依赖 yt-dlp
    if (url.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        int id = m_videoIdSeq++;  // 与视频/引擎任务区分的编号偏移
        auto* hd = new HlsDownloader(this);
        hd->setTaskId(id);

        connect(hd, &HlsDownloader::progressChanged, this, &MainWindow::onTaskProgress);
        connect(hd, &HlsDownloader::completed,      this, &MainWindow::onTaskCompleted);
        connect(hd, &HlsDownloader::stateChanged,   this, &MainWindow::onTaskStateChanged);

        TaskRow row;
        row.id = id;
        row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
        if (row.fileName.isEmpty())
            row.fileName = url;
        row.url       = url;
        row.fileSize  = -1;
        row.downloaded = 0;
        row.speedBps  = 0;
        row.lastConnection = QDateTime::currentDateTime();
        row.queue     = queue;
        // 无队列才立即开始；且无队列同样受「同时下载的任务数」全局上限约束——
        // 满额时留在 state 0，由 onQueueScheduler 有空位再自动拉起（与有队列的任务一致）。
        const bool startNow = queue.isEmpty() && hasFreeDownloadSlot();
        row.state     = startNow ? 1 : 0;
        row.statusText = TaskListModel::stateText(row.state);
        m_taskModel->addTask(row);

        m_speeds[id] = 0;
        m_states[id] = row.state;
        m_hlsTasks[id] = hd;

        DownloadRequest req;
        req.url       = url;
        req.savePath  = finalDir;
        req.fileName  = name;
        req.threadCount = threads > 0 ? threads : m_settings.maxThreads();
        req.isMagnet  = false;
        if (!format.isEmpty())
            req.extra.insert(QStringLiteral("format"), format);
        if (startNow) {
            hd->start(req);
            cancelPendingAutoPower();
        } else {
            m_pendingStream[id] = req;   // 交给 onQueueScheduler 延迟启动
            if (queue.isEmpty()) m_deferredByCap.insert(id);   // 等全局并发空位
        }

        syncStatus();
        HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
        Log::info(QStringLiteral("添加 HLS 任务 #%1: %2").arg(id).arg(url));
        return id;
    }

    // ── 视频站点（YouTube / B 站 / …）/ DASH：交给 yt-dlp 后端，不走分段 HTTP 引擎
    bool isVideo = VideoDownloader::isVideoUrl(url)
                   || url.endsWith(QStringLiteral(".mpd"),  Qt::CaseInsensitive);
    if (isVideo) {
        int id = m_videoIdSeq++;
        auto* vd = new VideoDownloader(this);
        vd->setTaskId(id);

        // 信号接入统一的任务进度/完成/状态处理（与 C 引擎任务共用同一套 UI 更新逻辑）
        connect(vd, &VideoDownloader::progressChanged, this, &MainWindow::onTaskProgress);
        connect(vd, &VideoDownloader::completed,      this, &MainWindow::onTaskCompleted);
        connect(vd, &VideoDownloader::stateChanged,   this, &MainWindow::onTaskStateChanged);

        TaskRow row;
        row.id = id;
        row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
        if (row.fileName.isEmpty())
            row.fileName = url;
        row.url       = url;
        row.fileSize  = -1;
        row.downloaded = 0;
        row.speedBps  = 0;
        row.lastConnection = QDateTime::currentDateTime();
        row.queue     = queue;
        // 无队列才立即开始；且无队列同样受「同时下载的任务数」全局上限约束——
        // 满额时留在 state 0，由 onQueueScheduler 有空位再自动拉起（与有队列的任务一致）。
        const bool startNow = queue.isEmpty() && hasFreeDownloadSlot();
        row.state     = startNow ? 1 : 0;
        row.statusText = TaskListModel::stateText(row.state);
        m_taskModel->addTask(row);

        m_speeds[id] = 0;
        m_states[id] = row.state;
        m_videoTasks[id] = vd;

        DownloadRequest req;
        req.url       = url;
        req.savePath  = finalDir;
        req.fileName  = name;
        req.threadCount = threads > 0 ? threads : m_settings.maxThreads();
        req.isMagnet  = false;
        if (!format.isEmpty())
            req.extra.insert(QStringLiteral("format"), format);
        if (startNow) {
            vd->start(req);
            cancelPendingAutoPower();
        } else {
            m_pendingStream[id] = req;   // 交给 onQueueScheduler 延迟启动
            if (queue.isEmpty()) m_deferredByCap.insert(id);   // 等全局并发空位
        }

        syncStatus();
        HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
        Log::info(QStringLiteral("添加视频任务 #%1 (yt-dlp): %2").arg(id).arg(url));
        return id;
    }

    int id = m_manager->addTask(url, finalDir, name, threads > 0 ? threads : m_settings.maxThreads());
    if (id <= 0)
        return -1;

    TaskRow row;
    row.id = id;
    row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
    if (row.fileName.isEmpty())
        row.fileName = url;
    row.url            = url;
    row.fileSize       = -1;
    row.downloaded     = 0;
    row.speedBps       = 0;
    row.lastConnection = QDateTime::currentDateTime();
    row.queue          = queue;
    // 与媒体任务一致：无队列也受「同时下载的任务数」约束，满额则排队等待
    const bool startNow = queue.isEmpty() && hasFreeDownloadSlot();
    if (startNow) {
        m_manager->startTask(id);
        row.state = 1;  // 下载中
    } else {
        row.state = 0;  // 等待队列调度（已入队列，或已达同时下载上限）
        if (queue.isEmpty()) m_deferredByCap.insert(id);   // 等全局并发空位
    }
    row.statusText = TaskListModel::stateText(row.state);
    m_taskModel->addTask(row);

    m_speeds[id] = 0;
    m_states[id] = row.state; cancelPendingAutoPower();
    syncStatus();
    HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
    Log::info(QStringLiteral("添加任务 #%1 (队列:%2): %3")
                  .arg(id).arg(queue.isEmpty() ? QStringLiteral("无") : queue).arg(url));
    return id;
}

// ── 后端无关的下载控制：按 taskId 自动分派到 C 引擎或 yt-dlp 视频后端 ──
void MainWindow::startTaskById(int id)
{
    m_deferredByCap.remove(id);  // 无论手动还是调度器放行，都算已经脱离等待
    // 加入队列的媒体任务在添加时未立即启动，缓存了 DownloadRequest；
    // 此处由队列调度器放行后真正拉起后端（视频/HLS/BT）。
    if (m_pendingStream.contains(id)) {
        DownloadRequest req = m_pendingStream.take(id);
        if (isTorrentTask(id)) {
            if (auto* td = m_torrentTasks.value(id)) td->start(req);
        } else if (isHlsTask(id)) {
            if (auto* hd = m_hlsTasks.value(id)) hd->start(req);
        } else if (isVideoTask(id)) {
            if (auto* vd = m_videoTasks.value(id)) vd->start(req);
        }
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    if (isStreamTask(id)) return;  // 视频/HLS 任务在添加时已由后端启动
    m_manager->startTask(id);
    m_states[id] = 1; cancelPendingAutoPower();
    m_taskModel->updateStatus(id, 1);
    syncStatus();
}

void MainWindow::pauseTaskById(int id)
{
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->pause();
        m_states[id] = 2;
        m_taskModel->updateStatus(id, 2);
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->pause();
        m_states[id] = 2;
        m_taskModel->updateStatus(id, 2);
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->pause();
        m_states[id] = 2;
        m_taskModel->updateStatus(id, 2);
        return;
    }
    m_manager->pauseTask(id);
    m_states[id] = 2;
    m_taskModel->updateStatus(id, 2);
}

void MainWindow::resumeTaskById(int id)
{
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->resume();
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->resume();
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->resume();
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    m_manager->resumeTask(id);
    m_states[id] = 1; cancelPendingAutoPower();
    m_taskModel->updateStatus(id, 1);
    syncStatus();
}

void MainWindow::cancelTaskById(int id)
{
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->cancel();
        m_states[id] = 5;
        m_taskModel->updateStatus(id, 5);
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->cancel();
        m_states[id] = 5;
        m_taskModel->updateStatus(id, 5);
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->cancel();
        m_states[id] = 5;
        m_taskModel->updateStatus(id, 5);
        return;
    }
    m_manager->cancelTask(id);
    m_states[id] = 5;
    m_taskModel->updateStatus(id, 5);
    syncStatus();
}

void MainWindow::restartTaskById(int id)
{
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->resume();  // HLS 无断点续传，重新下载
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->resume();  // yt-dlp 无断点续传，重新下载
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->resume();  // aria2 重新拉起任务
        m_states[id] = 1; cancelPendingAutoPower();
        m_taskModel->updateStatus(id, 1);
        syncStatus();
        return;
    }
    m_manager->restartTask(id);
    m_taskModel->updateProgress(id, 0, 0);
    m_speeds[id] = 0;
    m_states[id] = 1; cancelPendingAutoPower();
    syncStatus();
    Log::info(QStringLiteral("任务 #%1 已重新开始").arg(id));
}

void MainWindow::removeTaskById(int id)
{
    m_pendingStream.remove(id);  // 清理可能仍在等待队列调度的媒体任务请求
    // 任务没了，定时/重复设置也要一起清掉，否则 m_scheduledTasks 里会残留
    // 指向已删除 id 的条目（到期后 startTaskById 作用在不存在的任务上）。
    m_scheduledTasks.remove(id);
    m_recurringTasks.remove(id);
    m_deferredByCap.remove(id);  // 已被并发上限暂缓的任务删除后不必再等空位
    saveSchedules();             // 同步落盘，避免留下指向已删任务的幽灵定时
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) {
            hd->cancel();
            hd->deleteLater();
            m_hlsTasks.remove(id);
        }
        m_taskModel->removeTask(id);
        m_speeds.remove(id);
        m_states.remove(id);
        syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) {
            vd->cancel();
            vd->deleteLater();
            m_videoTasks.remove(id);
        }
        m_taskModel->removeTask(id);
        m_speeds.remove(id);
        m_states.remove(id);
        syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) {
            td->cancel();
            td->deleteLater();
            m_torrentTasks.remove(id);
        }
        m_taskModel->removeTask(id);
        m_speeds.remove(id);
        m_states.remove(id);
        syncStatus();
        return;
    }
    m_manager->removeTask(id);
    m_taskModel->removeTask(id);
    m_speeds.remove(id);
    m_states.remove(id);
    syncStatus();
}

// ── 站点抓取器 ──────────────────────────────────────
void MainWindow::onSiteExplorer()
{
    QList<QString> names;
    for (const auto& q : m_queueMgr->queues())
        names << q.name;
    auto* dlg = new SiteExplorerDialog(names, m_settings.defaultSaveDir(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &SiteExplorerDialog::requestAddUrl, this, [this](const QString& url) {
        internalAddTask(url, m_settings.defaultSaveDir(), QString(), 0, QString());
    });
    connect(dlg, &SiteExplorerDialog::requestAddUrlToQueue, this,
            [this](const QString& url, const QString& q) {
        internalAddTask(url, m_settings.defaultSaveDir(), QString(), 0, q);
    });
    connect(dlg, &SiteExplorerDialog::requestAddUrlWithDir, this,
            [this](const QString& url, const QString& dir, const QString& q) {
        // 站点抓取器已自行拼好 saveDir/<分类>/，传 archiveByType=false 避免二次归档
        internalAddTask(url, dir, QString(), 0, q, QString(), false);
    });
    dlg->show();
}

// ── 下载队列管理 ────────────────────────────────────
void MainWindow::onManageQueues()
{
    QueueManagerDialog dlg(m_queueMgr, this);
    dlg.exec();
}

void MainWindow::onShowHistory()
{
    HistoryDialog dlg(this);
    dlg.exec();
}

// ── 全局并发余量：设置页「同时下载的任务数」──
bool MainWindow::hasFreeDownloadSlot() const
{
    if (!m_taskModel)
        return true;
    const int limit = qMax(1, m_settings.maxConcurrent());
    int running = 0;
    for (const auto& t : m_taskModel->tasks())
        if (t.state == 1)
            running++;
    return running < limit;
}

// ── 定时/重复设置的持久化 ──
/* 原本这两张表只活在内存里，关掉程序就没了（对话框里只能如实写「退出后不保留」）。
 * 现在存成一条紧凑配置串："<任务ID>|<ISO 时间>|<是否每日重复>"，多条用 ';' 分隔。
 * 用**一条**键而不是每组一个键：写入是原子的（不会崩在半套设置上），
 * 删除任务时也不用去逐个清理 QSettings 里残留的键。
 * 存储走 AppPaths::settings()，这样便携模式（exe 同目录 ini）也能正常保存。 */
static const char *kSchedulesKey = "schedules";

void MainWindow::saveSchedules() const
{
    QStringList items;
    for (auto it = m_scheduledTasks.constBegin(); it != m_scheduledTasks.constEnd(); ++it) {
        items << QStringLiteral("%1|%2|%3")
                     .arg(it.key())
                     .arg(it.value().toString(Qt::ISODate))
                     .arg(m_recurringTasks.contains(it.key()) ? 1 : 0);
    }
    QSettings st = AppPaths::settings();
    st.setValue(QLatin1String(kSchedulesKey), items.join(QLatin1Char(';')));
}

void MainWindow::loadSchedules()
{
    QSettings st = AppPaths::settings();
    const QString raw = st.value(QLatin1String(kSchedulesKey)).toString();
    if (raw.isEmpty())
        return;

    int kept = 0, dropped = 0;
    const QStringList items = raw.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& s : items) {
        const QStringList f = s.split(QLatin1Char('|'));
        if (f.size() != 3)
            continue;
        const int id = f.at(0).toInt();
        const QDateTime at = QDateTime::fromString(f.at(1), Qt::ISODate);
        if (id <= 0 || !at.isValid())
            continue;
        // 任务已不存在（被删掉，或状态文件丢了）→ 丢弃这条，
        // 否则会留下一个到点后对着不存在任务反复触发的「幽灵定时」。
        if (!m_taskModel->contains(id)) {
            dropped++;
            continue;
        }
        m_scheduledTasks[id] = at;
        if (f.at(2) == QLatin1String("1"))
            m_recurringTasks.insert(id);
        kept++;
    }

    if (kept > 0) {
        Log::info(QStringLiteral("已恢复 %1 条定时设置（丢弃 %2 条失效记录）").arg(kept).arg(dropped));
        m_statusBar->showMessage(QStringLiteral("已恢复 %1 条定时下载设置").arg(kept), 5000);
    }
    if (dropped > 0 && kept == 0)
        saveSchedules();   // 顺手把失效记录清掉，别一直留着
}

// ── 队列调度器：各队列并发数超限时排队等待 ───────────
void MainWindow::onQueueScheduler()
{
    if (!m_queueMgr)
        return;

    // 统计每个队列正在下载（state==1）的任务数
    QMap<QString, int> running;
    int totalRunning = 0;                 // 全任务口径（含无队列的手动任务）
    const auto& tasks = m_taskModel->tasks();
    for (const auto& t : tasks) {
        if (t.state != 1)
            continue;
        totalRunning++;
        if (!t.queue.isEmpty())
            running[t.queue]++;
    }

    // 「同时下载的任务数」是全局上限：队列内并发再高也不能把总量顶穿，
    // 否则设置页那一项就只是摆设（各队列各管各的，加起来无上限）。
    const int globalLimit = qMax(1, m_settings.maxConcurrent());
    int globalBudget = globalLimit - totalRunning;

    QTime now = QTime::currentTime();
    for (const auto& q : m_queueMgr->queues()) {
        // 队列被停止：不启动该队列的任何新任务
        if (!q.enabled)
            continue;
        // 计划时间窗口：仅在开始~停止之间才启动新下载
        if (q.useSchedule && (now < q.startAt || now > q.stopAt))
            continue;

        int canStart = qMax(0, q.maxConcurrent - running.value(q.name, 0));
        if (canStart > globalBudget)
            canStart = globalBudget;
        if (canStart <= 0)
            continue;
        // 收集该队列中等待（state==0）的任务，按加入顺序（IDM 默认按序下载）
        QList<int> pending;
        for (const auto& t : tasks)
            if (t.queue == q.name && t.state == 0)
                pending << t.id;
        if (!q.ordered)
            std::reverse(pending.begin(), pending.end());
        for (int i = 0; i < pending.size() && i < canStart; ++i) {
            startTaskById(pending[i]);
            globalBudget--;
        }
    }

    // 无队列（手动/浏览器直下）的等待任务：只放行「因并发上限被暂缓」的那些
    // （m_deferredByCap），用户自己留着不开始的任务绝不擅自拉起。
    if (globalBudget > 0 && !m_deferredByCap.isEmpty()) {
        QList<int> waiting = m_deferredByCap.values();
        std::sort(waiting.begin(), waiting.end());   // 按加入顺序（ID 递增）
        for (int id : waiting) {
            if (globalBudget <= 0)
                break;
            if (m_states.value(id, -1) != 0) {   // 已不是等待态（被删/被手动开始）
                m_deferredByCap.remove(id);
                continue;
            }
            startTaskById(id);
            m_deferredByCap.remove(id);
            globalBudget--;
        }
    }
}

// ── 任务列表右键菜单 ────────────────────────────────
void MainWindow::onTaskContextMenu(const QPoint& pos)
{
    // 右键位置定位到具体行，并设为当前选中（避免操作到别的行）
    QModelIndex idx = m_taskList->indexAt(pos);
    if (!idx.isValid())
        return;
    m_taskList->setCurrentIndex(idx);

    int id = selectedTaskId();
    if (id <= 0)
        return;
    QMenu menu(this);
    menu.addAction(AppIcons::icon(AppIcons::Glyph::Start), QStringLiteral("开始"),
                   this, [this, id] { startTaskById(id); });
    menu.addAction(AppIcons::icon(AppIcons::Glyph::Pause), QStringLiteral("暂停"),
                   this, [this, id] { pauseTaskById(id); });
    menu.addAction(AppIcons::icon(AppIcons::Glyph::Cancel), QStringLiteral("取消"),
                   this, [this, id] { cancelTaskById(id); });
    menu.addAction(AppIcons::icon(AppIcons::Glyph::Restart), QStringLiteral("重新开始"),
                   this, [this, id] { restartTaskById(id); });
    menu.addAction(AppIcons::icon(AppIcons::Glyph::Remove), QStringLiteral("删除"),
                   this, [this, id] { removeTaskById(id); });
    menu.addSeparator();

    QMenu* queueMenu = menu.addMenu(QStringLiteral("移动到队列"));
    for (const auto& q : m_queueMgr->queues())
        queueMenu->addAction(q.name, this, [this, id, q] { m_taskModel->setQueue(id, q.name); });
    queueMenu->addAction(QStringLiteral("移出队列"), this,
                         [this, id] { m_taskModel->setQueue(id, QString()); });

    menu.exec(m_taskList->viewport()->mapToGlobal(pos));
}

// ── 空状态插画：视口尺寸变化时同步 overlay 几何 ───────────
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_taskList->viewport()) {
        if (event->type() == QEvent::Resize)
            m_emptyOverlay->setGeometry(m_taskList->viewport()->rect());
    }
    return QMainWindow::eventFilter(watched, event);
}

// ── 空状态插画：根据列表是否为空切换显隐，并区分“全库无任务/当前分类无任务” ──
void MainWindow::updateEmptyState()
{
    if (!m_emptyOverlay || !m_taskList || !m_proxy || !m_taskModel)
        return;

    const QRect vpr = m_taskList->viewport()->rect();
    m_emptyOverlay->setGeometry(vpr);          // 始终跟随视口尺寸

    const bool proxyEmpty = (m_proxy->rowCount() == 0);
    if (!proxyEmpty) {
        m_emptyOverlay->hide();
        return;
    }

    // 列表为空：区分“全库无任务”与“当前分类筛选无任务”
    if (m_taskModel->rowCount() == 0) {
        m_emptyOverlay->setTitle(QStringLiteral("还没有下载任务"));
        m_emptyOverlay->setSubtitle(QStringLiteral("点击「新建任务」或拖入链接即可开始"));
    } else {
        m_emptyOverlay->setTitle(QStringLiteral("该分类下暂无任务"));
        m_emptyOverlay->setSubtitle(QStringLiteral("切换其他分类或添加新任务"));
    }
    m_emptyOverlay->show();
    m_emptyOverlay->raise();   // 置于表格内容之上
}

// ── 首次显示：确保空状态插画在布局完成后再定位/显隐 ───────
void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    updateEmptyState();
}

// ── 完成提示音：内存合成双音和弦 WAV，经 Windows PlaySound 播放 ──
#ifdef Q_OS_WIN
namespace {
// 懒构建、进程级常驻的内存 WAV 缓冲（供 SND_MEMORY 异步播放，必须长生命周期）
const QByteArray& buildCompletionWav()
{
    static QByteArray wav;
    if (!wav.isEmpty())
        return wav;

    const int sampleRate = 44100;
    const int durationMs = 180;
    const int n = sampleRate * durationMs / 1000;
    const int channels = 1;
    const int bits = 16;
    const double PI = 3.14159265358979323846;

    QVector<qint16> samples(n);
    // A5(880Hz) + E6(1318.5Hz) 双音和弦，指数衰减包络，听感清脆不刺耳
    const double f1 = 880.0, f2 = 1318.5;
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / sampleRate;
        const double env = std::exp(-t * 9.0);
        const double s = 0.5 * std::sin(2 * PI * f1 * t) + 0.5 * std::sin(2 * PI * f2 * t);
        samples[i] = qint16(qBound(-32767.0, s * env * 30000.0, 32767.0));
    }

    const int dataSize = n * channels * (bits / 8);
    const int byteRate = sampleRate * channels * (bits / 8);
    const int blockAlign = channels * (bits / 8);

    QDataStream ds(&wav, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData("RIFF", 4);
    ds << quint32(36 + dataSize);
    ds.writeRawData("WAVE", 4);
    ds.writeRawData("fmt ", 4);
    ds << quint32(16);
    ds << quint16(1);              // PCM
    ds << quint16(channels);
    ds << quint32(sampleRate);
    ds << quint32(byteRate);
    ds << quint16(blockAlign);
    ds << quint16(bits);
    ds.writeRawData("data", 4);
    ds << quint32(dataSize);
    for (int i = 0; i < n; ++i)
        ds << samples[i];

    return wav;
}
} // namespace
#endif

void MainWindow::playCompletionChime()
{
#ifdef Q_OS_WIN
    const QByteArray& wav = buildCompletionWav();
    if (!wav.isEmpty())
        PlaySoundA(reinterpret_cast<LPCSTR>(wav.constData()), nullptr,
                   SND_MEMORY | SND_ASYNC);
#else
    // 非 Windows 平台暂无系统提示音后端，留空（不报错）
#endif
}

// ── 下载完成自动关机/休眠 ──────────────────────────────
bool MainWindow::hasActiveOrPendingTasks() const
{
    // 还有正在下载（state==1）或排队等待（state==0）的任务
    for (auto it = m_states.begin(); it != m_states.end(); ++it)
        if (it.value() == 1 || it.value() == 0)
            return true;
    // 还有未到期的定时任务
    if (!m_scheduledTasks.isEmpty())
        return true;
    return false;
}

bool MainWindow::hasCompletedOrFailed() const
{
    // 至少存在一个已完成（3）或失败（4）的任务，避免空列表误触发
    for (auto it = m_states.begin(); it != m_states.end(); ++it)
        if (it.value() == 3 || it.value() == 4)
            return true;
    return false;
}

void MainWindow::cancelPendingAutoPower()
{
    if (m_autoPowerDlg) {
        m_autoPowerDlg->close();
        m_autoPowerDlg->deleteLater();
        m_autoPowerDlg = nullptr;
    }
    m_autoPowerPending = false;
}

void MainWindow::maybeAutoPowerAction()
{
    if (m_autoPowerPending)
        return;  // 已经在倒计时，避免重复弹窗
    if (m_settings.shutdownAction() == QStringLiteral("none"))
        return;  // 未开启此功能
    if (hasActiveOrPendingTasks())
        return;  // 仍有下载中/排队/未到期定时任务
    if (!hasCompletedOrFailed())
        return;  // 没有任何已完成的下载，避免启动即触发

    m_autoPowerPending = true;
    QString action = m_settings.shutdownAction();
    QString actionText = (action == QStringLiteral("shutdown"))
                             ? QStringLiteral("关机") : QStringLiteral("休眠");
    int grace = qBound(10, m_settings.shutdownGraceSec(), 600);

    auto* dlg = new AutoPowerDialog(
        actionText, grace,
        [this]() {                       // 倒计时结束 → 执行关机/休眠
            m_autoPowerPending = false;
            m_autoPowerDlg = nullptr;
            performAutoPowerAction();
        },
        [this]() {                       // 用户取消
            m_autoPowerPending = false;
            m_autoPowerDlg = nullptr;
            Log::info(QStringLiteral("已取消下载完成后的关机/休眠"));
        },
        this);
    m_autoPowerDlg = dlg;
    dlg->show();
}

void MainWindow::performAutoPowerAction()
{
    // 关机/休眠前先保存任务状态，避免下次启动丢失进度
    if (!m_statePath.isEmpty())
        m_manager->saveState(m_statePath);

#ifdef Q_OS_WIN
    QString action = m_settings.shutdownAction();
    if (action == QStringLiteral("shutdown"))
        windowsShutdown();
    else if (action == QStringLiteral("sleep"))
        windowsHibernate();
#else
    Log::info(QStringLiteral("（非 Windows 平台）跳过电源动作"));
#endif
}

