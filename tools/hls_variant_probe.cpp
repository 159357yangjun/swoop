// hls_variant_probe.cpp — HLS 变体选择回归探针（TDD：先红后绿）
//
// 形状与套件里其它探针一致：env 开关触发、逐条 [PASS]/[FAIL]、退码=失败数。
// 被测对象是**真实的** HlsDownloader（链 idm_app_lib），不是复制出来的解析器。
//
// 三条判别式（缺一条这扇门就是哑的）：
//   V1 不传 hlsVariant ⇒ 下到的字节 == hi 变体的分片拼接（现状语义，不许被"顺手改默认值"偷走）
//   V2 传低带宽 variant ⇒ 下到的字节 == lo 变体的分片拼接（本轮要接通的能力）
//   V3 传的 variant master 里没有 ⇒ 回退 hi，绝不失败挂掉
//
// 证人形状＝**产物字节归属**（对两条 media playlist 声明的分片逐一 GET、拼接、全等比较），
// 不是内部变量——产品改字段名/日志措辞都不会把门改成哑的；而两族分片内容互斥（seed 不同）
// ⇒ "长度相同但属于另一条变体"不可能蒙过全等比较。
#include "hls_downloader.h"
#include "idownloader.h"
#include "task_controller.h"
#include "task_list_model.h"
#include "queue_scheduler.h"
#include "settings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QUrl>

static int g_fails = 0;

static void check(bool cond, const QString& name, const QString& detail) {
    if (cond) qInfo().noquote() << "[PASS]" << name << "—" + detail;
    else { qWarning().noquote() << "[FAIL]" << name << "—" + detail; ++g_fails; }
}

// 同步 GET（事件循环驱动，探针进程无并发顾虑）
static QByteArray httpGet(QNetworkAccessManager& nam, const QUrl& url, bool* okOut) {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* r = nam.get(req);
    QByteArray out; bool ok = false;
    QEventLoop loop; QTimer tw; tw.setSingleShot(true);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&tw, &QTimer::timeout, &loop, &QEventLoop::quit);
    tw.start(20000);
    loop.exec();
    if (r->error() == QNetworkReply::NoError) { out = r->readAll(); ok = true; }
    r->deleteLater();
    if (okOut) *okOut = ok;
    return out;
}

