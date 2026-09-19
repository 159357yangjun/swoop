#include "sidebar_panel.h"

#include <QPainter>
#include <QFontMetrics>
#include <QTreeView>
#include <QBoxLayout>
#include <QStyle>
#include <QStyleOption>

// ── 主题配色 ────────────────────────────────────────────────────
// 与 ProgressDelegate 的语义色、qss 里的中性灰底保持同一套取值。
static bool g_dark = false;

namespace {

struct Palette {
    QColor textPrimary, textSecondary, textFaint;
    QColor accent;
    QColor ok, fail, idle;
    QColor badgeBg, badgeText, badgeZeroText;
};

Palette pal()
{
    Palette c;
    if (g_dark) {
        c.textPrimary   = QColor(0xe8, 0xea, 0xed);
        c.textSecondary = QColor(0x9a, 0xa0, 0xaa);
        c.textFaint     = QColor(0x56, 0x5b, 0x64);
        c.accent        = QColor(0x4c, 0x8d, 0xff);
        c.ok            = QColor(0x3f, 0xb2, 0x7f);
        c.fail          = QColor(0xe0, 0x65, 0x65);
        c.idle          = QColor(0x6b, 0x70, 0x7a);
        c.badgeBg       = QColor(0x2c, 0x2d, 0x31);
        c.badgeText     = QColor(0xc3, 0xc7, 0xcd);
        c.badgeZeroText = QColor(0x56, 0x5b, 0x64);
    } else {
        c.textPrimary   = QColor(0x1f, 0x23, 0x29);
        c.textSecondary = QColor(0x6b, 0x72, 0x80);
        c.textFaint     = QColor(0xc9, 0xcd, 0xd4);
        c.accent        = QColor(0x25, 0x63, 0xeb);
        c.ok            = QColor(0x2f, 0x9e, 0x6f);
        c.fail          = QColor(0xd0, 0x52, 0x52);
        c.idle          = QColor(0x8a, 0x8f, 0x99);
        c.badgeBg       = QColor(0xee, 0xf0, 0xf3);
        c.badgeText     = QColor(0x4e, 0x59, 0x69);
        c.badgeZeroText = QColor(0xc9, 0xcd, 0xd4);
    }
    return c;
}

} // namespace

void SidebarPanel::setDarkTheme(bool dark) { g_dark = dark; }
bool SidebarPanel::isDarkTheme() { return g_dark; }

// ════════════════════════════════════════════════════════════════
// SidebarStatsBar
// ════════════════════════════════════════════════════════════════

SidebarStatsBar::SidebarStatsBar(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(26);   // 一行文字的高度，不再占掉侧栏四分之一
}

void SidebarStatsBar::setCounts(int active, int done, int failed)
{
    if (m_active == active && m_done == done && m_failed == failed)
        return;   // 值没变就别重绘（本函数会被 1Hz 定时器反复调用）
    m_active = active;
    m_done = done;
    m_failed = failed;
    update();
}

QSize SidebarStatsBar::sizeHint() const
{
    return QSize(180, 26);
}

