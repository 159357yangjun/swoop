#ifndef TRAFFIC_MODE_CONTROLLER_H
#define TRAFFIC_MODE_CONTROLLER_H

#include <QObject>
#include <QString>
#include <functional>

class Settings;
class TaskListModel;
class QComboBox;

// L2 UI 控制器：拥有「应用流量档位」行为（写设置 + 下发引擎/aria2/托盘 + 同步两处 UI）。
// 引擎/aria2/托盘动作经 sink 回调注入，避免直接依赖 DownloadManager/TorrentDownloader/TrayController；
// 显示用的下拉框与任务模型经 setWidgets 注入。
class TrafficModeController : public QObject {
    Q_OBJECT
public:
    explicit TrafficModeController(QObject* parent = nullptr);

    void setSettings(Settings* s) { m_settings = s; }
    void setWidgets(QComboBox* combo, TaskListModel* model);

    // 引擎/aria2/托盘动作经 sink 注入（原 MainWindow 直接调用 m_manager / TorrentDownloader / m_trayController）
    void setMaxSpeedSink(std::function<void(qint64)> f) { m_maxSpeedSink = f; }
    void setTorrentLimitSink(std::function<void(int)> f) { m_torrentSink  = f; }
    void setTraySyncSink(std::function<void(int)> f)    { m_traySink     = f; }

    // 把下拉框交给控制器：初始同步选中项 + 连接 currentIndexChanged → onComboChanged
    void attachCombo();

public slots:
    void onComboChanged(int index);   // 下拉变更 → 应用档位（原 onTrafficModeChanged）
    void applyMode(int mode);         // 统一应用流量档位（原 applyTrafficMode）
    void syncCombo();                 // 下拉选中项同步到当前 trafficMode（原 syncTrafficCombo）
    void updateIndicator();           // 全局档位/限速文本同步到任务列表「限速」列（原 updateTrafficIndicator）
    void syncTray();                  // 仅同步托盘流量子菜单勾选（设置里手动改限速时调用）

private:
    Settings*      m_settings = nullptr;
    QComboBox*     m_combo    = nullptr;
    TaskListModel* m_model    = nullptr;
    std::function<void(qint64)> m_maxSpeedSink;
    std::function<void(int)>    m_torrentSink;
    std::function<void(int)>    m_traySink;
};

#endif // TRAFFIC_MODE_CONTROLLER_H
