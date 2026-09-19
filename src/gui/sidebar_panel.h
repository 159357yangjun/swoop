#ifndef SIDEBAR_PANEL_H
#define SIDEBAR_PANEL_H

#include <QWidget>
#include <QStyledItemDelegate>

class QTreeView;

// ── 左侧顶部状态行 ──────────────────────────────────────────────
// 一行纯文字：● 下载中 N    ● 已完成 N    ● 失败 N
//
// 刻意不做卡片、不做图表。这一区曾经是一块带渐变速度曲线的「总览卡片」，
// 已撤掉：装饰性容器会把视线从任务列表上抢走，而这些数字本身一行就说得清。
// 总速度移回状态栏（那才是它原本该在的地方）。
class SidebarStatsBar : public QWidget {
    Q_OBJECT
public:
    explicit SidebarStatsBar(QWidget* parent = nullptr);

    void setCounts(int active, int done, int failed);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_active = 0, m_done = 0, m_failed = 0;
};

// ── 分类树行委托：数量徽标 ───────────────────────────────────────
// 计数不算在委托里——MainWindow 算好后写进模型的 countRole，这里只负责画。
// 这样委托不依赖任何业务类型，树节点增删也不用改这里。
class CategoryBadgeDelegate : public QStyledItemDelegate {
public:
    // countRole 值为 -1 表示该行不显示徽标（如「站点抓取方案」这类入口）
    explicit CategoryBadgeDelegate(int countRole, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    int m_countRole;
};

// ── 左侧导航面板 ─────────────────────────────────────────────────
// 状态行 + 分类树。树的模型/过滤逻辑不动，本类只负责容器与视觉。
class SidebarPanel : public QWidget {
    Q_OBJECT
public:
    explicit SidebarPanel(QTreeView* tree, QWidget* parent = nullptr);

    SidebarStatsBar* stats() const { return m_stats; }

    static void setDarkTheme(bool dark);
    static bool isDarkTheme();

private:
    SidebarStatsBar* m_stats = nullptr;
};

#endif // SIDEBAR_PANEL_H
