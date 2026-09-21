#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QMap>
#include <QString>
#include <QDateTime>

#include "settings.h"
#include "legacy/glass_effect.h"   // 玻璃拟态特效（已停用，留档；见 ARCHITECTURE.md）
#include "idownloader.h"   // DownloadRequest：媒体任务延迟启动（队列调度）时缓存请求
#include "task_controller.h"   // L2：任务控制（媒体后端实例表 + 任务 CRUD/分派）
#include "notification_aggregator.h" // L2：下载完成/失败气泡聚合
#include "traffic_mode_controller.h" // L2：流量档位应用（写设置+下发引擎/aria2/托盘+同步 UI）
#include "web_server_controller.h" // L2：Web 管理界面启动/停止/刷新

class DownloadManager;
class TaskListModel;
class QueueManager;
class VideoDownloader;
class HlsDownloader;
class TorrentDownloader;
class QSortFilterProxyModel;
class TaskGroupProxy;
class QTreeView;
class QTableView;
class QStandardItemModel;
class QStandardItem;
class QStatusBar;
class QToolBar;
class QComboBox;
class QActionGroup;
class QMenu;
class QLabel;
class QWidget;
class QSystemTrayIcon;
class QCloseEvent;
class QTimer;
class QDialog;

class AutoPowerController;
class IpcServer;
class ScheduleService;
class TrayController;
class QueueScheduler;
class EmptyStateOverlay;
class SidebarPanel;

// 左侧分类树节点的类型
enum class TreeItemType { Category, Feature, QueueParent, Queue };

// 树节点携带的过滤信息（UserRole 序号）
enum TreeNodeRole {
    TypeRole     = Qt::UserRole + 1,  // TreeItemType
    CategoryRole = Qt::UserRole + 2,  // Category 枚举
    FileTypeRole = Qt::UserRole + 3,  // FileType 枚举
    QueueNameRole= Qt::UserRole + 4,  // 队列名称
    FeatureRole  = Qt::UserRole + 5,  // Feature 标识
    CountRole    = Qt::UserRole + 6   // 该节点下的任务数（-1 = 不显示徽标）
};

// 主窗口：菜单栏 + 工具栏 + 左侧分类树 + 右侧任务列表 + 状态栏
// 所有下载操作经由 DownloadManager（引擎 C→Qt 桥接）完成
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    // 程序启动时从引擎恢复已有任务到列表（需先 dlmgr_load_state）
    void refreshFromEngine();

    // 从 URL 添加任务（IPC/剪贴板/拖放共用）
    // queue 非空 → 加入指定队列（延迟启动）；空 → 立即开始
    // format 仅视频任务使用（yt-dlp -f 格式码），空=最佳画质
    void addTaskFromUrl(const QString& url, const QString& filename = QString(),
                        const QString& dir = QString(), int threads = 0,
                        const QString& queue = QString(),
                        const QString& format = QString());

    // 外部入口：浏览器协议关联（magnet:/http(s):/ftp:）或命令行位置参数传入的 URL，
    // 加入下载并置前窗口。供 NSIS 安装的 magnet 协议处理器与 CLI 转发调用。
    void enqueueUrl(const QString& url);

