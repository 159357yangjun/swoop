#include "glass_effect.h"

#include <QPainter>
#include <QGraphicsDropShadowEffect>
#include <QRandomGenerator>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")
#endif

namespace Glass {

void enableGlassWindow(QWidget* w)
{
    if (!w)
        return;

    // 让 Qt 使用透明背景，paintEvent 绘制的渐变才能透出来
    w->setAttribute(Qt::WA_TranslucentBackground);
    w->setAutoFillBackground(false);

#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(w->winId());

    // Win11: 优先使用系统 backdrop (Mica/Acrylic/Blur)
    // DWMWA_SYSTEMBACKDROP_TYPE = 38, DWMSBT_MAINWINDOW = 2, DWMSBT_ACRYLIC = 3
    if (HMODULE hDwm = LoadLibraryW(L"dwmapi.dll")) {
        using DwmSetWindowAttribute_t = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = reinterpret_cast<DwmSetWindowAttribute_t>(
            GetProcAddress(hDwm, "DwmSetWindowAttribute"));
        if (fn) {
            DWORD backdrop = 3; // DWMSBT_ACRYLIC
            fn(hwnd, 38, &backdrop, sizeof(backdrop));
        }
        FreeLibrary(hDwm);
    }

    // Win10 备用：启用 blur behind
    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    bb.hRgnBlur = nullptr;
    DwmEnableBlurBehindWindow(hwnd, &bb);

    // 旧版未文档化 API 兜底（Win10 早期版本）
    struct ACCENTPOLICY {
        int nAccentState;
        int nFlags;
        int nColor;
        int nAnimationId;
    } policy{3, 0, 0, 0};

    struct WINCOMPATTRDATA {
        int  nAttribute;   // 19 = WCA_ACCENT_POLICY
        void* pData;
        ULONG ulDataSize;
    } data{19, &policy, sizeof(policy)};

    using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
    if (HMODULE hUser = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetWindowCompositionAttribute_t>(
            GetProcAddress(hUser, "SetWindowCompositionAttribute"));
        if (fn)
            fn(hwnd, &data);
    }
#endif
}

static void paintNoise(QPainter& p, int ww, int hh)
{
    // 细腻噪点纹理：低透明度白点，营造磨砂颗粒质感
    QPixmap noise(ww, hh);
    noise.fill(Qt::transparent);
    QPainter np(&noise);
    np.setPen(Qt::NoPen);
    for (int i = 0; i < (ww * hh) / 180; ++i) {
        int x = QRandomGenerator::global()->bounded(ww);
        int y = QRandomGenerator::global()->bounded(hh);
        int a = QRandomGenerator::global()->bounded(10, 30);
        np.setBrush(QColor(255, 255, 255, a));
        np.drawEllipse(x, y, 1, 1);
    }
    np.end();
    p.drawPixmap(0, 0, noise);
}

void paintFrostGradient(QWidget* w)
{
    if (!w)
        return;

    QPainter p(w);
    p.setRenderHint(QPainter::Antialiasing);
    const int ww = w->width();
    const int hh = w->height();
    if (ww <= 0 || hh <= 0)
        return;

    // 底层柔雾粉蓝线性渐变（冷调，高级不甜腻）
    QLinearGradient base(0, 0, ww, hh);
    base.setColorAt(0.0, QColor(252, 244, 252));
    base.setColorAt(0.45, QColor(240, 245, 252));
    base.setColorAt(1.0, QColor(250, 242, 251));
    p.fillRect(0, 0, ww, hh, base);

    // 多层径向粉蓝渐变叠加（虚化朦胧）
    auto radial = [&](qreal cx, qreal cy, qreal r, const QColor& c, int alpha) {
        QRadialGradient g(cx, cy, r);
        QColor col = c;
        col.setAlpha(alpha);
        g.setColorAt(0, col);
        g.setColorAt(1, Qt::transparent);
        p.fillRect(0, 0, ww, hh, g);
    };
    radial(ww * 0.12, hh * 0.22, ww * 0.65, QColor(231, 163, 207), 110); // 粉
    radial(ww * 0.88, hh * 0.18, ww * 0.60, QColor(156, 193, 236), 125); // 蓝（增强）
    radial(ww * 0.72, hh * 0.82, ww * 0.65, QColor(195, 156, 230), 105); // 紫
    radial(ww * 0.25, hh * 0.88, ww * 0.55, QColor(200, 224, 246),  85); // 淡蓝

    // 柔和中心光晕
    radial(ww * 0.50, hh * 0.45, ww * 0.75, QColor(255, 255, 255), 75);

    // 细腻噪点
    paintNoise(p, ww, hh);
}

void addSoftShadow(QWidget* w, const QColor& color, int blur, int yOffset)
{
    if (!w)
        return;
    auto* e = new QGraphicsDropShadowEffect(w);
    e->setBlurRadius(blur);
    e->setColor(color);
    e->setOffset(0, yOffset);
    w->setGraphicsEffect(e);
}

} // namespace Glass
