#include "tray_controller.h"

#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

TrayController::TrayController(QWidget* parent)
    : QObject(parent)
    , m_menuParent(parent)
{
}

void TrayController::setShowWindowCallback(std::function<void()> cb)    { m_showWindow = std::move(cb); }
void TrayController::setRestoreWindowCallback(std::function<void()> cb) { m_restoreWindow = std::move(cb); }
void TrayController::setNewTaskCallback(std::function<void()> cb)       { m_newTask = std::move(cb); }
void TrayController::setStartAllCallback(std::function<void()> cb)      { m_startAll = std::move(cb); }
void TrayController::setPauseAllCallback(std::function<void()> cb)      { m_pauseAll = std::move(cb); }
void TrayController::setTrafficModeCallback(std::function<void(int)> cb){ m_trafficMode = std::move(cb); }

void TrayController::setup()
{
    m_trayIcon = new QSystemTrayIcon(this);
    // 使用系统默认图标（后续可替换为自定义图标）
    m_trayIcon->setIcon(QIcon::fromTheme(QStringLiteral("download"),
                          QApplication::windowIcon()));
    m_trayIcon->setToolTip(QStringLiteral("IDM Next - 下载平台"));

    // 右键菜单（父窗口 = 传入的 QWidget，与原 MainWindow::setupTrayIcon 一致）
    auto* trayMenu = new QMenu(m_menuParent);
    if (m_showWindow)
        trayMenu->addAction(QStringLiteral("显示主窗口"), this, m_showWindow);
    if (m_newTask)
        trayMenu->addAction(QStringLiteral("新建任务..."), this, m_newTask);
    trayMenu->addSeparator();
    if (m_startAll)
        trayMenu->addAction(QStringLiteral("全部开始"), this, m_startAll);
    if (m_pauseAll)
        trayMenu->addAction(QStringLiteral("全部暂停"), this, m_pauseAll);
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
        if (m_trafficMode) {
            connect(a, &QAction::triggered, this, [this, mode = m.second] { m_trafficMode(mode); });
        }
    }
    m_trafficMenu = trafficMenu;

    trayMenu->addSeparator();
    trayMenu->addAction(QStringLiteral("退出"), qApp, &QApplication::quit);
    m_trayIcon->setContextMenu(trayMenu);

    // 双击托盘图标恢复窗口
    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick && m_restoreWindow)
            m_restoreWindow();
    });

    m_trayIcon->show();
}

bool TrayController::isVisible() const
{
    return m_trayIcon && m_trayIcon->isVisible();
}

void TrayController::syncTraffic(int mode)
{
    if (!m_trafficMenu)
        return;
    for (QAction* a : m_trafficMenu->actions())
        a->setChecked(a->data().toInt() == mode);
}