void SidebarStatsBar::paintEvent(QPaintEvent*)
{
    const Palette c = pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    struct Item { QString label; int value; QColor dot; };
    const Item items[3] = {
        {QStringLiteral("下载中"), m_active, c.accent},
        {QStringLiteral("已完成"), m_done,   c.ok},
        {QStringLiteral("失败"),   m_failed, c.fail},
    };

    QFont lf = font();
    lf.setPixelSize(11);
    const QFontMetrics lfm(lf);

    const int cy = height() / 2;
    int x = 14;   // 与树的条目文字大致左对齐（树有 viewport padding + item 内边距）

    for (int i = 0; i < 3; ++i) {
        const bool has = items[i].value > 0;

        // 圆点：数量为 0 时退成中性灰，避免三个彩点同时抢眼
        p.setPen(Qt::NoPen);
        p.setBrush(has ? items[i].dot : c.idle);
        p.drawEllipse(QPointF(x + 3.0, cy), 3.0, 3.0);
        x += 11;

        p.setFont(lf);
        p.setPen(c.textSecondary);
        p.drawText(QRect(x, cy - 9, lfm.horizontalAdvance(items[i].label) + 2, 18),
                   Qt::AlignLeft | Qt::AlignVCenter, items[i].label);
        x += lfm.horizontalAdvance(items[i].label) + 4;

        QFont vf = lf;
        vf.setBold(true);
        p.setFont(vf);
        p.setPen(has ? c.textPrimary : c.textFaint);
        const QString v = QString::number(items[i].value);
        const int vw = QFontMetrics(vf).horizontalAdvance(v);
        p.drawText(QRect(x, cy - 9, vw + 2, 18), Qt::AlignLeft | Qt::AlignVCenter, v);
        x += vw + 14;   // 组间距

        if (x > width() - 20)
            break;      // 侧栏被拖很窄时后面的组直接不画，而不是压成一团
    }
}

// ════════════════════════════════════════════════════════════════
// CategoryBadgeDelegate
// ════════════════════════════════════════════════════════════════

CategoryBadgeDelegate::CategoryBadgeDelegate(int countRole, QObject* parent)
    : QStyledItemDelegate(parent), m_countRole(countRole)
{
}

void CategoryBadgeDelegate::paint(QPainter* painter,
                                  const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
    // 先交回默认绘制：选中/悬停底色、文字、图标、缩进全部由 QSS 与样式处理，
    // 这里只补一个徽标，避免和 qt 的样式逻辑打架。
    QStyledItemDelegate::paint(painter, option, index);

    if (m_countRole <= 0 || !index.isValid())
        return;
    const QVariant raw = index.data(m_countRole);
    if (!raw.isValid())
        return;
    const int count = raw.toInt();
    if (count < 0)
        return;   // -1 = 该行不显示徽标（功能入口类节点）

    const Palette c = pal();
    const bool selected = option.state & QStyle::State_Selected;
    const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);

    QFont f = option.font;
    f.setPixelSize(10);
    f.setBold(count > 0);
    const QFontMetrics fm(f);

    const int bw = qMax(20, fm.horizontalAdvance(text) + 14);
    const int bh = 17;
    QRectF badge(option.rect.right() - bw - 8,
                 option.rect.center().y() - bh / 2.0 + 0.5,
                 bw, bh);
    // 侧栏被拖窄时徽标可能压出可视区：夹回行内，宁可贴边也不要悬空
    if (badge.left() < option.rect.left() + 4)
        badge.moveLeft(option.rect.left() + 4);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (count == 0) {
        // 零值：只留数字轮廓，不画底，让树整体安静下来
        painter->setPen(c.badgeZeroText);
        painter->setFont(f);
        painter->drawText(badge, Qt::AlignCenter, text);
    } else {
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? QColor(c.accent.red(), c.accent.green(), c.accent.blue(), 38)
                                   : c.badgeBg);
        painter->drawRoundedRect(badge, bh / 2.0, bh / 2.0);

        painter->setPen(selected ? c.accent : c.badgeText);
        painter->setFont(f);
        painter->drawText(badge, Qt::AlignCenter, text);
    }

    painter->restore();
}

QSize CategoryBadgeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(qMax(s.height(), 28));
    return s;
}

// ════════════════════════════════════════════════════════════════
// SidebarPanel
// ════════════════════════════════════════════════════════════════

SidebarPanel::SidebarPanel(QTreeView* tree, QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    m_stats = new SidebarStatsBar(this);
    v->addWidget(m_stats);

    if (tree) {
        tree->setParent(this);
        // 缩进收窄 + 行高收紧：默认 20px 缩进在 200px 侧栏里会吃掉一半宽度，
        // 三层展开后文字只剩几个字符，是旧版侧栏最明显的观感问题。
        tree->setIndentation(14);
        tree->setIconSize(QSize(16, 16));
        v->addWidget(tree, 1);
    }
}
