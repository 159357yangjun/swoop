#include "progress_delegate.h"
#include "task_list_model.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyle>

// 主题状态（由 MainWindow::applyTheme 通过 setDarkTheme 设置）
static bool g_dark = false;

void ProgressDelegate::setDarkTheme(bool dark) { g_dark = dark; }
bool ProgressDelegate::isDarkTheme() { return g_dark; }

namespace {
// 语义化扁平配色：中低饱和，配白色粗体文字，明/暗主题下对比度都足够。
// 不使用渐变（旧的粉→紫→蓝渐变既不符设计语言，暗色下文字也几乎不可读）。
QColor stateColor(int state) {
    switch (state) {
        case 0: return QColor(0x8a, 0x8f, 0x99);   // 等待中：中性灰
        case 2: return QColor(0xc9, 0x8a, 0x2e);   // 已暂停：琥珀
        case 3: return QColor(0x2f, 0x9e, 0x6f);   // 已完成：绿
        case 4: return QColor(0xd0, 0x52, 0x52);   // 失败：红
        case 5: return QColor(0x8a, 0x8f, 0x99);   // 已取消：中性灰
        default: break;                            // 1 = 下载中 → 强调色
    }
    return g_dark ? QColor(0x4c, 0x8d, 0xff) : QColor(0x25, 0x63, 0xeb);
}
} // namespace

ProgressDelegate::ProgressDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void ProgressDelegate::paint(QPainter* painter,
                             const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // 分组头行（被跨列合并前可能短暂绘制）：不画进度条，交回默认绘制
    if (index.data(TaskListModel::GroupHeaderRole).toBool()) {
        painter->restore();
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // 单元格内边距与进度条几何
    QRect cell = option.rect.adjusted(10, 5, -10, -5);
    const int barH = qMin(18, cell.height() - 2);
    QRect bar(cell.x(), cell.y() + (cell.height() - barH) / 2,
              cell.width(), barH);

    const int pct   = qBound(0, index.data(TaskListModel::ProgressRole).toInt(), 100);
    const int state = index.data(TaskListModel::StateRole).toInt();
    const bool running = (state == 1);   // 1 = 下载中

    // 轨道：低饱和中性底（亮/暗主题下均可见），背景仍由视图绘制
    painter->setPen(Qt::NoPen);
    painter->setBrush(g_dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 18));
    painter->drawRoundedRect(bar, barH / 2.0, barH / 2.0);

    QFont f = option.font;
    f.setPointSize(qMax(9, f.pointSize() - 1));
    f.setBold(true);

    if (running) {
        // 下载中：扁平强调色进度填充（无渐变，裁剪到圆角轨道内）
        if (pct > 0) {
            painter->save();
            QPainterPath clip;
            clip.addRoundedRect(bar, barH / 2.0, barH / 2.0);
            painter->setClipPath(clip);
            painter->setBrush(stateColor(state));
            QRect fill = bar;
            fill.setWidth(qMax(barH, bar.width() * pct / 100));
            painter->drawRoundedRect(fill, barH / 2.0, barH / 2.0);
            painter->restore();
        }
        // 百分比文字用主题前景色，在轨道/填充上都可读
        painter->setPen(g_dark ? QColor(0xe8, 0xea, 0xed) : QColor(0x1f, 0x23, 0x29));
        painter->setFont(f);
        painter->drawText(bar, Qt::AlignCenter, QStringLiteral("%1%").arg(pct));
    } else {
        // 终态/暂停：扁平语义徽章（实心 + 白色粗体），替代旧渐变胶囊
        QString label;
        switch (state) {
            case 0: label = QStringLiteral("等待中"); break;
            case 2: label = QStringLiteral("已暂停"); break;
            case 3: label = QStringLiteral("已完成"); break;
            case 4: label = QStringLiteral("失败");   break;
            case 5: label = QStringLiteral("已取消"); break;
            default:label = QStringLiteral("等待中"); break;
        }
        QRect badge = bar;
        const int w = qMin(bar.width(), 76);
        badge.setWidth(w);
        badge.moveLeft(bar.x() + (bar.width() - w) / 2);
        painter->setPen(Qt::NoPen);
        painter->setBrush(stateColor(state));
        painter->drawRoundedRect(badge, badge.height() / 2.0, badge.height() / 2.0);

        painter->setPen(QColor(0xff, 0xff, 0xff));
        painter->setFont(f);
        painter->drawText(badge, Qt::AlignCenter, label);
    }

    painter->restore();
}

QSize ProgressDelegate::sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(qMax(s.height(), 28));
    return s;
}
