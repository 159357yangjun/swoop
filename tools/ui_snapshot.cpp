/*
 * ui_snapshot.cpp — 离屏渲染 UI 截图工具（仅用于视觉走查，非产品代码）
 *
 * 用 Qt 的 offscreen 平台插件把真实窗口渲染成 PNG，便于无显示环境下评审界面。
 * 运行：QT_QPA_PLATFORM=offscreen IDM_SHOT_DIR=<dir> ui_snapshot.exe
 * 可选：IDM_THEME=dark|light 覆盖主题。
 */
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QDir>
#include <QLocalSocket>
#include <QTableWidget>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPixmap>
#include <QFile>
#include <QSettings>
#include <QMessageBox>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QTabWidget>

#include "main_window.h"
#include "new_task_dialog.h"
#include "settings_dialog.h"
#include "schedule_dialog.h"
#include "task_detail_dialog.h"
#include "download_core.h"
#include "task_list_model.h"   // 限流倒计时探针：TaskRow / stateText / throttleNoticeText
#include "settings.h"
#include "button_translator.h"
#include "app_icons.h"
#include "app_paths.h"
#include "schedule_service.h"
#include "tool_probe.h"
#include <stdio.h>
#include <QDateTime>
#include <QStatusBar>
#include <QToolButton>
#include <QToolBar>
#include <QFont>
#include <QElapsedTimer>
#include <atomic>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QNetworkProxy>
#include <QIcon>
#include <functional>
#include <QStringList>
#include <QDateTimeEdit>
#include <QMouseEvent>
#include <QStyleOptionSpinBox>
#include <QStyle>
#include <QAbstractSpinBox>
#include <QPainter>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>

/* 诊断探针（schedule / settings）用的隔离目录。
 *
 * 这些探针会预置 tasks.json 与 idm-next.ini，跑完再删掉 —— 所以它们必须落在一个
 * 「属于工具自己的」目录里。两条路径：
 *   - main() 已设 IDM_DATA_DIR（常态）→ 直接用它对，探针不需要再摆便携标记；
 *   - 没设（例如单独跑构建产物做对照）→ 退回便携模式：在 exe 目录摆 idm-next.portable，
 *     AppPaths 便会把数据与 ini 都解析到 exe 目录，与以前的行为一致。
 *
 * 同时守住一条底线：解析结果一旦等于真实用户数据目录，就拒绝运行。
 * 探针结尾会删掉 tasks.json 与 ini，万一有人把 IDM_DATA_DIR 指到真实目录上，
 * 一次走查就能把用户的队列和配置删干净 —— 这种事故必须在这里被拦住。 */
static bool probeIsolationDir(QString* outDir, bool* createdMark, QString* err) {
    const QString markPath = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/idm-next.portable");
    *createdMark = false;
    if (qEnvironmentVariableIsEmpty("IDM_DATA_DIR")) {
        QFile mk(markPath);
        if (mk.open(QIODevice::WriteOnly)) { mk.write("probe\n"); mk.close(); }
        *createdMark = true;
    }
    const QString dir = AppPaths::dataDir();
    const QString real = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!real.isEmpty() && QDir::cleanPath(dir) == QDir::cleanPath(real)) {
        *err = QStringLiteral("解析出的隔离目录就是真实数据目录（%1）；"
                              "探针结束时删除 tasks.json/ini，会伤到用户数据，故拒绝运行")
                   .arg(QDir::toNativeSeparators(dir));
        return false;
    }
    *outDir = dir;
    return true;
}

