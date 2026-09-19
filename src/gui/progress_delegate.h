#ifndef PROGRESS_DELEGATE_H
#define PROGRESS_DELEGATE_H

#include <QStyledItemDelegate>

// 状态列（下载进度）自定义绘制委托
// 下载中：扁平强调色进度条 + 居中百分比；终态/暂停：扁平语义状态徽章。
// 进度值取自 TaskListModel::ProgressRole，状态取自 StateRole。
// 配色随主题切换（setDarkTheme 由 MainWindow::applyTheme 调用），
// 不再使用玻璃拟态渐变（旧粉→紫→蓝渐变在暗色主题下文字几乎不可读）。
class ProgressDelegate : public QStyledItemDelegate {
public:
    explicit ProgressDelegate(QObject* parent = nullptr);

    // 由 applyTheme() 调用，切换亮/暗配色（进度条与徽章文字对比度均需随之调整）
    static void setDarkTheme(bool dark);
    static bool isDarkTheme();

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

#endif // PROGRESS_DELEGATE_H
