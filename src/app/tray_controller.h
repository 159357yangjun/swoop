#ifndef IDM_APP_TRAY_CONTROLLER_H
#define IDM_APP_TRAY_CONTROLLER_H

#include <QObject>
#include <functional>

class QSystemTrayIcon;
class QMenu;
class QWidget;

// ── L2 应用编排组件：系统托盘（图标 + 右键菜单 + 双击恢复） ─────────────
// 从 MainWindow 抽取。拥有 QSystemTrayIcon 与托盘右键菜单（含流量档位子菜单），
// 本身不碰业务：菜单动作经回调交回上层执行，双击恢复亦然。
// 通知气泡（showMessage）是各处的展示动作，经 trayIcon() 拿到图标后自行调用，
// 不在本组件再包一层 —— 保持「托盘=壳，业务=上层」的边界。
class TrayController : public QObject {
    Q_OBJECT
public:
    // parent 同时作为右键菜单的 Qt 父窗口（QMenu 需要 QWidget 父级），
    // 与原 MainWindow::setupTrayIcon 里 new QMenu(this) 的归属一致。
    explicit TrayController(QWidget* parent = nullptr);

    // 回调注入（MainWindow 构造时接线）
    void setShowWindowCallback(std::function<void()> cb);     // 菜单「显示主窗口」（原为仅 showNormal）
    void setRestoreWindowCallback(std::function<void()> cb);  // 双击恢复（showNormal+raise+activateWindow）
    void setNewTaskCallback(std::function<void()> cb);        // 菜单「新建任务...」
    void setStartAllCallback(std::function<void()> cb);       // 菜单「全部开始」
    void setPauseAllCallback(std::function<void()> cb);       // 菜单「全部暂停」
    void setTrafficModeCallback(std::function<void(int)> cb); // 流量档位子菜单

    // 建图标 + 菜单并显示。须在回调注入完成后调用。
    void setup();

    QSystemTrayIcon* trayIcon() const { return m_trayIcon; }
    bool isVisible() const;

    // 勾选同步：mode 与动作 data 相等者打勾；-1（自定义）→ 全不勾
    void syncTraffic(int mode);

private:
    std::function<void()>    m_showWindow;
    std::function<void()>    m_restoreWindow;
    std::function<void()>    m_newTask;
    std::function<void()>    m_startAll;
    std::function<void()>    m_pauseAll;
    std::function<void(int)> m_trafficMode;

    QSystemTrayIcon* m_trayIcon    = nullptr;
    QMenu*           m_trafficMenu = nullptr;   // 流量档位子菜单（自动/轻量/中等/重量）
    QWidget*         m_menuParent  = nullptr;   // 右键菜单的 Qt 父窗口（= 构造时传入的 parent）
};

#endif // IDM_APP_TRAY_CONTROLLER_H
