#ifndef APPEARANCE_CONTROLLER_H
#define APPEARANCE_CONTROLLER_H

#include <QObject>
#include <QString>
#include <functional>

class QToolBar;
class Settings;

// L2 组件：外观应用（主题 QSS + 视图缩放）。
// 自身不持有任何视图控件：主题应用后需重绘的自绘控件经 repaint sink 触发，
// 工具栏图标尺寸经 ToolBar* 注入；缩放级数（m_zoomLevel）作为外观状态由本组件持有。
class AppearanceController : public QObject {
    Q_OBJECT
public:
    explicit AppearanceController(QObject* parent = nullptr);

    void setSettings(Settings* s) { m_settings = s; }
    void setToolBar(QToolBar* bar) { m_toolBar = bar; }
    void setRepaintSink(std::function<void()> f) { m_repaintSink = f; }

    void applyTheme();   // 依据设置加载 light/dark QSS 并同步自绘委托 + 触发重绘
    void applyZoom();    // 依据缩放级数应用全局字体 + 工具栏图标尺寸

public slots:
    void zoomIn();
    void zoomOut();
    void zoomReset();

private:
    Settings* m_settings = nullptr;
    QToolBar* m_toolBar = nullptr;
    std::function<void()> m_repaintSink;
    int m_zoomLevel = 0;            // 字体缩放级数（每级 +/-1pt）
    static constexpr int BASE_FONT_PT = 13;
    static constexpr int BASE_ICON_SZ = 22;
};

#endif // APPEARANCE_CONTROLLER_H
