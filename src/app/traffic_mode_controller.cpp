#include "traffic_mode_controller.h"

#include "settings.h"
#include "task_list_model.h"
#include "logger.h"

#include <QComboBox>

namespace {

// 流量档位预设 → 限速值（KB/s）；对标 FDM 工具栏流量使用模式
int trafficKbpsForMode(int mode)
{
    switch (mode) {
        case 1:  return 512;    // 轻量：严格保留浏览带宽
        case 2:  return 2048;   // 中等：平衡
        case 3:  return 4096;   // 重量：接近全速，仍留余量
        default: return 0;      // 自动/未知 → 不限速
    }
}

QString trafficModeName(int mode)
{
    switch (mode) {
        case 1:  return QStringLiteral("轻量");
        case 2:  return QStringLiteral("中等");
        case 3:  return QStringLiteral("重量");
        case -1: return QStringLiteral("自定义");
        default: return QStringLiteral("自动");
    }
}

} // namespace

TrafficModeController::TrafficModeController(QObject* parent) : QObject(parent) {}

void TrafficModeController::setWidgets(QComboBox* combo, TaskListModel* model)
{
    m_combo = combo;
    m_model = model;
}

void TrafficModeController::attachCombo()
{
    if (!m_combo) return;
    syncCombo();
    connect(m_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TrafficModeController::onComboChanged);
}

void TrafficModeController::onComboChanged(int index)
{
    if (!m_combo || index < 0) return;
    int mode = m_combo->itemData(index).toInt();
    // “自定义”项仅作显示态，不允许被选中（手动限速请在设置里改）
    if (mode == -1) {
        syncCombo();   // 复位回当前实际档位
        return;
    }
    applyMode(mode);
}

void TrafficModeController::applyMode(int mode)
{
    if (!m_settings) return;
    int kbps = trafficKbpsForMode(mode);
    m_settings->setTrafficMode(mode);
    m_settings->setSpeedLimitKBps(kbps);
    if (m_maxSpeedSink) m_maxSpeedSink(kbps * 1024);
    if (m_torrentSink)  m_torrentSink(kbps);
    m_settings->save();
    Log::info(QStringLiteral("流量档位切换为「%1」(%2 KB/s)")
                  .arg(trafficModeName(mode)).arg(kbps));
    syncCombo();
    if (m_traySink) m_traySink(m_settings->trafficMode());  // 同步托盘流量子菜单勾选
    updateIndicator();   // 同步任务列表「限速」列
}

void TrafficModeController::syncCombo()
{
    if (!m_combo || !m_settings) return;
    int idx = m_combo->findData(m_settings->trafficMode());
    if (idx < 0) idx = m_combo->findData(-1);  // 落到“自定义”
    m_combo->setCurrentIndex(idx);
}

void TrafficModeController::updateIndicator()
{
    if (!m_model || !m_settings) return;

    // 这个函数也承担“从持久化设置恢复运行态”的职责。MainWindow 在 m_settings.load()
    // 之后调用它，因此必须把保存的实际 KB/s 同步给两个下载后端，而不能只更新文字。
    // 尤其 trafficMode == -1（自定义）时，旧代码不会触发 applyMode()，导致 HTTP 引擎
    // 已恢复限速但 aria2/BT 后端仍保持不限速。
    const int kbps = qMax(0, m_settings->speedLimitKBps());
    if (m_maxSpeedSink) m_maxSpeedSink(kbps * 1024);
    if (m_torrentSink)  m_torrentSink(kbps);

    int mode = m_settings->trafficMode();
    QString text;
    if (mode == 0)
        text = QStringLiteral("自动");          // 自动 = 不限速
    else if (mode == -1)
        text = QStringLiteral("自定义 %1").arg(kbps);
    else
        text = QStringLiteral("%1 %2").arg(trafficModeName(mode)).arg(kbps);
    m_model->setGlobalTraffic(text);

    // 状态栏下拉也要跟着走：它现在承担原来「限速胶囊」的职责——让人一眼看出
    // 当前是不是在限速、限到哪一档。档位名与列的文本同源，不会说两套话。
    syncCombo();
}

void TrafficModeController::syncTray()
{
    if (!m_settings || !m_traySink) return;
    m_traySink(m_settings->trafficMode());
}