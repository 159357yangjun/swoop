#include "queue_manager_dialog.h"
#include "queue_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QTimeEdit>
#include <QPushButton>

QueueManagerDialog::QueueManagerDialog(QueueManager* mgr, QWidget* parent)
    : QDialog(parent), m_mgr(mgr)
{
    setWindowTitle(QStringLiteral("下载队列管理"));
    resize(520, 460);
    setMinimumSize(460, 420);

    // ── 左侧：队列列表 + 名称输入 + 增删改名 ──
    m_list = new QListWidget(this);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("输入队列名称后点「新建队列」"));
    m_addBtn = new QPushButton(QStringLiteral("新建队列"), this);
    m_renameBtn = new QPushButton(QStringLiteral("重命名"), this);
    m_renameBtn->setEnabled(false);
    m_removeBtn = new QPushButton(QStringLiteral("删除"), this);
    m_removeBtn->setEnabled(false);
    m_toggleBtn = new QPushButton(QStringLiteral("停止队列"), this);
    m_toggleBtn->setEnabled(false);

    QVBoxLayout* left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("队列列表"), this));
    left->addWidget(m_list, 1);
    QHBoxLayout* nameRow = new QHBoxLayout;
    nameRow->addWidget(m_nameEdit);
    nameRow->addWidget(m_addBtn);
    left->addLayout(nameRow);
    QHBoxLayout* actRow = new QHBoxLayout;
    actRow->addWidget(m_renameBtn);
    actRow->addWidget(m_removeBtn);
    left->addWidget(m_toggleBtn);
    left->addLayout(actRow);

    // ── 右侧：选中队列的属性（IDM「队列属性」风格） ──
    m_concSpin = new QSpinBox(this);
    m_concSpin->setRange(1, 50);
    m_concSpin->setValue(2);
    m_orderedCheck = new QCheckBox(QStringLiteral("按添加顺序下载文件"), this);
    m_orderedCheck->setChecked(true);
    m_schedCheck = new QCheckBox(QStringLiteral("启用计划时间"), this);
    m_startTime = new QTimeEdit(QTime(0, 0), this);
    m_stopTime = new QTimeEdit(QTime(23, 59), this);

    auto* dlGroup = new QGroupBox(QStringLiteral("下载设置"), this);
    QFormLayout* dlForm = new QFormLayout(dlGroup);
    dlForm->addRow(QStringLiteral("同时下载文件数:"), m_concSpin);
    dlForm->addRow(m_orderedCheck);
    dlGroup->setLayout(dlForm);

    auto* schedGroup = new QGroupBox(QStringLiteral("计划下载"), this);
    QFormLayout* schedForm = new QFormLayout(schedGroup);
    schedForm->addRow(m_schedCheck);
    schedForm->addRow(QStringLiteral("开始下载:"), m_startTime);
    schedForm->addRow(QStringLiteral("停止下载:"), m_stopTime);
    schedGroup->setLayout(schedForm);

    QVBoxLayout* right = new QVBoxLayout;
    right->addWidget(dlGroup);
    right->addWidget(schedGroup);
    right->addStretch(1);

    QHBoxLayout* mainRow = new QHBoxLayout;
    mainRow->addLayout(left, 2);
    mainRow->addLayout(right, 3);

    QDialogButtonBox* box = new QDialogButtonBox(
        QDialogButtonBox::Close | QDialogButtonBox::Help, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::helpRequested, this, [this]() {
        QMessageBox::information(this, QStringLiteral("下载队列"),
            QStringLiteral("队列用于把多个下载任务分组并按规则调度。\n\n"
                           "· 同时下载文件数：该队列最多并行下载的任务数。\n"
                           "· 按添加顺序下载：开启后严格按加入先后依次下载。\n"
                           "· 启用计划时间：仅在设定的开始/停止时间段内启动下载。\n"
                           "· 停止队列：暂停该队列的调度（已在进行中的任务不受影响）。"));
    });

    QVBoxLayout* root = new QVBoxLayout(this);
    root->addLayout(mainRow);
    root->addWidget(box);

    // 信号
    connect(m_addBtn, &QPushButton::clicked, this, &QueueManagerDialog::onAdd);
    connect(m_removeBtn, &QPushButton::clicked, this, &QueueManagerDialog::onRemove);
    connect(m_renameBtn, &QPushButton::clicked, this, &QueueManagerDialog::onRename);
    connect(m_toggleBtn, &QPushButton::clicked, this, &QueueManagerDialog::onToggleQueue);
    connect(m_list, &QListWidget::currentItemChanged,
            this, [this]() { onSelectionChanged(); });
    connect(m_concSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &QueueManagerDialog::onParamsChanged);
    connect(m_orderedCheck, &QCheckBox::toggled,
            this, &QueueManagerDialog::onParamsChanged);
    connect(m_schedCheck, &QCheckBox::toggled,
            this, &QueueManagerDialog::onScheduleToggled);
    connect(m_startTime, &QTimeEdit::timeChanged,
            this, &QueueManagerDialog::onParamsChanged);
    connect(m_stopTime, &QTimeEdit::timeChanged,
            this, &QueueManagerDialog::onParamsChanged);

    refreshList();
    setPropsEnabled(false);
}

