#include "appearance_controller.h"
#include "settings.h"
#include "logger.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QSize>
#include <QToolBar>

#include "progress_delegate.h"
#include "sidebar_panel.h"
#include "app_icons.h"

static constexpr int ZOOM_MAX = 6;
static constexpr int ZOOM_MIN = -4;

AppearanceController::AppearanceController(QObject* parent)
    : QObject(parent) {}

void AppearanceController::applyTheme()
{
    if (!m_settings) return;
    const QString theme = m_settings->theme();
    QString qssPath = QStringLiteral(":/qss/%1.qss").arg(theme);
    QFile f(qssPath);
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
        Log::info(QStringLiteral("已加载主题: %1").arg(theme));
    } else {
        QFile fallback(QStringLiteral(":/qss/light.qss"));
        if (fallback.open(QFile::ReadOnly | QFile::Text))
            qApp->setStyleSheet(QString::fromUtf8(fallback.readAll()));
        Log::warn(QStringLiteral("主题 %1 加载失败，回退到 light").arg(theme));
    }

    // 状态列/侧栏/图标均为自绘，QSS 管不到；同步主题标记位并触发重绘
    const bool dark = (theme == QStringLiteral("dark"));
    ProgressDelegate::setDarkTheme(dark);
    SidebarPanel::setDarkTheme(dark);
    AppIcons::setDarkTheme(dark);
    if (m_repaintSink) m_repaintSink();
}

void AppearanceController::applyZoom()
{
    int pt = BASE_FONT_PT + m_zoomLevel;
    if (pt < 8) pt = 8;
    if (pt > 24) pt = 24;

    QFont font = qApp->font();
    font.setPointSize(pt);
    qApp->setFont(font);

    int iconSz = BASE_ICON_SZ + m_zoomLevel * 2;
    if (iconSz < 16) iconSz = 16;
    if (iconSz > 48) iconSz = 48;
    if (m_toolBar) m_toolBar->setIconSize(QSize(iconSz, iconSz));
}

void AppearanceController::setZoomLevel(int level)
{
    /* 启动时从设置里恢复。夹到 [ZOOM_MIN, ZOOM_MAX]：配置是明文可改的，
       越界值不该让字体算出负字号。 */
    if (level > ZOOM_MAX) level = ZOOM_MAX;
    if (level < ZOOM_MIN) level = ZOOM_MIN;
    m_zoomLevel = level;
}

void AppearanceController::saveZoom()
{
    if (!m_settings) return;
    m_settings->setViewZoom(m_zoomLevel);
    m_settings->save();
}

void AppearanceController::zoomIn()
{
    if (m_zoomLevel < ZOOM_MAX) {
        ++m_zoomLevel;
        applyZoom();
        saveZoom();
        Log::info(QStringLiteral("视图放大: %1pt").arg(BASE_FONT_PT + m_zoomLevel));
    }
}

void AppearanceController::zoomOut()
{
    if (m_zoomLevel > ZOOM_MIN) {
        --m_zoomLevel;
        applyZoom();
        saveZoom();
        Log::info(QStringLiteral("视图缩小: %1pt").arg(BASE_FONT_PT + m_zoomLevel));
    }
}

void AppearanceController::zoomReset()
{
    m_zoomLevel = 0;
    applyZoom();
    saveZoom();
    Log::info(QStringLiteral("视图缩放重置"));
}
