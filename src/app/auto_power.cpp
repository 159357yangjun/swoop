#include "auto_power.h"
#include "settings.h"
#include "logger.h"

#include <QDialog>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLibrary>
#include <QProcess>
#include <QCloseEvent>
#include <algorithm>

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

// ── 可取消倒计时对话框（非模态）──────────────────────────────
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

// ── AutoPowerController ──────────────────────────────────
AutoPowerController::AutoPowerController(Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
}

void AutoPowerController::setHasActiveOrPendingCallback(std::function<bool()> cb)
{
    m_hasActiveOrPending = std::move(cb);
}

void AutoPowerController::setHasCompletedOrFailedCallback(std::function<bool()> cb)
{
    m_hasCompletedOrFailed = std::move(cb);
}

void AutoPowerController::setSaveStateCallback(std::function<void(const QString&)> cb)
{
    m_saveState = std::move(cb);
}

void AutoPowerController::setStatePathProvider(std::function<QString()> cb)
{
    m_statePathProvider = std::move(cb);
}

void AutoPowerController::maybeTrigger()
{
    if (m_pending)
        return;  // 已经在倒计时，避免重复弹窗
    if (!m_settings)
        return;
    if (m_settings->shutdownAction() == QStringLiteral("none"))
        return;  // 未开启此功能
    if (m_hasActiveOrPending && m_hasActiveOrPending())
        return;  // 仍有下载中/排队/未到期定时任务
    if (m_hasCompletedOrFailed && !m_hasCompletedOrFailed())
        return;  // 没有任何已完成的下载，避免启动即触发

    m_pending = true;
    QString action = m_settings->shutdownAction();
    QString actionText = (action == QStringLiteral("shutdown"))
                             ? QStringLiteral("关机") : QStringLiteral("休眠");
    int grace = qBound(10, m_settings->shutdownGraceSec(), 600);

    auto* dlg = new AutoPowerDialog(
        actionText, grace,
        [this]() {                       // 倒计时结束 → 执行关机/休眠
            m_pending = false;
            m_dlg = nullptr;
            perform();
        },
        [this]() {                       // 用户取消
            m_pending = false;
            m_dlg = nullptr;
            Log::info(QStringLiteral("已取消下载完成后的关机/休眠"));
        },
        qobject_cast<QWidget*>(parent()));
    m_dlg = dlg;
    dlg->show();
}

void AutoPowerController::cancelPending()
{
    if (m_dlg) {
        m_dlg->close();
        m_dlg->deleteLater();
        m_dlg = nullptr;
    }
    m_pending = false;
}

void AutoPowerController::perform()
{
    // 关机/休眠前先保存任务状态，避免下次启动丢失进度
    QString p = m_statePathProvider ? m_statePathProvider() : QString();
    if (!p.isEmpty() && m_saveState)
        m_saveState(p);

#ifdef Q_OS_WIN
    QString action = m_settings->shutdownAction();
    if (action == QStringLiteral("shutdown"))
        windowsShutdown();
    else if (action == QStringLiteral("sleep"))
        windowsHibernate();
#else
    Log::info(QStringLiteral("（非 Windows 平台）跳过电源动作"));
#endif
}