void QueueManagerDialog::setPropsEnabled(bool on)
{
    m_concSpin->setEnabled(on);
    m_orderedCheck->setEnabled(on);
    m_schedCheck->setEnabled(on);
    m_startTime->setEnabled(on && m_schedCheck->isChecked());
    m_stopTime->setEnabled(on && m_schedCheck->isChecked());
    m_toggleBtn->setEnabled(on);
}

void QueueManagerDialog::refreshList()
{
    m_list->clear();
    for (const auto& q : m_mgr->queues()) {
        auto* it = new QListWidgetItem(q.name, m_list);
        it->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
    }
}

void QueueManagerDialog::refreshParams()
{
    auto* item = m_list->currentItem();
    if (!item) {
        m_renameBtn->setEnabled(false);
        m_removeBtn->setEnabled(false);
        setPropsEnabled(false);
        return;
    }
    DownloadQueue q = m_mgr->queue(item->text());
    QSignalBlocker b1(m_concSpin), b2(m_orderedCheck), b3(m_schedCheck),
                   b4(m_startTime), b5(m_stopTime);
    m_concSpin->setValue(q.maxConcurrent);
    m_orderedCheck->setChecked(q.ordered);
    m_schedCheck->setChecked(q.useSchedule);
    m_startTime->setTime(q.startAt);
    m_stopTime->setTime(q.stopAt);

    m_renameBtn->setEnabled(true);
    m_removeBtn->setEnabled(true);
    setPropsEnabled(true);
    m_toggleBtn->setText(q.enabled ? QStringLiteral("停止队列")
                                   : QStringLiteral("开始队列"));
}

void QueueManagerDialog::onSelectionChanged()
{
    refreshParams();
}

void QueueManagerDialog::onAdd()
{
    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请输入队列名称"));
        return;
    }
    if (!m_mgr->addQueue(name)) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("队列已存在或名称无效"));
        return;
    }
    m_nameEdit->clear();
    refreshList();
    QList<QListWidgetItem*> items = m_list->findItems(name, Qt::MatchExactly);
    if (!items.isEmpty())
        m_list->setCurrentItem(items.first());
}

void QueueManagerDialog::onRemove()
{
    auto* item = m_list->currentItem();
    if (!item)
        return;
    if (QMessageBox::question(this, QStringLiteral("确认"),
                              QStringLiteral("删除队列「%1」？\n已加入该队列的任务将变为普通任务。")
                                  .arg(item->text()),
                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        m_mgr->removeQueue(item->text());
        refreshList();
        if (m_list->count() > 0)
            m_list->setCurrentRow(0);
        else
            onSelectionChanged();
    }
}

void QueueManagerDialog::onRename()
{
    auto* item = m_list->currentItem();
    if (!item)
        return;
    QString oldName = item->text();
    QString newName = m_nameEdit->text().trimmed();
    if (newName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请输入新名称"));
        return;
    }
    if (!m_mgr->renameQueue(oldName, newName)) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("重命名失败（可能重名）"));
        return;
    }
    m_nameEdit->clear();
    refreshList();
    QList<QListWidgetItem*> items = m_list->findItems(newName, Qt::MatchExactly);
    if (!items.isEmpty())
        m_list->setCurrentItem(items.first());
}

void QueueManagerDialog::onToggleQueue()
{
    auto* item = m_list->currentItem();
    if (!item)
        return;
    DownloadQueue q = m_mgr->queue(item->text());
    q.enabled = !q.enabled;
    m_mgr->setQueueParams(item->text(), q.maxConcurrent, q.ordered, q.enabled,
                          q.useSchedule, q.startAt, q.stopAt);
    m_toggleBtn->setText(q.enabled ? QStringLiteral("停止队列")
                                   : QStringLiteral("开始队列"));
}

void QueueManagerDialog::onScheduleToggled(bool on)
{
    // 计划开关只控制时间控件可用，真实计划窗口在 onParamsChanged 里保存
    m_startTime->setEnabled(on);
    m_stopTime->setEnabled(on);
    onParamsChanged();
}

void QueueManagerDialog::onParamsChanged()
{
    auto* item = m_list->currentItem();
    if (!item)
        return;
    DownloadQueue q = m_mgr->queue(item->text());
    m_mgr->setQueueParams(item->text(), m_concSpin->value(),
                          m_orderedCheck->isChecked(), q.enabled,
                          m_schedCheck->isChecked(),
                          m_startTime->time(), m_stopTime->time());
}
