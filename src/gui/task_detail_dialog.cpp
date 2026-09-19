#include "task_detail_dialog.h"
#include "speed_chart_widget.h"
#include "hash_verify_dialog.h"
#include "download_manager.h"
#include "download_core.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QTimer>
#include <QHeaderView>
#include <QTabWidget>
#include <QPushButton>
#include <QFileDialog>

// 辅助：字节数 → 人类可读字符串
static QString formatBytes(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("未知");
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1048576) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1073741824) return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 2);
    return QStringLiteral("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
}

// 辅助：速度 → 人类可读字符串
static QString formatSpeed(int bps)
{
    if (bps <= 0) return QStringLiteral("0 B/s");
    if (bps < 1024) return QStringLiteral("%1 B/s").arg(bps);
    if (bps < 1048576) return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB/s").arg(bps / 1048576.0, 0, 'f', 2);
}

// 辅助：秒数 → 时间字符串
static QString formatEta(int sec)
{
    if (sec <= 0) return QStringLiteral("--");
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3")
                      .arg(h, 2, 10, QLatin1Char('0'))
                      .arg(m, 2, 10, QLatin1Char('0'))
                      .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2")
              .arg(m, 2, 10, QLatin1Char('0'))
              .arg(s, 2, 10, QLatin1Char('0'));
}

TaskDetailDialog::TaskDetailDialog(int taskId, DownloadManager* mgr, QWidget* parent)
    : QDialog(parent)
    , m_taskId(taskId)
    , m_mgr(mgr)
    , m_timer(new QTimer(this))
{
    setupUi();
    setWindowTitle(QStringLiteral("任务详情 #%1").arg(taskId));
    setMinimumSize(480, 420);

    // 500ms 轮询引擎刷新数据
    connect(m_timer, &QTimer::timeout, this, &TaskDetailDialog::refreshData);
    m_timer->start(500);
    refreshData();  // 立即刷新一次
}

TaskDetailDialog::~TaskDetailDialog()
{
    m_timer->stop();
}

void TaskDetailDialog::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    // 标签页容器：「任务详情」+ 「速度图表」
    m_tabWidget = new QTabWidget(this);

    // ── Tab 1：任务详情 ──
    auto* detailTab = new QWidget(this);
    auto* detailLayout = new QVBoxLayout(detailTab);
    detailLayout->setContentsMargins(0, 0, 0, 0);

    // 基本信息
    auto* infoGroup = new QGroupBox(QStringLiteral("基本信息"), detailTab);
    auto* form = new QFormLayout(infoGroup);
    form->setLabelAlignment(Qt::AlignRight);

    m_fileNameLabel = new QLabel(detailTab);
    m_urlLabel      = new QLabel(detailTab);
    m_urlLabel->setWordWrap(true);
    m_urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel   = new QLabel(detailTab);
    m_protocolLabel = new QLabel(detailTab);
    m_sizeLabel     = new QLabel(detailTab);
    m_downloadedLabel = new QLabel(detailTab);
    m_speedLabel    = new QLabel(detailTab);
    m_etaLabel      = new QLabel(detailTab);

    form->addRow(QStringLiteral("文件名："), m_fileNameLabel);
    form->addRow(QStringLiteral("URL："), m_urlLabel);
    form->addRow(QStringLiteral("状态："), m_statusLabel);
    form->addRow(QStringLiteral("协议版本："), m_protocolLabel);
    form->addRow(QStringLiteral("总大小："), m_sizeLabel);
    form->addRow(QStringLiteral("已下载："), m_downloadedLabel);
    form->addRow(QStringLiteral("速度："), m_speedLabel);
    form->addRow(QStringLiteral("剩余时间："), m_etaLabel);

    detailLayout->addWidget(infoGroup);

    // 进度条
    auto* progGroup = new QGroupBox(QStringLiteral("进度"), detailTab);
    auto* progLayout = new QVBoxLayout(progGroup);
    m_progressBar = new QProgressBar(detailTab);
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(true);
    progLayout->addWidget(m_progressBar);
    detailLayout->addWidget(progGroup);

    // 分片与错误信息
    auto* errGroup = new QGroupBox(QStringLiteral("分片与错误"), detailTab);
    auto* errForm = new QFormLayout(errGroup);
    errForm->setLabelAlignment(Qt::AlignRight);

    m_chunksLabel = new QLabel(detailTab);
    m_errorLabel  = new QLabel(detailTab);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));

    errForm->addRow(QStringLiteral("分片："), m_chunksLabel);
    errForm->addRow(QStringLiteral("错误："), m_errorLabel);

    // 文件校验按钮（下载完成后可用）
    // 靠右并给自然宽度：直接 addRow 会把按钮拉成整行宽的实心蓝条，像横幅而不是按钮。
    m_verifyBtn = new QPushButton(QStringLiteral("校验文件完整性..."), detailTab);
    m_verifyBtn->setMinimumWidth(150);
    auto* verifyRow = new QHBoxLayout();
    verifyRow->setContentsMargins(0, 0, 0, 0);
    verifyRow->addStretch(1);
    verifyRow->addWidget(m_verifyBtn);
    errForm->addRow(QString(), verifyRow);
    connect(m_verifyBtn, &QPushButton::clicked, this, &TaskDetailDialog::onVerifyFile);

    detailLayout->addWidget(errGroup);
    detailLayout->addStretch();

    m_tabWidget->addTab(detailTab, QStringLiteral("任务详情"));

    // ── Tab 2：速度图表 ──
    m_speedChart = new SpeedChartWidget(this);
    m_tabWidget->addTab(m_speedChart, QStringLiteral("速度图表"));

    mainLayout->addWidget(m_tabWidget);
}

