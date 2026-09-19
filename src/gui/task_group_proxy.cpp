#include "task_group_proxy.h"
#include "task_list_model.h"
#include "category_filter_proxy.h"

#include <QFont>
#include <QColor>

TaskGroupProxy::TaskGroupProxy(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void TaskGroupProxy::setSourceModel(QAbstractItemModel* src)
{
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = src;
    if (!m_source) { m_rows.clear(); m_sourceToProxy.clear(); return; }

    // 结构变化：整体重建（reset）。reset 简单且正确；分组变动本就不频繁。
    // 注：基类受保护信号（modelReset/rowsInserted 等）带 QPrivateSignal，子类无法转发，
    // 故统一走 reset；分类切换导致的滚动跳顶由 MainWindow::onCategoryClicked 存滚动作补偿。
    connect(m_source, &QAbstractItemModel::modelReset,
            this, [this] { rebuildAndEmit(); });
    connect(m_source, &QAbstractItemModel::layoutChanged,
            this, [this] { rebuildAndEmit(); });
    connect(m_source, &QAbstractItemModel::rowsInserted,
            this, [this] { rebuildAndEmit(); });
    connect(m_source, &QAbstractItemModel::rowsRemoved,
            this, [this] { rebuildAndEmit(); });

    // 数据变化：进度/状态类只按列转发（不重建）；队列归属变化需重建（影响 ByQueue 分组）
    connect(m_source, &QAbstractItemModel::dataChanged,
            this, [this](const QModelIndex& tl, const QModelIndex& br,
                         const QVector<int>& roles) {
                if (m_mode == ByQueue && roles.contains(TaskListModel::QueueRole)) {
                    rebuildAndEmit();
                    return;
                }
                // 逐源行转发映射后的索引，跳过被折叠隐藏（无代理行）的子行
                for (int r = tl.row(); r <= br.row(); ++r) {
                    QModelIndex p = mapFromSource(m_source->index(r, tl.column()));
                    if (p.isValid()) {
                        QModelIndex pbr = p.siblingAtColumn(br.column());
                        emit dataChanged(p, pbr, roles);
                    }
                }
            });

    rebuildAndEmit();
}

void TaskGroupProxy::setGroupMode(GroupMode m)
{
    if (m_mode == m) return;
    m_mode = m;
    rebuildAndEmit();
}

void TaskGroupProxy::toggleGroup(int proxyRow)
{
    if (proxyRow < 0 || proxyRow >= m_rows.size()) return;
    const Row& row = m_rows[proxyRow];
    if (!row.isHeader) return;
    if (m_collapsed.contains(row.groupOrdinal))
        m_collapsed.remove(row.groupOrdinal);
    else
        m_collapsed.insert(row.groupOrdinal);
    rebuildAndEmit();
}

int TaskGroupProxy::rowCount(const QModelIndex&) const
{
    return m_rows.size();
}

int TaskGroupProxy::columnCount(const QModelIndex&) const
{
    return m_source ? m_source->columnCount() : 0;
}

QVariant TaskGroupProxy::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (!m_source) return QVariant();
    return m_source->headerData(section, orientation, role);
}

Qt::ItemFlags TaskGroupProxy::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    const Row& row = m_rows[index.row()];
    if (row.isHeader)
        return Qt::ItemIsEnabled;   // 分组头可点击（折叠），但不可选中/编辑
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant TaskGroupProxy::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || !m_source) return QVariant();
    const Row& row = m_rows[index.row()];

    if (row.isHeader) {
        if (role == GroupHeaderRole)    return true;
        if (role == GroupCollapsedRole) return m_collapsed.contains(row.groupOrdinal);
        if (role == GroupKeyRole)       return row.key;

        if (role == Qt::DisplayRole) {
            if (index.column() == 0) {
                // 展开/折叠箭头（▾ 展开 / ▸ 折叠），提示分组头可点击
                QString arrow = m_collapsed.contains(row.groupOrdinal)
                                   ? QStringLiteral("▸ ")
                                   : QStringLiteral("▾ ");
                return arrow + QStringLiteral("%1 (%2)").arg(row.key).arg(row.childCount);
            }
            return QVariant();   // 其余列在分组头行留空（由视图 setSpan 跨列合并）
        }
        if (role == Qt::FontRole) {
            QFont f; f.setBold(true); return f;
        }
        if (role == Qt::ForegroundRole)
            return QColor(90, 90, 96);                 // 中性深灰，符合简洁取向
        if (role == Qt::BackgroundRole)
            return QColor(238, 240, 244);             // 浅灰分组头底，与交替行区分
        if (role == Qt::TextAlignmentRole)
            return QVariant(static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter));
        return QVariant();
    }

    // 子行：原样转发到源模型（含 StateRole / ProgressRole / QueueRole 等）
    return m_source->data(m_source->index(row.sourceRow, index.column()), role);
}

