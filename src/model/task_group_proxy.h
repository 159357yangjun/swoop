#ifndef TASK_GROUP_PROXY_H
#define TASK_GROUP_PROXY_H

#include <QAbstractTableModel>
#include <QVector>
#include <QMap>
#include <QSet>
#include <QString>

class TaskListModel;

// 任务列表分组代理：在「分类过滤代理」与视图之间插入一层，
// 把扁平任务列表按「类型 / 队列」折叠成可展开的分组头行。
//
// 设计要点：
// - 直接继承 QAbstractTableModel（而非 QAbstractProxyModel），完全掌控结构变化，
//   避免基类对 source layoutChanged 的自动转发与注入的分组头行错位。
// - None 模式为 1:1 透传；ByType/ByQueue 模式按 source 行顺序收集分组，
//   每个分组前插入一个分组头行（GroupHeaderRole=true），折叠时分组的子行不进入代理。
// - 高频进度刷新走 source 的 dataChanged，仅按列转发、不重建结构；
//   仅结构变化（增删任务）或队列归属变化（影响 ByQueue 分组）才重建。
class TaskGroupProxy : public QAbstractTableModel {
    Q_OBJECT
public:
    enum GroupMode { None = 0, ByType = 1, ByQueue = 2 };

    // 与 TaskListModel::GroupHeaderRole 对齐（视图/委托识别分组头行用）
    enum { GroupHeaderRole   = Qt::UserRole + 6,   // bool：该行是分组头
           GroupKeyRole      = Qt::UserRole + 7,   // 分组 key 文本
           GroupCollapsedRole= Qt::UserRole + 8 };  // bool：该分组是否折叠

    explicit TaskGroupProxy(QObject* parent = nullptr);

    void setSourceModel(QAbstractItemModel* src);
    QAbstractItemModel* sourceModel() const { return m_source; }

    void setGroupMode(GroupMode m);
    GroupMode groupMode() const { return m_mode; }
    void toggleGroup(int proxyRow);   // 折叠/展开 proxyRow 所在分组（仅分组头行有效）

    // 视图索引 ↔ 源模型索引 翻译（main_window 右键/双击/选中翻译用）
    QModelIndex mapToSource(const QModelIndex& proxyIndex) const;
    QModelIndex mapFromSource(const QModelIndex& sourceIndex) const;

    // QAbstractItemModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    struct Row { bool isHeader; int groupOrdinal; int sourceRow; int childCount; QString key; };

    void rebuild();                 // 依据当前模式重建 m_rows / m_sourceToProxy
    void rebuildAndEmit();          // beginResetModel + rebuild + endResetModel
    QString groupKeyFor(int sourceRow) const;

    QAbstractItemModel* m_source = nullptr;
    GroupMode           m_mode  = None;
    QVector<Row>        m_rows;                 // 代理行 -> 行描述
    QMap<int, int>      m_sourceToProxy;        // sourceRow -> 代理行（仅可见子行）
    QSet<int>           m_collapsed;            // 已折叠的分组 ordinal
};

#endif // TASK_GROUP_PROXY_H
