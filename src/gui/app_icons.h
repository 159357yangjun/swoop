#ifndef APP_ICONS_H
#define APP_ICONS_H

#include <QIcon>
#include <QColor>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

#include "category_filter_proxy.h"   // FileType：模型报上来的类型键（gui → model，向下依赖）

class QPainter;

// 应用自绘图标集（单色线性，主题感知）
//
// 为什么不用 QStyle::standardIcon：
//   系统标准图标是 Windows 自己的位图（回收站、蓝色文件夹、蓝色信息气泡…），
//   与手绘的播放/暂停/停止图标风格完全不是一套；而且语义经常对不上
//   （例如「历史」原本用的 SP_FileDialogInfoView 是个「信息」气泡，
//     「新建任务」用的 SP_FileDialogNewFolder 是「新建文件夹」带星星）。
//   混在一起看就是「拼凑感」，这是界面美观上最扎眼的一处。
//
// 这里改为全部 QPainter 矢量绘制，统一 16×16 逻辑网格、统一线宽 1.5、统一圆头圆角，
// 颜色随主题（亮色深灰 / 暗色浅灰，禁用态降对比，强调态用主题蓝），
// 通过 QIconEngine 在每次 paint/pixmap 时实时取色，所以切换主题无需重建图标。
namespace AppIcons {

enum class Glyph {
    NewTask,      // 下载箭头 + 托盘
    Start,        // 实心播放三角
    Restart,      // 环形箭头（重新开始）
    Pause,        // 双竖条
    Cancel,       // 实心圆角方块
    Remove,       // 垃圾桶
    Queue,        // 列表
    Site,         // 地球
    History,      // 时钟
    Settings,     // 滑杆
    Category,     // 文件夹
    Pending,      // 时钟（未完成）
    Completed,    // 圆圈对勾
    Failed,       // 三角感叹号
    Cancelled,    // 禁止符
    Compressed,   // 压缩包
    Document,     // 文档
    Music,        // 音符
    Program,      // 应用窗口
    Video,        // 影片
    OtherFile,    // 无扩展名/未识别类型：光页（比「文档」少两行正文线，避免两个图标看起来一样）
    More,         // 三个点（工具栏「更多」溢出菜单）
};

// 语义着色槽位。取值本身不代表颜色，颜色在 paint 时按当前主题实时解析 ——
// 这样主题切换后不需要重建任何 QIcon（与 icon(Glyph) 同一套机制）。
// 用途：文件类型图标按类型着色，让用户在列表/分类树里一眼扫出文件种类。
enum class Tint {
    Default,   // 跟随主题前景色（工具栏等普通场景）
    Accent,    // 主题强调色
    Green,     // 完成 / 压缩包
    Amber,     // 待定 / 程序
    Indigo,    // 文档
    Purple,    // 音乐
    Rose,      // 视频
    Red,       // 失败
    Slate,     // 中性
};

// 由 MainWindow::applyTheme 调用，切换图标配色
void setDarkTheme(bool dark);
bool isDarkTheme();

// 取得图标（可安全跨主题复用；内部实时取色）
QIcon icon(Glyph g);

// 取带语义着色的图标（同上：颜色实时解析，可跨主题复用）
QIcon icon(Glyph g, Tint tint);

// 按文件类型给出「文件类型图标」（带类型语义色）。
// 参数是 model 层报上来的类型键（TaskListModel::FileTypeRole），不是文件名——
// 后缀表全项目只有 CategoryFilterProxy::detectFileType 那一张，判定归模型，
// 键到图标的翻译归这一层，所以依赖方向是 gui → model（见 tools/lint_layering.py）。
QIcon fileTypeIcon(FileType fileType);

/* 文件名列的绘制委托：把模型发来的类型键换成 QIcon。
 * 用法：view->setItemDelegateForColumn(0, new AppIcons::FileTypeIconDelegate(TaskListModel::FileTypeRole, view));
 * 只做一件事——在 QStyledItemDelegate 已经算好的样式选项上补图标，不重画文本、不改尺寸。
 * 键无效（例如分组头行、或该列不是文件名列）时完全不干预，保持默认外观。 */
class FileTypeIconDelegate : public QStyledItemDelegate {
public:
    explicit FileTypeIconDelegate(int fileTypeRole, QObject* parent = nullptr);

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;

private:
    int m_fileTypeRole;
};

// 内部使用：绘制单个字形（坐标系固定 16×16，原点左上）
void paintGlyph(QPainter* painter, Glyph g, const QColor& color);

} // namespace AppIcons

#endif // APP_ICONS_H
