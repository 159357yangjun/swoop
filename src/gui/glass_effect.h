#pragma once
#include <QWidget>
#include <QColor>

// 玻璃拟态特效封装：Windows DWM 整窗模糊 + 柔雾粉径向渐变背景 + 柔和投影
// 用法见 main_window.cpp：构造里 enableGlassWindow(this)，paintEvent 里 paintFrostGradient(this)
namespace Glass {

// 启用整窗毛玻璃：
//   Windows  → 调用 DWM SetWindowCompositionAttribute 开启客户区背后模糊（亚克力近似）
//   其他平台 → 优雅降级为半透明背景（无系统级模糊）
void enableGlassWindow(QWidget* w);

// 在窗口（或容器）的 paintEvent 中调用，绘制柔雾粉多层径向渐变背景。
// 半透明面板（QSS 里 rgba 背景）叠在其上即呈毛玻璃片效果。
void paintFrostGradient(QWidget* w);

// 给控件添加柔和投影（玻璃悬浮感）。
// 注意：阴影沿控件 bounding 矩形绘制，圆角面板建议用细边框代替，
//       此函数更适合独立浮层 / 对话框 / 圆形头像等。
void addSoftShadow(QWidget* w,
                   const QColor& color = QColor(170, 140, 200, 80),
                   int blur = 30, int yOffset = 8);

} // namespace Glass