// 按某条 media playlist 声明的分片顺序拼出"该变体应有的字节"
static QByteArray buildExpected(QNetworkAccessManager& nam, const QUrl& mediaUrl) {
    bool ok = false;
    const QByteArray m = httpGet(nam, mediaUrl, &ok);
    if (!ok) return QByteArray();
    QString text = QString::fromUtf8(m)
                       .replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QByteArray acc;
    static const QRegularExpression segRe(QStringLiteral("^[^#\\r\\n].*\\.ts\\s*$"),
                                           QRegularExpression::MultilineOption);
    auto it = segRe.globalMatch(text);
    while (it.hasNext()) {
        const QString uri = it.next().captured().trimmed();
        bool ok2 = false;
        const QByteArray d = httpGet(nam, mediaUrl.resolved(uri), &ok2);
        if (!ok2) return QByteArray();
        acc += d;
    }
    return acc;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("idm-hls-variant-probe"));

    /* ── 单例判别式面（⑥ L4）：IDM_PROBE_CASE_VARIANT=<串|__none__> + IDM_HLS_BASE_URL/OUT_DIR
     *    ⇒ 只跑一例，产物留在 OUT_DIR/master.ts 由调用方（hls_variant_logic_test.py）比对。
     *    存在的理由：整跑 V1–V3 的期望值从**固定 URI**（media_hi/media_lo）拼出，
     *    "同带宽不同 URI"的交换夹具会让三条全红、失去诊断能力；单例面允许只切一个变量的判别式。
     *    ⚠ 必须排在 V0-V3 整跑之前——它不设 IDM_HLS_VARIANT_PROBE，两闸共设时 case 优先。 ── */
    if (qEnvironmentVariableIsSet("IDM_PROBE_CASE_VARIANT")) {
        const QString base = qEnvironmentVariable("IDM_HLS_BASE_URL");
        const QString dir  = qEnvironmentVariable("IDM_HLS_OUT_DIR");
        const QString var  = qEnvironmentVariable("IDM_PROBE_CASE_VARIANT");
        const QString variant = (var == QStringLiteral("__none__")) ? QString() : var;
        if (base.isEmpty() || dir.isEmpty()) {
            qCritical().noquote() << "[FAIL] case 面需要 IDM_HLS_BASE_URL 与 IDM_HLS_OUT_DIR";
            return 9;
        }
        QDir().mkpath(dir);
        Settings settings; settings.load();
        TaskListModel model;
        QueueScheduler scheduler;
        TaskController controller;
        controller.setModel(&model);
        controller.setQueueScheduler(&scheduler);
        controller.setSettings(&settings);
        controller.setStateSink([](int, int) {}, [](int) {});
        controller.setSpeedSink([](int, qint64) {}, [](int) {});
        int id = controller.internalAddTask(base + QStringLiteral("/master.m3u8"), dir,
                                            QString(), 1, QString(), QString(), true, variant);
        bool done = false, okFlag = false;
        QObject::connect(&controller, &TaskController::mediaCompleted,
                         [&](int tid, bool success, const QString&) {
                             if (tid != id) return;
                             okFlag = success; done = true;
                         });
        QElapsedTimer t; t.start();
        while (!done && t.elapsed() < 60000)
            QCoreApplication::processEvents();
        qInfo().noquote() << (done && okFlag ? "[CASE-DONE]" : "[CASE-FAIL]")
                         << QStringLiteral("variant=%1 done=%2 ok=%3")
                                .arg(variant.isEmpty() ? QStringLiteral("(none)") : variant)
                                .arg(done).arg(okFlag);
        return (done && okFlag) ? 0 : 3;
    }

    if (!qEnvironmentVariableIsSet("IDM_HLS_VARIANT_PROBE")) {
        qInfo().noquote() << "[SKIP] IDM_HLS_VARIANT_PROBE 未设置";
        return 0;
    }


    const QString base = qEnvironmentVariable("IDM_HLS_BASE_URL");   // 例 http://127.0.0.1:8272/x36xhzz
    const QString dir  = qEnvironmentVariable("IDM_HLS_OUT_DIR");
    if (base.isEmpty() || dir.isEmpty()) {
        qCritical().noquote() << "[FAIL] 需要 IDM_HLS_BASE_URL 与 IDM_HLS_OUT_DIR";
        return 9;
    }
    QDir().mkpath(dir);
    QNetworkAccessManager nam;

    // 前置正例对照（本仓规矩：缺失/坏命令在管道里吐 0 ⇒ 期望 0 的桶给不了牙）
    bool okH = false, okL = false;
    const QByteArray expHi = buildExpected(nam, QUrl(base + QStringLiteral("/media_hi.m3u8")));
    const QByteArray expLo = buildExpected(nam, QUrl(base + QStringLiteral("/media_lo.m3u8")));
    check(!expHi.isEmpty() && !expLo.isEmpty() && expHi != expLo,
          QStringLiteral("V0 fixture sanity"),
          QStringLiteral("hi=%1 lo=%2 distinct=%3")
              .arg(expHi.size()).arg(expLo.size()).arg(expHi != expLo));
    if (expHi.isEmpty() || expLo.isEmpty()) return g_fails + 1;   // 夹具瞎了，后面读数无效

    struct Case { const char* name; QString variant; const QByteArray* expect; };
    const Case cases[] = {
        { "V1 no-variant downloads highest",   QString(),                        &expHi },
        { "V2 low variant honored",            QStringLiteral("246440"),         &expLo },
        { "V3 unknown variant falls back",     QStringLiteral("999999999"),      &expHi },
    };
    // 判别式走**生产链**：TaskController::internalAddTask(…, hlsVariant) ⇒ req.extra["hlsVariant"]
    // ⇒ HlsDownloader。探针直接 new HlsDownloader 会绕过全部待接线的线（摘掉产品注入行门都不红——
    // 第一版就是这么假绿的）。GUI 未运行 ⇒ 无队列立即开下，与扩展同一分支。
    Settings settings; settings.load();
    TaskListModel model;
    QueueScheduler scheduler;
    TaskController controller;
    controller.setModel(&model);
    controller.setQueueScheduler(&scheduler);
    controller.setSettings(&settings);
    // 状态/速度 sink 在 internalAddTask 里是**无条件调用**（task_controller.cpp:158-159），
    // MainWindow 注入的是模型槽；探针没有窗口 ⇒ 注 noop，否则 std::bad_function_call。
    controller.setStateSink([](int, int) {}, [](int) {});
    controller.setSpeedSink([](int, qint64) {}, [](int) {});
    for (const auto& c : cases) {
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        int id = controller.internalAddTask(base + QStringLiteral("/master.m3u8"), dir,
                                            QString(), 1, QString(), QString(), true, c.variant);
        bool done = false, okFlag = false;
        QObject::connect(&controller, &TaskController::mediaCompleted,
                         [&](int tid, bool success, const QString&) {
                             if (tid != id) return;
                             okFlag = success; done = true;
                         });
        QElapsedTimer t; t.start();
        while (!done && t.elapsed() < 60000)
            QCoreApplication::processEvents();
        QByteArray got;
        QFile f(QDir(dir).filePath(QStringLiteral("master.ts")));
        if (done && okFlag && f.open(QIODevice::ReadOnly)) { got = f.readAll(); f.close(); }
        if (!done || !okFlag) {
            check(false, QString::fromLatin1(c.name),
                  QStringLiteral("任务未成功完成(done=%1 ok=%2)").arg(done).arg(okFlag));
            continue;
        }
        check(got == *c.expect, QString::fromLatin1(c.name),
              QStringLiteral("产物 %1 B == 期望变体 %2 B").arg(got.size()).arg(c.expect->size()));
    }
    return g_fails;
}
