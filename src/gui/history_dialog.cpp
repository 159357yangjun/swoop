#include "history_dialog.h"
#include "history_store.h"
#include "logger.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonObject>
#include <QTableWidgetItem>

HistoryDialog::HistoryDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("下载历史"));
    setMinimumWidth(880);
    setupUi();
    refresh();  // 打开即加载
}

void HistoryDialog::setupUi()
{
    auto* root = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("文件名"),
        QStringLiteral("下载链接"),
        QStringLiteral("大小"),
        QStringLiteral("状态"),
        QStringLiteral("加入时间"),
        QStringLiteral("完成时间"),
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setShowGrid(false);
    auto* hdr = m_table->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Interactive);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    connect(m_table, &QTableWidget::cellDoubleClicked,
            this, &HistoryDialog::onRowDoubleClicked);
    root->addWidget(m_table, 1);

    // 底部：操作按钮 + 计数提示
    auto* bottom = new QHBoxLayout;
    m_refreshBtn = new QPushButton(QStringLiteral("刷新"), this);
    m_clearBtn   = new QPushButton(QStringLiteral("清空历史"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    connect(m_refreshBtn, &QPushButton::clicked, this, &HistoryDialog::refresh);
    connect(m_clearBtn,   &QPushButton::clicked, this, &HistoryDialog::onClear);
    connect(closeBtn,     &QPushButton::clicked, this, &QDialog::accept);

    m_countLabel = new QLabel(QStringLiteral("双击任意行可复制下载链接"), this);
    m_countLabel->setStyleSheet(QStringLiteral("color: #888;"));

    bottom->addWidget(m_refreshBtn);
    bottom->addWidget(m_clearBtn);
    bottom->addWidget(m_countLabel, 1);  // 提示占位，左对齐
    bottom->addWidget(closeBtn);
    root->addLayout(bottom);
}

void HistoryDialog::refresh()
{
    QJsonArray arr = HistoryStore::instance().recent(500);
    m_table->setRowCount(arr.size());

    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject o = arr.at(i).toObject();
        const QString filename = o.value(QStringLiteral("filename")).toString();
        const QString url      = o.value(QStringLiteral("url")).toString();
        const qint64 size      = o.value(QStringLiteral("size")).toVariant().toLongLong();
        const QString status   = o.value(QStringLiteral("status")).toString();
        const QString added    = o.value(QStringLiteral("added_at")).toString();
        const QString finished = o.value(QStringLiteral("finished_at")).toString();

        auto* nameItem = new QTableWidgetItem(filename.isEmpty() ? url : filename);
        nameItem->setToolTip(url);
        m_table->setItem(i, 0, nameItem);
        m_table->setItem(i, 1, new QTableWidgetItem(url));
        m_table->setItem(i, 2, new QTableWidgetItem(formatSize(size)));
        m_table->setItem(i, 3, new QTableWidgetItem(statusText(status)));
        m_table->setItem(i, 4, new QTableWidgetItem(added));
        m_table->setItem(i, 5, new QTableWidgetItem(finished));
    }
    m_table->resizeColumnToContents(0);
    m_countLabel->setText(QStringLiteral("共 %1 条记录").arg(arr.size()));
}

void HistoryDialog::onClear()
{
    const int n = HistoryStore::instance().count();
    if (n <= 0) {
        QMessageBox::information(this, QStringLiteral("下载历史"),
                                 QStringLiteral("当前没有可清空的历史记录。"));
        return;
    }
    auto btn = QMessageBox::question(this, QStringLiteral("清空下载历史"),
        QStringLiteral("确定要清空全部 %1 条下载历史吗？\n此操作不可撤销。").arg(n),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (btn != QMessageBox::Yes)
        return;
    HistoryStore::instance().clear();
    Log::info(QStringLiteral("用户清空了下载历史"));
    refresh();
}

void HistoryDialog::onRowDoubleClicked(int row, int /*column*/)
{
    QTableWidgetItem* urlItem = m_table->item(row, 1);
    if (!urlItem)
        return;
    const QString url = urlItem->text();
    if (url.isEmpty())
        return;
    QApplication::clipboard()->setText(url);
    m_countLabel->setText(QStringLiteral("已复制链接: %1").arg(
        url.length() > 60 ? url.left(60) + QStringLiteral("…") : url));
}

QString HistoryDialog::formatSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    static const QStringList units = {
        QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"),
        QStringLiteral("GB"), QStringLiteral("TB")
    };
    double val = static_cast<double>(bytes);
    int u = 0;
    while (val >= 1024.0 && u < units.size() - 1) {
        val /= 1024.0;
        ++u;
    }
    if (u == 0)
        return QString::number(static_cast<qint64>(val)) + QStringLiteral(" B");
    return QString::number(val, 'f', 2) + QStringLiteral(" ") + units.at(u);
}

QString HistoryDialog::statusText(const QString& status)
{
    if (status == QStringLiteral("completed"))
        return QStringLiteral("已完成");
    if (status == QStringLiteral("failed"))
        return QStringLiteral("失败");
    if (status == QStringLiteral("queued"))
        return QStringLiteral("已加入");
    return status.isEmpty() ? QStringLiteral("—") : status;
}