private slots:
    void onNewTask();
    void onBatchImport();   // 批量导入 URL
    void onStartSelected();
    void onPauseSelected();
    void onResumeSelected();
    void onCancelSelected();
    void onRestartSelected();
    void onRemoveSelected();
    void onRemoveAll();          // 删除全部任务（工具栏“删除全部”）
    void onCategoryClicked(const QModelIndex& index);
    void onTaskDoubleClicked(const QModelIndex& index);  // 双击任务行 → 详情对话框
    void onScheduleDownload();  // 定时下载设置
    void onSettings();
    void onAbout();
    void onSiteExplorer();       // 站点抓取器
    void onManageQueues();       // 下载队列管理
    void onShowHistory();        // 下载历史查看/清空对话框（#46 界面入口）
    // 流量档位切换已抽到 L2 组件 TrafficModeController（见 src/app/traffic_mode_controller.*）

    // 任务列表分组（按类型 / 按队列）折叠
    void applyGroupMode(int mode);       // 统一应用分组模式（视图菜单 / 启动恢复共用）
    void syncGroupMenu();                // 把视图菜单单选状态同步到当前 groupMode
    void onTaskListClicked(const QModelIndex& index); // 点击分组头 → 折叠/展开
    void applyGroupSpans();      // 为分组头行设置跨列合并（modelReset 后重排）

    // 视图缩放
    void onZoomIn();
    void onZoomOut();
    void onZoomReset();

    // 批量操作（接入引擎 start_all / stop_all / remove_completed）
    void onStartAll();
    void onPauseAll();
    void onRemoveCompleted();

    // 引擎信号转发
    void onTaskProgress(int taskId, qint64 downloaded, qint64 total, int speedBps);
    void onTaskCompleted(int taskId, bool success, const QString& error);
    void onTaskStateChanged(int taskId, int state);

    // 下载完成自动关机/休眠（转调 AutoPowerController，见 src/app/auto_power.*）
    void maybeAutoPowerAction();     // 检测是否满足触发条件，满足则弹出倒计时
    void cancelPendingAutoPower();   // 中止待定的关机/休眠（如有新任务开始）

    // 任务右键菜单
    void onTaskContextMenu(const QPoint& pos);

    // 定时任务到期（由 ScheduleService::taskDue 转发，启动下载 + 托盘/状态栏反馈）
    void onScheduledTaskDue(int id);

    // 完成通知聚合已抽到 L2 组件 NotificationAggregator（onTaskCompleted 内调用其 enqueue）
    void updateEmptyState();      // 空状态插画显隐
    void playCompletionChime();   // Windows PlaySound 播放内存合成提示音
    bool eventFilter(QObject* watched, QEvent* event) override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;  // 首次显示时定位空状态插画

private slots:
    void onClipboardChanged();  // 剪贴板变化时检测下载链接
    void rebuildQueueTree();  // 队列增删改后重建左侧树的队列节点

