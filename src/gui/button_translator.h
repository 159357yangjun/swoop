#ifndef BUTTON_TRANSLATOR_H
#define BUTTON_TRANSLATOR_H

#include <QTranslator>
#include <QHash>
#include <QString>

// ── 简体中文标准按钮翻译 ─────────────────────────
// 打包时 windeployqt 使用 --no-translations，未附带 Qt 自带 qtbase_zh_CN.qm，
// 于是标准对话框按钮（OK/Cancel/Yes/No/Close…）会显示英文，与整体中文界面不一致。
// 这里用一个轻量翻译器直接映射这些内置字符串，无需额外部署 .qm 文件。
//
// 抽成公共头文件的原因：产品入口（src/main.cpp）与离屏截图工具（tools/ui_snapshot.cpp）
// 各有一个 main()，若只在产品入口安装，截图走查看到的仍是英文按钮，评审结论会失真。
//
// ⚠️ 文件名不能叫 ui_*.h：CMake 的 AUTOUIC 会把 `#include "ui_xxx.h"` 当成
//    由 xxx.ui 生成的产物，找不到 xxx.ui 就报 AutoUic error 并中断构建。
class ZhCnButtonTranslator : public QTranslator {
public:
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr, int n = -1) const override
    {
        Q_UNUSED(context); Q_UNUSED(disambiguation); Q_UNUSED(n);
        const QString s = QString::fromUtf8(sourceText ? sourceText : "");
        static const QHash<QString, QString> kMap = {
            { QStringLiteral("OK"),      QStringLiteral("确定") },
            { QStringLiteral("&OK"),     QStringLiteral("确定(&O)") },
            { QStringLiteral("Cancel"),  QStringLiteral("取消") },
            { QStringLiteral("&Cancel"), QStringLiteral("取消(&C)") },
            { QStringLiteral("Yes"),     QStringLiteral("是") },
            { QStringLiteral("&Yes"),    QStringLiteral("是(&Y)") },
            { QStringLiteral("No"),      QStringLiteral("否") },
            { QStringLiteral("&No"),     QStringLiteral("否(&N)") },
            { QStringLiteral("Close"),   QStringLiteral("关闭") },
            { QStringLiteral("&Close"),  QStringLiteral("关闭(&C)") },
            { QStringLiteral("Apply"),   QStringLiteral("应用") },
            { QStringLiteral("&Apply"),  QStringLiteral("应用(&A)") },
            { QStringLiteral("Help"),    QStringLiteral("帮助") },
            { QStringLiteral("&Help"),   QStringLiteral("帮助(&H)") },
            { QStringLiteral("Open"),    QStringLiteral("打开") },
            { QStringLiteral("Save"),    QStringLiteral("保存") },
            { QStringLiteral("Retry"),   QStringLiteral("重试") },
            { QStringLiteral("Ignore"),  QStringLiteral("忽略") },
            { QStringLiteral("Abort"),   QStringLiteral("中止") },
            { QStringLiteral("Reset"),   QStringLiteral("重置") },
        };
        return kMap.value(s, QString());   // 未命中则回退到原文
    }
};

// 安装到 QApplication（translator 以 app 为父对象，随其析构）
// 需在 QApplication 构造之后调用。
#include <QCoreApplication>
inline void installZhCnTranslator()
{
    if (!qApp) return;
    auto* tr = new ZhCnButtonTranslator();
    tr->setParent(qApp);
    qApp->installTranslator(tr);
}

#endif // UI_TRANSLATOR_H