void TaskDetailDialog::refreshData()
{
    TaskInfo info;
    if (dlmgr_get_task_info(m_taskId, &info) != 0) {
        m_statusLabel->setText(QStringLiteral("无法获取任务信息（可能已删除）"));
        m_timer->stop();
        return;
    }

    m_fileNameLabel->setText(QString::fromUtf8(info.filename));

    // 获取完整 URL（TaskInfo 不含 URL，从 TaskSummary 获取）
    TaskSummary summaries[128];
    int n = dlmgr_get_task_summary(summaries, 128);
    for (int i = 0; i < n; ++i) {
        if (summaries[i].task_id == m_taskId) {
            m_urlLabel->setText(QString::fromUtf8(summaries[i].url));
            break;
        }
    }

    // 状态文本
    static const char* stateNames[] = {
        "等待中", "下载中", "已暂停", "已完成", "失败", "已取消"
    };
    int s = info.status;
    if (s >= 0 && s <= 5)
        m_statusLabel->setText(QString::fromUtf8(stateNames[s]));
    else
        m_statusLabel->setText(QStringLiteral("未知(%1)").arg(s));

    // 协议版本：HTTP/2 用强调色高亮（协商提速成功），未知显示占位
    if (info.http_version[0]) {
        m_protocolLabel->setText(QString::fromUtf8(info.http_version));
        if (strcmp(info.http_version, "HTTP/2") == 0)
            m_protocolLabel->setStyleSheet(QStringLiteral("color: #1a8f4b; font-weight: bold;"));
        else
            m_protocolLabel->setStyleSheet(QStringLiteral("color: #555;"));
    } else {
        m_protocolLabel->setText(QStringLiteral("未知"));
        m_protocolLabel->setStyleSheet(QStringLiteral("color: #999;"));
    }

    m_sizeLabel->setText(formatBytes(info.file_size));
    m_downloadedLabel->setText(formatBytes(info.downloaded));

    // 进度百分比
    if (info.file_size > 0) {
        int pct = static_cast<int>(info.downloaded * 100 / info.file_size);
        m_progressBar->setValue(pct);
    } else {
        m_progressBar->setValue(0);
    }

    m_speedLabel->setText(formatSpeed(static_cast<int>(info.speed_bps)));
    m_etaLabel->setText(formatEta(info.eta_sec));

    // 速度图表数据喂入：仅在下载中时添加数据点
    if (info.status == 1 && m_speedChart)
        m_speedChart->addDataPoint(static_cast<qreal>(info.speed_bps));

    // 分片信息：直接读引擎的真实分片数组（TaskInfo 现已暴露 chunk_count / chunks_done）。
    // 原先这里写死「详见引擎日志（阶段5扩展）」——对用户是零信息量的内部术语，看起来像没做完。
    if (info.chunk_count > 0) {
        m_chunksLabel->setText(QStringLiteral("%1 个分片，已完成 %2 个")
                                   .arg(info.chunk_count)
                                   .arg(info.chunks_done));
    } else if (info.file_size < 0) {
        m_chunksLabel->setText(QStringLiteral("—（大小未知，未分段）"));
    } else {
        m_chunksLabel->setText(QStringLiteral("—（尚未开始分段）"));
    }

    // 错误信息：优先显示引擎记录的具体原因（HTTP 状态码 / 解析失败 / 重命名失败…），
    // 只在引擎确实没记录时才退回笼统提示，避免用户「只知道失败、不知道为何失败」。
    if (s == 4) {
        const QString em = QString::fromUtf8(info.error_msg).trimmed();
        m_errorLabel->setText(em.isEmpty()
                                  ? QStringLiteral("下载失败（引擎未记录具体原因）")
                                  : em);
    } else {
        m_errorLabel->setText(QStringLiteral("无"));
    }
}

void TaskDetailDialog::onVerifyFile()
{
    // 获取文件名与保存目录，构造完整路径（TaskSummary 现已含 save_dir）
    TaskInfo info;
    QString filename;
    if (dlmgr_get_task_info(m_taskId, &info) == 0)
        filename = QString::fromUtf8(info.filename);

    QString fullPath;
    TaskSummary summaries[128];
    int n = dlmgr_get_task_summary(summaries, 128);
    for (int i = 0; i < n; ++i) {
        if (summaries[i].task_id == m_taskId) {
            QString dir = QString::fromUtf8(summaries[i].save_dir);
            QString fn  = QString::fromUtf8(summaries[i].filename);
            fullPath = dir.isEmpty() ? fn : (dir + QDir::separator() + fn);
            break;
        }
    }

    // 无法构造完整路径时退化为仅文件名，让用户在对话框中浏览
    HashVerifyDialog dlg(this, fullPath.isEmpty() ? filename : fullPath);
    dlg.exec();
}