private:
    void setupUi();
    void setupMenus();
    void setupToolBar();
    void setupStatusBar();
    void setupCategoryTree();

    void updateStatusBar();                 // O(1)：直接用缓存聚合值刷新状态栏
    void recomputeStatusAggregates();        // O(n)：由 m_speeds/m_states 权威重算缓存
    void syncStatus();                       // 重算缓存 + 刷新状态栏（结构/状态变更时调用）
    // 流量档位/限速状态同步已抽到 L2 组件 TrafficModeController（见 src/app/traffic_mode_controller.*）
    void refreshSidebarCounts();    // 重算左侧树各节点的任务数，写进模型的 CountRole
    void applyTheme();       // 根据 m_settings.theme() 加载 QSS 样式表
    int  selectedTaskId() const;

    // 任务 CRUD/分派已抽取至 L2 组件 TaskController（见 src/app/task_controller.*），
    // 此处仅保留媒体类型判定作为转发（后端实例表由 m_taskController 持有）。
    bool isVideoTask(int id) const { return m_taskController && m_taskController->isVideoTask(id); }
    bool isHlsTask(int id) const   { return m_taskController && m_taskController->isHlsTask(id); }
    bool isTorrentTask(int id) const { return m_taskController && m_taskController->isTorrentTask(id); }
    bool isStreamTask(int id) const { return isVideoTask(id) || isHlsTask(id) || isTorrentTask(id); }

    void applyZoom();            // 应用视图缩放（字体 + 工具栏图标）
    void applyNetworkProxy();    // 应用 Qt 应用级代理（站点抓取器用）

    DownloadManager*       m_manager;
    TaskListModel*         m_taskModel;
    QSortFilterProxyModel* m_proxy;
    TaskGroupProxy*        m_groupProxy = nullptr;  // 分组折叠代理（过滤代理与视图之间）

    QTreeView*  m_categoryTree;
    SidebarPanel* m_sidebar = nullptr;   // 左侧导航面板（状态行 + 分类树）
    QTableView* m_taskList;
    QStatusBar* m_statusBar;
    QToolBar*   m_toolBar;
    QComboBox*  m_trafficCombo = nullptr;   // 状态栏流量档位下拉（自动/轻量/中等/重量/自定义）
    QActionGroup* m_groupActions = nullptr; // 「视图 → 列表分组」单选动作组
    QLabel*     m_speedPill = nullptr;      // 状态栏总速度胶囊
    QLabel*     m_taskCountLabel = nullptr; // 状态栏任务计数胶囊（文案格式被并发上限自测依赖，勿改）

    QMap<int, qint64> m_speeds;  // taskId -> 当前速度（B/s）
    QMap<int, int>    m_states;  // taskId -> 状态枚举
    // 状态栏聚合值的增量缓存：避免 4Hz 刷新与每次进度回调都做 O(n) 全量遍历
    qint64 m_totalSpeed  = 0;    // 所有活动中任务速度之和（B/s，恒 >= 0）
    int    m_activeCount = 0;    // 状态 == 1（下载中）的任务数
    QString           m_statePath; // 持久化文件路径（tasks.json）
    Settings          m_settings;  // 全局设置（默认目录/线程数/限速/主题）
    QSystemTrayIcon*  m_trayIcon = nullptr;  // 系统托盘图标（由 TrayController 创建，供各处 showMessage）
    bool              m_trayMinimize = false;  // 是否最小化到托盘

    // 系统托盘（L2 组件，见 src/app/tray_controller.*）
    TrayController*      m_trayController = nullptr;

    // 定时下载（L2 组件，见 src/app/schedule_service.*）
    ScheduleService*     m_scheduleService = nullptr;

    // 队列调度器（L2 组件，见 src/app/queue_scheduler.*）——拥有「被并发上限暂缓」的任务集合
    QueueScheduler*      m_queueScheduler = nullptr;

    // 任务控制（L2 组件，见 src/app/task_controller.*）——媒体后端实例表 + 任务 CRUD/分派
    TaskController*      m_taskController = nullptr;
    // 完成通知聚合（L2 组件，见 src/app/notification_aggregator.*）——750ms 窗口合并气泡 + 提示音
    NotificationAggregator* m_notificationAggregator = nullptr;

    // 流量档位应用（L2 组件，见 src/app/traffic_mode_controller.*）——写设置+下发引擎/aria2/托盘+同步两处 UI
    TrafficModeController* m_trafficController = nullptr;

    // IPC 单实例通信（L2 组件，见 src/app/ipc_server.*）
    IpcServer*            m_ipcServer = nullptr;

    // Web 远程管理界面（内嵌 HTTP 服务器）
    WebServerController* m_webServerController = nullptr;

    // 下载队列管理
    QueueManager*        m_queueMgr = nullptr;
    QStandardItemModel*  m_categoryModel = nullptr;  // 左侧树模型（含分类+队列）
    QStandardItem*       m_queueParentItem = nullptr; // “下载队列”父节点
    QTimer*              m_statusTimer = nullptr;      // 状态栏低频刷新节拍（4Hz）

    qint64               m_speedEma = 0;               // 状态栏总速度 EMA 平滑值
    int                  m_sidebarTick = 0;            // 侧栏降频计数：每 4 次状态栏刷新（≈1s）重算一次分类计数

    // 空状态插画
    EmptyStateOverlay*   m_emptyOverlay = nullptr;

    // 下载完成自动关机/休眠（L2 组件，状态与倒计时对话框由其内部持有）
    AutoPowerController* m_autoPower = nullptr;

    // 视频/HLS/BT 后端实例表、编号偏移与延迟启动缓存已移入 TaskController
    // （见 src/app/task_controller.*），MainWindow 不再持有媒体后端状态。

    // 视图缩放
    int m_zoomLevel = 0;            // 字体缩放级数（每级 ±1pt）
    static constexpr int BASE_FONT_PT = 13;
    static constexpr int BASE_ICON_SZ = 22;
};

#endif // MAIN_WINDOW_H
