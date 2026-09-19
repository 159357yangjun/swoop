#ifndef IDM_APP_AUTO_POWER_H
#define IDM_APP_AUTO_POWER_H

#include <QObject>
#include <QString>
#include <functional>

class Settings;
class QDialog;

// ── L2 应用编排组件：下载完成后的自动关机/休眠 ───────────────────────
// 从 MainWindow 抽取，解耦其对 m_states / m_scheduledTasks / m_statePath /
// m_manager 的直接依赖，改为通过注入的回调查询任务状态与触发状态保存。
//
// 行为契约（与原 MainWindow 实现一致）：
//  - maybeTrigger()：仅当 ①功能已开启（shutdownAction != "none"）
//    ②无任何活动/排队/未到期定时任务 ③至少存在一个已完成或失败任务 时，
//    才弹出可取消倒计时对话框；已处于倒计时中则直接返回，避免重复弹窗。
//  - cancelPending()：关闭并销毁倒计时对话框，用于「新下载开始」等场景中止待定动作。
//  - 倒计时归零 → perform()：先保存任务状态，再按平台执行关机/休眠。
class AutoPowerController : public QObject {
    Q_OBJECT
public:
    explicit AutoPowerController(Settings* settings, QObject* parent = nullptr);

    // 任务状态查询（由 MainWindow 注入，封装 m_states / m_scheduledTasks 的判定）
    void setHasActiveOrPendingCallback(std::function<bool()> cb);
    void setHasCompletedOrFailedCallback(std::function<bool()> cb);

    // 保存任务状态（由 MainWindow 注入：m_manager->saveState(statePath)）
    void setSaveStateCallback(std::function<void(const QString&)> cb);

    // 当前持久化文件路径提供器（每次触发时取值，保证拿到最新路径）
    void setStatePathProvider(std::function<QString()> cb);

    // 检测是否满足触发条件，满足则弹出可取消倒计时对话框
    void maybeTrigger();

    // 中止待定的关机/休眠（新下载开始/状态变更时调用）
    void cancelPending();

    // 是否正处于倒计时待定中（供 UI 状态判断）
    bool isPending() const { return m_pending; }

private:
    void perform();

    Settings* m_settings = nullptr;
    std::function<bool()>            m_hasActiveOrPending;
    std::function<bool()>            m_hasCompletedOrFailed;
    std::function<void(const QString&)> m_saveState;
    std::function<QString()>         m_statePathProvider;

    QDialog* m_dlg   = nullptr;   // 倒计时对话框（非模态）
    bool     m_pending = false;   // 是否已弹出倒计时待定
};

#endif // IDM_APP_AUTO_POWER_H