int main(int argc, char** argv) {
    const QString outDir = qEnvironmentVariable("IDM_SHOT_DIR", "C:/Users/yyyy/idm_shots");
    QDir().mkpath(outDir);

    // ── 与真实用户数据隔离 ──
    // 本工具每次运行都会建任务（截图需要任务行）、改设置（各诊断探针）、写历史。
    // 若与正式程序共用 %LOCALAPPDATA%/IDM Next 与注册表，用户自己的队列会被
    // 截图任务一点点撑大——实测真实 tasks.json 里 117 条全是 ui-shots/idm_shots
    // 目录下的假任务，且因为同一进程开了两个 MainWindow 而出现成对的重复 id。
    // 改走 IDM_DATA_DIR 后，数据/配置都落在 build/ui-shots/_data，用户目录零改动。
    // 显式设置时以调用方为准，方便需要观察真实数据的场合（默认不这么做）。
    if (qEnvironmentVariableIsEmpty("IDM_DATA_DIR")) {
        const QString isoDir = outDir + QStringLiteral("/_data");
        QDir().mkpath(isoDir);
        qputenv("IDM_DATA_DIR", isoDir.toUtf8());
    }
    printf("[iso] 数据目录 = %s\n", qUtf8Printable(QDir::toNativeSeparators(
               qEnvironmentVariable("IDM_DATA_DIR"))));

    // 必须先建 QApplication 并设置应用名，AppPaths::settings() 才能定位到与产品一致的配置位置
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("IDM Next"));
    // 与产品入口保持一致：否则截图里标准按钮是英文，走查结论会失真
    installZhCnTranslator();

    // 诊断模式：IDM_ICON_PROBE=1 时把所有自绘图标铺开成一张对照表并退出，
    // 便于肉眼检查字形是否清晰、语义是否直观、主题取色是否正确。
    if (qEnvironmentVariableIsSet("IDM_ICON_PROBE")) {
        struct Item { const char* name; AppIcons::Glyph g; };
        const QVector<Item> items = {
            {"NewTask",    AppIcons::Glyph::NewTask},
            {"Start",      AppIcons::Glyph::Start},
            {"Restart",    AppIcons::Glyph::Restart},
            {"Pause",      AppIcons::Glyph::Pause},
            {"Cancel",     AppIcons::Glyph::Cancel},
            {"Remove",     AppIcons::Glyph::Remove},
            {"Queue",      AppIcons::Glyph::Queue},
            {"Site",       AppIcons::Glyph::Site},
            {"History",    AppIcons::Glyph::History},
            {"Settings",   AppIcons::Glyph::Settings},
            {"Category",   AppIcons::Glyph::Category},
            {"Pending",    AppIcons::Glyph::Pending},
            {"Completed",  AppIcons::Glyph::Completed},
            {"Failed",     AppIcons::Glyph::Failed},
            {"Cancelled",  AppIcons::Glyph::Cancelled},
            {"Compressed", AppIcons::Glyph::Compressed},
            {"Document",   AppIcons::Glyph::Document},
            {"Music",      AppIcons::Glyph::Music},
            {"Program",    AppIcons::Glyph::Program},
            {"Video",      AppIcons::Glyph::Video},
        };
        const int cols = 4;
        const int cell = 150, rowH = 78;
        const int rows = (items.size() + cols - 1) / cols;
        QPixmap out(cell * cols + 20, rowH * rows + 60);

        for (int theme = 0; theme < 2; ++theme) {
            const bool dark = (theme == 1);
            AppIcons::setDarkTheme(dark);
            out.fill(dark ? QColor(0x1e, 0x1f, 0x22) : QColor(0xf7, 0xf8, 0xfa));
            QPainter p(&out);
            p.setPen(dark ? QColor(0xe8, 0xea, 0xed) : QColor(0x1f, 0x23, 0x29));
            QFont hf = p.font(); hf.setPointSize(11); hf.setBold(true);
            p.setFont(hf);
            p.drawText(QRect(10, 6, out.width() - 20, 22), Qt::AlignLeft,
                       dark ? QStringLiteral("暗色主题") : QStringLiteral("亮色主题"));
            p.setFont(QFont());
            for (int i = 0; i < items.size(); ++i) {
                const int cx = 10 + (i % cols) * cell;
                const int cy = 34 + (i / cols) * rowH;
                // 24px 与 16px 各画一次：小尺寸下是否糊成一团是判断字形好坏的关键
                // 注意 Qt 6.11 的 QIcon::paint 没有 3 参重载，alignment 必须显式传
                QIcon ic = AppIcons::icon(items[i].g);
                ic.paint(&p, QRect(cx, cy, 24, 24), Qt::AlignCenter, QIcon::Normal);
                ic.paint(&p, QRect(cx + 40, cy + 4, 16, 16), Qt::AlignCenter, QIcon::Normal);
                ic.paint(&p, QRect(cx + 68, cy + 4, 16, 16), Qt::AlignCenter, QIcon::Disabled);
                p.setPen(dark ? QColor(0x9a, 0xa0, 0xa6) : QColor(0x6b, 0x72, 0x80));
                p.drawText(QRect(cx, cy + 32, cell - 12, 20),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           QString::fromLatin1(items[i].name));
                p.setPen(dark ? QColor(0xe8, 0xea, 0xed) : QColor(0x1f, 0x23, 0x29));
            }
            p.end();
            out.save(outDir + QStringLiteral("/probe_icons_%1.png")
                              .arg(dark ? "dark" : "light"));
        }
        return 0;
    }


    // 诊断模式：IDM_ARROW_PROBE=1 时只跑一个「下拉箭头渲染能力」探针并退出。
    // 目的：分清 QSS 里 image:url(...) 不生效到底是 data URI 不支持还是 SVG 不支持。
    if (qEnvironmentVariableIsSet("IDM_ARROW_PROBE")) {
        const char* kSvgData =
            "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxMiIgaGVpZ2h0PSIxMiIgdmlld0JveD0iMCAwIDEyIDEyIj48cGF0aCBmaWxsPSJub25lIiBzdHJva2U9IiM0ZTU5NjkiIHN0cm9rZS13aWR0aD0iMS42IiBzdHJva2UtbGluZWNhcD0icm91bmQiIHN0cm9rZS1saW5lam9pbj0icm91bmQiIGQ9Ik0yLjUgNC41TDYgOEw5LjUgNC41Ii8+PC9zdmc+";
        // 12x12 深灰实心方块 PNG（data URI）：只验证「data URI 通道」是否可用
        const char* kPngData =
            "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAwAAAAMCAYAAABWdVznAAAAHElEQVQoz2P8z8Dwn4GKgIlhFIxsGEbOGQUjHgAAtQUB/9b6uL4AAAAASUVORK5CYII=";

        QWidget probe;
        probe.resize(560, 260);
        auto* lay = new QVBoxLayout(&probe);
        const QStringList labels = {
            QStringLiteral("A 无 image（基准）"),
            QStringLiteral("B data-URI SVG"),
            QStringLiteral("C data-URI PNG"),
            QStringLiteral("D qrc SVG 文件"),
            QStringLiteral("E qrc PNG 文件"),
        };
        const QStringList images = {
            QString(),
            QString::fromUtf8(kSvgData),
            QString::fromUtf8(kPngData),
            QStringLiteral(":/icons/chevron-down-light.svg"),
            QStringLiteral(":/icons/chevron-down-light.png"),
        };
        for (int i = 0; i < labels.size(); ++i) {
            auto* row = new QHBoxLayout();
            auto* lab = new QLabel(labels[i]);
            lab->setFixedWidth(160);
            auto* combo = new QComboBox();
            combo->addItems({ QStringLiteral("自动"), QStringLiteral("轻量") });
            QString css = QStringLiteral(
                "QComboBox{border:1px solid #d0d3d9;border-radius:6px;padding:4px 8px;"
                "background:#fff;color:#1f2329;}"
                "QComboBox::drop-down{border:none;width:22px;}");
            if (!images[i].isEmpty())
                css += QStringLiteral("QComboBox::down-arrow{image:url(%1);width:12px;height:12px;}").arg(images[i]);
            combo->setStyleSheet(css);
            row->addWidget(lab);
            row->addWidget(combo);
            row->addStretch(1);
            lay->addLayout(row);
        }
        probe.show();
        probe.grab().save(outDir + "/probe_arrow.png");
        return 0;
    }

    dlmgr_init(nullptr);
    Settings settings;
    settings.load();
    const QString origTheme = settings.theme();   // 记录原主题，截图后还原，避免污染用户设置
    const QString theme = qEnvironmentVariable("IDM_THEME");
    if (!theme.isEmpty()) {
        settings.setTheme(theme);
        settings.save();
    }
    settings.applyToEngine();

    const QString tag = theme.isEmpty() ? QStringLiteral("light") : theme;

    // 诊断：确认主题是否真正写入、可被重新读回
    {
        Settings chk;
        chk.load();
        // 探测「产品实际使用的」配置存储（AppPaths::settings），而不是默认构造的 QSettings——
        // 后者没有组织名，是无效存储，拿它做往返测试只会得到误导性的空结果。
        QSettings st = AppPaths::settings();
        st.setValue(QStringLiteral("__probe__"), QStringLiteral("42"));
        st.sync();
        QSettings st2 = AppPaths::settings();
        const QString probeVal = st2.value(QStringLiteral("__probe__")).toString();
        st2.remove(QStringLiteral("__probe__"));
        QFile dbg(outDir + "/debug.txt");
        if (dbg.open(QIODevice::WriteOnly | QIODevice::Text)) {
            dbg.write(QStringLiteral(
                "org='%1' app='%2'\nsettingsFile='%3'\nprobeRoundTrip='%4'\n"
                "origTheme=%5 setTheme=%6 reloadTheme=%7\n"
                "primaryScreenAvail=%8x%9\n")
                .arg(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName(),
                     st.fileName(), probeVal,
                     origTheme, theme, chk.theme(),
                     QString::number(QGuiApplication::primaryScreen()
                                         ? QGuiApplication::primaryScreen()->availableGeometry().width() : -1),
                     QString::number(QGuiApplication::primaryScreen()
                                         ? QGuiApplication::primaryScreen()->availableGeometry().height() : -1))
                .toUtf8());
        }
    }

    /* ── 诊断模式：IDM_SCHEDULE_PROBE=1 ──
     * 验证「定时 / 每日重复」设置持久化是否真的闭环：读 → 触发 → 递推 → 回写 → 再读。
     * 只测「写进去能读出来」是不够的，必须证明到点真的会把任务拉起来。
     * 隔离：全程只在本工具自己的目录里活动（见 probeIsolationDir），
     * 不碰用户的注册表与 %LOCALAPPDATA%；结束时把预置文件删掉。 */
    if (qEnvironmentVariableIsSet("IDM_SCHEDULE_PROBE")) {
        QString isoDir, isoErr;
        bool createdMark = false;
        if (!probeIsolationDir(&isoDir, &createdMark, &isoErr)) {
            printf("[schedule] ★%s\n", qUtf8Printable(isoErr));
            return 2;
        }
        const QString iniPath  = isoDir + QStringLiteral("/idm-next.ini");
        const QString stPath   = isoDir + QStringLiteral("/tasks.json");
        const QString markPath = QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/idm-next.portable");
        printf("[schedule] 隔离目录 = %s\n", qUtf8Printable(QDir::toNativeSeparators(isoDir)));

        // 1) 预置一条「已暂停」的任务（格式与引擎状态文件一致）
        {
            QFile f(stPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write("{\"tasks\":[\n"
                        "    {\"id\":1, \"url\":\"http://10.255.255.1:8081/sched.bin\", "
                        "\"file\":\"sched.bin\", \"dir\":\".\", \"size\":-1, \"downloaded\":0, "
                        "\"status\":2,\n     \"chunks\":[] }\n  ]\n}\n");
                f.close();
            }
        }
        // 2) 预置一条「昨天到点 + 每日重复」的定时设置
        const QDateTime past = QDateTime::currentDateTime().addDays(-1);
        {
            QSettings s(iniPath, QSettings::IniFormat);
            s.setValue(QStringLiteral("schedules"),
                       QStringLiteral("1|%1|1").arg(past.toString(Qt::ISODate)));
            s.sync();
        }

        bool loaded = false, fired = false, persisted = false;
        {
            MainWindow sw;
            sw.show();
            const QString restored = sw.statusBar()->currentMessage();
            printf("[schedule] 启动后状态栏: 「%s」\n", qUtf8Printable(restored));
            loaded = restored.contains(QStringLiteral("已恢复 1 条"));

            // 3) 触发一次到期检查。定时检查逻辑已迁到 L2 组件 ScheduleService::onTick
            //    （私有槽，经元对象系统按名调用，走真实代码路径，不用为测试开后门）。
            //    它按日递推 + 删除已到点任务 + 写回配置，并对每个到期任务 emit taskDue(id)；
            //    该信号已连到 MainWindow::onScheduledTaskDue → startTaskById，所以任务会被真正拉起。
            ScheduleService* ss = sw.findChild<ScheduleService*>();
            const bool invoked = ss && QMetaObject::invokeMethod(ss, "onTick", Qt::DirectConnection);
            /* 「被拉起」= 状态离开「已暂停」。必须轮询，不能只采一次样：
             * HEAD 探测已经从 dlmgr_start（GUI 线程）搬进监督线程，startTask 现在立刻返回，
             * 状态是几毫秒~几百毫秒后才变成「下载中」的。采一次样看到的还是 2，
             * 于是这条探针会把自己要验的那次"不卡顿"优化误判成"没触发"。
             * invoked 同时纳入必要条件：信号没派发成功就不算拉起。 */
            TaskInfo ti;
            bool gotInfo = false;
            int  seen = -1;
            for (int i = 0; i < 40; i++) {                 /* 最多等 2 秒 */
                QCoreApplication::processEvents();
                gotInfo = (dlmgr_get_task_info(1, &ti) == 0);
                if (gotInfo) { seen = ti.status; if (seen != 2) break; }
                QThread::msleep(50);
            }
            printf("[schedule] 触发检查: invoked=%d 任务1状态=%d（2=暂停，1/4=已被拉起/已尝试）\n",
                   invoked, seen);
            fired = invoked && gotInfo && seen != 2;

            // 4) 回读配置：重复任务的下次时间应被推进到未来（证明回写真的发生了）
            QSettings s2(iniPath, QSettings::IniFormat);
            const QString raw = s2.value(QStringLiteral("schedules")).toString();
            const QStringList f = raw.split(QLatin1Char('|'));
            QDateTime next;
            if (f.size() == 3)
                next = QDateTime::fromString(f.at(1), Qt::ISODate);
            printf("[schedule] 配置回读: 「%s」→ 下次 %s\n", qUtf8Printable(raw),
                   qUtf8Printable(next.isValid() ? next.toString(Qt::ISODate)
                                                 : QStringLiteral("(无效)")));
            persisted = next.isValid() && next > QDateTime::currentDateTime();
        }

        printf("%s 定时设置能随配置恢复（读）\n",        loaded    ? "[PASS]" : "[FAIL]");
        printf("%s 到点后任务确实被拉起（触发）\n",      fired     ? "[PASS]" : "[FAIL]");
        printf("%s 递推后的下次时间已回写配置（写）\n",  persisted ? "[PASS]" : "[FAIL]");

        /* ⚠️ 清理必须放在 MainWindow 析构**之后**（上面那个内层作用域的作用）。
         * 实测踩坑：直接在 sw 还活着的时候删，MainWindow 析构时引擎会再落一次盘，
         * build/tasks.json 又被写回来——看起来「清理过了」，实际留了个脏文件。 */
        const bool rmOk = QFile::remove(stPath) || !QFile::exists(stPath);
        QFile::remove(iniPath);
        if (createdMark) QFile::remove(markPath);
        printf("[schedule] 已清理隔离目录里的预置文件（不触碰真实配置） leftover_tasks_json_removed=%d\n",
               rmOk ? 1 : 0);
        return (loaded && fired && persisted && rmOk) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_SETTINGS_PROBE=1 ──
     * 验证两个「设置存了盘、但某一半代码不用它」类问题：
     *  ① 流量档位被设置对话框无条件改写成「自定义」——用户在状态栏选了「轻量」，
     *     只是进设置看了一眼主题再点确定，档位就被悄悄改了；
     *  ② 代理用户名/密码没下发到 Qt 网络栈（只喂了 C 引擎），需要认证的代理下
     *     站点抓取器必然 407。
     * 同样只在工具自己的隔离目录里活动，结束时删掉预置的 ini。 */
    if (qEnvironmentVariableIsSet("IDM_SETTINGS_PROBE")) {
        QString isoDir, isoErr;
        bool createdMark = false;
        if (!probeIsolationDir(&isoDir, &createdMark, &isoErr)) {
            printf("[settings] ★%s\n", qUtf8Printable(isoErr));
            return 2;
        }
        const QString iniPath  = isoDir + QStringLiteral("/idm-next.ini");
        const QString markPath = QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/idm-next.portable");
        printf("[settings] 隔离目录 = %s\n", qUtf8Printable(QDir::toNativeSeparators(isoDir)));

        /* 用「点确定按钮」而不是直接调 accept()：accept 是私有的，而 QDialogButtonBox
         * 的 accepted 信号接到它 —— 点按钮既是用户的真实操作，也是唯一不依赖私有访问
         * 的路径。找不到按钮就报 FAIL，不静默跳过。 */
        auto pressOk = [](SettingsDialog& dlg) -> bool {
            QDialogButtonBox* box = dlg.findChild<QDialogButtonBox*>();
            if (!box) return false;
            QPushButton* ok = box->button(QDialogButtonBox::Ok);
            if (!ok) return false;
            ok->click();
            return true;
        };

        /* ① 档位：预置「轻量(1) + 512KB/s」（两者自洽），然后原样走一遍设置对话框的
         *    保存路径（不碰限速输入框）——档位必须还是 1。 */
        { Settings s; s.setTrafficMode(1); s.setSpeedLimitKBps(512); s.save(); }
        bool okClicked = false;
        {
            Settings s; s.load();
            SettingsDialog dlg(s);
            okClicked = pressOk(dlg);
        }
        int modeAfterNoTouch = -99;
        { Settings s; s.load(); modeAfterNoTouch = s.trafficMode(); }
        printf("[settings] 未改限速直接确定后的档位: %d（期望 1=轻量，-1 表示被误标自定义）"
               "，确定键点击=%d\n", modeAfterNoTouch, okClicked ? 1 : 0);

        /* 反向对照：真的动了限速值 → 必须记成「自定义」(-1)。
         * 限速框是私有的，按后缀定位（构造里只有它带 " KB/s"），命中数必须是 1，
         * 否则说明定位方式失效，本项按 SKIP 处理而不是假装通过。 */
        bool changedMarked = false, spinFound = false;
        {
            Settings s; s.load();
            SettingsDialog dlg(s);
            QList<QSpinBox*> spins = dlg.findChildren<QSpinBox*>();
            QSpinBox* limit = nullptr;
            int hits = 0;
            for (QSpinBox* sp : spins) {
                if (sp->suffix().contains(QStringLiteral("KB/s"))) { limit = sp; hits++; }
            }
            spinFound = (hits == 1 && limit);
            if (spinFound) {
                limit->setValue(900);
                pressOk(dlg);
                Settings s2; s2.load();
                changedMarked = (s2.trafficMode() == -1);
            }
        }
        printf("[settings] 真改了限速后的档位: %s\n",
               spinFound ? (changedMarked ? "已记为自定义(-1)" : "★没记成自定义")
                         : "★没定位到限速输入框（SKIP）");

        /* ③ 同名文件策略：走完整链路「设置页勾选 → Settings → 引擎 cfg.overwrite_existing」。
         *    只验 QSettings 存盘是不够的 —— 本项目反复栽在「存了盘但没人读」。所以这里
         *    在两个方向上都点一次「确定」，再直接读引擎的运行时配置。 */
        bool owFound = false, owDefaultOff = false, owOnWorks = false;
        {
            auto findOwBox = [](SettingsDialog& dlg) -> QCheckBox* {
                QCheckBox* hit = nullptr; int n = 0;
                for (QCheckBox* cb : dlg.findChildren<QCheckBox*>())
                    if (cb->text().contains(QStringLiteral("覆盖同名文件"))) { hit = cb; n++; }
                return (n == 1) ? hit : nullptr;      /* 命中不唯一 = 定位方式失效 */
            };
            /* 默认方向：不勾 → 引擎必须收到 0（自动重命名） */
            { Settings s; s.load(); SettingsDialog dlg(s);
              QCheckBox* box = findOwBox(dlg);
              owFound = (box != nullptr);
              if (owFound) {
                  printf("[settings] 同名文件复选框默认状态 = %d（期望 0=自动重命名）\n",
                         box->isChecked() ? 1 : 0);
                  pressOk(dlg);
                  owDefaultOff = (dlmgr_get_config().overwrite_existing == 0);
              } }
            /* 反向：勾上 → 引擎必须收到 1（覆盖） */
            { Settings s; s.load(); SettingsDialog dlg(s);
              QCheckBox* box = findOwBox(dlg);
              if (box) {
                  box->setChecked(true);
                  pressOk(dlg);
                  owOnWorks = (dlmgr_get_config().overwrite_existing == 1);
              } }
            /* 还原默认，别把「覆盖」留在配置里 */
            { Settings s; s.load(); s.setOverwriteExisting(false); s.save(); s.applyToEngine(); }
            printf("[settings] 同名文件下发到引擎: %s\n",
                   owFound ? (owDefaultOff && owOnWorks ? "勾选/不勾选 两个方向都对"
                                                        : "★引擎收到的值与界面不一致")
                           : "★没定位到同名文件复选框（SKIP）");
        }

        /* ② 代理凭据：存一套代理设置，建 MainWindow（构造里会 applyNetworkProxy），
         *    读 Qt 的应用级代理配置。 */
        const QNetworkProxy prevProxy = QNetworkProxy::applicationProxy();
        { Settings s; s.setProxyType(QStringLiteral("http"));
          s.setProxyHost(QStringLiteral("127.0.0.1")); s.setProxyPort(8888);
          s.setProxyUser(QStringLiteral("probeuser")); s.setProxyPass(QStringLiteral("probepass"));
          s.save(); }
        bool proxyOk = false;
        {
            MainWindow pw;
            const QNetworkProxy ap = QNetworkProxy::applicationProxy();
            printf("[settings] Qt 应用代理: type=%d host=%s port=%d user=「%s」 pass=%s\n",
                   (int)ap.type(), qUtf8Printable(ap.hostName()), (int)ap.port(),
                   qUtf8Printable(ap.user()),
                   ap.password().isEmpty() ? "(空)" : "(已设置)");
            proxyOk = (ap.type() == QNetworkProxy::HttpProxy
                       && ap.hostName() == QStringLiteral("127.0.0.1")
                       && ap.port() == 8888
                       && ap.user() == QStringLiteral("probeuser")
                       && ap.password() == QStringLiteral("probepass"));
        }
        QNetworkProxy::setApplicationProxy(prevProxy);   // 还原，别把探针的代理留在进程里

        const bool modeKept = (modeAfterNoTouch == 1);
        printf("%s 未改限速时档位保持原样（不被误标为自定义）\n", modeKept     ? "[PASS]" : "[FAIL]");
        printf("%s 真改限速时档位记为自定义\n",                changedMarked ? "[PASS]" : "[FAIL]");
        printf("%s 代理用户名密码已下发到 Qt 网络栈\n",        proxyOk       ? "[PASS]" : "[FAIL]");
        printf("%s 同名文件策略下发到引擎（默认不覆盖 + 勾选后覆盖）\n",
               (owDefaultOff && owOnWorks) ? "[PASS]" : "[FAIL]");

        QFile::remove(iniPath);
        if (createdMark) QFile::remove(markPath);
        return (modeKept && changedMarked && proxyOk && owDefaultOff && owOnWorks) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_SITEAUTH_PROBE=1 ──
     * 「设置 → 站点登录」里那条凭据的密码，在用户**什么都没改、只是点了确定**之后
     * 必须还是原密码。缺陷形态：表格第三列存的是显示掩码「••••••••」（刻意不回显明文），
     * 而 accept() 又把整张表读回去当作真值写进 Settings 并下发引擎 ——
     * 于是「打开设置→点确定」这一个动作就把所有站点密码变成 8 个圆点，
     * 用户看到的是「账号密码明明填了，站点一直 401」。
     * 四条断言两两成对：显示列必须**仍是掩码**（守住不发明文回显），
     * 存储与引擎里的密码必须**仍是真值**（守住不被掩码覆盖）。
     * 走点「确定」按钮这条路：accept() 是重写的保护成员，按钮的 accepted 信号才是
     * 用户真实入口，也顺带覆盖了 save()+applyToEngine() 的整条链。 */
    if (qEnvironmentVariableIsSet("IDM_SITEAUTH_PROBE")) {
        QString isoDir, isoErr;
        bool createdMark = false;
        if (!probeIsolationDir(&isoDir, &createdMark, &isoErr)) {
            printf("[siteauth] ★%s\n", qUtf8Printable(isoErr));
            return 2;
        }
        const QString iniPath  = isoDir + QStringLiteral("/idm-next.ini");
        const QString markPath = QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/idm-next.portable");
        const QString realPass = QStringLiteral("S3cr3t-Pass!");
        const QString realUser = QStringLiteral("dluser");
        const QString realHost = QStringLiteral("files.example.com");
        const QString mask     = QStringLiteral("••••••••");

        {   /* 预置：等同用户填完凭据并保存之后的状态 */
            SiteLogin l;
            l.url      = realHost;
            l.username = realUser;
            l.password = realPass;
            Settings s;
            s.setSiteLogins(QList<SiteLogin>() << l);
            s.save();
        }

        bool okFound = false;
        bool displayStillMasked = false;
        {
            Settings s;
            s.load();
            SettingsDialog dlg(s);
            QDialogButtonBox* box = dlg.findChild<QDialogButtonBox*>();
            QPushButton* ok = box ? box->button(QDialogButtonBox::Ok) : nullptr;
            okFound = (ok != nullptr);
            if (okFound) {
                QTableWidget* tw = nullptr;
                int hits = 0;
                for (QTableWidget* t : dlg.findChildren<QTableWidget*>()) {
                    if (t->columnCount() == 3 && t->rowCount() == 1) { tw = t; hits++; }
                }
                /* 定位方式失效（0 个或不止 1 个）时按 FAIL 处理，不静默跳过 */
                displayStillMasked = (hits == 1 && tw && tw->item(0, 2)
                                      && tw->item(0, 2)->text() == mask
                                      && tw->item(0, 2)->text() != realPass);
                ok->click();
            }
        }

        QString hostAfter, userAfter, passAfter;
        int nAfter = -1;
        {
            Settings s;
            s.load();
            const QList<SiteLogin> ls = s.siteLogins();
            nAfter = ls.size();
            if (nAfter == 1) { hostAfter = ls[0].url; userAfter = ls[0].username; passAfter = ls[0].password; }
        }
        const bool storedKept = (nAfter == 1 && passAfter == realPass
                                 && userAfter == realUser && hostAfter == realHost);

        bool engineKept = false;
        {
            const DownloadConfig cfg = dlmgr_get_config();
            engineKept = (cfg.site_login_count == 1
                          && QString::fromUtf8(cfg.site_logins[0].pass) == realPass
                          && QString::fromUtf8(cfg.site_logins[0].user) == realUser);
        }

        /* 反方向的保险：修法是「accept() 不再从表格读回」，那么界面上点「删除」
         * 之后必须仍然真的删掉 —— 增删改都走 setSiteLogins()，m_settings 是权威值。
         * 这条要是红了，说明读回其实是删除生效的唯一途径，就不能简单删掉那段代码。 */
        bool deleteWorks = false;
        {
            Settings s;
            s.load();
            SettingsDialog dlg(s);
            QTableWidget* tw = nullptr;
            int hits = 0;
            for (QTableWidget* t : dlg.findChildren<QTableWidget*>()) {
                if (t->columnCount() == 3) { tw = t; hits++; }
            }
            QPushButton* del = nullptr;
            int delHits = 0;
            for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
                if (b->text() == QStringLiteral("删除")) { del = b; delHits++; }
            }
            if (hits == 1 && tw && tw->rowCount() == 1 && delHits == 1) {
                tw->selectRow(0);
                del->click();
                QPushButton* ok = dlg.findChild<QDialogButtonBox*>()
                                    ? dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)
                                    : nullptr;
                if (ok) ok->click();
                Settings chk;
                chk.load();
                deleteWorks = chk.siteLogins().isEmpty();
            } else {
                printf("[siteauth] 定位「删除」按钮/表格失败（表格命中 %d、按钮命中 %d），"
                       "该判据按不通过处理\n", hits, delHits);
            }
        }

        printf("[siteauth] 确定后存储里的密码=「%s」 引擎里=「%s」（真值应为「%s」）\n",
               qUtf8Printable(passAfter),
               qUtf8Printable(engineKept ? realPass
                                         : QString::fromUtf8(dlmgr_get_config().site_logins[0].pass)),
               qUtf8Printable(realPass));
        printf("%s 表格第三列仍然只显示掩码，不明文回显密码\n",
               displayStillMasked ? "[PASS]" : "[FAIL]");
        printf("%s 什么都没改、只点确定：密码/用户名/站点都原样保留\n",
               storedKept ? "[PASS]" : "[FAIL]");
        printf("%s 真密码确实下发到引擎（applyToEngine 之后）\n",
               engineKept ? "[PASS]" : "[FAIL]");
        printf("%s 找到「确定」按钮（找不到则以上结论都无意义）\n",
               okFound ? "[PASS]" : "[FAIL]");
        printf("%s 界面点「删除」+确定 之后凭据真的被删掉（不依赖从表格读回）\n",
               deleteWorks ? "[PASS]" : "[FAIL]");

        QFile::remove(iniPath);
        if (createdMark) QFile::remove(markPath);
        return (displayStillMasked && storedKept && engineKept && okFound && deleteWorks) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_NOTICE_PROBE=1 ──
     * 限流倒计时「看得见」这件事的界面侧：状态列真的换成那句中文、撤掉后回落、
     * 状态跃迁时不许残留。用独立的 TaskListModel，不建窗口、不碰真实数据目录。
     * 为什么不在这里连服务器：429 的计时窗口只有几秒，离屏快照里等它不稳定；
     * 「引擎能不能读到剩余秒数」由 engine_selftest 的 [24] 端到端负责，
     * 两边合起来才是这条特性完整的证据。 */
    if (qEnvironmentVariableIsSet("IDM_NOTICE_PROBE")) {
        TaskListModel m;
        TaskRow r{};
        r.id = 901;
        r.fileName       = QStringLiteral("notice.bin");
        r.state          = 1;
        r.statusText     = TaskListModel::stateText(1);
        r.fileSize       = 1000;
        r.downloaded     = 100;
        m.addTask(r);

        const QString c0 = m.index(0, 2).data(Qt::DisplayRole).toString();
        m.setStatusNotice(901, TaskListModel::throttleNoticeText(3, 429));
        const QString c1 = m.index(0, 2).data(Qt::DisplayRole).toString();
        m.updateStatus(901, 3);                       /* 下载中 → 已完成 */
        const QString c2 = m.index(0, 2).data(Qt::DisplayRole).toString();
        m.setStatusNotice(901, TaskListModel::throttleNoticeText(5, 503));
        m.setStatusNotice(901, TaskListModel::throttleNoticeText(0, 503));   /* 到点自撤 */
        const QString c3 = m.index(0, 2).data(Qt::DisplayRole).toString();

        const bool ok0 = c0.contains(QStringLiteral("下载中")) && !c0.contains(QStringLiteral("重试"));
        const bool ok1 = c1.contains(QStringLiteral("服务器限流，3 秒后重试"));
        const bool ok2 = c2.contains(QStringLiteral("已完成")) && !c2.contains(QStringLiteral("重试"));
        const bool ok3 = c3.contains(QStringLiteral("已完成")) && !c3.contains(QStringLiteral("过载"));
        const bool ok4 = TaskListModel::throttleNoticeText(7, 429) == QStringLiteral("服务器限流，7 秒后重试")
                      && TaskListModel::throttleNoticeText(7, 503) == QStringLiteral("服务器过载，7 秒后重试")
                      && TaskListModel::throttleNoticeText(2, 408) == QStringLiteral("服务器暂时不可用，2 秒后重试")
                      && TaskListModel::throttleNoticeText(0, 429).isEmpty();

        printf("[notice] 初始=「%s」 限流=「%s」 跃迁到已完成=「%s」 撤掉后=「%s」\n",
               qUtf8Printable(c0), qUtf8Printable(c1), qUtf8Printable(c2), qUtf8Printable(c3));
        printf("%s 没在等的时候状态列就是「下载中」，不多话\n",       ok0 ? "[PASS]" : "[FAIL]");
        printf("%s 429 等待期间显示「服务器限流，3 秒后重试」\n",     ok1 ? "[PASS]" : "[FAIL]");
        printf("%s 状态跃迁到「已完成」时倒计时被摘掉\n",             ok2 ? "[PASS]" : "[FAIL]");
        printf("%s 倒计时归零（sec=0）自动回落到状态文本\n",           ok3 ? "[PASS]" : "[FAIL]");
        printf("%s 429/503/408 三种文案与 sec<=0 空串都对\n",          ok4 ? "[PASS]" : "[FAIL]");
        return (ok0 && ok1 && ok2 && ok3 && ok4) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_IPC_PROBE=1 ──
     * 单实例交接的时序契约：命名管道只能在「事件循环已经能服务它」的那一刻才出现。
     * 以前 IpcServer::start() 在 MainWindow 构造里同步跑，于是「构造完成 ~ app.exec()」
     * 之间管道存在但没人读——第二实例连得上、写得进、等不到回话，3 秒后按失败退出 1
     * （CI「secondary IDM Next launch exited with code 1」就是这个 1）。
     * 这里不起第二个进程（那会往本机再拉一个 GUI），而是在本进程内用**另一条线程**
     * 跑 QLocalSocket 扮演那个迟到的第二实例：
     *   ① 窗口构造完、循环第一轮之前：必须**连不上**（黑洞窗口已消除）；
     *   ② 转一圈事件循环（start() 的 singleShot(0) 被触发）后：必须连得上；
     *   ③ 发一条 activate：必须拿到 success=true 的回话。
     * ⚠️ 前置：本机不能有正在运行的 idm-next GUI——那样 ① 连上的是他的实例，
     * 结论跟本进程无关，所以直接判失败并要求先关掉，而不是假装通过。 */
    if (qEnvironmentVariableIsSet("IDM_IPC_PROBE")) {
        {
            QLocalSocket guard;
            guard.connectToServer(QStringLiteral("idm-next-ipc"));
            const bool occupied = guard.waitForConnected(200);
            guard.disconnectFromServer();
            if (occupied) {
                printf("[FAIL] idm-next-ipc 已被别的 IDM Next 实例占用，请先关闭它再跑本探针\n");
                return 1;
            }
        }

        bool notListeningYet = false;
        bool listeningAfterLoop = false;
        bool gotReply = false;
        bool replySaysSuccess = false;
        {
            MainWindow w;                       /* 只把 start() 排进队列，不 listen */
            {
                QLocalSocket early;
                early.connectToServer(QStringLiteral("idm-next-ipc"));
                notListeningYet = !early.waitForConnected(300);
            }

            QCoreApplication::processEvents();   /* 第一轮：singleShot(0) → IpcServer::start() */

            /* 客户端放到**另一条线程**里：真实场景它就是另一个进程。
               同线程既当客户端又当服务端时，「字节能不能在 handleClient 进去之前推出去」
               取决于一次 processEvents() 里 notifier 的先后 —— 那是测具自己的竞态，
               不是被测代码的。实测踩到：构造函数里少跑 1.5 秒组件探测，这个竞态就翻脸
               （③④ 假红），所以不能靠"多泵一圈事件循环"糊过去。
               GUI 线程这边要继续转，否则 singleShot(0) 排队的 listen 和 newConnection
               永远不会被处理 —— 那正是本探针要验的东西。 */
            std::atomic<bool> clientFinished { false };
            std::atomic<bool> connected { false };
            std::atomic<bool> replyOk { false };
            std::atomic<bool> successOk { false };

            QThread* client = QThread::create([&] {
                QLocalSocket late;
                late.connectToServer(QStringLiteral("idm-next-ipc"));
                connected = late.waitForConnected(2000);
                if (connected) {
                    QJsonObject req;
                    req[QStringLiteral("command")] = QStringLiteral("activate");
                    late.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
                    late.flush();
                    if (late.waitForBytesWritten(2000) && late.waitForReadyRead(3000)) {
                        const QJsonObject resp = QJsonDocument::fromJson(late.readAll()).object();
                        replyOk   = !resp.isEmpty();
                        successOk = resp.value(QStringLiteral("success")).toBool(false);
                    }
                    late.disconnectFromServer();
                }
                clientFinished = true;
            });
            client->start();
            for (int i = 0; i < 300 && !clientFinished.load(); ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            client->wait(5000);
            delete client;

            listeningAfterLoop = connected.load();
            gotReply           = replyOk.load();
            replySaysSuccess   = successOk.load();

        }

        printf("%s 窗口构造完、事件循环第一轮之前没人监听（不留下「连得上但没人读」的黑洞）\n",
               notListeningYet ? "[PASS]" : "[FAIL]");
        printf("%s 事件循环转起来之后管道才存在，可被连接\n",
               listeningAfterLoop ? "[PASS]" : "[FAIL]");
        printf("%s activate 请求拿到了回话\n", gotReply ? "[PASS]" : "[FAIL]");
        printf("%s 回话里 success=true（交接成功 ⇒ 第二实例退出码 0）\n",
               replySaysSuccess ? "[PASS]" : "[FAIL]");
        return (notListeningYet && listeningAfterLoop && gotReply && replySaysSuccess) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_PROXY_PROBE=1 ──
     * 「代理」这一项在两侧（libcurl 引擎 / Qt 网络栈）必须得出同一个结论。
     * 缺陷形态：Qt 侧原来只看 type ——
     *   · 用户把代理改回「不使用代理」时函数提前 return，从不清除
     *     QNetworkProxy::setApplicationProxy() 装过的全局代理（全仓库只有那一处写它），
     *     于是「设置里关了代理，抓取还是走它」，一直残留到重启；
     *   · host/port 还没填就装一个 host="" port=0 的代理，站点抓取整条 Qt 网络被打死，
     *     而引擎按「无代理」正常直连 —— 同一个设置两种结果。
     * 这里直接调自由函数断言判据本身，不构造主窗口（那测不到判据，只测到接线）。 */
    if (qEnvironmentVariableIsSet("IDM_PROXY_PROBE")) {
        const QNetworkProxy prev = QNetworkProxy::applicationProxy();
        auto probe = [](const char* type, const char* host, int port) {
            applyQtApplicationProxy(QString::fromLatin1(type), QString::fromLatin1(host), port,
                                    QStringLiteral("u"), QStringLiteral("p"));
            return QNetworkProxy::applicationProxy();
        };
        /* 顺序有意义：先装两种可用代理，再试「关代理」，才测得出"关"是否真的清掉了 */
        const QNetworkProxy on     = probe("http",  "127.0.0.1", 8888);
        const QNetworkProxy socks  = probe("socks", "10.0.0.1",  1080);
        const QNetworkProxy off    = probe("none",  "127.0.0.1", 8888);
        const QNetworkProxy nohost = probe("http",  "",          8888);
        const QNetworkProxy zero   = probe("http",  "127.0.0.1", 0);

        const bool okOn = (on.type() == QNetworkProxy::HttpProxy
                           && on.hostName() == QStringLiteral("127.0.0.1") && on.port() == 8888
                           && on.user() == QStringLiteral("u")
                           && on.password() == QStringLiteral("p"));
        const bool okSocks = (socks.type() == QNetworkProxy::Socks5Proxy && socks.port() == 1080);
        const bool okOff   = (off.type() == QNetworkProxy::NoProxy);
        const bool okNoHost = (nohost.type() == QNetworkProxy::NoProxy);
        const bool okZero   = (zero.type() == QNetworkProxy::NoProxy);

        printf("[proxy] 关代理后 type=%d 空host后 type=%d port=0后 type=%d（NoProxy=%d）\n",
               (int)off.type(), (int)nohost.type(), (int)zero.type(),
               (int)QNetworkProxy::NoProxy);
        printf("%s HTTP 代理连凭据一起下发到 Qt 网络栈\n", okOn ? "[PASS]" : "[FAIL]");
        printf("%s SOCKS 代理走 Socks5 通道\n", okSocks ? "[PASS]" : "[FAIL]");
        printf("%s 装过代理后改回「不使用代理」：Qt 全局代理真的被清掉（不残留到重启）\n",
               okOff ? "[PASS]" : "[FAIL]");
        printf("%s 选了代理但没填服务器 → 按无代理处理（与引擎一致）\n",
               okNoHost ? "[PASS]" : "[FAIL]");
        printf("%s 选了代理但端口为 0 → 按无代理处理（与引擎 download_core.c:1593 一致）\n",
               okZero ? "[PASS]" : "[FAIL]");

        QNetworkProxy::setApplicationProxy(prev);   // 还原，别把探针的代理留在进程里
        return (okOn && okSocks && okOff && okNoHost && okZero) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_ZOOM_PROBE=1 ──
     * 「视图 → 放大/缩小/重置缩放」以前只改组件内存里的 m_zoomLevel，
     * 既不写盘也不读盘 → 用户调好的字号一重启就回到默认。
     * 这里断言的是**用户看得见的量**：全局字号点值（13+级数）与工具栏图标边长（22+2×级数），
     * 以及点一次缩放之后设置里确实换了值（新 Settings 实例从盘上读回来）。 */
    if (qEnvironmentVariableIsSet("IDM_ZOOM_PROBE")) {
        auto fontPt  = [] { return qApp->font().pointSize(); };
        auto stored  = [] { Settings s; s.load(); return s.viewZoom(); };
        auto toolbar = [](MainWindow& w, int* hits) {
            *hits = 0;
            QToolBar* found = nullptr;
            for (QToolBar* b : w.findChildren<QToolBar*>()) { found = b; (*hits)++; }
            return *hits == 1 ? found : nullptr;
        };

        { Settings s; s.setViewZoom(3); s.save(); }
        int zoomAfterRestore = -99, iconAfterRestore = -99;
        int zoomAfterOut = -99, zoomAfterReset = -99, zoomClamp = -99;
        {
            /* 只建一个 MainWindow：同一进程里建第二个会再跑一次 dlmgr_load_state，
               那是本项目踩过的「任务成对重复」坑。越界那条改用同一个实例走
               setZoomLevel()+applyZoom()——正是构造函数用的那两步。 */
            MainWindow w;
            AppearanceController* ac = w.findChild<AppearanceController*>();
            int hits = 0;
            QToolBar* bar = toolbar(w, &hits);
            if (!ac || !bar) {
                printf("[FAIL] 没找到 AppearanceController 或工具栏（命中 %d 个工具栏），"
                       "本探针其余判据无意义\n", hits);
                return 1;
            }
            zoomAfterRestore = fontPt();
            iconAfterRestore = bar->iconSize().height();

            ac->zoomOut();                     /* 3 → 2，必须同时改字号并落盘 */
            zoomAfterOut = stored();
            ac->zoomReset();                   /* → 0 */
            zoomAfterReset = stored();

            { Settings s; s.setViewZoom(99); s.save(); }
            ac->setZoomLevel(stored());        /* 越界配置必须被夹住 */
            ac->applyZoom();
            zoomClamp = fontPt();
        }
        { Settings s; s.setViewZoom(0); s.save(); }   /* 还原，别把探针字号留下 */

        const bool okRestore = (zoomAfterRestore == 16 && iconAfterRestore == 28);
        const bool okPersist = (zoomAfterOut == 2);
        const bool okReset   = (zoomAfterReset == 0);
        const bool okClamp   = (zoomClamp == 19);      /* 夹到 ZOOM_MAX=6 → 13+6 */

        printf("[zoom] 恢复后 字号=%d 图标=%d | zoomOut 后存储=%d | 重置后存储=%d | 99 越界后字号=%d\n",
               zoomAfterRestore, iconAfterRestore, zoomAfterOut, zoomAfterReset, zoomClamp);
        printf("%s 启动时按设置恢复字号与工具栏图标（存 3 → 16pt / 28px）\n",
               okRestore ? "[PASS]" : "[FAIL]");
        printf("%s 点「缩小」后新级数落盘（3→2，新实例读得到）\n",
               okPersist ? "[PASS]" : "[FAIL]");
        printf("%s 点「重置缩放」后落回 0\n", okReset ? "[PASS]" : "[FAIL]");
        printf("%s 越界的 viewZoom=99 被夹到上限，不会算出离谱字号\n",
               okClamp ? "[PASS]" : "[FAIL]");
        return (okRestore && okPersist && okReset && okClamp) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_TOOLPROBE_PROBE=1 ──
     * 判「外部工具在不在」必须真跑一次 --version：实测 yt-dlp 1.34s、
     * ffmpeg 0.08~0.47s、aria2c 0.06~0.12s，而调用点有四处（启动时的组件自愈、
     * **每次**打开新建任务对话框、任务开始前、HLS 合成前）—— 同一份 1.3 秒被反复付。
     * 这里拿系统自带的 ping/cmd 当替身：ping -n 2 有稳定的 ~1s 真实耗时，
     * 于是"第二次没有重跑子进程"能被**时间**证明，而不是靠数 spawn 次数
     * （那要造桩程序，反而测不到真路径）。
     * 另外两条守住键的正确性：参数必须进键，且每个键各自记忆（不是最后一次写覆盖）。 */
    if (qEnvironmentVariableIsSet("IDM_TOOLPROBE_PROBE")) {
        const QString sysDir = QStringLiteral("C:/Windows/System32/");
        const QString ping   = sysDir + QStringLiteral("ping.exe");
        const QString cmd    = sysDir + QStringLiteral("cmd.exe");
        const QStringList pingArgs { QStringLiteral("-n"), QStringLiteral("2"),
                                     QStringLiteral("127.0.0.1") };

        QElapsedTimer et;
        et.start();
        const bool first  = ToolProbe::available(ping, pingArgs);
        const qint64 ms1  = et.elapsed();
        et.restart();
        const bool second = ToolProbe::available(ping, pingArgs);
        const qint64 ms2  = et.elapsed();

        const bool okTrue  = (first && second);
        const bool okMemo  = (ms1 >= 700 && ms2 < 100);

        const bool exitZero = ToolProbe::available(cmd, { QStringLiteral("/c"),
                                QStringLiteral("exit"), QStringLiteral("0") });
        const bool exitThree = ToolProbe::available(cmd, { QStringLiteral("/c"),
                                 QStringLiteral("exit"), QStringLiteral("3") });
        const bool exitZeroAgain = ToolProbe::available(cmd, { QStringLiteral("/c"),
                                      QStringLiteral("exit"), QStringLiteral("0") });
        const bool okArgs = (exitZero && !exitThree && exitZeroAgain);

        const bool missing = ToolProbe::available(
            sysDir + QStringLiteral("definitely-not-here-9f3a.exe"), pingArgs);
        et.restart();
        const bool missingAgain = ToolProbe::available(
            sysDir + QStringLiteral("definitely-not-here-9f3a.exe"), pingArgs);
        const qint64 msMissing = et.elapsed();
        const bool okMissing = (!missing && !missingAgain && msMissing < 100);

        printf("[toolprobe] 首次=%lldms 二次=%lldms | 不存在的文件二次=%lldms\n",
               (long long)ms1, (long long)ms2, (long long)msMissing);
        printf("%s 探测结果正确（ping 退出码 0 → 可用）\n", okTrue ? "[PASS]" : "[FAIL]");
        printf("%s 第二次调用不再 spawn 子进程（首次≥700ms，二次<100ms）\n",
               okMemo ? "[PASS]" : "[FAIL]");
        printf("%s 参数进键：同一路径 exit 0/exit 3 各自记忆，互不串味\n",
               okArgs ? "[PASS]" : "[FAIL]");
        printf("%s 路径不存在 → false，且重复调用也不会反复 spawn\n",
               okMissing ? "[PASS]" : "[FAIL]");
        return (okTrue && okMemo && okArgs && okMissing) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_TEXT_PROBE=1 ──
     * 改了设置页三处说明文字（代理通道、扩展名作用范围、组件路径需重启），
     * 文字变长就可能被裁：QLabel 开了 wordWrap，但外层若给它的高度小于
     * heightForWidth(width)，末尾几行会直接看不见——这种回归只有量高度+看图能抓到。
     * 每个 tab 存一张 PNG 到探针目录，供肉眼复核；同时给出"没有裁切"的机器判据。 */
    if (qEnvironmentVariableIsSet("IDM_TEXT_PROBE")) {
        Settings s;
        s.load();
        SettingsDialog dlg(s);
        /* 用对话框**自己声明的最小尺寸**（settings_dialog.cpp:78 的 560x470）来量：
         * 那是文字换行最挤、最可能裁尾的一档；给个宽敞尺寸再检查等于没检查。
         * 顺带把"最小尺寸下不裁"变成契约——用户把窗口缩到最小也应当读得完整。 */
        dlg.resize(dlg.minimumWidth(), dlg.minimumHeight());
        dlg.show();
        QCoreApplication::processEvents();
        QTabWidget* tabs = dlg.findChild<QTabWidget*>();
        if (!tabs) { printf("[FAIL] 找不到设置页的 QTabWidget，无法检查文字裁切\n"); return 1; }
        int bad = 0;
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QCoreApplication::processEvents();
            QWidget* page = tabs->widget(i);
            QString pageName = tabs->tabText(i);
            for (QLabel* lab : page->findChildren<QLabel*>()) {
                if (!lab->wordWrap() || lab->text().length() < 20) continue;
                const int need = lab->heightForWidth(lab->width());
                printf("[text]   p%d 高=%d 需=%d 宽=%d 「%s」\n",
                       i + 1, lab->height(), need, lab->width(),
                       qUtf8Printable(lab->text().left(16)));
                if (lab->height() < need) {
                    printf("[FAIL] 第 %d 页「%s」里一段说明文字被裁：高 %d < 需要 %d\n",
                           i + 1, qUtf8Printable(pageName), lab->height(), need);
                    bad++;
                }
            }
            dlg.grab().save(outDir + QStringLiteral("/probe_settings_tab%1.png").arg(i));
        }
        printf("%s 设置页 %d 个 tab 的换行说明文字都没有被裁切\n",
               bad == 0 ? "[PASS]" : "[FAIL]", tabs->count());
        printf("[text] 截图已存 %s/probe_settings_tab*.png\n", qUtf8Printable(outDir));
        return bad == 0 ? 0 : 1;
    }

    /* ── 诊断模式：IDM_CONCURRENCY_PROBE=1 ──
     * 验证设置页「同时下载的任务数」是否真的封顶。
     * 做法：把上限设成 2、连接超时压到 2 秒，再加 5 个任务，然后读状态栏的
     * 「任务: N  下载中: M」——M 必须是 2 而不是 5。
     * 任务指向不可路由地址（10.255.255.1），会一直停在「下载中」，便于观察；
     * 用本机可达地址的话任务几百毫秒就失败退场，根本来不及判断有没有封顶。
     * 这项检查走的是界面自己的调度器，引擎自测覆盖不到，所以单独放这里。
     *
     * ⚠️ 分支必须放在 MainWindow 前面。之前它在 MainWindow w 之后，于是同一个进程里
     * 建了两个窗口、各自调了一次 dlmgr_load_state —— 引擎当时是「往数组里追加」，
     * 队列直接翻倍并写回了真实 tasks.json（用户目录里 61 条任务变成 117 条）。
     * 引擎侧已经改成载入前清空，但这里也不该再制造「一个进程两套窗口」。 */
    if (qEnvironmentVariableIsSet("IDM_CONCURRENCY_PROBE")) {
        QString isoDir, isoErr;
        bool createdMark = false;
        if (!probeIsolationDir(&isoDir, &createdMark, &isoErr)) {
            printf("[concurrency] ★%s\n", qUtf8Printable(isoErr));
            return 2;
        }
        /* 从空队列开始：上一轮走查留下的任务会累进来，「任务: N」越跑越大，
         * 反而看不出这一轮到底加了几个 —— 正是之前真实数据被撑大时踩的坑。
         * 这一步只在隔离目录里做（probeIsolationDir 已确认不是真实数据目录）。 */
        QFile::remove(isoDir + QStringLiteral("/tasks.json"));

        /* ⚠️ 上限是 MainWindow 构造时从 QSettings 读进它自己那份 m_settings 的，
         * 只改一个临时 Settings 对象不会影响窗口。所以必须真的保存一次再建窗口。
         * 因此这里先记下原值，结束时无论成败都还原——绝不把走查用的数值留在用户配置里。 */
        Settings probeSettings;
        const int prevMaxConc = probeSettings.maxConcurrent();
        const int prevTimeout = probeSettings.connectTimeoutSec();
        probeSettings.setMaxConcurrent(2);
        probeSettings.setConnectTimeoutSec(2);
        probeSettings.save();
        probeSettings.applyToEngine();

        MainWindow pw;
        pw.resize(1100, 700);
        pw.show();
        for (int i = 0; i < 5; ++i) {
            pw.addTaskFromUrl(QStringLiteral("http://10.255.255.1:8081/probe%1.bin").arg(i),
                              QStringLiteral("probe%1.bin").arg(i), outDir);
        }

        QString text;
        for (QLabel* l : pw.findChildren<QLabel*>()) {
            if (l->text().startsWith(QStringLiteral("任务: "))) {
                text = l->text();
                break;
            }
        }
        printf("[concurrency] 上限=2 加入 5 个任务 → 状态栏「%s」\n", qUtf8Printable(text));
        const bool capOk = text.contains(QStringLiteral("下载中: 2"));
        printf("%s 同时下载的任务数封顶生效（只跑 2 个）\n", capOk ? "[PASS]" : "[FAIL]");
        if (!capOk)
            printf("       期望「下载中: 2」；不等于 2 说明上限没约束住新任务\n");

        Settings restore;
        restore.setMaxConcurrent(prevMaxConc);
        restore.setConnectTimeoutSec(prevTimeout);
        restore.save();
        restore.applyToEngine();
        printf("[concurrency] 已还原设置：同时下载任务数=%d 连接超时=%d 秒\n",
               prevMaxConc, prevTimeout);
        if (createdMark)
            QFile::remove(QCoreApplication::applicationDirPath()
                          + QStringLiteral("/idm-next.portable"));
        return capOk ? 0 : 1;
    }

    MainWindow w;
    w.resize(1300, 800);
    w.show();

    int step = 0;
    QTimer* t = new QTimer();
    QObject::connect(t, &QTimer::timeout, [&]() {
        if (step == 0) {
            w.grab().save(outDir + "/01_main_empty_" + tag + ".png");
            // 造几个任务行以观察列表渲染（用可达 URL，避免探测长时间阻塞）
            w.addTaskFromUrl(QStringLiteral("https://curl.se/ca/cacert.pem"),
                             QStringLiteral("cacert.pem"), outDir);
            w.addTaskFromUrl(QStringLiteral("https://curl.se/windows/dl-8.22.0_1/curl-8.22.0_1-win64-mingw.zip"),
                             QStringLiteral("curl-8.22.0_1-win64-mingw.zip"), outDir);
            w.addTaskFromUrl(QStringLiteral("https://example.com/some-really-quite-long-file-name-for-layout-stress-test-0123456789.bin"),
                             QStringLiteral("一个很长的中文文件名用于观察布局是否会被撑破-abcdefghijklmnop.bin"), outDir);
            // 补足剩余文件类型：布局本身不受影响，但没有它们的话分类树与列表里的
            // 「类型图标 + 类型语义色」就只能看到两三种，走查时等于没覆盖。
            w.addTaskFromUrl(QStringLiteral("https://curl.se/ca/cacert.pem"),
                             QStringLiteral("示例文档-产品说明.pdf"), outDir);
            w.addTaskFromUrl(QStringLiteral("https://curl.se/ca/cacert.pem"),
                             QStringLiteral("示例音乐-主题曲.mp3"), outDir);
            w.addTaskFromUrl(QStringLiteral("https://curl.se/ca/cacert.pem"),
                             QStringLiteral("示例程序-安装包.exe"), outDir);
            w.addTaskFromUrl(QStringLiteral("https://curl.se/ca/cacert.pem"),
                             QStringLiteral("示例视频-演示片段.mp4"), outDir);
            step = 1;
        } else if (step == 1) {
            // 主界面（任务列表 + 左侧状态行 + 工具栏 + 状态栏）。
            // 注意：侧栏曾经是一块带速度曲线的总览卡片，需要先灌模拟样本才看得成形；
            // 卡片已撤掉，改成一行计数文字与状态栏总速度，都是真实值，不再需要任何造假数据。
            w.grab().save(outDir + "/02_main_tasks_" + tag + ".png");
            step = 2;
        } else if (step == 2) {
            NewTaskDialog nd(nullptr, outDir, 8);
            nd.resize(600, 340);
            nd.show();
            nd.grab().save(outDir + "/03_new_task_" + tag + ".png");
            nd.hide();

            // 定时下载对话框：它的提示文案直接承诺「会保存 / 程序未运行时怎么办」，
            // 属于一旦代码行为变了就必须同步复核的界面文案，故固定纳入走查。
            //
            // IDM_EXTRA_QSS 允许运行时追加一段样式表。排查「某个 2px 接缝到底是谁画的」
            // 这类问题时特别有用：改 qrc 里的 qss 要重编译，用它可以直接 A/B 对比，
            // 一次编译能试十几套写法。
            // IDM_QSS_REPLACE=1 则整体替换（留空即为「完全不用样式表」）——
            // 用来判断某个像素是 QSS 画的还是平台样式画的：不装样式表时还在，就是平台样式画的。
            if (qEnvironmentVariableIsSet("IDM_QSS_REPLACE"))
                qApp->setStyleSheet(QString::fromUtf8(qgetenv("IDM_EXTRA_QSS")));
            else if (const QByteArray extra = qgetenv("IDM_EXTRA_QSS"); !extra.isEmpty())
                qApp->setStyleSheet(qApp->styleSheet() + QString::fromUtf8(extra));
            ScheduleDialog sch;
            sch.adjustSize();
            sch.show();
            if (qEnvironmentVariableIsSet("IDM_DBG_SCHED")) {
                // 先报一下当前样式：离屏平台下拿到的样式可能跟真机不一样，
                // 凡是「样式画的像素」（不是 QSS 画的）都可能只在截图里出现——
                // 不先确认样式，就会去修一个只存在于走查工具里的假 bug。
                printf("[dbg] 当前样式 = %s（离屏平台）\n",
                       qApp->style()->objectName().toUtf8().constData());
                // 诊断：把整棵控件树打出来。
                // 教训：只 findChild<QToolButton*>() 会命中日历内部的「上/下月」导航按钮
                // （QtPrivate::QPrevNextCalButton），完全不是日历弹出按钮本身——
                // 曾据此误判「图标已挂上」。必须看几何位置才能分清谁是谁。
                QStringList pending;
                std::function<void(QWidget*, int)> dump = [&](QWidget* w, int depth) {
                    pending << QString("%1%2 obj=[%3] geo=(%4,%5 %6x%7) vis=%8")
                                   .arg(QString(depth * 2, QLatin1Char(' ')))
                                   .arg(QString::fromLatin1(w->metaObject()->className()))
                                   .arg(w->objectName())
                                   .arg(w->x()).arg(w->y())
                                   .arg(w->width()).arg(w->height())
                                   .arg(w->isVisible() ? 1 : 0);
                    const auto kids = w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
                    for (auto* c : kids)
                        dump(c, depth + 1);
                };
                dump(&sch, 0);
                for (const QString& line : pending)
                    printf("[dbg] %s\n", line.toUtf8().constData());

                // 日历弹出按钮在 Qt 6.11 里**不是 QToolButton**（QDateTimeEdit 的子控件只有
                // QLineEdit + QCalendarPopup），所以只能问样式要子控件矩形：
                // QSS 的 subcontrol-position 到底把箭头放哪、按钮热区有多大，看矩形最准。
                if (auto* dte = sch.findChild<QDateTimeEdit*>()) {
                    QStyleOptionSpinBox so;
                    so.initFrom(dte);
                    so.subControls = QStyle::SC_All;
                    so.buttonSymbols = dte->buttonSymbols();
                    // stepEnabled() 是 protected，取不到；QDateTimeEdit 开日历弹窗时步进其实无效，
                    // 这里按「上下都可用」问样式，好让基础样式把两个矩形都算出来看个明白。
                    so.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
                    so.frame = true;
                    QStyle* st = dte->style();
                    auto rect = [&](QStyle::SubControl sc) {
                        return st->subControlRect(QStyle::CC_SpinBox, &so, sc, dte);
                    };
                    const QRect up = rect(QStyle::SC_SpinBoxUp);
                    const QRect dn = rect(QStyle::SC_SpinBoxDown);
                    const QRect field = up.united(dn);
                    printf("[dbg] 字段 %dx%d  按钮区=[%d,%d %dx%d] up=[%d,%d %dx%d] down=[%d,%d %dx%d]\n",
                           dte->width(), dte->height(),
                           field.x(), field.y(), field.width(), field.height(),
                           up.x(), up.y(), up.width(), up.height(),
                           dn.x(), dn.y(), dn.width(), dn.height());

                    // 功能回归：光有箭头不算数，点热区必须真能弹出日历。
                    // （曾为给「按钮」换图标而 setButtonSymbols(NoButtons)，万一连热区一起
                    //   弄没了，就是「看着能点、点不开」的隐性 bug——必须实测。）
                    // ⚠️ QCalendarPopup 是**懒创建**的：不调 calendarWidget() 时，点击前
                    //    findChild 拿到的是空指针，必须每点一次就重新 findChild 一次。
                    //
                    // 顺着右侧竖列自上而下扫一遍，把「哪些高度真的能点开」打出来：
                    // 曾经热区是整列全高，改 QSS 后可能只剩图标那一小条——必须量清楚，
                    // 否则就是「图标在中间、只有中间能点」这种用户会觉得别扭的隐性退化。
                    QString band;
                    int firstHit = -1;
                    int lastHit = -1;
                    for (int y = 2; y < dte->height() - 2; ++y) {
                        const QPoint pt(dte->width() - 14, y);
                        QMouseEvent press(QEvent::MouseButtonPress, pt, dte->mapToGlobal(pt),
                                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(dte, &press);
                        QMouseEvent rel(QEvent::MouseButtonRelease, pt, dte->mapToGlobal(pt),
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(dte, &rel);
                        QApplication::processEvents();
                        auto* popup = dte->findChild<QWidget*>(
                            QStringLiteral("qt_datetimedit_calendar"));
                        const bool ok = popup && popup->isVisible();
                        band += ok ? QLatin1Char('#') : QLatin1Char('.');
                        if (ok) {
                            if (firstHit < 0) {
                                firstHit = y;
                                if (popup->grab().save(outDir + "/03c_calendar_popup_" + tag + ".png"))
                                    printf("[dbg] 已存日历弹窗截图\n");
                            }
                            lastHit = y;
                            popup->hide();
                            QApplication::processEvents();
                        }
                    }
                    printf("[dbg] y=2..%d 可点性: %s\n", dte->height() - 3,
                           band.toUtf8().constData());
                    if (firstHit < 0)
                        printf("[dbg] ⚠️ 右侧竖列整列都点不开日历——热区被 QSS 弄没了\n");
                    else
                        printf("[dbg] 热区 y ∈ [%d, %d]（字段高 %d）→ %s\n",
                               firstHit, lastHit, dte->height(),
                               (firstHit <= 6 && lastHit >= dte->height() - 7)
                                   ? "整列都能点，OK"
                                   : "只有中段能点（图标附近可用，但边缘点不动）");
                }
            }
            sch.grab().save(outDir + "/03b_schedule_" + tag + ".png");
            sch.hide();

            SettingsDialog sd(settings, nullptr);
            // 可选：IDM_SETTINGS_TAB=<n> 指定要截的标签页（0=常规 … 5=站点登录）。
            // 不指定就只截默认页——那样「下载/连接」等页上的文案永远走查不到。
            const QString tabEnv = qEnvironmentVariable("IDM_SETTINGS_TAB");
            int tabIdx = tabEnv.isEmpty() ? 0 : tabEnv.toInt();
            sd.selectTab(tabIdx);
            // ⚠️ 不要写死 resize(720,560)：内容多的页（如「下载」）会被强行压扁，
            // FormLayout 会把行挤到互相重叠，看起来像排版 bug，其实是截图尺寸造成的假象。
            // 先按当前页的自然尺寸，再保证不小于 720x560。
            sd.adjustSize();
            sd.resize(sd.size().expandedTo(QSize(720, 560)));
            // 可选：IDM_SETTINGS_SIZE=<W>x<H> 覆盖尺寸。用来给「内容很长的页」量真实
            // 需要的高度：压扁到 560 时行与行会重叠，只有放大到自然高度才能分清
            // 是「页面本身排版有问题」还是「窗口太小」。
            const QString sizeEnv = qEnvironmentVariable("IDM_SETTINGS_SIZE");
            if (!sizeEnv.isEmpty()) {
                const QStringList wh = sizeEnv.split(QLatin1Char('x'));
                if (wh.size() == 2)
                    sd.resize(wh[0].toInt(), wh[1].toInt());
            }
            sd.show();
            sd.grab().save(outDir + QStringLiteral("/04_settings_tab%1_%2.png")
                                       .arg(tabIdx).arg(tag));
            sd.hide();
            step = 3;
        } else if (step == 3) {
            // 标准按钮本地化走查：QMessageBox 用的是 Qt 内置 OK/Cancel 字符串，
            // 正是 windeployqt --no-translations 后最容易露出英文的地方。
            QMessageBox mb(QMessageBox::Question, QStringLiteral("确认删除"),
                           QStringLiteral("确定要删除选中的 3 个任务吗？此操作不可撤销。"),
                           QMessageBox::Ok | QMessageBox::Cancel);
            mb.show();
            mb.grab().save(outDir + "/05_msgbox_" + tag + ".png");
            mb.hide();
            step = 4;
        } else if (step == 4) {
            // 任务详情（分片/错误信息走查；mgr 参数在该对话框内仅被保存，传 nullptr 安全）
            int ids[8] = {0};
            int n = dlmgr_list(ids, 8);
            if (n > 0) {
                TaskDetailDialog td(ids[0], nullptr, nullptr);
                td.resize(720, 560);
                td.show();
                td.grab().save(outDir + "/06_task_detail_" + tag + ".png");
                td.hide();
            }
            step = 5;
        } else {
            // 还原原始主题设置，避免污染用户配置
            if (!theme.isEmpty() && origTheme != theme) {
                Settings restore;
                restore.load();
                restore.setTheme(origTheme);
                restore.save();
            }
            app.quit();
        }
    });
    t->start(1600);
    return app.exec();
}