QModelIndex TaskGroupProxy::mapToSource(const QModelIndex& proxyIndex) const
{
    if (!proxyIndex.isValid() || !m_source) return QModelIndex();
    const Row& row = m_rows[proxyIndex.row()];
    if (row.isHeader) return QModelIndex();   // 分组头无对应源行
    return m_source->index(row.sourceRow, proxyIndex.column());
}

QModelIndex TaskGroupProxy::mapFromSource(const QModelIndex& sourceIndex) const
{
    if (!sourceIndex.isValid() || !m_source) return QModelIndex();
    if (m_mode == None)
        return index(sourceIndex.row(), sourceIndex.column());
    auto it = m_sourceToProxy.find(sourceIndex.row());
    if (it == m_sourceToProxy.end()) return QModelIndex();   // 被折叠隐藏
    return index(it.value(), sourceIndex.column());
}

// ── 内部 ────────────────────────────────────────────────

QString TaskGroupProxy::groupKeyFor(int sourceRow) const
{
    QModelIndex idx = m_source->index(sourceRow, 0);
    QString name = m_source->data(idx, Qt::DisplayRole).toString();
    if (m_mode == ByType)
        return CategoryFilterProxy::fileTypeFolderName(
                   CategoryFilterProxy::detectFileType(name));
    // ByQueue：队列名（空 → 调用方转「未分组」）
    return m_source->data(idx, TaskListModel::QueueRole).toString();
}

void TaskGroupProxy::rebuild()
{
    m_rows.clear();
    m_sourceToProxy.clear();
    if (!m_source) return;

    int n = m_source->rowCount();
    if (m_mode == None) {
        m_rows.reserve(n);
        for (int r = 0; r < n; ++r) {
            m_rows.append({false, -1, r, 0, QString()});
            m_sourceToProxy[r] = m_rows.size() - 1;
        }
        return;
    }

    const QString ungrouped = QStringLiteral("未分组");
    QMap<QString, QVector<int>> children;   // key -> 源行列表
    QStringList presentKeys;

    for (int r = 0; r < n; ++r) {
        QString key = groupKeyFor(r);
        if (key.isEmpty()) key = ungrouped;
        if (!children.contains(key)) {
            children[key] = QVector<int>();
            presentKeys.append(key);
        }
        children[key].append(r);
    }

    // 分组顺序：按类型用固定优先级；按队列「未分组」放最后，其余按首次出现顺序
    if (m_mode == ByType) {
        static const QStringList fixed = {
            QStringLiteral("视频"), QStringLiteral("音乐"), QStringLiteral("压缩包"),
            QStringLiteral("文档"), QStringLiteral("程序"), QStringLiteral("其他")
        };
        QStringList ordered;
        for (const QString& k : fixed)
            if (children.contains(k)) ordered.append(k);
        for (const QString& k : presentKeys)
            if (!fixed.contains(k)) ordered.append(k);   // 兜底：理论不会命中
        presentKeys = ordered;
    } else {
        if (presentKeys.contains(ungrouped)) {
            presentKeys.removeAll(ungrouped);
            presentKeys.append(ungrouped);
        }
    }

    int ordinal = 0;
    for (const QString& key : presentKeys) {
        int g = ordinal++;
        int cnt = children[key].size();
        m_rows.append({true, g, -1, cnt, key});   // 分组头
        if (!m_collapsed.contains(g)) {
            for (int r : children[key]) {
                m_rows.append({false, g, r, 0, QString()});
                m_sourceToProxy[r] = m_rows.size() - 1;
            }
        }
    }
}

void TaskGroupProxy::rebuildAndEmit()
{
    beginResetModel();
    rebuild();
    endResetModel();
}
