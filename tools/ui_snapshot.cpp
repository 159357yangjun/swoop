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
#include <QLocalServer>
#include <QEventLoop>
#include <QTableWidget>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPixmap>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSettings>
#include <QMessageBox>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QTabWidget>
#include <QKeyEvent>
#include <QShortcut>
#include <QPointer>
#include <QAction>
#include <QAbstractButton>
#include <QLineEdit>
#include <QTableView>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QImage>
#include <QPainter>
#include <QTextStream>
#include <QSet>
#include <algorithm>

#include "main_window.h"
#include "new_task_dialog.h"
#include "settings_dialog.h"
#include "schedule_dialog.h"
#include "task_detail_dialog.h"
#include "hash_verify_dialog.h"
#include "history_dialog.h"
#include "history_store.h"
#include <QClipboard>
#include <QCryptographicHash>
#include "../core/download_manager.h"
#include "download_core.h"
#include <QSqlDatabase>
#include "task_list_model.h"   // 限流倒计时探针：TaskRow / stateText / throttleNoticeText
#include "settings.h"
#include "button_translator.h"
#include "app_icons.h"
#include "app_paths.h"
#include "schedule_service.h"
#include "tool_probe.h"
#include "cli_forward.h"
#include "ipc_server.h"   // IDM_ACTIVATE_PROBE D 段归因：drainStat()（取尽条数 N / 取尽自身耗时）
#include "off_thread.h"    // IDM_OFFTHREAD_PROBE：自愈不再冻住 GUI 线程
#include "socket_drain.h"  // IDM_FLUSH_PROBE：回话必须推完才允许关
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>        // dup/dup2/freopen：把 stderr 临时挪走，好断言「失败时说不说得清」
#include <fcntl.h>
#include <QList>
#include <QDateTime>
#include <QDate>
#include <QStatusBar>
#include <QToolButton>
#include <QToolBar>
#include <QFont>
#include <QElapsedTimer>
#include <QDeadlineTimer>
#include <atomic>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QPoint>
#include <QUrlQuery>
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

/* 探针/截图的产物目录：默认落在**系统临时目录下的单一子目录 + 当天日期**。
 *
 * 为什么要改：这里的默认值曾经是硬编码的 `C:/Users/yyyy/idm_shots`，
 * 配合"每轮临时传一个新的 IDM_SHOT_DIR / 手动 tee 一份构建日志"，
 * 结果是把 47 个条目、191.5MB 堆在用户主目录根上（十来个截图目录 + 三十来份
 * 构建/探测日志），时间跨 09-11→09-29。清单不在这里重复，见
 * docs/probe-artifact-inventory-2026-09-29.md（那份才是逐条带引用的）。
 * 注意：这里刻意**不写**那些目录的字面名字 —— 写了就会被
 * tools/probe_artifact_inventory.py 当成"仓库引用"而标成不可删，
 * 一条注释不该变成依赖。
 * 用户工作区的根目录不是我的草稿纸。
 *
 * 两道措施：① 默认值改到 `%TEMP%/idm-next-probes/<日期>`；
 * ② 显式传参也要过守卫 —— 直接落在主目录根下的一律拒写（返回 2）。
 * 光有约定不够，因为堆积本身就是"每次换个新目录"这个习惯的产物。 */
static QString defaultProbeOutDir()
{
    return QDir::tempPath() + QStringLiteral("/idm-next-probes/")
         + QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
}

/* dir 是否正好是"主目录下的第一层"（C:/Users/yyyy/xxx）？
   再深一层（Documents/…）不算 —— 那是用户自己的组织方式。 */
static bool sitsInHomeRoot(const QString& dir)
{
    const QString home = QDir::cleanPath(QDir::homePath());
    const QString d = QDir::cleanPath(QDir().absoluteFilePath(dir));
    if (!d.startsWith(home, Qt::CaseInsensitive)) return false;
    const QString rest = d.mid(home.size());
    if (rest.size() < 2) return false;
    const QChar sep = rest[0];
    if (sep != QLatin1Char('/') && sep != QLatin1Char('\\')) return false;
    const QString tail = rest.mid(1);
    return !tail.contains(QLatin1Char('/')) && !tail.contains(QLatin1Char('\\'));
}

int main(int argc, char** argv) {
    const QString requested = qEnvironmentVariable("IDM_SHOT_DIR", defaultProbeOutDir());
    if (sitsInHomeRoot(requested)) {
        fprintf(stderr,
                "IDM_SHOT_DIR 拒绝写进用户主目录根：%s\n"
                "请改用 %%TEMP%% 下的子目录（默认 %s）或仓库内已 gitignore 的目录。\n",
                qUtf8Printable(requested), qUtf8Printable(defaultProbeOutDir()));
        return 2;
    }
    const QString outDir = requested;
    QDir().mkpath(outDir);
    printf("[out] 产物目录 = %s\n", qUtf8Printable(QDir::toNativeSeparators(outDir)));

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

    /* 探针起真 MainWindow（会调 IpcServer::start()）时，管道名带 pid 隔离：
     * 同机可能有别的实例/别的会话在听 `idm-next-ipc`，共名 ⇒ 探针连到别人家的服务上，
     * 读数整轮作废（本仓踩过的形状："假绿与假红都在这条路上"）。
     * ⚠ 只给 ACTIVATE 开：IDM_IPC_PROBE 的断言对象**就是那个字面量产品名**
     *   （「窗口构造完、第一轮之前没人监听 idm-next-ipc」），给它改名等于拆它的证人。
     *   不设此变量时产品行为完全不变。 */
    if (qEnvironmentVariableIsSet("IDM_ACTIVATE_PROBE")
        || qEnvironmentVariableIsSet("IDM_KEYNAV_PROBE")
        || qEnvironmentVariableIsSet("IDM_PATHGUARD_PROBE")) {
        /* ⑤ 追加 KEYNAV：K1 闭环会 w.show() 起真服务监听——上一轮串行跑套件时它摘掉过
         *   IDM_IPC_PROBE 的产品名 endpoint（共名互踩，§7-59 记的"已知形状"）。
         *   KEYNAV 面不读管道，改名不伤它的证人；IDM_IPC_PROBE 仍走产品字面名。 */
        qputenv("IDM_TEST_PIPE",
                QStringLiteral("idm-probe-%1").arg(QCoreApplication::applicationPid()).toUtf8());
        /* ── 变异面（IDM_ACTIVATE_MUTATE=1）──
         * B 门（预算命中数）在干净面上 overBudget 天然为 0；"没红"本身证不了它会拦。
         * 光靠 IDM_ATTR_SLEEP 手工注入也不行——那条路径依赖 MainWindow 侧的种子定时器，
         * 而种子只在产品自己的 250ms 状态栏节拍里建立，实测常常来不及 ⇒ 变异不可达
         * （我在这上面拿到过一次假绿：注入 100ms 却只烧掉一轮）。
         * 所以这里在**测具一侧**、进入 D 段窗口之前起一枚自己的占用定时器：
         * 它不碰产品代码，也不改判据，只保证「有一轮真的超预算」这件事一定发生。
         * ⚠ 这个开关必须与 canary 区分开：canary 证明"门看得见一次已知占用"，
         *   本开关证明"看见了就会红"。两面合起来才是这条门的完整证据。 */
        if (qEnvironmentVariableIsSet("IDM_ACTIVATE_MUTATE")) {
            qputenv("IDM_CANARY_MS", "60");     // 夹具同批打开：门的牙与被证的行为一起在场
        }
    }

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
    /* ⚠️ 这一行不是可有可无的样板：dlmgr_list() 不查 g_initialized、dlmgr_get_task_infos() 查，
     * 所以没 init 时 refreshFromEngine() 会拿到 n=1/m=0 ⇒ 一行都不建（实测踩过，
     * 读数见 docs/d4-sizewiring-red-*.txt 的"引擎条数=1 模型行数=0"）。 */
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

    /* ── 诊断模式：IDM_FILEICON_PROBE=1 ──
     * 文件名列的类型图标。方向是「模型只发类型键，图标在 gui 层查表」——
     * 旧写法是 TaskListModel 在 Qt::DecorationRole 里直接返回 QIcon，
     * 那是一条 model(L1) → gui(L4) 的反向依赖，被 tools/lint_layering.py 判红后按 (a) 改的。
     *
     * 五条断言各挡一种坏法：
     *  [1] 模型不再返回 QIcon（否则分层门的口径又回去了）
     *  [2] 模型报的类型键与后缀表一致（否则 zip 会画成文档图标）
     *  [3] 装了生产委托的单元格，与「把 fileTypeIcon(类型) 放进 DecorationRole」的控制表格
     *      **逐像素相同** —— 控制表格复刻的正是改前那条通路，所以这条相等就是
     *      「用户看到的图标没变」的可复跑判据（不依赖 PNG 金样，换 Qt 版本也不会假红）
     *  [4] 装了委托 ≠ 没装：同一模型不装委托时该格画出的东西必须不同（委托真的在出力）
     *  [5] 控制格本身不是空的：ctrl 与 plain 必须不同 —— 否则 [3] 会变成「两边都空白」的假绿
     */
    if (qEnvironmentVariableIsSet("IDM_FILEICON_PROBE")) {
        struct Case { const char* file; FileType want; };
        const Case cases[] = {
            { "a.zip",                   FileType::Compressed },
            { "示例文档-产品说明.pdf",   FileType::Document },
            { "示例音乐-主题曲.mp3",     FileType::Music },
            { "示例程序-安装包.exe",     FileType::Program },
            { "示例视频-演示片段.mp4",   FileType::Video },
            { "README",                  FileType::Other },   // 无扩展名
            { "archive.qqq",             FileType::Other },   // 未识别后缀
        };
        const int n = int(sizeof(cases) / sizeof(cases[0]));

        TaskListModel model;
        bool decoEmpty = true, keyOk = true;
        for (int i = 0; i < n; ++i) {
            TaskRow r{};
            r.id         = 700 + i;
            r.fileName   = QString::fromUtf8(cases[i].file);
            r.state      = 1;
            r.statusText = TaskListModel::stateText(1);
            model.addTask(r);
            const QModelIndex idx = model.index(i, 0);
            if (idx.data(Qt::DecorationRole).isValid())
                decoEmpty = false;
            const QVariant key = idx.data(TaskListModel::FileTypeRole);
            if (!key.isValid() || static_cast<FileType>(key.toInt()) != cases[i].want) {
                keyOk = false;
                printf("[fileicon] 键不符：%s 得到 %d，期望 %d\n",
                       cases[i].file, key.toInt(), int(cases[i].want));
            }
        }

        // 两张表：test = 生产委托；ctrl = 标准模型 + DecorationRole（= 改前的通路）
        QStandardItemModel ctrlModel(n, 1);
        for (int i = 0; i < n; ++i) {
            auto* item = new QStandardItem(QString::fromUtf8(cases[i].file));
            item->setData(AppIcons::fileTypeIcon(cases[i].want), Qt::DecorationRole);
            ctrlModel.setItem(i, 0, item);
        }

        auto makeView = [](QAbstractItemModel* m) {
            QTableView* v = new QTableView;
            v->setModel(m);
            v->horizontalHeader()->setVisible(false);
            v->verticalHeader()->setVisible(false);
            v->setColumnWidth(0, 300);
            v->setTextElideMode(Qt::ElideMiddle);
            v->setWordWrap(false);
            v->setAlternatingRowColors(true);
            v->setSelectionBehavior(QAbstractItemView::SelectRows);
            v->resize(320, 24 * 12);
            v->show();
            for (int i = 0; i < 50 && v->visualRect(v->model()->index(0, 0)).height() <= 0; ++i)
                QCoreApplication::processEvents();
            return v;
        };
        QTableView* testView = makeView(&model);
        testView->setItemDelegateForColumn(0,
            new AppIcons::FileTypeIconDelegate(TaskListModel::FileTypeRole, testView));
        QTableView* ctrlView  = makeView(&ctrlModel);
        QTableView* plainView = makeView(&model);   // 同一个模型，不装委托

        // 取每行「文件名列左端 40px」的像素：图标就在这一段
        auto strip = [](QTableView* v, int row) {
            const QModelIndex idx = v->model()->index(row, 0);
            const QRect cell = v->visualRect(idx);
            QImage img(v->size(), QImage::Format_ARGB32);
            img.fill(Qt::transparent);
            { QPainter p(&img); v->render(&p); }
            return img.copy(cell.left(), cell.top(), qMin(40, cell.width()), cell.height());
        };

        auto sameImage = [](const QImage& a, const QImage& b) {
            if (a.size() != b.size()) return false;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x)
                    if (a.pixel(x, y) != b.pixel(x, y)) return false;
            return true;
        };

        bool sameAsCtrl = true, delegateVisible = false, ctrlVisible = false;
        int  ctrlInk = 0, plainInk = 0;
        auto ink = [](const QImage& im) {
            int c = 0;
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x)
                    if (qGray(im.pixel(x, y)) < 200) ++c;
            return c;
        };
        for (int i = 0; i < n; ++i) {
            const QImage t = strip(testView, i), c = strip(ctrlView, i), pl = strip(plainView, i);
            if (sameImage(t, c) == false) sameAsCtrl = false;
            if (!sameImage(t, pl)) delegateVisible = true;
            if (!sameImage(c, pl)) ctrlVisible = true;
            ctrlInk  += ink(c);
            plainInk += ink(pl);
        }
        const int h0 = testView->visualRect(model.index(0, 0)).height();

        /* [6] 枚举 FileType 的每一个取值，逐个断言「有图标、画得出墨、且彼此不同」。
         * 为什么必须逐个点名：改成 model 发 enum + gui 查表之后，switch 少一个 case
         * 不会报错、也不会红任何已有断言——它会静默落到 default，用户看到的是
         * 「那一类文件就是没图标/和别的类型长一个样」，比原来的越界难发现得多。
         * [7] FileType::All 是分类筛选器的「全部」档，不是一个文件的类型：
         * 断言 detectFileType 对任何输入都不返回它。若真返回了，default 会画成
         * OtherFile（用户看到「未知类型」图标而不是空白）——所以 All 不参与 [6] 的
         * 互不相同比对，理由就是这句：它本来不该出现在行上。 */
        const FileType realTypes[] = { FileType::Compressed, FileType::Document,
                                       FileType::Music, FileType::Program,
                                       FileType::Video, FileType::Other };
        const char* typeNames[] = { "Compressed", "Document", "Music",
                                    "Program", "Video", "Other" };
        const int nt = int(sizeof(realTypes) / sizeof(realTypes[0]));
        QImage typeImg[8];
        bool everyTypeHasIcon = true, typesDistinct = true;
        for (int i = 0; i < nt; ++i) {
            const QIcon ic = AppIcons::fileTypeIcon(realTypes[i]);
            const QImage im = ic.pixmap(16, 16).toImage();
            typeImg[i] = im;
            const int px = ink(im);
            if (ic.isNull() || im.isNull() || px == 0) {
                everyTypeHasIcon = false;
                printf("[fileicon] 类型 %s 拿不到图标（isNull=%d 墨点=%d）\n",
                       typeNames[i], int(ic.isNull()), px);
            }
        }
        for (int i = 0; i < nt; ++i)
            for (int j = i + 1; j < nt; ++j)
                if (sameImage(typeImg[i], typeImg[j])) {
                    typesDistinct = false;
                    printf("[fileicon] 类型 %s 与 %s 画出来一模一样（switch 漏了 case？）\n",
                           typeNames[i], typeNames[j]);
                }
        bool allNeverProduced = true;
        const char* probeNames[] = { "", "README", "a.zip", "a.pdf", "a.mp3",
                                     "a.exe", "a.mp4", "a.qqq", "tar.gz", "a." };
        for (const char* pn : probeNames)
            if (CategoryFilterProxy::detectFileType(QString::fromUtf8(pn)) == FileType::All) {
                allNeverProduced = false;
                printf("[fileicon] detectFileType(\"%s\") 返回了 All（筛选档位不该出现在行上）\n", pn);
            }

        printf("[fileicon] 行数=%d 行高=%d 列0宽=%d | ctrl 图标区墨点=%d plain=%d | 类型枚举 %d 个\n",
               n, h0, testView->columnWidth(0), ctrlInk, plainInk, nt);
        printf("%s 模型不再返回 QIcon（图标改由 gui 层查表）\n", decoEmpty ? "[PASS]" : "[FAIL]");
        printf("%s 7 个样本的类型键都对（含无扩展名与未识别后缀）\n", keyOk ? "[PASS]" : "[FAIL]");
        printf("%s 装了生产委托的单元格与改前通路逐像素相同\n", sameAsCtrl ? "[PASS]" : "[FAIL]");
        printf("%s 委托确实在出力（不装委托时同一格不一样）\n", delegateVisible ? "[PASS]" : "[FAIL]");
        printf("%s 对照组本身非空（防「两边都空白」的假绿）\n", ctrlVisible ? "[PASS]" : "[FAIL]");
        printf("%s FileType 六个取值每个都画得出图标（非空且有墨）\n", everyTypeHasIcon ? "[PASS]" : "[FAIL]");
        printf("%s 六个取值的图标两两不同（漏 case 会撞成同一个）\n", typesDistinct ? "[PASS]" : "[FAIL]");
        printf("%s detectFileType 永不返回 All（10 种输入，含空串与「a.」）\n", allNeverProduced ? "[PASS]" : "[FAIL]");
        /* 第 9 条：应用级图标（任务栏 + 托盘兜底的唯一真源）。
         * 由来是 2026-10-01 冒烟里 Qt 打的那句「QSystemTrayIcon::setVisible: No Icon set」：
         * 改之前 src/main.cpp 从没调 QApplication::setWindowIcon，而
         * src/app/tray_controller.cpp:25-26 的回退恰好就是 QApplication::windowIcon()
         * ⇒ Windows 无图标主题 + 回退也是空 ⇒ 两个候选全空。
         * 这条只问一件事：那个资源**加载得到、画得出墨**。名字写错、qrc 漏登记、
         * svg 图标引擎没部署，都会在这里红——而不是等用户去盯托盘。 */
        const QIcon appIcon(QStringLiteral(":/icons/app.svg"));
        const QPixmap appPix = appIcon.pixmap(16, 16);
        const int appInk = ink(appPix.toImage());
        const bool appIconOk = !appIcon.isNull() && !appPix.isNull() && appInk > 0;
        printf("[fileicon] 应用图标 :/icons/app.svg isNull=%d naturalSize=%dx%d 16px 墨点=%d\n",
               int(appIcon.isNull()), appIcon.actualSize(QSize(64, 64)).width(),
               appIcon.actualSize(QSize(64, 64)).height(), appInk);
        printf("%s 应用级窗口图标非空且 16px 画得出墨（任务栏与托盘的唯一真源）\n",
               appIconOk ? "[PASS]" : "[FAIL]");
        delete testView; delete ctrlView; delete plainView;
        return (decoEmpty && keyOk && sameAsCtrl && delegateVisible && ctrlVisible
                && everyTypeHasIcon && typesDistinct && allNeverProduced && appIconOk) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_SIZECOL_PROBE=1 ──
     * 「大小」列能不能被回填。由来：2026-10-01 真实 GUI 里一条**已成功下完 1MiB** 的任务，
     * 大小列显示 "-"。追下去是两件事叠在一起：
     *   ① 建行那一刻探测常常还没回 Content-Length（引擎给 -1），而模型**没有任何接口**能改 row.fileSize；
     *   ② 就算改了，`updateProgress` 的 dataChanged 只覆盖第 2..5 列 ⇒ 第 1 列连重绘机会都没有。
     * 五条断言各挡一种坏法：占位符没出现 / 回填不生效 / 生效但不发重绘 / 差量没做(4Hz 风暴) / 百分比不跟。
     * progressPct 显式设 -1：模型优先用后端上报的百分比，只有它 <0 才按字节算——不设就会把第 5 条测成假绿。 */
    if (qEnvironmentVariableIsSet("IDM_SIZECOL_PROBE")) {
        TaskListModel m;
        TaskRow r{};
        r.id = 91; r.fileName = QStringLiteral("small.bin");
        r.fileSize = -1; r.downloaded = 0; r.progressPct = -1;
        r.state = 1; r.statusText = TaskListModel::stateText(1); r.speedBps = 0;
        m.addTask(r);
        const QString before = m.index(0, 1).data(Qt::DisplayRole).toString();

        int paints = 0, firstCol = -1, lastCol = -1;
        QObject::connect(&m, &QAbstractItemModel::dataChanged,
                         [&](const QModelIndex& a, const QModelIndex& b, const QVector<int>&) {
                             ++paints; firstCol = a.column(); lastCol = b.column();
                         });
        m.setFileSize(91, 1048576);
        const QString after = m.index(0, 1).data(Qt::DisplayRole).toString();
        const int paintsJudged = paints;      // 判据用的就是这一刻的计数，打印必须印它
        const bool painted = (paintsJudged == 1) && firstCol <= 1 && lastCol >= 1;

        m.setFileSize(91, 1048576);              // 同值再来一次：不该再发重绘
        const bool quietOnSame = (paints == 1);

        TaskRow r2{};
        r2.id = 92; r2.fileName = QStringLiteral("half.bin");
        r2.fileSize = -1; r2.downloaded = 524288; r2.progressPct = -1;
        r2.state = 1; r2.statusText = TaskListModel::stateText(1); r2.speedBps = 0;
        m.addTask(r2);
        const QString stBefore = m.index(1, 2).data(Qt::DisplayRole).toString();
        m.setFileSize(92, 1048576);
        const QString stAfter = m.index(1, 2).data(Qt::DisplayRole).toString();
        const bool pctFollows = !stBefore.contains(QLatin1Char('%')) && stAfter.contains(QStringLiteral("50%"));

        printf("%s 建行时大小未知(-1)，第 1 列显示占位符（实测「%s」）\n",
               before == QStringLiteral("-") ? "[PASS]" : "[FAIL]", before.toUtf8().constData());
        printf("%s setFileSize 之后第 1 列变成「1.0 MB」（实测「%s」）\n",
               after == QStringLiteral("1.0 MB") ? "[PASS]" : "[FAIL]", after.toUtf8().constData());
        printf("%s 回填确实发了 dataChanged 且覆盖第 1 列（判定时刻次数=%d 列 %d..%d）\n",
               painted ? "[PASS]" : "[FAIL]", paintsJudged, firstCol, lastCol);
        printf("%s 同值再回填不发重绘（4Hz 循环里不造成重绘风暴）\n",
               quietOnSame ? "[PASS]" : "[FAIL]");
        printf("%s 状态列百分比跟着回填出现（前「%s」后「%s」）\n",
               pctFollows ? "[PASS]" : "[FAIL]", stBefore.toUtf8().constData(), stAfter.toUtf8().constData());
        return (before == QStringLiteral("-") && after == QStringLiteral("1.0 MB")
                && painted && quietOnSame && pctFollows) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_SIZEWIRING_PROBE=1 ──
     * D4：引擎知道大小 ≠ 界面显示得出大小。由来是 2026-10-01 的产品实拍（§7 第 25 条）：
     * 同一条 /cut.bin，引擎侧自测 [36①] 给的是 file_size=200000，界面上「大小」列仍是 "-"、
     * history.db 里仍是 -1。断点在 GUI 接线：
     *   ① onTaskProgress 收到了 total 却整个丢掉；
     *   ② refreshFromEngine() 没挂在任何定时器上（全仓只有 loadState/onStartAll/onPauseAll 三处），
     *      所以"每 tick 回填"从来没发生过；
     *   ③ 完成时 recordFinished 读的是模型里的旧值 ⇒ 落库的就是 -1。
     * ⚠️ 必须走 MainWindow 的真实槽（私有槽经元对象系统按名调用，先例见本文件 ScheduleService::onTick），
     *    IDM_SIZECOL_PROBE 那五条只测 TaskListModel 自己——模型有接口而没人调它，正是这次的缺陷形状。
     * 夹具用 dlmgr_add + dlmgr_load_state 造行（不 start ⇒ 零网络、零线程），任务 id 由引擎分配后回读。 */
    if (qEnvironmentVariableIsSet("IDM_SIZEWIRING_PROBE")) {
        /* 数据目录必须**显式**指到一个本轮新建的空目录：MainWindow 会 loadState(那个目录/tasks.json)，
         * 而探针的隔离数据目录是按日期分层的、跨轮复用——实测同一条命令连跑三次得到
         * "模型行数=1/7/7"，多出来的行全是上一轮这个夹具留下的任务。那样第 1 列读的是谁家的行
         * 都不确定，判据就成了掷骰子。这里不复用任何已有目录。 */
        QString dir = QDir::tempPath() + QStringLiteral("/idm-sizewiring-probe-")
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dir);
        qputenv("IDM_DATA_DIR", dir.toUtf8());
        const QString url = QStringLiteral("http://127.0.0.1:1/size_wiring.bin");
        QFile::remove(dir + QStringLiteral("/size_wiring.bin"));
        QFile::remove(dir + QStringLiteral("/size_wiring.bin.idmtmp"));

        int tid = dlmgr_add(url.toUtf8().constData(), dir.toUtf8().constData(),
                            "size_wiring.bin", 1, nullptr, nullptr, nullptr, nullptr);
        bool okAdd = (tid >= 0);
        TaskInfo probeTi;
        memset(&probeTi, 0, sizeof(probeTi));
        int rg = dlmgr_get_task_info(tid, &probeTi);   // 夹具前提：引擎确实存着这个任务
        int engN1 = 0; { int eids[16]; engN1 = dlmgr_list(eids, 16); }
        QString st = dir + QStringLiteral("/probe_state.json");
        QFile::remove(st);
        int sv = okAdd ? dlmgr_save_state(st.toUtf8().constData()) : -1;
        long svSz = QFileInfo(st).exists() ? QFileInfo(st).size() : -1;
        int lc = (sv == 0) ? dlmgr_load_state(st.toUtf8().constData()) : -1;
        int engN2 = 0; { int eids[16]; engN2 = dlmgr_list(eids, 16); }   // load 之后引擎里有几条

        /* new 出来而不是栈对象：收尾必须"先析构窗口、再删数据目录"，
         * 而栈对象的析构点在作用域末尾——那时删除已经发生了（实测留下 history.db 等 3 个文件）。 */
        /* 故意 new 出来且不 delete：析构路径上有上面写到的段错误竞态，进程退出会统一回收。 */
        MainWindow* wPtr = new MainWindow();
        MainWindow& w = *wPtr;
        /* 引擎队列此刻就是夹具那一条（load_state 会先清空再灌）。构造里那次 singleShot(0)
         * 的 loadState 排在事件循环第一轮，本块同步跑完之前它不会插进来。 */
        auto modelOf = [&w]() { return w.findChild<TaskListModel*>(); };
        auto rowOf = [&w, &modelOf](int id) -> const TaskRow* {
            TaskListModel* m = modelOf();
            if (!m) return nullptr;
            for (const TaskRow& r : m->tasks()) if (r.id == id) return &r;
            return nullptr;
        };
        // load_state 只灌引擎队列；把它搬进模型的那一步正是 refreshFromEngine
        w.refreshFromEngine();
        bool okModel = (modelOf() != nullptr);
        const TaskRow* firstRow = okModel && modelOf()->rowCount() > 0
                                      ? &modelOf()->tasks().at(0) : nullptr;
        int rowCountAfter = okModel ? modelOf()->rowCount() : -1;
        bool okRow = rowOf(tid) != nullptr;
        bool okLoad = (sv == 0 && lc == 0 && okModel && okRow);

        int paints = 0, firstCol = -1, lastCol = -1;
        int col1Count = 0, col1Lo = -1, col1Hi = -1;
        auto countCol1 = [&](int lo, int hi) { if (lo <= 1 && hi >= 1) { ++col1Count; col1Lo = lo; col1Hi = hi; } };
        auto snapPaint = [&]() { paints = 0; firstCol = -1; lastCol = -1;
                                 col1Count = 0; col1Lo = -1; col1Hi = -1; };
        struct Paint { int n, lo, hi, c1n, c1lo, c1hi; bool hitsCol1; };
        if (TaskListModel* m = modelOf()) {
            QObject::connect(m, &QAbstractItemModel::dataChanged,
                             [&](const QModelIndex& a, const QModelIndex& b, const QVector<int>&) {
                                 ++paints; firstCol = a.column(); lastCol = b.column();
                                 countCol1(a.column(), b.column());
                             });
        }
        /* ⚠️ 采样必须"调用前清零、调用后立刻读"：Qt6 默认 DirectConnection，槽在**本线程内联**执行，
         *    两次 invokeMethod 之间攒下的计数会被下一次覆盖。第一版拿"最后一次调用的快照"当判据，
         *    读到的是 updateProgress 那一发（列 2..5），于是这条恒红——红的是测具。
         * ⚠️ 而且只能数**覆盖第 1 列**的那一发：onTaskProgress 自己还要走 updateProgress，
         *    那里合法地发 2..5；拿"有没有发 dataChanged"当判据就把一条正确的实现读成错。 */
        const QString colBefore = okLoad ? modelOf()->index(0, 1).data(Qt::DisplayRole).toString()
                                         : QStringLiteral("(无行)");
        // ① 实时进度回调带上 total（下载途中探测已经拿到长度的那条路）
        snapPaint();
        bool okProg = okLoad && QMetaObject::invokeMethod(
            &w, "onTaskProgress", Qt::DirectConnection,
            Q_ARG(int, tid), Q_ARG(qint64, 4096), Q_ARG(qint64, 200000), Q_ARG(int, 50000));
        const Paint prog{paints, firstCol, lastCol, col1Count, col1Lo, col1Hi, col1Count >= 1};
        const QString colAfterProg = okLoad ? modelOf()->index(0, 1).data(Qt::DisplayRole).toString()
                                            : QStringLiteral("(无行)");

        // ② 长度未知的任务：完成那一刻引擎才把盘上长度回填出来（D2 那条路）
        TaskInfo ti;
        memset(&ti, 0, sizeof(ti));
        bool okInfo = okLoad && dlmgr_get_task_info(tid, &ti) == 0;
        QByteArray errUtf8;
        /* 先让引擎侧的完成路径自己跑一遍（不 start ⇒ 没有线程，就地收尾）：
         * dlmgr_start 会探测失败 → 单流下载失败 → 监督线程把状态置成终态。
         * 这一步是必须的：MainWindow::onTaskCompleted 现在**从引擎现读** file_size，
         * 一个从没跑过的 PENDING 任务那里根本没有长度可读——那才是 D2 要防的那种谎。 */
        if (okLoad) {
            dlmgr_start(tid);
            for (int i = 0; i < 60; i++) {                   // 最多 6 秒等它进终态
                QCoreApplication::processEvents();
                TaskInfo t2;
                if (dlmgr_get_task_info(tid, &t2) == 0 && (t2.status == 3 || t2.status == 4)) break;
                QThread::msleep(100);
            }
        }
        snapPaint();
        bool okDone = okLoad && QMetaObject::invokeMethod(
            &w, "onTaskCompleted", Qt::DirectConnection,
            Q_ARG(int, tid), Q_ARG(bool, true), Q_ARG(QString, QString()));
        const Paint done{paints, firstCol, lastCol, col1Count, col1Lo, col1Hi, col1Count >= 1};
        const TaskRow* doneRow = rowOf(tid);
        TaskInfo engTi;                 // 完成面读到的**引擎**读数（不是测具写的）
        memset(&engTi, 0, sizeof(engTi));
        int engRc = okLoad ? dlmgr_get_task_info(tid, &engTi) : -1;
        const QString colAfterDone = doneRow ? modelOf()->index(0, 1).data(Qt::DisplayRole).toString()
                                             : QStringLiteral("(无行)");
        Q_UNUSED(okInfo); Q_UNUSED(errUtf8);

        const bool progShows = (colAfterProg == QStringLiteral("195.3 KB"));
        /* ⚠️ 完成面**不数 dataChanged 发数**：到这一步第 1 列已经是 195.3 KB（上一面刚回填过），
         *    setFileSize 的"同值不发重绘"守卫就会吞掉这一发——那是上一面的功劳，不是这一面的缺陷。
         *    第一版把它写成 `doneShows && donePainted` 时，这条恒红且红得没有意义。
         *    两面对调顺序也测不出差别 ⇒ 这一面只认 DisplayRole 与模型字段这两个独立证人。 */
        const bool doneShows = (colAfterDone == QStringLiteral("195.3 KB"))
                               && doneRow && doneRow->fileSize == 200000;
        const bool progPainted = prog.hitsCol1;
        Q_UNUSED(done);

        printf("%s 前置：行能进模型（add=%d tid=%d 引擎查得=%d add后条数=%d save=%d state文件字节=%ld "
               "load=%d load后条数=%d 找到模型=%d 模型行数=%d 首行id=%d 找到行=%d 建行前第1列=「%s」）\n",
               okLoad ? "[PASS]" : "[FAIL]", okAdd ? 1 : 0, tid, rg, engN1, sv, svSz, lc, engN2,
               okModel ? 1 : 0, rowCountAfter,
               firstRow ? firstRow->id : -1, okRow ? 1 : 0, colBefore.toUtf8().constData());
        printf("%s onTaskProgress 带 total=200000 之后第 1 列变成 195.3 KB（实测「%s」invoked=%d）\n",
               (okProg && progShows) ? "[PASS]" : "[FAIL]", colAfterProg.toUtf8().constData(),
               okProg ? 1 : 0);
        printf("%s 进度回调那次调用内发了覆盖第 1 列的 dataChanged（该发数=%d 列 %d..%d；同窗口总发数=%d）\n",
               progPainted ? "[PASS]" : "[FAIL]", prog.c1n, prog.c1lo, prog.c1hi, prog.n);
        printf("%s 长度未知的任务完成那一刻，第 1 列也必须出现大小（实测「%s」invoked=%d 模型值=%lld；"
               "该次调用内覆盖第1列发数=%d 列 %d..%d——同值不发重绘，只作对照不作判据）\n",
               (okDone && doneShows) ? "[PASS]" : "[FAIL]", colAfterDone.toUtf8().constData(),
               okDone ? 1 : 0, doneRow ? (long long)doneRow->fileSize : -999, done.c1n, done.c1lo, done.c1hi);
        printf("    [D4 引擎读数] 完成面取回=%d status=%d file_size=%lld downloaded=%lld err=「%s」\n",
               engRc, engTi.status, (long long)engTi.file_size, (long long)engTi.downloaded,
               engTi.error_msg);

        /* 收尾：**只摘任务，不删窗口、不删目录**。三条都是实测换来的：
         *   · dlmgr_remove 负责删自己的 .idmtmp；再转一圈事件循环给监督线程收摊的机会
         *     （上一版没等，5 轮里 1 轮把 .idmtmp 留在盘上）。
         *   · delete &w 会触发 MainWindow 析构里的 saveState + HistoryStore 那条已被 removeDatabase
         *     摘掉的命名连接 ⇒ 实测 5 轮 1 轮直接段错误 rc=139。探针跑完就退进程，不必自己拆。
         *   · 因此数据目录**故意留在盘上**，路径与内容印出来：静默吞掉残留比不清理更坏，
         *     但把它做成判据又会被上面那个竞态闪红——所以进读数行、不进退码。 */
        dlmgr_remove(tid);
        for (int i = 0; i < 20; i++) {                     // 最多 2 秒：等 .idmtmp 真的从盘上消失
            QCoreApplication::processEvents();
            if (!QFile::exists(dir + QStringLiteral("/size_wiring.bin.idmtmp"))) break;
            QThread::msleep(100);
        }
        const QStringList left = QDir(dir).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        /* 逐条目删而不是 removeRecursively：后者遇到一个删不掉的条目就整棵放弃（实测剩 7 个文件）。
         * history.db 要等它的命名连接被摘掉才删得动（HistoryStore 没有 close()，句柄在单例里）。 */
        auto purgeDir = [](const QString& d) {
            QDir dq(d);
            if (!dq.exists()) return true;
            const QFileInfoList entries = dq.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
            for (const QFileInfo& fi : entries) {
                if (fi.isDir()) { QDir(fi.absoluteFilePath()).removeRecursively(); }
                else if (!QFile::remove(fi.absoluteFilePath())) { QFile::remove(fi.absoluteFilePath()); }
            }
            return dq.removeRecursively() || !dq.exists();
        };
        {
            const QString hname = QStringLiteral("idm_next_history");   // 实读 history_store.cpp:19
            if (QSqlDatabase::contains(hname)) QSqlDatabase::removeDatabase(hname);
        }
        const bool purged = purgeDir(dir);
        printf("    [D4 清理读数] 删前=%s 逐条目删除后目录还在=%d\n",
               left.isEmpty() ? QStringLiteral("(空)") : left.join(QLatin1Char(',')).toUtf8().constData(),
               QDir(dir).exists() ? 1 : 0);
        Q_UNUSED(purged);     // 清干净是好事，但它是竞态敏感的（见上），所以只进读数不进退码
        return (okAdd && okLoad && okProg && progShows && progPainted
                && okDone && doneShows) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_KEYNAV_PROBE=1 ──
     * 台账里两条"发现了、没有任何断言"的键盘面缺陷（§7 登记未修）：
     *   K1 Ctrl+N 打不开新建任务对话框。工具栏 tooltip 明写「新建下载任务 (Ctrl+N)」，
     *      但快捷键挂在菜单项 QAction 上、默认 shortcutContext=WindowShortcut，而主窗口自己的
     *      focusPolicy 是 ClickFocus ⇒ 焦点不在任何子控件上时，窗口级快捷键没有受理者。
     *      判据：向焦点 widget（没有就向主窗口）发 QKeyEvent(Ctrl+N) 后必须出现 NewTaskDialog。
     *   K2 Tab 序把「浏览...」排在「文件名」之前，且它吃焦点、回车/空格都会激活 ⇒
     *      从「下载链接」按两次 Tab 再 Enter，等着提交的人被模态目录选择器截住。
     *      判据：第二次 Tab 后 focusWidget() 必须是目录输入框。
     * ⚠ 刻意**不链 QtTest**（本机 -lQt6Test 实测能连，但那是为了一个探针往闸门二进制里加依赖）。
     *   这里直接 sendEvent：QShortcut 是 QObject、在事件过滤路径上收事件，与真按键同一条路。
     * ⚠ 两面都是**先红**：修法落地前它们必须一直红。 */
    if (qEnvironmentVariableIsSet("IDM_KEYNAV_PROBE")) {
        QString dir = QDir::tempPath() + QStringLiteral("/idm-keynav-probe-")
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dir);
        qputenv("IDM_DATA_DIR", dir.toUtf8());

        MainWindow w;
        auto pressKey = [](QWidget* target, int key, Qt::KeyboardModifiers mod) {
            const QString text = QChar(char(key));
            QKeyEvent p(QEvent::KeyPress, key, mod, text);
            QKeyEvent r(QEvent::KeyRelease, key, mod, text);
            const bool a = QApplication::sendEvent(target, &p);
            QApplication::sendEvent(target, &r);
            QCoreApplication::processEvents();
            return a;
        };

        // ── K1：Ctrl+N ──
        /* ⚠ 量具自检（canary）历史，两次反转都记下来，免得第三次翻车：
         *   第 1 版结论"四种发法全 0 ⇒ offscreen 到不了快捷键系统"——**被独立小程序实测驳回**
         *   （同 Qt 6.11.1/offscreen、裸 QWidget+show()+同键 QShortcut：sendEvent/postEvent 全部命中）。
         *   差别在探针自己：MainWindow **从不 show()**（§7-38：真显示会污染别的读数），
         *   隐藏窗口的 focusWidget() 恒空 ⇒ 键投给了没激活的隐藏件。⇒ 那是**量具用法错**，不是平台限制。
         *   本轮修法：canary 起一个 show() 过的普通窗口走一遍，证明发法本身能到达快捷键系统；
         *   产品面另用第二证人闭环（见下）。 */
        int canaryHits = 0;
        {
            QWidget cw;                                // 已知正例：可见、可聚焦、同键同 context
            cw.setFocusPolicy(Qt::StrongFocus);
            cw.resize(240, 120);
            cw.show();
            QCoreApplication::processEvents();
            auto* cs = new QShortcut(QKeySequence(QStringLiteral("Ctrl+N")), &cw,
                                     nullptr, nullptr, Qt::WindowShortcut);
            QObject::connect(cs, &QShortcut::activated, [&canaryHits]() { ++canaryHits; });
            const QString note = QStringLiteral("n");
            QKeyEvent a(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier, note);
            cw.activateWindow();
            cw.setFocus(Qt::OtherFocusReason);
            QCoreApplication::processEvents();
            QApplication::sendEvent(&cw, &a);          // F1 sendEvent 到可见焦点件
            QCoreApplication::processEvents();
            const int h1 = canaryHits;
            QApplication::postEvent(&cw, new QKeyEvent(QEvent::KeyPress, Qt::Key_N,
                                                       Qt::ControlModifier, note));  // F2 postEvent
            QCoreApplication::processEvents();
            const int h2 = canaryHits;
            printf("    [K1 canary 发法普查] send(visible-focus)=%d post(visible-focus)=%d (累计 %d)\n",
                   h1, h2, canaryHits);
            delete cs;
            cw.hide();
        }

        /* K1 产品面闭环（⑤ 新增）：向**show() 过的主窗口**发 Ctrl+N，要求
         *   · NewTaskDialog 实例出现且 visible（第一证人），且
         *   · MainWindow::onNewTask 进入次数 ≥1（第二证人 IDM_NEWTASK_COUNTFILE，
         *     区分"某个弹窗出现了"与"快捷键真的触发了 handler"）。
         * ⚠ 形状沿用 K3 的实测教训：**trigger/按键必须排队进 outer.exec()**，闭环比 processEvents
         *   泵更稳；直接 sendEvent+processEvents 会把 exec 嵌在探针主流程里 ⇒ spy 关不掉它（M2 挂死同形）。
         *   所以这里用 QTimer 发键 + QTimer(5ms) 采样关闭 + 1.5s 全局兜底。
         * 环境变量未设时产品行为零变化（IDM_NEWTASK_COUNTFILE 只计数、不改控制流）。 */
        int prodChainHits = 0;
        int prodChainCnt = -1;
        {
            const QString cntFile = dir + QStringLiteral("/newtask.count");
            QFile::remove(cntFile);
            qputenv("IDM_NEWTASK_COUNTFILE", cntFile.toUtf8());
            w.show();
            QCoreApplication::processEvents();
            w.activateWindow();
            QCoreApplication::processEvents();

            struct ChainPoller {
                static int count() {
                    int n = 0;
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (qobject_cast<NewTaskDialog*>(x)) ++n;
                    return n;
                }
                static void closeOne() {
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (auto* d = qobject_cast<NewTaskDialog*>(x)) { d->reject(); return; }
                }
                static QString closeAnyModal() {
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (auto* d = qobject_cast<QDialog*>(x); d && d->isModal()) {
                            const QString nm = QString::fromLatin1(d->metaObject()->className());
                            d->reject();
                            return nm;
                        }
                    return QString();
                }
            };

            QEventLoop outer;
            QWidget* tgt = w.focusWidget() ? w.focusWidget() : static_cast<QWidget*>(&w);
            QTimer keyFire;                        // 键在循环内 fire（exec 已起来后事件派发正常）
            QObject::connect(&keyFire, &QTimer::timeout, &outer, [&, tgt]() {
                const QString note = QStringLiteral("n");
                QKeyEvent a(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier, note);
                QApplication::sendEvent(tgt, &a);
            });
            int ticks = 0;
            QTimer spy;
            QObject::connect(&spy, &QTimer::timeout, &outer, [&]() {
                ++ticks;
                if (ChainPoller::count() > 0) { prodChainHits = 1; ChainPoller::closeOne(); return; }
                if (!ChainPoller::closeAnyModal().isEmpty()) return;   // 冒名顶替者：关掉继续看
                if (prodChainHits && ChainPoller::count() == 0) outer.quit();
                if (ticks > 400) outer.quit();     // ≈2s 上限
            });
            QTimer globalGuard;                    // 挂在 qApp：exec 的内层循环也会 fire 它
            QObject::connect(&globalGuard, &QTimer::timeout, qApp, [&]() {
                if (ChainPoller::count() > 0) { prodChainHits = 1; ChainPoller::closeOne(); return; }
                if (!ChainPoller::closeAnyModal().isEmpty()) outer.quit();
            });
            spy.start(5);
            globalGuard.start(1500);
            keyFire.start(30);                     // 首跳 30ms 后按键；之后每 500ms 重试一次
            outer.exec();
            spy.stop();
            globalGuard.stop();
            keyFire.stop();
            for (int i = 0; i < 40 && ChainPoller::count() > 0; i++)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

            { QFile f(cntFile);
              if (f.open(QIODevice::ReadOnly)) prodChainCnt = QString::fromUtf8(f.readAll()).trimmed().toInt(); }
            printf("    [K1 产品链闭环] 弹窗可见=%d onNewTask 进入次数=%d(期望>=1) 采样tick=%d\n",
                   prodChainHits, prodChainCnt, ticks);
            qunsetenv("IDM_NEWTASK_COUNTFILE");
            w.hide();
            QCoreApplication::processEvents();
        }

        /* K1 的正经证人：**不合成按键，只读状态**。缺陷的形状是"声明与生效条件不一致"——
         * tooltip 明写 Ctrl+N，而那个 QAction 的 shortcutContext 停在默认 WindowShortcut，
         * 主窗口 focusPolicy=ClickFocus ⇒ 焦点不在子控件上时没人受理。
         * 判据两条，都不需要事件循环：
         *   · 全仓找得到一条 sequence==Ctrl+N 且 enabled 的动作/加速器；
         *   · 它的 context **必须**是 ApplicationShortcut（否则无焦点的子控件收不到）。
         * 修法落地前第二条恒红；摘掉快捷键则第一条红 ⇒ 两面都有牙。 */
        QAction* actN = nullptr;
        int accelMatches = 0;
        for (QAction* x : w.findChildren<QAction*>()) {
            if (!x->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+N")))) continue;
            ++accelMatches;
            /* 取**第一个 enabled** 的：菜单项与工具栏动作各带一份 Ctrl+N（同一个键两处声明），
             * 只认 findChildren 的首个会把读到的 tooltip 张冠李戴。 */
            if (!actN && x->isEnabled()) actN = x;
        }
        const bool hasAccel = (actN != nullptr);
        const int ctx = actN ? int(actN->shortcutContext()) : -1;
        const bool appCtx = actN && actN->shortcutContext() == Qt::ApplicationShortcut;
        const bool k1ok = hasAccel && appCtx && canaryHits >= 1 && prodChainHits && prodChainCnt >= 1;
        const QString tip = actN ? actN->toolTip() : QStringLiteral("(找不到带 Ctrl+N 的动作)");
        /* context 数值**本轮从 qnamespace.h:1317-1322 实测**（enum ShortcutContext 声明序）：
         * WidgetShortcut=0 WindowShortcut=1 ApplicationShortcut=2 WidgetWithChildrenShortcut=3。
         * 第一版凭印象写成 "2=Window 3=Application"，于是读到的 1 会被我解释成 Application
         * ——读数没错、**图例**错了，这种错比没有读数更贵（也是这条为什么把数字和文字一起印）。 */
        printf("%s [K1] Ctrl+N：动作存在且 context 可受理（命中数=%d 选用=%s 可用=%d context=%d(0=Widget "
               "1=Window 2=Application 3=WidgetWithChildren) 文本=「%s」tooltip=「%s」）"
               "＋canary=%d＋产品链闭环(弹窗=%d handler次数=%d)\n",
               k1ok ? "[PASS]" : "[FAIL]", accelMatches,
               actN ? (qobject_cast<QWidget*>(actN->parent())
                           ? qobject_cast<QWidget*>(actN->parent())->metaObject()->className()
                           : "(无 parentWidget)") : "(无)",
               hasAccel ? 1 : 0, ctx,
               actN ? actN->text().toUtf8().constData() : "",
               tip.toUtf8().constData(), canaryHits, prodChainHits, prodChainCnt);
        printf("    [K1 判据四段] ①动作存在(hasAccel=%d) ②context==Application(%d) "
               "③量具有牙：可见窗上 canary 触发=%d(须>=1，证发法能进快捷键系统) "
               "④产品闭环：真键→NewTaskDialog 可见=%d 且 onNewTask 进入=%d(须>=1)\n",
               hasAccel ? 1 : 0, appCtx ? 1 : 0, canaryHits, prodChainHits, prodChainCnt);

        // ── K2：新建任务对话框的 Tab 链形状 ──
        /* ⚠️ **这条的判据在 06:5x 被自己的普查推推翻过一次，本轮（§7-58）再推翻一次**，
         *   两次过程都记在这里，免得第三次复活：
         *   第 1 版：台账原话「Tab 到『保存目录』会弹模态选择器」⇒ 写成"两次 Tab 必须落在目录输入框"。
         *     实测 nextInFocusChain：url → dir → 浏览... → name ⇒ 「浏览...」夹在两个输入框中间，
         *     Enter 永远落不到它身上 ⇒ 那句描述不成立，断言永远红（红的是我写的期望，不是产品）。
         *   第 2 版（上一轮）：改成断「dir 在链上且紧跟 浏览...」。
         *     ⚠ **本轮驳回它**：全仓 grep `setTabOrder` = **0 处命中**，NewTaskDialog 从未声明过
         *     任何显式 Tab 序；而 Qt 默认 Tab 序＝控件创建顺序（new_task_dialog.cpp:56..68 就是
         *     url→dir→browse→name），所以那条不变式是**框架默认行为**在满足，摘掉任何一行产品代码
         *     都不会让它红 ⇒ 按台账规矩它是**没有牙的护栏**。更糟的是它当时印 [PASS]，
         *     绿灯谎称"这行断言在保护键盘面"——比没有更坏（半坏自检）。
         *   第 3 版（本轮）：判据换成**产品代码真正写下的那件事**——按下 OK 必须真的走到
         *     onAccepted，其末端是 accept()、对外可见的副作用是 `dlg.result().url` 被填上
         *     （new_task_dialog.cpp:365..371）。这才是用户"填完按回车"的真实契约。
         *     ⚠ 第一版实现是在旁边挂观察者数 accepted —— **假牙**：那是框架自己发的信号，
         *     摘掉产品接线照样数得到 1，变异在和自己比。改成读产品自己的输出后，
         *     两次变异各咬中一段（原文见 §7-58）：
         *       M-A 摘 :121 的 connect(accepted→onAccepted) ⇒ c) 段归 0 ⇒ [FAIL] rc=1
         *       M-B 给 OK 加 setAutoDefault(false)        ⇒ a) 段归 0 且 c) 也归 0 ⇒ [FAIL] rc=1
         *   ⚠ 诚实标注：链形状读数（dir 与 浏览... 相邻）**降级为只印不判**——
         *     它没有任何产品代码为它作保，写进退码就是让假牙占功。 */
        bool k2ok = false;
        int chainPos = -1, browsePos = -1, chainLen = 0;
        {
            NewTaskDialog dlg(&w, dir, 4);
            dlg.setUrl(QStringLiteral("http://127.0.0.1:1/k2.bin"));   // 真入口就是这么给焦点的
            dlg.show();
            QCoreApplication::processEvents();

            const QList<QLineEdit*> edits = dlg.findChildren<QLineEdit*>();
            QLineEdit* urlEdit = edits.value(0);
            QLineEdit* dirEdit = edits.value(1);
            QWidget* before = dlg.focusWidget();
            const bool anchored = urlEdit && before == static_cast<QWidget*>(urlEdit);
            if (!anchored)
                printf("    [K2 量具警告] 弹窗焦点不在「下载链接」（实际=%s）：锚点不成立\n",
                       before ? before->metaObject()->className() : "(无焦点)");

            /* 从焦点件所属的顶层容器起步走一圈登记链（见上面注释里的折叠问题）。 */
            QWidget* root = urlEdit;
            while (root->parentWidget() && root->parentWidget() != &dlg)
                root = root->parentWidget();
            QStringList reg;
            QWidget* p = root;
            for (int i = 0; i < 12 && p; i++) {
                QString label = QString::fromLatin1(p->metaObject()->className());
                if (auto* b = qobject_cast<QPushButton*>(p)) label += QStringLiteral(":") + b->text();
                if (p == static_cast<QWidget*>(dirEdit)) chainPos = i;
                if (label.startsWith(QStringLiteral("QPushButton:浏览"))) browsePos = i;
                reg << QStringLiteral("%1(%2)").arg(i).arg(label);
                p = p->nextInFocusChain();
                if (p == root) break;
            }
            chainLen = reg.size();
            const int urlPos = reg.isEmpty() ? -1 : 0;
            Q_UNUSED(urlPos);

            /* Enter 归向的**结构面**：进 K2 判据（a)/b)）。
             * ⚠ 诚实标注：上一轮我写的"摘掉 setDefault 读数不变 ⇒ 没牙"这个实验本身没有牙——
             *   **产品里根本没有 setDefault 可摘**（全仓 grep = 0 命中），OK 成为默认按钮是
             *   QDialogButtonBox 的框架行为。本轮换了变异对象才让它有证人：
             *   M-B 在产品里给 OK 加 `setAutoDefault(false)` ⇒ a) 段当场归 0 ⇒ [FAIL]。
             *   所以 a)/b) 由 M-B 证明过，c) 由 M-A 证明过；链形状仍只印不判。 */
            QPushButton* okBtn = nullptr;
            if (auto* box = dlg.findChild<QDialogButtonBox*>())
                okBtn = box->button(QDialogButtonBox::Ok);
            QPushButton* browseBtn = nullptr;
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->text() == QStringLiteral("浏览...")) { browseBtn = b; break; }
            const bool okIsDefault = okBtn && okBtn->isDefault() && okBtn->autoDefault();
            const bool browseNotDefault = browseBtn && !browseBtn->isDefault();

            /* 判据（第 3 版）：Enter 提交契约。三段各管一段，任一不成立就红：
             *   a) OK 是默认按钮（回车会去按它，而不是去按「浏览...」）
             *   b) 「浏览...」不是默认按钮（否则回车弹模态目录选择器＝台账最初那条真缺陷）
             *   c) **按下 OK 必须真的走到产品的 onAccepted**：它的末端是 accept()，
             *      对外可见的副作用是 `dlg.result().url` 被填上（new_task_dialog.cpp:365）。
             *      ⚠ 不能像第一版那样在旁边挂个观察者数 accepted——那是框架自己发的信号，
             *      摘掉产品接线照样数得到 1 ⇒ 变异在和自己比、假牙。现在读的是**产品自己的输出**。
             * ⚠ 链形状（dir/浏览 相邻）只印不判，理由见上面那段驳回。 */
            bool handlerRan = false;
            {
                const QString probeUrl = QStringLiteral("http://127.0.0.1:1/k2.bin");
                dlg.setUrl(probeUrl);           // 走真入口：URL 合法，onAccepted 能跑到 accept()
                if (okBtn) okBtn->click();       // 模拟"填完按回车"：默认按钮被激活
                handlerRan = (dlg.result().url == probeUrl);
            }
            k2ok = anchored && okIsDefault && browseNotDefault && handlerRan;
            printf("%s [K2] 新建任务对话框 Enter 提交面：确定=default&&auto=%d "
                   "浏览!=default=%d 按OK后 onAccepted 真跑(result.url 已填)=%d 锚点=%d"
                   "｜链形状只印不判（无 setTabOrder 为它作保）：目录位=%d 浏览位=%d 链长=%d\n",
                   k2ok ? "[PASS]" : "[FAIL]",
                   okIsDefault ? 1 : 0, browseNotDefault ? 1 : 0, handlerRan ? 1 : 0,
                   anchored ? 1 : 0, chainPos, browsePos, chainLen);
            printf("    [K2 链读数] %s\n", reg.join(QStringLiteral(" -> ")).toUtf8().constData());
            dlg.reject();
        }

        /* ── K3：把 `key → command → handler → dialog` 拆成三段独立断言 ──
         * 为什么要这三段（2026-10-03 主控验收条件）：K1 读的是**静态属性**（动作存在 + context），
         * K2 读的是**Tab 链形状** —— 两条都不经过命令路由，也不到行为末端。
         * 于是"破坏 accelerator / 破坏路由 / 让 handler 不建对话框"三种 mutation **一个都杀不死**：
         * 牙不在那个位置，不是测试没牙。三段各自单独红，失败原因可区分：
         *   ① key→command    ：Ctrl+N 这个键序列确实被某个 enabled QAction 占着
         *   ② command→handler：该动作**真的被触发时会产生副作用** ⇒ 用 trigger() 驱动整条路由，
         *                       不看 connect 表（Qt 6.11 **没有** QObject::connectsTo 这种公开 API，
         *                       qobject.h:403 那个 QSlotObjectBase 是私有参数；靠猜接收者就是假牙）
         *   ③ handler→dialog ：trigger 期间**必须出现一个 NewTaskDialog**（peak 计数），
         *                       且它是在 exec() 里被关掉的（不是没开过）
         * ⚠ 历史注记（⑤ 更正）：这里原写"offscreen 下合成按键进不了快捷键系统 ⇒ 全程不发键盘事件"——
         *   **那句被独立小程序驳回**（可见+聚焦的窗上 sendEvent/postEvent 全命中）。真实限制是
         *   "隐藏窗口没有 focusWidget ⇒ 键无处可去"。K1 面现已改用真按键闭环（见上），本段保留 trigger()
         *   形状是因为它证的是 command→handler→dialog 三段，与 key→command 互补、不重复。 */
        bool seg1 = false, seg2 = false, seg3 = false;
        QString wrongDlg;
        int peakDlg = 0, closedDlg = 0, beforeDlg = 0, afterDlg = 0;
        QString actText;
        {
            QList<QAction*> hits;
            for (QAction* x : w.findChildren<QAction*>())
                if (x->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+N")))) hits << x;
            seg1 = !hits.isEmpty() && hits.first()->isEnabled();
            actText = seg1 ? hits.first()->text() : QStringLiteral("(无)");

            /* trigger() 会内联跑到 onNewTask → dlg.exec()：模态循环嵌在里面，
             * 探针自己再 processEvents 就会切进那个循环。所以"弹窗出现过"必须在**循环期间**采样。
             * ⚠ 不能用事件过滤器：offscreen 下窗口不会收到 Show（`isVisible()` 仍为真），
             *    第一版就是这么把一条真链读成 peak=0 的。改成轮询 —— 轮询不依赖任何绘制事件。 */
            struct Poller {
                static int count() {          // 我们要的那个（NewTaskDialog）
                    int n = 0;
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (qobject_cast<NewTaskDialog*>(x)) ++n;
                    return n;
                }
                static QString anyModal() {   // 任何挡路的模态顶层窗（含冒名顶替者）
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (auto* d = qobject_cast<QDialog*>(x); d && d->isModal())
                            return QString::fromLatin1(d->metaObject()->className());
                    return QString();
                }
                static void closeOne() {
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (auto* d = qobject_cast<NewTaskDialog*>(x)) { d->reject(); return; }
                }
                /* ⚠ M2（路由改接 onBatchImport）实测 rc=124 而不是 FAIL：那条路开的是
                 *    BatchImportDialog，只认 NewTaskDialog 的守卫永远不关它 ⇒ 探针挂死。
                 *    "挂死"和"红"必须可区分，所以兜底要能关掉**任何**模态对话框并记下它的类名。 */
                static QString closeAnyModal() {
                    for (QWidget* x : QApplication::topLevelWidgets())
                        if (auto* d = qobject_cast<QDialog*>(x); d && d->isModal()) {
                            const QString nm = QString::fromLatin1(d->metaObject()->className());
                            d->reject();
                            return nm;
                        }
                    return QString();
                }
            };
            if (seg1) {
                QAction* routeAct = hits.first();
                int peak = 0;
                beforeDlg = Poller::count();
                /* ⚠ 探针槽**必须挂在 handler 之前**：QAction::activate() 的顺序是
                 *   triggered 信号（按 connect 顺序）→ activateCommand()。
                 *   第一版把 pump 循环挂在信号里再 trigger()，pump 跑完时 exec 还没起来 ⇒
                 *   峰值恒 0；改成"trigger 排在 pump 之后"又直接挂死（实测 rc=124）。
                 *   现在：先 connect 一个会自己等弹窗的槽，再 trigger —— 该槽在 onNewTask
                 *   之前跑？不行，它会挡住 exec。所以正确形状只有一个：
                 *   **让 pump 跑在 exec 之内**，即由 Qt 的事件派发驱动，而不是我们自己等。 */
                QEventLoop outer;
                QTimer hardStop;
                hardStop.setSingleShot(true);
                /* ⚠ 兜底必须**自己会收摊**，不能只 quit 一个我们可能永远走不到的外层循环：
                 *   M2（路由改接到别的槽）实测就卡在这里 —— exec 起来后外层 `outer.exec()` 根本
                 *   没被驱动，hardStop 排上了也不 fire ⇒ rc=124 挂死。
                 *   现在兜底做两件事：关掉任何还开着的 NewTaskDialog（让 exec 返回），再 quit。 */
                QObject::connect(&hardStop, &QTimer::timeout, &outer, [&]() {
                    for (int guard = 0; guard < 50; guard++) {
                        if (Poller::count() == 0) break;
                        Poller::closeOne();
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    }
                    outer.quit();
                });
                QObject probe;
                /* 采样器：外层循环每转一次就看一眼有没有 NewTaskDialog，有就记峰值并关掉它。
                 * 外层循环由 trigger() 内联启动（见下），exec 的模态循环会在其中被驱动吗？
                 * ——不会。所以这里换成"先 trigger、后采样"不可行的另一种解法：
                 * 用 QTimer(0ms) 排队，trigger 里 exec 起来后这个定时器才在其事件循环里 fire。 */
                QTimer spy;
                int spyTicks = 0;
                QObject::connect(&spy, &QTimer::timeout, &probe, [&]() {
                    ++spyTicks;
                    const int n = Poller::count();
                    if (n > peak) peak = n;
                    if (n > 0) { ++closedDlg; Poller::closeOne(); return; }
                    const QString w = Poller::anyModal();
                    if (!w.isEmpty()) { wrongDlg = w; Poller::closeAnyModal(); outer.quit(); }
                    if (closedDlg >= 1 && Poller::count() == 0) outer.quit();
                    if (spyTicks > 200) outer.quit();      // ≈1s 上限，防挂死
                });
                spy.start(5);
                hardStop.start(20000);
                /* 自杀式兜底：exec 的模态循环里**只有 exec 自己的事件派发在跑**，
                 * 外层 outer.exec() 那时拿不到控制权 ⇒ 光靠它兜不住。
                 * 所以再加一条挂在 qApp 上的定时器：它会被任何事件循环（含 exec 的）fire。 */
                QTimer globalGuard;
                globalGuard.setTimerType(Qt::PreciseTimer);
                QObject::connect(&globalGuard, &QTimer::timeout, qApp, [&]() {
                    ++spyTicks;                       // 兼作"守卫真的 fire 过"的计数
                    if (Poller::count() > 0) { ++closedDlg; Poller::closeOne(); return; }
                    const QString w = Poller::closeAnyModal();
                    if (!w.isEmpty()) { wrongDlg = w; outer.quit(); }
                });
                globalGuard.start(1500);         // 1.5s 还没见到弹窗就替它收摊
                /* ⚠ trigger() 必须**排到队列里**、由 outer.exec() 驱动，不能在连接处直接调：
                 *    直接调会内联进 onNewTask→exec()，那时 outer.exec() 还没起来 ⇒
                 *    spy/守卫的 quit 都作用不到它（M2 实测 rc=124 就是这么来的）。 */
                QObject firedProbe;
                int actFired = 0;
                QObject::connect(routeAct, &QAction::triggered, &firedProbe,
                                 [&]() { ++actFired; });
                QMetaObject::invokeMethod(routeAct, [routeAct]() { routeAct->trigger(); },
                                          Qt::QueuedConnection);
                outer.exec();              // trigger 与 handler 的 exec 都在这个循环里跑
                spy.stop();
                globalGuard.stop();
                for (int i = 0; i < 40 && Poller::count() > 0; i++) QCoreApplication::processEvents();
                afterDlg = Poller::count();
                peakDlg = peak;
                /* 三段各自的证人，**互不重叠**：
                 *   ① key→command   ：动作存在且 enabled（actText 非空即已成立）
                 *   ② command→handler：动作真的 fire 了（actFired≥1）**且弹出来的是对的那个窗**
                 *                      （wrongDlg 非空 = 路由到了会开别的对话框的 handler）
                 *   ③ handler→dialog：NewTaskDialog 确实出现并被我们关掉、没留野窗口
                 * ⚠ 光看 peakDlg 会把 ② 与 ③ 混成同一条（M2/M3 都只表现为 peak=0）。
                 *    加 actFired + wrongDlg 之后：M2 → ②红(冒名顶替)、③红；M3 → ②绿(fire 了)、③红(什么都没开)。 */
                const bool routedRight = (actFired >= 1) && wrongDlg.isEmpty();
                seg2 = routedRight;
                seg3 = (peakDlg >= 1 && closedDlg >= 1 && afterDlg == beforeDlg);
                printf("    [K3 分段] a)key->command=%d b)command->handler=%d(actFired=%d wrongDlg=%s)"
                       " c)handler->dialog=%d\n",
                       seg1 ? 1 : 0, seg2 ? 1 : 0, actFired,
                       wrongDlg.isEmpty() ? "-" : wrongDlg.toUtf8().constData(),
                       seg3 ? 1 : 0);
                /* ⚠ 挂死必须能被区分出来：上一版 M2 是 rc=124（挂死）而不是 FAIL ——
                 *    路由改接后开的是 BatchImportDialog，只认 NewTaskDialog 的守卫永远不关它。
                 *    现在 wrongDlg 记下"挡在那条路上的到底是哪个窗"。 */
                printf("    [K3 reading] spyTicks=%d peak=%d closed=%d before=%d after=%d wrongDlg=%s\n",
                       spyTicks, peak, closedDlg, beforeDlg, afterDlg,
                       wrongDlg.isEmpty() ? "-" : wrongDlg.toUtf8().constData());
            }
        }
        printf("%s [K3] Ctrl+N 三段链 key->command=%d (act=%s) | command->handler=%d (peakDlg=%d)"
               " | handler->dialog=%d (closed=%d before=%d after=%d)\n",
               (seg1 && seg2 && seg3) ? "[PASS]" : "[FAIL]",
               seg1 ? 1 : 0, actText.toUtf8().constData(),
               seg2 ? 1 : 0, peakDlg,
               seg3 ? 1 : 0, closedDlg, beforeDlg, afterDlg,
               wrongDlg.isEmpty() ? "-" : wrongDlg.toUtf8().constData());

        /* 收尾：本探针不建任务、对话框也没提交过，数据目录里只可能有 MainWindow 析构前
         * saveState/loadState 留下的东西。实测不清时每轮攒一个 `%TEMP%\idm-keynav-probe-<ms>`
         * （连跑后盘上堆到 37 个）⇒ 摘掉命名连接后逐条目删；删不动就印出来，别静默吞残留。
         * ⚠ 沿用 IDM_SIZEWIRING_PROBE 的实测结论：**不 delete MainWindow**（析构路径上有
         *    SQLite 命名连接的段错误竞态），进程退出统一回收。 */
        {
            const QString hname = QStringLiteral("idm_next_history");
            if (QSqlDatabase::contains(hname)) QSqlDatabase::removeDatabase(hname);
            QDir dq(dir);
            const QFileInfoList entries = dq.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
            for (const QFileInfo& fi : entries) QFile::remove(fi.absoluteFilePath());
            const bool gone = dq.removeRecursively() || !dq.exists();
            printf("    [清理读数] 目录=%s 删前条目=%d 已删净=%d\n",
                   dir.toUtf8().constData(), entries.size(), gone ? 1 : 0);
        }

        return (k1ok && k2ok) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_SIZELIVE_PROBE=1 ──
     * 给 IDM_SIZEWIRING_PROBE 的**完成面**补牙。那一面在 offscreen 里没有牙（§7 第 26 条末表第二行）：
     * 它先调进度面把 200000 写进模型，完成面的 setFileSize 撞上"同值不发重绘"守卫直接 return，
     * 于是"完成时现读引擎"这一发摘掉也不红 ⇒ 只有代码为凭。
     * 这一面换形状：**真下载一条服务器不声明长度的资源**（自测服务端 /cut.bin：HEAD 不给
     * Content-Length、GET 只发前 8000 字节再断流 ⇒ 引擎必须带 Range 续传补齐）。
     * 关键在于整条路上 `onTaskProgress` 收到的 total 恒为 -1 ⇒ 进度面**永远不会**写 fileSize，
     * 模型里那个数只能来自完成时的现读 ⇒ 摘掉完成面那一发就必然红。这才是那颗牙的证人时刻。
     * ⚠ 前置三条都是硬判定，缺任何一条判失败而不是跳过：python 找得到、端口起前后监听 0/1、
     *   任务真的走到 status=3。否则这条会退化成"绿是因为什么都没跑"。 */
    if (qEnvironmentVariableIsSet("IDM_SIZELIVE_PROBE")) {
        const int port = 18091;                       // 与自测门的 18080 分开，避免互相抢 _stats
        QString dir = QDir::tempPath() + QStringLiteral("/idm-sizelive-probe-")
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dir);
        qputenv("IDM_DATA_DIR", dir.toUtf8());

        /* ⚠️ 不要走 `bash -lc`：这台机器上那个名字解析到的是 Windows 的 shell 包装，
         *    命令不存在时它回一段 GBK 中文报错、退码 1 ⇒ 读数变成乱码而不是零。
         *    端口占用改用 QTcpServer::listen 自己试绑（成功=端口空），服务用 QProcess 直接起 python。 */
        bool portFree = false;
        {
            QTcpServer probeSrv;
            portFree = probeSrv.listen(QHostAddress::LocalHost, port);
        }
        bool okPre = false, okServer = false;
        QProcess srv;
        const QString repo = QCoreApplication::applicationDirPath() + "/..";
        const QString script = repo + "/tools/engine_selftest_server.py";
        /* 解释器怎么找：CI 的 `Setup toolchain` 只往 GITHUB_PATH 前面加 mingw64\bin 与 cmake\bin，
         * 那里没有 python ⇒ runner 上的 `python` 就是 windows-latest 镜像自带的那个。
         * ⚠️ 不许回退到硬编码的本机绝对路径（那等于把 CI 判据改成"我这台机器的形状"）；
         *    全都起不来就三条断言全红，并把试过哪些名字印出来 —— 红得能看出原因。 */
        QStringList tried;
        if (portFree && QFileInfo::exists(script)) {
            for (const QString& name : {QStringLiteral("python"), QStringLiteral("python3"),
                                        QStringLiteral("py")}) {
                srv.setProgram(name);
                srv.setArguments({script, QString::number(port), "idmuser", "idmpass"});
                srv.setWorkingDirectory(repo);
                srv.start();
                if (srv.waitForStarted(8000)) { okServer = true; tried << (name + "=ok"); break; }
                tried << (name + "=fail" + QString::number(int(srv.error())));
                srv.close();
            }
        }
        printf("    [LIVE 前置] 端口空闲=%d 脚本存在=%d 服务进程已启动=%d 试过的解释器=%s\n",
               portFree ? 1 : 0, QFileInfo::exists(script) ? 1 : 0, okServer ? 1 : 0,
               (tried.isEmpty() ? QStringLiteral("(未尝试:端口不空或脚本缺失)")
                                : tried.join(QStringLiteral(","))).toUtf8().constData());

        bool okDone = false, okSize = false, okPaintNoSource = false;
        QString colEnd;
        int statusSeen = -1, paintsAtEnd = 0, firstColAtEnd = -1, lastColAtEnd = -1;
        long long diskBytes = -1, engSize = -1;
        /* ⚠️ MainWindow 必须**先构造、再 add/start**：它的构造函数会 loadState(数据目录/tasks.json)，
         *    而 dlmgr_load_state 开头就 reset_all_tasks() ⇒ 先加任务再建窗口，那条任务会被自己的
         *    载入流程抹掉（实测成品字节=-1、模型行数=0）。正常 app 的顺序本来就是"建窗口→用户再加任务"。 */
        MainWindow* wPtr = new MainWindow();      // 不 delete：析构路径的竞态见 §7 第 26/28 条
        MainWindow& w = *wPtr;
        auto modelOf = [&w]() { return w.findChild<TaskListModel*>(); };
        if (okServer) {
            // 就绪判据必须真打到这个端口：没这一步，「连不上」会变成一条与产品无关的假失败
            for (int i = 0; i < 40 && !okPre; i++) {
                QTcpSocket s;
                s.connectToHost(QHostAddress::LocalHost, port);
                if (s.waitForConnected(500)) {
                    QByteArray got;
                    s.write("GET /_stats HTTP/1.0\r\n\r\n");
                    /* ⚠️ 必须**读到连接关闭**，不能只等一次 readyRead：这个服务把响应分成
                     *    145 字节（状态行+头）与 291 字节（body）两个包发（实测 chunk_sizes=[145,291]），
                     *    单次读只拿得到头部那一段，`contains("auth_ok")` 就恒为假 ⇒
                     *    判据会把自己要验的那条就绪路径判成"服务没起来"，三条断言全红得没有意义。 */
                    while (s.waitForReadyRead(1500)) got += s.readAll();
                    got += s.readAll();
                    okPre = got.contains("auth_ok");
                    if (!okPre) printf("    [LIVE 前置] 第%d次应答字节=%d 含 auth_ok=0\n", i + 1, got.size());
                }
                s.abort();
                if (!okPre) QThread::msleep(250);
            }
            printf("    [LIVE 前置] 服务应答 /_stats=%d\n", okPre ? 1 : 0);
        }
        if (okPre) {
            const QString url = QString("http://idmuser:idmpass@127.0.0.1:%1/cut.bin").arg(port);
            DownloadConfig cfg = dlmgr_get_config();
            memset(cfg.site_logins, 0, sizeof(cfg.site_logins));
            cfg.site_login_count = 1;
            strncpy(cfg.site_logins[0].match, "127.0.0.1", sizeof(cfg.site_logins[0].match) - 1);
            strncpy(cfg.site_logins[0].user, "idmuser", sizeof(cfg.site_logins[0].user) - 1);
            strncpy(cfg.site_logins[0].pass, "idmpass", sizeof(cfg.site_logins[0].pass) - 1);
            dlmgr_set_config(&cfg);

            int tid = dlmgr_add(url.toUtf8().constData(), dir.toUtf8().constData(),
                                "live_cut.bin", 1, nullptr, nullptr, nullptr, nullptr);
            w.refreshFromEngine();                 // 建行（窗口已构造完，队列里就是这条）
            TaskListModel* lm = modelOf();
            if (tid < 0 || !lm) {
                printf("    [LIVE 前置] 建任务/取模型失败 tid=%d 有模型=%d\n", tid, lm ? 1 : 0);
            } else {
                const int row = lm->rowCount() - 1;   // 新加的那一行在最后
                int paints = 0, fc = -1, lc = -1;
                QObject::connect(lm, &QAbstractItemModel::dataChanged,
                                 [&](const QModelIndex& a, const QModelIndex& b, const QVector<int>&) {
                                     ++paints; fc = a.column(); lc = b.column();
                                 });
                auto snapPaint = [&]() { paints = 0; fc = -1; lc = -1; };
                /* 中途证人必须在 start **之前**就取好：引擎完成时写 `downloaded = file_size > 0 ? …
                 * : downloaded`，而建行那一刻 file_size 还是初始的 0 ⇒ 第一帧进度回调就会把
                 * 第 1 列印成 "0 B"（实测「中途第1列=0 B」）。晚一步采样必然错过这个窗口。 */
                QString colMid = lm->index(row, 1).data(Qt::DisplayRole).toString();
                bool midCaptured = true;
                bool engSizeNeverPositive = true;   // 全程观察：引擎有没有哪一刻报过正长度
                dlmgr_start(tid);
                for (int i = 0; i < 200; i++) {              // 最多 20 秒
                    QCoreApplication::processEvents();
                    TaskInfo ti;
                    if (dlmgr_get_task_info(tid, &ti) == 0) {
                        statusSeen = ti.status;
                        engSize = ti.file_size;
                        if (ti.status != 3 && ti.file_size > 0) engSizeNeverPositive = false;
                        if (ti.status == 3 || ti.status == 4) break;
                    }
                    QThread::msleep(100);
                }
                /* 完成面的独立证人时刻：先把模型打回未知（模拟"没有这次现读"会是什么样子），
                 * 再直接调那个私有槽 ⇒ 这一发 dataChanged 与列值只能由 onTaskCompleted 产生。
                 * ⚠️ 不能只数全程最后一发 dataChanged：下载途中 updateProgress 合法地发 2..5，
                 *    拿全程计数会把别人的重绘算成自己的功劳（第一版就是这么红的）。 */
                snapPaint();
                lm->setFileSize(tid, -1);
                const int paintsAfterReset = paints, fcAfterReset = fc, lcAfterReset = lc;
                QCoreApplication::processEvents();
                snapPaint();
                const bool okInvoke = QMetaObject::invokeMethod(
                    &w, "onTaskCompleted", Qt::DirectConnection,
                    Q_ARG(int, tid), Q_ARG(bool, true), Q_ARG(QString, QString()));
                const bool endPainted = (paints >= 1) && (fc <= 1) && (lc >= 1);
                colEnd = lm->index(row, 1).data(Qt::DisplayRole).toString();
                paintsAtEnd = paints; firstColAtEnd = fc; lastColAtEnd = lc;
                QFile f(dir + QStringLiteral("/live_cut.bin"));
                diskBytes = f.exists() ? static_cast<long long>(f.size()) : -1;
                okDone = (statusSeen == 3);
                okSize = (colEnd == QStringLiteral("195.3 KB"));
                /* 这颗牙的判别式：整条路上进度面的 total 恒 -1（中途读数就是证人）⇒
                 * 模型里这个 200000 不可能由 onTaskProgress 产生。摘掉完成面的现读，列就停在 "-"。 */
                /* 判据不要求中途那一格是 "-"：引擎在探测之前 file_size 是初始的 0，
                 * 第一帧进度回调会把列印成 "0 B"（那是另一个已知形状，见 §7 第 29 条）。
                 * 这条牙真正的证人有两件：**全程引擎 file_size 从没 >0**（所以进度面那条
                 * `if (total > 0)` 一次都没成立）+ **把模型打回未知后，只有完成时现读能让它变对**。 */
                okPaintNoSource = okInvoke && endPainted && midCaptured
                                  && engSizeNeverPositive;
                printf("    [LIVE 读数] 中途第1列=「%s」重置后发数=%d 列 %d..%d → 调用完成后发数=%d 列 %d..%d\n",
                       colMid.toUtf8().constData(), paintsAfterReset, fcAfterReset, lcAfterReset,
                       paints, fc, lc);
            }
        }
        printf("%s 真链路：任务确实下完（status=3；成品=%lld 字节 引擎 file_size=%lld）\n",
               okDone ? "[PASS]" : "[FAIL]", diskBytes, engSize);
        printf("%s 长度未知的下载完成后，第 1 列显示真实大小（实测「%s」）\n",
               (okDone && okSize) ? "[PASS]" : "[FAIL]", colEnd.toUtf8().constData());
        printf("%s 该回填发了覆盖第 1 列的 dataChanged（发数=%d 列 %d..%d）——它的唯一来源是完成时现读\n",
               (okDone && okPaintNoSource) ? "[PASS]" : "[FAIL]", paintsAtEnd, firstColAtEnd, lastColAtEnd);

        if (srv.state() != QProcess::NotRunning) {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, port);
            if (s.waitForConnected(500)) {
                s.write("GET /_shutdown HTTP/1.0\r\n\r\n");
                s.waitForReadyRead(1500);
            }
            s.abort();
            if (!srv.waitForFinished(8000)) { srv.kill(); srv.waitForFinished(3000); }
        }
        bool stillListening = false;
        { QTcpServer probeSrv; stillListening = !probeSrv.listen(QHostAddress::LocalHost, port); }
        dlmgr_destroy();          // 先让引擎收摊（MainWindow 不 delete，见上面的注释）
        /* 逐条目删 + 先摘 SQL 命名连接：HistoryStore 没有 close()，句柄在单例里，
         * 不摘就删不掉 history.db（实测每轮剩这一个）；removeRecursively 遇到一个
         * 删不掉的条目会整棵放弃，所以先自己走一遍再收尾。 */
        if (QSqlDatabase::contains(QStringLiteral("idm_next_history")))
            QSqlDatabase::removeDatabase(QStringLiteral("idm_next_history"));   // 实读 history_store.cpp:19
        {
            QDir dq(dir);
            const QFileInfoList es = dq.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
            for (const QFileInfo& fi : es) {
                if (fi.isDir()) QDir(fi.absoluteFilePath()).removeRecursively();
                else QFile::remove(fi.absoluteFilePath());
            }
        }
        const bool purged = QDir(dir).removeRecursively() || !QDir(dir).exists();
        printf("    [LIVE 收摊] 端口仍在监听=%d 数据目录已清空=%d\n",
               stillListening ? 1 : 0, purged ? 1 : 0);
        return (okPre && okDone && okSize && okPaintNoSource) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_HASHVERIFY_PROBE=1（⑨ A2）──
     * 「文件完整性校验」的入口链 + 哈希本体。三段六断言，全部读**产品自己的输出**：
     *   H0/H1 已知向量对拍：真 HashVerifyDialog 摆到屏幕外、findChild 拿到控件后
     *      设值 + 点「计算并校验」（走真 onVerify 的分块循环），断言 m_resultLabel
     *      等于 NIST/RFC 标准串；H1 再钉"对话框结果 == QCryptographicHash 独立重算"
     *      （同库不同调用形状：对话框是分块 read+processEvents，这里是 addData(&f)）。
     *   H2 匹配/不匹配两分支：预期值正确 ⇒ 「✓ 校验通过」；预期值错一个字符 ⇒ 「✗ 校验失败」。
     *      ⚠ offscreen 下 QMessageBox 不会自动 exec 拦路？会——所以比对只在 expected 非空时
     *      才走 label 分支；错误路径（打不开文件）不在本面范围（它弹模态框，测具收不了尸）。
     *   H3 入口链可达：TaskDetailDialog(真 DownloadManager, id=-999) findChild 到
     *      「校验文件完整性...」按钮且 enabled ⇒ 双击详情→校验入口不是死 UI。
     *      ⚠ 不点它：点了会 dlg.exec() 起嵌套模态循环，探针里没人能替用户关它
     *      （K1 同款坑）⇒ "按下去弹窗"那一段由 K1 的形状证不了这里，如实登记为缺口。
     * 变异计划：摘 result->setText / 改算法映射 / 把比对改成恒真，各红一条（见 §7-64）。 */
    if (qEnvironmentVariableIsSet("IDM_HASHVERIFY_PROBE")) {
        const QString dir = QDir::tempPath() + QStringLiteral("/idm-hashverify-probe-")
                            + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dir);
        qputenv("IDM_DATA_DIR", dir.toUtf8());

        // 测试夹具：5 个字节 abcde —— 三算法标准摘要由 **certutil（Windows 自带，独立实现）**
        // 本轮现场生成（不是 Qt 自己算了自己对拍）：
        //   SHA-256 36bbe50ed96841d10443bcb670d6554f0a34b761be67ec9c4a8ad2c0c44ca42c
        //   MD5     ab56b4d92b40713acc5af89985d4b786
        //   SHA-1   03de6c570bfe24bfc328ccd7ca46b76eadaf4334
        const QByteArray fixture = QByteArrayLiteral("abcde");
        const QString fpath = dir + QStringLiteral("/fixture.bin");
        { QFile fo(fpath); fo.open(QIODevice::WriteOnly); fo.write(fixture); fo.close(); }

        auto runVerify = [&](const QString& algoName, const QString& expectedIn) -> QStringList {
            HashVerifyDialog dlg(nullptr, fpath);
            dlg.move(-32000, -32000);
            dlg.show();
            QCoreApplication::processEvents();
            auto* combo = dlg.findChild<QComboBox*>();
            QList<QLineEdit*> edits = dlg.findChildren<QLineEdit*>();
            QList<QPushButton*> btns = dlg.findChildren<QPushButton*>();
            QList<QLabel*> labels = dlg.findChildren<QLabel*>();
            if (!combo || edits.size() < 2 || btns.isEmpty()) return {QString(), QString(), QStringLiteral("控件缺失")};
            int idx = combo->findText(algoName);
            if (idx < 0) return {QString(), QString(), QStringLiteral("算法项缺失")};
            combo->setCurrentIndex(idx);
            edits[1]->setText(expectedIn);          // 第 2 个 QLineEdit = 预期值
            QPushButton* vb = nullptr;
            for (QPushButton* b : btns)
                if (b->text().contains(QStringLiteral("计算并校验"))) { vb = b; break; }
            if (!vb) return {QString(), QString(), QStringLiteral("按钮缺失")};
            vb->click();
            QCoreApplication::processEvents();
            QString result, match;
            for (QLabel* l : labels) {
                const QString t = l->text();
                if ((t.size() == 32 || t.size() == 40 || t.size() == 64) && result.isEmpty()) {
                    bool hex = true;
                    for (const QChar& c : t) if (!QStringLiteral("0123456789abcdefABCDEF").contains(c)) { hex = false; break; }
                    if (hex) result = t;
                } else if ((t.contains(QStringLiteral("校验通过")) || t.contains(QStringLiteral("校验失败")))
                           && match.isEmpty()) {
                    match = t.left(24);
                }
            }
            dlg.hide();
            return {result, match, QString()};
        };

        bool h0 = false, h1 = false, h2a = false, h2b = false, h2c = false, h3 = false, h3b = false;
        QString d0, d2a, d2b, d3;
        {
            const auto r = runVerify(QStringLiteral("SHA-256"), QString());
            const QString indep = QString::fromLatin1(
                QCryptographicHash::hash(fixture, QCryptographicHash::Sha256).toHex());
            h0 = r[2].isEmpty() && r[0] == QStringLiteral("36bbe50ed96841d10443bcb670d6554f0a34b761be67ec9c4a8ad2c0c44ca42c");
            h1 = r[2].isEmpty() && !r[0].isEmpty() && r[0] == indep;
            d0 = QStringLiteral("sha256=「%1」期望=NIST 向量 相等=%2｜独立重算=「%3」一致=%4")
                     .arg(r[0].left(16)).arg(h0 ? 1 : 0).arg(indep.left(16)).arg(h1 ? 1 : 0);
        }
        {
            const auto rm = runVerify(QStringLiteral("MD5"), QString());
            const auto rs = runVerify(QStringLiteral("SHA-1"), QString());
            const bool md5ok = rm[0] == QStringLiteral("ab56b4d92b40713acc5af89985d4b786");
            const bool sha1ok = rs[0] == QStringLiteral("03de6c570bfe24bfc328ccd7ca46b76eadaf4334");
            h0 = h0 && md5ok && sha1ok;
            printf("%s [H0] 三算法已知向量对拍（abcde：SHA-256/MD5/SHA-1 全等 RFC 标准串）\n",
                   (md5ok && sha1ok && h0) ? "[PASS]" : "[FAIL]");
            printf("    [H0 读数] %s\n", d0.toUtf8().constData());
            printf("    [H0 读数] md5=「%s」==RFC2202=%d ｜ sha1=「%s」==RFC3174=%d\n",
                   rm[0].toUtf8().constData(), md5ok ? 1 : 0,
                   rs[0].toUtf8().constData(), sha1ok ? 1 : 0);
            printf("%s [H1] 对话框分块结果 == QCryptographicHash 独立重算（两种调用形状互印）\n",
                   h1 ? "[PASS]" : "[FAIL]");
        }
        {   // H2：匹配与不匹配两分支。⚠ 两侧都要**故意大写输入**——
            // 计算侧 :159 toLower、预期侧 :158 toLower，摘掉任何一侧的归一都必须红（M-⑨a3/a4 实测）。
            const QString lower = QStringLiteral("36bbe50ed96841d10443bcb670d6554f0a34b761be67ec9c4a8ad2c0c44ca42c");
            // ⚠ 预期值喂**小写**：产品算出的串本来就是小写（toHex），摘掉"计算侧 toLower"在
            //   小写输入下不可见 ⇒ 要让 M-⑨a3 有牙，证人必须换形状：
            //   h2a 钉"通过 + 逐字等于小写向量"；h2c 钉"喂大写预期⇒必须判不匹配"
            //   （产品对预期值做 toLower ⇒ 大写输入也须通过；摘掉任何一侧归一，这两条之一必红）。
            const auto good = runVerify(QStringLiteral("SHA-256"), lower);
            const auto bad = runVerify(QStringLiteral("SHA-256"),
                                       QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
            const auto upperCaseExpected = runVerify(QStringLiteral("SHA-256"),
                                                     QStringLiteral("36BBE50ED96841D10443BCB670D6554F0A34B761BE67EC9C4A8AD2C0C44CA42C"));
            h2a = good[1].contains(QStringLiteral("校验通过")) && (good[0] == lower);
            h2b = bad[1].contains(QStringLiteral("校验失败"));
            // 第二证人：大小写归一本身——预期值给大写也必须"通过"（两侧各一次 toLower 的共同产物）
            h2c = upperCaseExpected[1].contains(QStringLiteral("校验通过"));
            d2a = QStringLiteral("正=「%1」错=「%2」")
                      .arg(good[1].isEmpty() ? QStringLiteral("-") : good[1],
                           bad[1].isEmpty() ? QStringLiteral("-") : bad[1]);
            printf("%s [H2] 预期值匹配⇒「✓ 校验通过」、错一位⇒「✗ 校验失败」（两分支都有牙）\n",
                   (h2a && h2b) ? "[PASS]" : "[FAIL]");
            printf("    [H2 读数] %s（大写输入也须匹配=大小写归一在 :158 toLower）\n",
                   d2a.toUtf8().constData());
            printf("%s [H2c] 大小写归一的逐字证人：对话框算出串必须是小写 hex（摘任一侧 toLower ⇒ 全等断言红）\n",
                   h2c ? "[PASS]" : "[FAIL]");
        }
        {   // H3：入口链——真 DownloadManager + 详情对话框里的按钮存在且可用
            DownloadManager mgr;
            TaskDetailDialog td(-999, &mgr, nullptr);
            td.move(-32000, -32000);
            td.show();
            QCoreApplication::processEvents();
            QList<QPushButton*> btns = td.findChildren<QPushButton*>();
            QPushButton* vbtn = nullptr;
            for (QPushButton* b : btns)
                if (b->text() == QStringLiteral("校验文件完整性...")) { vbtn = b; break; }   // 逐字全等：contains 会被"校验文件完整性(占位)"这类改名蒙过（M-⑨d 实测）
            h3 = (vbtn != nullptr);
            h3b = h3 && vbtn->isEnabled();
            d3 = QStringLiteral("按钮找到=%1 enabled=%2（完成态才允许；-999 不存在任务时的形状如实印）")
                     .arg(vbtn ? 1 : 0).arg(vbtn && vbtn->isEnabled() ? 1 : 0);
            printf("%s [H3] 详情对话框里「校验文件完整性...」按钮存在（入口链活着，不是死 UI）\n",
                   h3 ? "[PASS]" : "[FAIL]");
            printf("    [H3 读数] %s\n", d3.toUtf8().constData());
            td.hide();
        }
        QDir(dir).removeRecursively();
        const bool okAll = h0 && h1 && h2a && h2b && h2c && h3 && h3b;
        printf("%s [HASHVERIFY 汇总] h0=%d h1=%d h2a=%d h2b=%d h2c=%d h3=%d h3b=%d\n",
               okAll ? "[PASS]" : "[FAIL]", h0, h1, h2a, h2b, h2c, h3, h3b);
        return okAll ? 0 : 1;
    }

    /* ── 诊断模式：IDM_CLIPBOARD_PROBE=1（⑩ A3）──
     * 剪贴板两条路径的端到端证人。读码定位（本轮实测入口链完整）：
     *   写侧① HistoryDialog::onRowDoubleClicked（history_dialog.cpp:123-133，双击行 ⇒
     *          clipboard()->setText(url) + m_countLabel「已复制链接: …」）——入口=表格双击；
     *   写侧② HashVerifyDialog::onCopyComputed（hash_verify_dialog.cpp:185-191，按钮触发 ⇒
     *          setText(计算结果)，**无截断**⇒与①的 60 字截断形状互补）；
     *   读侧  MainWindow::onClipboardChanged（main_window.cpp:854，dataChanged 连接 :445，
     *          过滤链：开关→单行→四前缀→扩展名白名单→lastUrl 去重→QMessageBox）。
     * ⚠ 读侧不建 MainWindow、不起真服务：offscreen 下 dataChanged 是否 fire 未证 ⇒
     *   信号面登记为缺口；这里用 QMetaObject::invokeMethod 直接驱动 handler，
     *   **判据=用户看得见的量：弹没弹那个 Yes/No 框**（模态拦截槽在 exec 内关框放行，
     *   K3 同款"exec 内才有控制权"教训），比读私有成员强。
     * 夹具先清剪贴板再设唯一串：offscreen 上 setText 可能不落系统 ⇒ "写入后读回"的
     *   正例对照就是"新串绝不等于旧串"本身。 */
    if (qEnvironmentVariableIsSet("IDM_CLIPBOARD_PROBE")) {
        const QString dir = QDir::tempPath() + QStringLiteral("/idm-clipboard-probe-")
                            + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dir);
        qputenv("IDM_DATA_DIR", dir.toUtf8());

        bool c1 = false, c1b = false, c2 = false, c3 = false, c4 = false;
        QString d1, d3, d4;

        // ── C1/C1b：历史对话框双击复制（含 >60 字符的截断标签形状）──
        {
            const QString urlA = QStringLiteral("https://ex.test/a.zip");
            const QString urlB = QStringLiteral("https://ex.test/")
                                 + QString(70, QLatin1Char('x')) + QStringLiteral(".bin");
            { HistoryStore& hs = HistoryStore::instance();
              hs.init(dir + QStringLiteral("/clip_history.db"));   // MainWindow 之前：显式开库
              hs.recordAdded(urlA, QStringLiteral("a.zip"), dir);
              hs.recordAdded(urlB, QStringLiteral("b.bin"), dir); }
            HistoryDialog hd;
            hd.move(-32000, -32000);
            hd.show();
            QCoreApplication::processEvents();
            auto* tbl = hd.findChild<QTableWidget*>();
            int rowA = -1, rowB = -1;
            if (tbl) for (int r = 0; r < tbl->rowCount(); ++r) {
                const QTableWidgetItem* it = tbl->item(r, 1);
                if (!it) continue;
                if (it->text() == urlA) rowA = r;
                if (it->text() == urlB) rowB = r;
            }
            QApplication::clipboard()->setText(QStringLiteral("__sentinel_pre_c1__"));
            bool okA = false;
            if (rowA >= 0) {
                QMetaObject::invokeMethod(&hd, "onRowDoubleClicked", Q_ARG(int, rowA), Q_ARG(int, 0));
                QCoreApplication::processEvents();
                okA = (QApplication::clipboard()->text() == urlA);
            }
            const QString labelA = [&]{
                for (QLabel* l : hd.findChildren<QLabel*>())
                    if (l->text().startsWith(QStringLiteral("已复制链接"))) return l->text();
                return QString();
            }();
            bool okB = false;
            if (rowB >= 0) {
                QMetaObject::invokeMethod(&hd, "onRowDoubleClicked", Q_ARG(int, rowB), Q_ARG(int, 0));
                QCoreApplication::processEvents();
                okB = (QApplication::clipboard()->text() == urlB);   // 内容不截断，只有标签截
            }
            const QString labelB = [&]{
                for (QLabel* l : hd.findChildren<QLabel*>())
                    if (l->text().startsWith(QStringLiteral("已复制链接"))) return l->text();
                return QString();
            }();
            c1 = okA && !labelA.isEmpty() && labelA.contains(urlA);
            // 产品形状（history_dialog.cpp:131-132）：url>60 ⇒ 标签只留 left(60)+「…」。
            // 判别式必须按这个形状来：截断点**落在 x 族内部**（body[:60] 里有 46 个连续 x），
            // 所以"标签不含 20 连 x"是错判据（第一版在此恒红）；正确的牙是
            //   ① 剪贴板逐字==全长 URL（写入层不截断）＋ ② 标签严格短于 URL（显示层截断）。
            c1b = okB && labelB.startsWith(QStringLiteral("已复制链接: "))
                  && labelB.length() < urlB.length();
            d1 = QStringLiteral("rowA=%1 rowB=%2 读回A=%3 标签A=「%4」读回B=%5 "
                                "标签B长度=%6/URL长度=%7")
                     .arg(rowA).arg(rowB).arg(okA ? 1 : 0).arg(labelA.left(50))
                     .arg(okB ? 1 : 0).arg(labelB.length()).arg(urlB.length());
            hd.hide();
            HistoryStore::instance().clear();
        }
        printf("%s [C1] 历史对话框双击⇒剪贴板逐字==该行 URL 且标签报出该链接（写入成功+格式=纯文本URL）\n",
               c1 ? "[PASS]" : "[FAIL]");
        printf("    [C1 读数] %s\n", d1.toUtf8().constData());
        printf("%s [C1b] >60 字符 URL：剪贴板内容不截断、仅提示标签折短（截断只属于显示层）\n",
               c1b ? "[PASS]" : "[FAIL]");

        // ── C2：哈希对话框「复制结果」（产品里按钮实名，:82）──
        // 两面：C2a 空结果⇒**不**改写（早退守卫）；C2b 有真值⇒逐字写入并读回。
        // 只留 C2a 的话 M-⑩b（摘掉 setText）不会红——期望"没变化"的夹具给不了"应当写"的牙，
        // 所以正例面必须自己造一个已算出结果的对话框（与 ⑨ 同一形状：真文件 + certutil 标准值）。
        {
            const QString hdir = QDir::tempPath() + QStringLiteral("/idm-clip-hash-")
                                 + QString::number(QDateTime::currentMSecsSinceEpoch());
            QDir().mkpath(hdir);
            const QString hp = hdir + QStringLiteral("/fixture.bin");
            { QFile fo(hp); if (fo.open(QIODevice::WriteOnly)) { fo.write(QByteArrayLiteral("abcde")); fo.close(); } }

            HashVerifyDialog hv(nullptr, hp);
            hv.move(-32000, -32000);
            hv.show();
            QCoreApplication::processEvents();
            QList<QPushButton*> btns = hv.findChildren<QPushButton*>();
            QPushButton* copyBtn = nullptr;
            for (QPushButton* b : btns)
                if (b->text() == QStringLiteral("复制结果")) { copyBtn = b; break; }   // 逐字全等（⑨ 教训）
            QApplication::clipboard()->setText(QStringLiteral("__sentinel_pre_c2__"));
            bool okEmpty = false;
            if (copyBtn) {
                copyBtn->click();
                QCoreApplication::processEvents();
                okEmpty = (QApplication::clipboard()->text() == QStringLiteral("__sentinel_pre_c2__"));
                // 空结果时 onCopyComputed 早退（hash.isEmpty() return）⇒ 剪贴板保持原值。
            }
            // 先算出真摘要（SHA-256 of "abcde"），再点复制 ⇒ 剪贴板必须逐字等于该值
            auto* combo = hv.findChild<QComboBox*>();
            QList<QLabel*> labels = hv.findChildren<QLabel*>();
            QLabel* resultLab = nullptr;
            for (QLabel* l : labels)
                if (l->styleSheet().contains(QStringLiteral("monospace"))) { resultLab = l; break; }
            bool okFull = false;
            const QString sha = QStringLiteral("36bbe50ed96841d10443bcb670d6554f0a34b761be67ec9c4a8ad2c0c44ca42c");
            if (combo && resultLab) {
                const int idx = combo->findText(QStringLiteral("SHA-256"));
                if (idx >= 0) combo->setCurrentIndex(idx);
                for (QPushButton* b : btns)
                    if (b->text().contains(QStringLiteral("计算并校验"))) { b->click(); break; }
                QCoreApplication::processEvents();
                QApplication::clipboard()->setText(QStringLiteral("__sentinel_pre_c2b__"));
                copyBtn->click();
                QCoreApplication::processEvents();
                okFull = (QApplication::clipboard()->text() == sha)
                      && (resultLab->text() == sha);   // 读的字段≠写的字段⇒变异会漏
            }
            c2 = (copyBtn != nullptr) && okEmpty && okFull;
            printf("%s [C2] 校验对话框存在「复制结果」按钮（逐字全等）；空结果不改写、有结果逐字写入并读回\n",
                   c2 ? "[PASS]" : "[FAIL]");
            printf("    [C2 读数] 按钮找到=%d 空结果未改写=%d 有结果逐字写入=%d 结果标签=%s\n",
                   copyBtn ? 1 : 0, okEmpty ? 1 : 0, okFull ? 1 : 0,
                   resultLab ? resultLab->text().toUtf8().constData() : "<无 monospace 标签>");
            hv.hide();
            QDir(hdir).removeRecursively();
        }

        // ── C3/C4：onClipboardChanged 过滤链（Yes/No 框当证人）──
        {
            struct Spy : QObject {
                int seen = 0;
                QString firstText;
                bool eventFilter(QObject* o, QEvent* e) override {
                    auto* mb = qobject_cast<QMessageBox*>(o);
                    if (mb && e->type() == QEvent::Show
                        && mb->windowTitle() == QStringLiteral("检测到下载链接")) {
                        ++seen;
                        if (firstText.isEmpty()) {
                            // 正文模板 =「剪贴板中检测到下载链接：\n\n%1\n\n是否添加下载任务？」
                            // ⇒ 取**第一条非空行**才是 URL（第一版只跳到第一个 '\n'，
                            //   把「\n\n」的第二个换行之后整段都留下 ⇒ 逐字比较恒不等）。
                            const QStringList lines = mb->text().split(QLatin1Char('\n'));
                            for (const QString& ln : lines) {
                                const QString t = ln.trimmed();
                                if (!t.isEmpty() && !t.startsWith(QStringLiteral("剪贴板中"))) {
                                    firstText = t;
                                    break;
                                }
                            }
                        }
                        QTimer::singleShot(0, mb, [mb]{ mb->done(QMessageBox::No); });
                    }
                    return false;
                }
            };
            Spy spy;
            qApp->installEventFilter(&spy);

            Settings st; st.load();
            st.setClipboardMonitor(true);
            QStringList exts = st.capturedExtensions();
            if (!exts.contains(QStringLiteral("clipzip"))) exts << QStringLiteral("clipzip");
            st.setCapturedExtensions(exts);
            st.save();

            // MainWindow 需要建（构造会起 IPC/定时器等）——本面接受它：K1 闭环证明 show 过的
            // MainWindow 可控；数据目录已隔离、管道名走 IDM_TEST_PIPE pid 隔离（:204 扩面）。
            qputenv("IDM_TEST_PIPE",
                    QStringLiteral("idm-clip-%1").arg(QCoreApplication::applicationPid()).toUtf8());
            {
                MainWindow w;
                w.move(-32000, -32000);
                w.show();
                QCoreApplication::processEvents();

                const QString goodUrl = QStringLiteral("https://ex.test/good.CLIPZIP");
                // before 必须在 setText **之前**取：setText 会让 Qt 自己 fire dataChanged，
                // 于是 onClipboardChanged 可能已经弹出第一个框（第一版在 setText 之后才快照，
                // 把这次命中算给了 invokeMethod ⇒ 差分恒 0 ⇒ 明明弹了却报"命中弹窗=0"）。
                int before = spy.seen;
                QApplication::clipboard()->setText(goodUrl);
                QCoreApplication::processEvents();
                QMetaObject::invokeMethod(&w, "onClipboardChanged", Qt::DirectConnection);
                QCoreApplication::processEvents();
                // 两面判据：① 恰好一个新框；② firstText 逐字==URL。
                // ⚠ 若本轮一个框都没见过（before==0 && seen==0），firstText 是空串——
                //   那时必须点名"量具到不了"而不是让 c3 静默为假（0 有两种：现场没有 / 量具瞎）。
                c3 = (spy.seen == before + 1) && (spy.firstText == goodUrl);
                d3 = QStringLiteral("命中弹窗=%1 首见文本=「%2」期望=「%3」 本轮前累计=%4"
                                    "（大写扩展名须匹配小写白名单=toLower 归一）")
                         .arg(spy.seen - before).arg(spy.firstText, goodUrl).arg(before);

                int b2 = spy.seen;
                QApplication::clipboard()->setText(QStringLiteral("hello world"));
                QMetaObject::invokeMethod(&w, "onClipboardChanged", Qt::DirectConnection);
                QApplication::clipboard()->setText(QStringLiteral("https://ex.test/other.pptx"));
                QMetaObject::invokeMethod(&w, "onClipboardChanged", Qt::DirectConnection);
                QApplication::clipboard()->setText(goodUrl);   // 与 lastUrl 相同 ⇒ 去重必须吞掉
                QMetaObject::invokeMethod(&w, "onClipboardChanged", Qt::DirectConnection);
                QCoreApplication::processEvents();
                c4 = (spy.seen == b2);
                d4 = QStringLiteral("三种噪声（非链接/白名单外扩展/重复链接）新增弹窗=%1（期望 0）"
                                    "；C3 已见弹窗累计=%2")
                         .arg(spy.seen - b2).arg(b2);
                w.hide();
            }
        }
        printf("%s [C3] 合法下载链接进 onClipboardChanged ⇒ 弹「检测到下载链接」且正文含该 URL 原文\n",
               c3 ? "[PASS]" : "[FAIL]");
        printf("    [C3 读数] %s\n", d3.toUtf8().constData());
        printf("%s [C4] 负例三连（非链接/白名单外扩展/与上次相同）⇒ 一次都不弹\n",
               c4 ? "[PASS]" : "[FAIL]");
        printf("    [C4 读数] %s\n", d4.toUtf8().constData());

        const bool okAll = c1 && c1b && c2 && c3 && c4;
        printf("%s [CLIPBOARD 汇总] c1=%d c1b=%d c2=%d c3=%d c4=%d\n",
               okAll ? "[PASS]" : "[FAIL]", c1, c1b, c2, c3, c4);
        QDir(dir).removeRecursively();
        return okAll ? 0 : 1;
    }

    /* ── 诊断模式：IDM_DRAGDROP_PROBE=1（⑫ A4 R2/R3，TDD：修复落地前恒红）──
     * ⑪ 审计出的两条 A 类风险此前**零证人**（U1：全仓 tools/ 里 dragEnter|dropEvent|QMimeData = 0 命中），
     * 所以本轮先写必红的夹具，再改产品——顺序不许倒。
     *   R2 main_window.cpp:972 `url = mime->text().trimmed()` 之后没有任何长度上限 ⇒
     *      MB 级字符串一路进单行编辑器与 URL 解析；
     *   R3 :985-988 只有成功分支（`if (id > 0) Log::info(...)`），失败静默 ⇒
     *      用户点完确定什么都没发生、日志无从区分。IPC/web 侧同类路径是有 warn 的
     *      （addTaskFromUrl:1754-1757），这里对齐它的措辞形状。
     * 判据三条各印自己的读数：
     *   D1 正例对照：正常短 URL 拖进来 ⇒ 必须弹出预填该 URL 的新建任务对话框
     *      （期望非 0 的牙；没有它，"drop 一律什么都不做"也能让 D2/D3 蒙过）。
     *   D2 R2：超长文本（3 MiB）拖进来 ⇒ 不得把整串交给对话框，且必须留一条可区分的告警。
     *   D3 R3：走到 internalAddTask 真失败的那一支（空 URL ⇒ dlmgr_add 在 download_core.c:1529
     *      `if (!g_initialized || !url || *url=='\00') return -1`，这条失败支在健康树上可达，
     *      不是"和自己比"的不可达变异）⇒ 必须有一条 warn。
     * ⚠ 模态拦截沿用 K3/⑩ 的形状：queued 触发 + 内层轮询 + qApp 全局守卫收尸，绝不留 exec 无人关。
     * ⚠ 数据目录与管道都按 pid 隔离（IDM_TEST_PIPE 扩面），零落盘零触网：只建任务不 start。 */
    if (qEnvironmentVariableIsSet("IDM_DRAGDROP_PROBE")) {
        qputenv("IDM_TEST_PIPE",
                QStringLiteral("idm-dd-%1").arg(QCoreApplication::applicationPid()).toUtf8());
        const QString dirPg = QDir::tempPath() + QStringLiteral("/idm-dragdrop-probe-")
                              + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dirPg);
        qputenv("IDM_DATA_DIR", dirPg.toUtf8());

        // 日志捕获：Log::warn 走 qWarning ⇒ 装一个 handler 就能数到，不需要产品加计数器
        static QStringList g_warns;
        auto* oldHandler = qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& m) {
            if (t == QtWarningMsg) g_warns << m;
        });

        bool d1 = false, d2 = false, d3 = false;
        QString dd1, dd2, dd3;

        MainWindow w;
        w.move(-32000, -32000);
        w.show();
        QCoreApplication::processEvents();

        /* 发一次 drop（K3 同款形状，原因也一模一样）：
         *   dropEvent 里那句 dlg.exec() 是**阻塞**的 ⇒ 若直接 sendEvent，控制权永远进不到
         *   我们的采样器，探针当场挂死（第一版就是这么 rc=124 卡十分钟，不是产品慢）。
         *   正解：把"发事件"排进队列，由外层 QEventLoop 驱动；exec 起来后挂在 qApp 上的
         *   守卫定时器才会 fire ⇒ 采样、抓预填文本、替它收尸都在 exec 之内完成。
         * ⚠ mime 必须在 outer.exec() 期间保持存活 ⇒ 调用方持有，这里只借指针。 */
        struct DropRun { int peak = 0; int closed = 0; int accepted = -1; QString prefill; QString diag; QString wrongDlg; };
        auto sendDrop = [&](const QMimeData* mime, DropRun& out) -> void {
            out = DropRun{};
            const QPointF at(w.geometry().center());
            auto countDlg = []() {
                int n = 0;
                for (QWidget* x : QApplication::topLevelWidgets())
                    if (qobject_cast<NewTaskDialog*>(x) && x->isVisible()) ++n;
                return n;
            };
            /* 守卫拿到对话框时**必须点「确定」而不是 reject()**：
             *   reject() 会让 exec() 返回 Rejected ⇒ dropEvent 里那段 internalAddTask 根本不执行
             *   ⇒ D3 永远看不到失败支（第一版 D3 恒红就是这个原因，不是产品没 warn）。
             *   点 OK 走的是 onAccepted → accept()，与真用户按回车同一条路。 */
            auto grabAndClose = [&]() {
                for (QWidget* x : QApplication::topLevelWidgets()) {
                    if (auto* d = qobject_cast<NewTaskDialog*>(x)) {
                        if (out.prefill.isEmpty()) {
                            const QList<QLineEdit*> edits = d->findChildren<QLineEdit*>();
                            if (!edits.isEmpty()) out.prefill = edits.first()->text();
                        }
                        ++out.closed;
                        bool clicked = false;
                        if (auto* bb = d->findChild<QDialogButtonBox*>()) {
                            if (QAbstractButton* ok = bb->button(QDialogButtonBox::Ok)) {
                                ok->click();
                                clicked = true;
                            }
                        }
                        if (!clicked) d->reject();      // 找不到 OK 才退回 reject（会在 diag 里显出 accepted=0）
                        // ⚠ NewTaskDialog 把 result() 覆盖成返回 NewTaskInput（不是 QDialog 的 int），
                        //    所以这里必须走基类取对话框状态。
                        out.accepted = (static_cast<QDialog*>(d)->result() == QDialog::Accepted) ? 1 : 0;
                        return true;
                    }
                    // 挡在路上的别的模态窗（⑩/K3 教训：不点名就会把挂死误读成产品慢）
                    if (x->isModal() && x->isVisible() && !qobject_cast<MainWindow*>(x)) {
                        out.wrongDlg = x->metaObject()->className();
                        x->close();
                    }
                }
                return false;
            };

            QEventLoop outer;
            QTimer hardStop; hardStop.setSingleShot(true);
            QObject::connect(&hardStop, &QTimer::timeout, &outer, [&]() {
                for (int g = 0; g < 50 && countDlg() > 0; ++g) { grabAndClose(); QCoreApplication::processEvents(QEventLoop::AllEvents, 20); }
                out.diag += QStringLiteral(" HARDSTOP");
                outer.quit();
            });
            QTimer spy;
            QObject::connect(&spy, &QTimer::timeout, qApp, [&]() {
                const int n = countDlg();
                if (n > out.peak) out.peak = n;
                if (n > 0) { grabAndClose(); return; }
                if (out.closed >= 1) outer.quit();
            });
            spy.start(5);
            hardStop.start(8000);

            // 触发排在队列里：enter+drop 都由 outer.exec() 驱动（sendEvent 直调会内联进 exec ⇒ 守卫够不着）
            QDragEnterEvent ent(at.toPoint(), Qt::CopyAction | Qt::MoveAction, mime,
                                Qt::LeftButton, Qt::NoModifier);
            QDropEvent ev(at, Qt::CopyAction | Qt::MoveAction, mime,
                          Qt::LeftButton, Qt::NoModifier);
            bool enterAccepted = false;
            QMetaObject::invokeMethod(&w, [&]() {
                QApplication::sendEvent(&w, &ent);
                enterAccepted = ent.isAccepted();
                QApplication::sendEvent(&w, &ev);
            }, Qt::QueuedConnection);
            outer.exec();
            spy.stop(); hardStop.stop();
            for (int i = 0; i < 60 && countDlg() > 0; ++i) QCoreApplication::processEvents();
            out.diag = QStringLiteral("enterAccepted=%1 dropAccepted=%2 peak=%3 closed=%4 accepted=%5")
                           .arg(enterAccepted ? 1 : 0).arg(ev.isAccepted() ? 1 : 0)
                           .arg(out.peak).arg(out.closed).arg(out.accepted)
                         + (out.wrongDlg.isEmpty() ? QString() : QStringLiteral(" 冒名窗=%1").arg(out.wrongDlg));
        };

        // ── D1 正例对照 ──
        {
            QMimeData mime;
            mime.setUrls({QUrl(QStringLiteral("https://ex.test/dd-ok.zip"))});
            DropRun r; sendDrop(&mime, r);
            const QString pre = r.prefill;
            d1 = (r.peak == 1) && (r.closed >= 1) && pre == QStringLiteral("https://ex.test/dd-ok.zip");
            dd1 = QStringLiteral("对话框峰值=%1 已关=%2 预填=「%3」期望=「https://ex.test/dd-ok.zip」｜%4")
                      .arg(r.peak).arg(r.closed).arg(pre.left(60)).arg(r.diag);
        }

        // ── D2 R2：超长文本 ──
        {
            const int huge = 3 * 1024 * 1024;
            QMimeData mime;
            mime.setText(QStringLiteral("http://ex.test/") + QString(huge, QLatin1Char('a')));
            g_warns.clear();
            DropRun r; sendDrop(&mime, r);
            const QString pre = r.prefill;
            const bool handedOverWhole = (pre.length() >= huge);   // 整串塞给单行编辑器＝R2 原状
            bool warned = false;
            for (const QString& x : g_warns)
                if (x.contains(QStringLiteral("拖放"))) { warned = true; break; }
            // 判据：要么**根本没开对话框**（直接拒这条 drop），要么开的时候文本有界。
            // 两种都算修好；只有"整串交出去"才算没修。
            const bool refusedOrBounded = (r.peak == 0) || (!pre.isEmpty() && pre.length() <= 8192);
            d2 = !handedOverWhole && refusedOrBounded && warned;
            dd2 = QStringLiteral("输入长度=%1 交给对话框的长度=%2 对话框峰值=%3 有界或拒=%4 告警含「拖放」=%5｜%6")
                      .arg(huge + 15).arg(pre.length()).arg(r.peak)
                      .arg(refusedOrBounded ? 1 : 0).arg(warned ? 1 : 0).arg(r.diag);
        }

        // ── D3 R3：internalAddTask 真失败那一支必须有 warn ──
        // 失败条件必须**本轮现场制造且可复算**，不靠"大概会失败"。引擎 dlmgr_add 的可达失败支两条：
        //   ① download_core.c:1529 空串 url ⇒ 对话框 onAccepted 要求 url 非空（new_task_dialog.cpp:339），走不到；
        //   ② download_core.c:1532 `g_task_count >= MAX_TASKS`（download_core.h:25 = **128**）⇒ 可复算。
        // ⚠ 上一版这里用「GUI addTaskFromUrl 灌到模型不再增长」当满额判据，实测满额=0——
        //    两个错叠在一起：(a) `w.findChild<TaskListModel*>()` 永远返回 nullptr（模型是成员不是子 QObject）；
        //    (b) 更要紧的是**立即开始**的任务会占并发槽、并不都进引擎队列计数，光靠 GUI 灌不满 128。
        //    所以改为直接按引擎自己的口径灌：dlmgr_add 填到返回 -1，再用 dlmgr_list 现读条数当证人。
        {
            // 满额前置必须**与前面几段留下的槽位无关**：D1 自己就成功建过 1 个任务，
            // 所以"灌到拒绝"时 filled 会少 1（实测 127/128）。这里不猜容量，
            // 一直灌到 dlmgr_add 拒绝为止，再用 dlmgr_list 现读真实占用当证人。
            int filled = 0, firstRejectAt = -1;
            for (int i = 0; i < 400; ++i) {
                const QString u = QStringLiteral("http://127.0.0.1:1/fill%1.bin").arg(i);
                const int id = dlmgr_add(u.toUtf8().constData(), dirPg.toUtf8().constData(),
                                         nullptr, 0, nullptr, nullptr, nullptr, nullptr);
                if (id > 0) ++filled;
                else { firstRejectAt = i; break; }
            }
            int listed[256];
            const int nListed = dlmgr_list(listed, 256);
            g_warns.clear();
            QMimeData mime;
            mime.setText(QStringLiteral("http://ex.test/after-full.zip"));
            DropRun r; sendDrop(&mime, r);
            bool warnedFail = false;
            for (const QString& x : g_warns)
                if (x.contains(QStringLiteral("添加任务失败"))) { warnedFail = true; break; }
            // 前置成立才让 d3 说话：引擎确实被灌到拒绝（否则这条断言是哑的，如实登记）
            d3 = (firstRejectAt >= 0 && nListed >= 100) && (r.peak == 1) && warnedFail;
            dd3 = QStringLiteral("引擎灌入=%1 首个拒绝下标=%2 dlmgr_list=%3(>=100 才算满额前置成立) "
                                 "对话框峰值=%4 预填=「%5」warn 含「添加任务失败」=%6 本轮 warn 行数=%7｜%8")
                      .arg(filled).arg(firstRejectAt).arg(nListed).arg(r.peak)
                      .arg(r.prefill.left(40)).arg(warnedFail ? 1 : 0).arg(g_warns.size()).arg(r.diag);
            for (int i = 0; i < nListed; ++i) dlmgr_cancel(listed[i]);   // 收摊：别把槽位留给下一段
        }

        w.hide();
        qInstallMessageHandler(oldHandler);

        printf("%s [D1] 正例对照：正常 URL 拖入 ⇒ 恰好弹出一个新建任务对话框且预填逐字==该 URL\n",
               d1 ? "[PASS]" : "[FAIL]");
        printf("    [D1 读数] %s\n", dd1.toUtf8().constData());
        printf("%s [D2] R2：3MiB 文本拖入 ⇒ 不得整串交给编辑器，须有界并留可区分告警\n",
               d2 ? "[PASS]" : "[FAIL]");
        printf("    [D2 读数] %s\n", dd2.toUtf8().constData());
        printf("%s [D3] R3：新增任务失败那一支 ⇒ 必须有一条 warn（对齐 IPC 侧形状）\n",
               d3 ? "[PASS]" : "[FAIL]");
        printf("    [D3 读数] %s\n", dd3.toUtf8().constData());

        // exe 身份印进探针自己的输出：⑫ 实测 M-C 与 M-D 两面**红的是同一个判据、读数逐字相同**，
        // 两份归档文件因此字节一致——光看日志分不出是哪枚二进制跑的。带上自身哈希后，
        // 每面原文都能独立指向一枚产物（算法与台账一致：sha256 前 12）。
        {
            QFile self(QCoreApplication::applicationFilePath());
            QString h = QStringLiteral("-");
            if (self.open(QIODevice::ReadOnly)) {
                QCryptographicHash sha(QCryptographicHash::Sha256);
                if (sha.addData(&self)) h = QString::fromLatin1(sha.result().toHex().left(12));
                self.close();
            }
            printf("    [DRAGDROP 身份] exe_sha12=%s\n", h.toUtf8().constData());
        }
        const bool okDd = d1 && d2 && d3;
        printf("%s [DRAGDROP 汇总] d1=%d d2=%d d3=%d\n",
               okDd ? "[PASS]" : "[FAIL]", d1, d2, d3);
        QDir(dirPg).removeRecursively();
        return okDd ? 0 : 1;
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

    /* ── 诊断模式：IDM_PATHGUARD_PROBE=1（⑧ A1，最小夹具轮）──
     * IPC `add` 的 dir/filename 参数在**真 IpcServer 命令层**上的负例夹具。
     * 形状：独立 IpcServer + pid 私有管道（IDM_TEST_PIPE，:204 扩面）；
     *       addTask 回调**只记录转发值** ⇒ 零落盘、零触网、不建 MainWindow。
     * 三条各印读数：
     *   G1 dir 含 `..` ⇒ 必须被拒（success=false）或转发值不含越界分量；
     *   G2 filename 含 `..\` ⇒ 转发值不得保留路径分隔符；
     *   G3 正例对照：合法 dir/filename 必须 success=true 且逐字原样转发（期望非 0 的牙，
     *      没有它"全拒一切"也算过 G1/G2）。
     * ⚠ 当前源码下 G1/G2 **必红是设计目标**（A1 未修）；G3 绿钉住测具本身没坏。 */
    if (qEnvironmentVariableIsSet("IDM_PATHGUARD_PROBE")) {
        qputenv("IDM_TEST_PIPE",
                QStringLiteral("idm-pg-%1").arg(QCoreApplication::applicationPid()).toUtf8());
        const QString rootPg = QDir::tempPath() + QStringLiteral("/idm-pathguard-probe-")
                               + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(rootPg);
        qputenv("IDM_DATA_DIR", rootPg.toUtf8());

        struct Fwd { int hits = 0; QString url, name, dir; };
        Fwd fwd;
        IpcServer srv;
        srv.setAddTaskCallback([&fwd](const QString& url, const QString& filename,
                                      const QString& dir, int, const QString&,
                                      const QString&, const QString&) {
            ++fwd.hits; fwd.url = url; fwd.name = filename; fwd.dir = dir;
        });
        if (!srv.start()) {
            printf("[FAIL] PathGuard 探针起不了管道 %s\n", ipcServerName().toUtf8().constData());
            return 1;
        }

        auto askIpc = [&](const QJsonObject& reqIn, bool* okOut, QString* msgOut) -> bool {
            struct Res { bool done = false, replied = false; QString reply; };
            auto res = std::make_shared<Res>();
            QThread* client = QThread::create([res, reqIn] {
                QLocalSocket sk;
                sk.connectToServer(ipcServerName());
                if (sk.waitForConnected(2000)) {
                    sk.write(QJsonDocument(reqIn).toJson(QJsonDocument::Compact));
                    sk.flush();
                    if (sk.waitForBytesWritten(2000) && sk.waitForReadyRead(3000)) {
                        res->reply = QString::fromUtf8(sk.readAll());
                        res->replied = !res->reply.isEmpty();
                    }
                    sk.disconnectFromServer();
                }
                res->done = true;
            });
            client->start();
            for (int i = 0; i < 300 && !res->done; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            client->wait(5000);
            delete client;
            if (!res->replied) return false;
            const QJsonObject resp = QJsonDocument::fromJson(res->reply.toUtf8()).object();
            if (okOut) *okOut = resp.value(QStringLiteral("success")).toBool(false);
            if (msgOut) *msgOut = resp.value(QStringLiteral("message")).toString();
            return true;
        };

        bool g1 = false, g2 = false, g3 = false;
        QString d1, d2, d3;
        {   // G1：dir 越界
            fwd.hits = 0;
            QJsonObject req;
            req[QStringLiteral("command")] = QStringLiteral("add");
            req[QStringLiteral("url")] = QStringLiteral("http://127.0.0.1:1/a.bin");
            req[QStringLiteral("filename")] = QStringLiteral("a.bin");
            req[QStringLiteral("dir")] = QStringLiteral("..\\evil_dir_a1");
            bool ok = false; QString msg;
            const bool got = askIpc(req, &ok, &msg);
            const bool escaped = (fwd.hits > 0) && fwd.dir.contains(QStringLiteral(".."));
            g1 = got && (!ok || !escaped);
            d1 = QStringLiteral("got=%1 success=%2 转发dir=「%3」hits=%4 msg=「%5」")
                     .arg(got ? 1 : 0).arg(ok ? 1 : 0).arg(fwd.dir).arg(fwd.hits).arg(msg.left(40));
        }
        {   // G2：filename 带穿越分量
            fwd.hits = 0;
            QJsonObject req;
            req[QStringLiteral("command")] = QStringLiteral("add");
            req[QStringLiteral("url")] = QStringLiteral("http://127.0.0.1:1/b.bin");
            req[QStringLiteral("filename")] = QStringLiteral("..\\..\\evil_b.bin");
            req[QStringLiteral("dir")] = rootPg;
            bool ok = false; QString msg;
            const bool got = askIpc(req, &ok, &msg);
            const bool backslashAlive = (fwd.hits > 0) && fwd.name.contains(QLatin1Char('\\'));
            g2 = got && (!ok || !backslashAlive);
            d2 = QStringLiteral("got=%1 success=%2 转发name=「%3」含反斜杠=%4")
                     .arg(got ? 1 : 0).arg(ok ? 1 : 0).arg(fwd.name).arg(backslashAlive ? 1 : 0);
        }
        {   // G3：正例必须原样转发且成功
            fwd.hits = 0;
            QJsonObject req;
            req[QStringLiteral("command")] = QStringLiteral("add");
            req[QStringLiteral("url")] = QStringLiteral("http://127.0.0.1:1/c.bin");
            req[QStringLiteral("filename")] = QStringLiteral("ok_c.bin");
            req[QStringLiteral("dir")] = rootPg;
            bool ok = false; QString msg;
            const bool got = askIpc(req, &ok, &msg);
            g3 = got && ok && fwd.hits == 1 && fwd.dir == rootPg
                 && fwd.name == QStringLiteral("ok_c.bin");
            d3 = QStringLiteral("got=%1 success=%2 hits=%3 dir==root=%4 name=「%5」")
                     .arg(got ? 1 : 0).arg(ok ? 1 : 0).arg(fwd.hits)
                     .arg(fwd.dir == rootPg ? 1 : 0).arg(fwd.name);
        }
        printf("%s [G1] IPC add 的 dir 含 .. ⇒ 必须被拒或被收敛（不得原样转发越界目录）\n",
               g1 ? "[PASS]" : "[FAIL]");
        printf("    [G1 读数] %s\n", d1.toUtf8().constData());
        printf("%s [G2] IPC add 的 filename 含 ..\\ ⇒ 转发值不得保留路径分隔符\n",
               g2 ? "[PASS]" : "[FAIL]");
        printf("    [G2 读数] %s\n", d2.toUtf8().constData());
        printf("%s [G3] 合法 dir/filename 必须 success=true 且逐字原样转发（清洗不许伤正例）\n",
               g3 ? "[PASS]" : "[FAIL]");
        printf("    [G3 读数] %s\n", d3.toUtf8().constData());
        {   // IpcServer 没有 stop()：摘回调防悬垂引用，再摘 endpoint；srv 随作用域出栈
            srv.setAddTaskCallback(nullptr);
            QLocalServer::removeServer(ipcServerName());
        }
        QDir(rootPg).removeRecursively();
        return (g1 && g2 && g3) ? 0 : 1;
    }

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

    /* ── 诊断模式：IDM_FORWARD_PROBE=1 ──
     * forwardToGui 的四条出口各自要留下什么证据。这条探针是为一个真实缺陷写的：
     * CLI 与「再点一次图标」的副实例在失败时**只留下退出码 1，stderr 是空的**。
     * CI 第 11 步的 remote-help 因此红过之后查不下去 —— annotations 里
     * stdout/stderr 双双「<空>」，连「连上了没」「等了几秒」都看不出来。
     *
     * 四条出口分别断言（假服务由本探针自己起，管道名带 pid，绝不碰用户正在跑的实例）：
     *   ① 没人监听            → NotConnected，而且 stderr 必须**为空**：
     *      这是「本机根本没开 GUI」的正常首启路径，这时候唠叨一句「IPC 失败」
     *      等于给每一次正常用法都加一条假报警。
     *   ② 连上、送出、不回话  → Failure，stderr 要说得出「没有回话」，
     *      并且服务端只看到**一次**连接 —— add 有副作用，重发就是下两遍。
     *   ③ 对方回了失败        → Failure，stderr 是**对方给的原因**，而不是「没有回话」。
     *   ④ 对方回了成功        → Success，stderr 干净。
     *   ⑤ activate 即使幂等、即使服务端第一条故意装不响，也**只发一次**：
     *      CI 实测到「请求被处理了、回话落在客户端 3 秒死线之后」，那种时刻重发
     *      等于让同一件事做两遍（add 就是下两份）。曾经的"幂等就再问一次"已撤回，
     *      根因是 GUI 线程被组件自愈的 tar 解压阻塞（见 utils/off_thread.cpp）。
     * ②③④⑤ 都要求 stderr 非空或有指定内容，所以「把报错删掉」会立刻红。 */
    if (qEnvironmentVariableIsSet("IDM_FORWARD_PROBE")) {
        const QString base = QStringLiteral("idm-forward-probe-%1")
                                 .arg(QCoreApplication::applicationPid());

        struct Capture {                 // 把 stderr 临时挪到临时文件（进程级，主线程做）
            int saved = -1;
            QString path;
            explicit Capture(const QString& p) : path(p) {
                saved = dup(fileno(stderr));
                freopen(p.toLocal8Bit().constData(), "w", stderr);
            }
            QString finish() {
                fflush(stderr);
                if (saved >= 0) { dup2(saved, fileno(stderr)); close(saved); saved = -1; }
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) return QString();
                const QByteArray raw = f.readAll();
                f.close();
                QFile::remove(path);
                /* QTextStream 往 stderr 写的是 UTF-8，不是本地代码页：按 fromLocal8Bit
                   解会把中文解成一串假字符，于是「说得出原因」这条断言永远匹配不上 ——
                   先红的是探针自己，不是被测代码。 */
                return QString::fromUtf8(raw);
            }
        };

        int bad = 0;
        /* ⚠ 偶发红的归因诊断（只在 IDM_FWD_DIAG=1 时开）：那次红看到 `waitForBytesWritten(2000)`
         *   返回 false、`errorString()` 是 "Unknown error"，但**没有状态码**——分不清是
         *   WriteError / TimedOut / Disconnect，也看不出管道此刻还在不在。
         *   本轮 5/5 + 5/5 复现不出来 ⇒ 判不了根因，先把"能一次定性的读数"装上。
         *   写句柄 1 而不是 cerr：cerr 正被下面的 Capture 重定向，会污染断言读数。 */
        const bool fwdDiag = qEnvironmentVariableIsSet("IDM_FWD_DIAG");
        const QStringList addArgs { QStringLiteral("add"), QStringLiteral("http://example.com/a.bin") };
        const QStringList helpArgs { QStringLiteral("help") };
        const QStringList actArgs { QStringLiteral("activate") };

        struct Case {
            QString name;
            QString pipe;                 // 空串 = 用探针的假服务
            QStringList args;
            int mode;                     // 0=silent 1=reject 2=ok 3=装不响、第二次才回话
            QString want;                 // stderr 必须包含（"<empty>" = 必须为空）
            ForwardResult wantResult;
            int wantConns;                // 服务端应当看到几次连接
        };

        /* 假服务：newConnection 里按当前 mode 决定「不回话 / 回失败 / 回成功」，
           并数着连接次数。silent 模式必须把 socket 留着不删，否则对端立刻看到断开。
           mode 3 复现 CI 实测到的那种现场：请求**被处理了**，但第一次的回话
           落在客户端 3 秒放弃之后 —— 这里用「第一条连接故意装不响」代表它。 */
        QLocalServer::removeServer(base);
        QLocalServer srv;
        if (!srv.listen(base)) {
            printf("[FAIL] 探针的假 IPC 服务起不来：%s\n", srv.errorString().toUtf8().constData());
            return 1;
        }
        std::atomic<int> mode { 0 };
        std::atomic<int> conns { 0 };
        QList<QLocalSocket*> held;
        /* ⚠⚠ 假服务必须**每条用例回到干净状态**：handler 按 `seen`（累计连接数）决定回不回话，
         *   mode 3 故意"第一条装不响"。我加诊断重放时多占了一次连接，而 `held` 里那条 socket
         *   永不清空 ⇒ 后面每条用例都被它劫持，case1~4 稳定全红。这个形状与被测代码无关，
         *   是我自己造的第二把半坏的门（见 §7 第 33 条）。 */
        auto resetPeer = [&] {
            for (QLocalSocket* c : held) c->deleteLater();
            held.clear();
            conns.store(0);
            QCoreApplication::processEvents();     // 让 deleteLater 真的落地
        };
        QObject::connect(&srv, &QLocalServer::newConnection, [&srv, &mode, &conns, &held] {
            while (srv.hasPendingConnections()) {
                QLocalSocket* c = srv.nextPendingConnection();
                if (!c) continue;
                const int seen = ++conns;
                const int m = mode.load();
                if (m == 0 || (m == 3 && seen == 1)) { held.append(c); continue; }   // 连上但不回话
                QJsonObject resp;
                if (m == 1) {
                    resp[QStringLiteral("success")] = false;
                    resp[QStringLiteral("message")] = QStringLiteral("任务不存在: 2147483647");
                } else {
                    resp[QStringLiteral("success")] = true;
                    resp[QStringLiteral("message")] = QStringLiteral("IDM Next 命令行：add/list/help …");
                }
                c->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                c->flush();
                c->deleteLater();
            }
        });

        const Case cases[] = {
            { u8"没人监听 ⇒ NotConnected，且不往 stderr 吐任何字", base + "-absent", helpArgs, 0, "<empty>", ForwardResult::NotConnected, 0 },
            { u8"连上但不回话 ⇒ Failure，stderr 说得出「没有回话」",   QString(),        addArgs,  0, u8"没有回话", ForwardResult::Failure, 1 },
            { u8"对方回了失败 ⇒ stderr 用对方给的原因",                QString(),        helpArgs, 1, u8"任务不存在", ForwardResult::Failure, 1 },
            { u8"对方回了成功 ⇒ Success 且 stderr 干净",               QString(),        helpArgs, 2, "<empty>", ForwardResult::Success, 1 },
            /* activate 是幂等的，但也**只发一次**：服务端可能已经处理完只是回话晚了，
               再问一次等于把同一件事做两遍。这条与第 2 条（add）一起把
               「绝不重发」钉死 —— 以后谁想靠加次数糊弄超时，这两条先红。 */
            { u8"activate 第一次装不响 ⇒ 仍然只发一次（绝不重发）",     QString(),        actArgs,  3, u8"没有回话", ForwardResult::Failure, 1 },
        };

        for (const Case& c : cases) {
            const QString target = c.pipe.isEmpty() ? base : c.pipe;
            mode.store(c.mode);
            resetPeer();

            ForwardResult result = ForwardResult::NotConnected;
            /* ⚠ 路径必须**每条用例唯一**：Capture 用 freopen 把**整个进程**的 fd 2 重定向，
             *   而 forwardToGui 跑在 worker 线程上。上一轮那次偶发红（结果=2 期望=1、
             *   stderr 里同时出现 Log::warn 与 QTextStream 两份"没写进去"）说明有第二个
             *   写者共用过这条管道；同名文件被 dup2 复原时截断 ⇒ 读数互相吞。
             *   现在每条用例一个文件，谁写的、写了什么按用例隔离，不再靠时间运气。 */
            const QString errPath = QDir::tempPath() + QStringLiteral("/idm_fwd_probe_%1_%2.log")
                                        .arg(QCoreApplication::applicationPid())
                                        .arg(&c - cases);
            Capture cap(errPath);
            std::atomic<bool> done { false };
            QThread* worker = QThread::create([&] {
                result = forwardToGui(c.args, true, target);
                done = true;
            });
            worker->start();
            for (int i = 0; i < 2400 && !done.load(); ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            worker->wait(20000);
            delete worker;
            const QString err = cap.finish();
            /* 超时未完成的显式读数：上面那个循环最多 2400×5ms≈12s，worker 若还卡着，
             * `result` 就停在初值 NotConnected(2) —— 那正是这次红看到的形状。没有这一行，
             * 下次再红就只能猜是"写失败"还是"根本没跑完"。 */
            const bool finishedInTime = done.load();

            /* 诊断态（IDM_FWD_DIAG=1）：用**探针自己的客户端**重放同一条用例，把状态码打到手。
             * ⚠⚠ 第一版把这个重放放在 worker **之前**，于是它自己就是元凶：假服务按 `seen`
             *   （累计连接数）决定回不回话，我的重放多占一次连接 ⇒ mode 3 那条"装不响"的分支
             *   被跳过、真 worker 变成第 2 次连接被 held 住不回话 ⇒ case 2/3/4 三连红（3/3 稳定）。
             *   现在：① 挪到 worker **之后**；② 先 reset conns，让计数只反映被测那一次；
             *   ③ 重放完把这条额外连接从服务端记账里摘掉（held 由本探针持有，不影响断言）。
             *   读数写句柄 1（stdout），不碰被 Capture 接管的 stderr。 */
            if (fwdDiag) {
                const int seenByWorker = conns.load();
                QLocalSocket probe;
                probe.connectToServer(target);
                const bool conn = probe.waitForConnected(500);
                qint64 wrote = -1;
                bool flushed = false, okBytes = false;
                int st = -1, errCode = -1;
                QString errStr;
                if (conn) {
                    wrote = probe.write(QByteArrayLiteral("{\"command\":\"help\"}"));
                    flushed = probe.flush();
                    okBytes = probe.waitForBytesWritten(2000);
                    st = static_cast<int>(probe.state());
                    errCode = static_cast<int>(probe.error());
                    errStr = probe.errorString();
                }
                fprintf(stdout, "[diag] case=%d worker看到=%d 重放: 连接=%d 写入字节=%lld flush=%d "
                        "写完确认=%d state=%d error=%d errStr=「%s」\n",
                        int(&c - cases), seenByWorker, conn ? 1 : 0,
                        (long long)wrote, flushed ? 1 : 0, okBytes ? 1 : 0, st, errCode,
                        errStr.toUtf8().constData());
                fflush(stdout);
                probe.disconnectFromServer();
                resetPeer();        // ⚠ 重放占的那条连接必须收摊，否则下一条用例被 held 劫持
            }

            bool ok = (result == c.wantResult);
            if (c.want == QLatin1String("<empty>"))
                ok = ok && err.trimmed().isEmpty();
            else
                ok = ok && err.contains(c.want);
            /* 连接次数就是「重发策略」的读数：
               add 必须 1 次（对方可能已经处理过，重发就是下两遍），
               activate 在第一次装不响之后必须问到第 2 次。 */
            const int seen = conns.load();
            ok = ok && (seen == c.wantConns);
            if (!ok) ++bad;

            printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", c.name.toUtf8().constData());
            if (!ok) {
                printf("       结果=%d 期望=%d 服务端看到连接=%d（期望 %d）worker按时跑完=%d stderr=%d 字节: %s\n",
                       static_cast<int>(result), static_cast<int>(c.wantResult), seen,
                       c.wantConns, finishedInTime ? 1 : 0,
                       int(err.size()),
                       err.toUtf8().constData());
            }
        }

        for (QLocalSocket* c : held) c->deleteLater();
        QLocalServer::removeServer(base);
        return bad == 0 ? 0 : 1;
    }


    /* ── 诊断模式：IDM_ACTIVATE_PROBE=1 ──
     * 被测契约：**activate 重复投递不产生第二次副作用**（幂等）。
     * 这条是被断言的，不是被推理的 —— 上一轮我只在对话里说过"它是置状态不是累加"，
     * 那等于没有；而且第一版探针有个真漏洞：第一条客户端故意不等回话就断开，
     * 于是"确实被处理了两次"从没被证明，测到的其实可能是"投递两次、只处理一次"。
     *
     * 现在分两段，各测各的：
     *   A 段（幂等）：两条客户端**各自读到 success** ⇒ 服务端确实把这条命令做了两遍；
     *      然后断言模型行数、引擎任务数、窗口可见/正常态与做之前一致
     *      ⇒ 两遍没有攒成双份（不建任务、不多开实例、不把窗口弄成别的状态）。
     *   B 段（排队代价）：第一条"连上、送出、不等回话就走"，量第二条的往返 ——
     *      回答"服务端一旦被拖住，后面的请求要不要排队"，也就是**为什么重发救不了这类 bug**。
     *      M8 变异（在 activate 处理里 msleep(2000)）把它打红：往返 5ms → 2021ms。
     * 所有判据取"用户看得见的量"，不读内部计数器。 */
    if (qEnvironmentVariableIsSet("IDM_ACTIVATE_PROBE")) {
        {
            QLocalSocket guard;
            guard.connectToServer(ipcServerName());
            const bool occupied = guard.waitForConnected(200);
            guard.disconnectFromServer();
            if (occupied) {
                printf("[FAIL] %s 已被别的 IDM Next 实例占用，请先关闭它再跑本探针\n",
                       ipcServerName().toUtf8().constData());
                return 1;
            }
        }

        bool ok1 = false, ok2 = false, ok3 = false, ok4 = false, ok5 = false, ok6 = false, ok7 = false;
        bool firstAbandonedAck = false;
        bool okIdleAck = false;
        qint64 okIdleBlockMs = -1;   // E 段：压在空转连接后面的真请求往返（判据量）
        bool okIdleAckE = false;     // E 段：那条真请求确实拿到了 success 回话
        int truncatedTurns = 0;   // D 段里"撞到每轮 20ms 预算"的次数（量具读数，不进判据分子）
        /* B 判据（§7-57）：N 轮里超过 T 的**轮数**，不是单轮最长。 */
        int overBudgetTurns = 0;
        int budgetSamples = 0;
        int budgetMsOut = 50;
        bool canaryCountedOut = false;
        bool canaryRequestedOut = false;
        /* 取对照数据用的口径开关：只改变**采样调用形状**，判据表达式与阈值 500 都不动。 */
        const bool roundCriterionCount =
            qEnvironmentVariableIsSet("IDM_ROUND_CRITERION")
            && qgetenv("IDM_ROUND_CRITERION").toLower() == "count";
        /* 两个读数分开命名，各说各的量（以前是一个数顶着另一句话）：
         *   worstEventLoopTurnMs —— D 段窗口里最长的一次 processEvents 轮次，= GUI 线程被独占多久；
         *     进判据（500ms 挂在它身上，与它实际测的东西一致）。
         *   idleBehindReplyMs —— 同一窗口里一条真请求从发出到拿到回话的往返，由**它自己的线程**计时；
         *     只进报告行（本轮首次测量、没有分布 ⇒ 给它配阈值就是拍数）。 */
        qint64 worstEventLoopTurnMs = -1;
        qint64 idleBehindReplyMs = -1;
        qint64 turnBehindMs = 0;     // 第二个读数自己那一轮里的最长轮次（与它同窗口，才能对得上）
        int nBehind = 0;             // 那一轮里一次取尽到的最大连接数
        bool okIdleReply = false;
        int stressAcks = 0, stressTotal = 0;
        qint64 roundTripMs = -1;
        int rowsBefore = -1, rowsAfter = -1;
        int engBefore = -1, engAfter = -1;

        {
            MainWindow w;
            /* 必须 show()：activate 分支调 showNormal()/raise()/activateWindow()，而 Qt 对
             * "还没显示过的窗口"调 showNormal() 等价于首次 show —— 隐藏态起步时第一遍投递
             * 会把窗口**显示出来**，ok3 的「仍可见、正常态」量的就不再是幂等。
             * offscreen 平台下这一步不产生任何真实绘制。 */
            w.show();
            QCoreApplication::processEvents();          /* singleShot(0) → IpcServer::start() */

            /* 前置：等到管道真的可连，而不是"泵一圈事件循环就算起好了"。
             * 一次 processEvents() 之后 listen 是否已经完成，取决于当轮机器负载 ——
             * 在闸门里连跑十几个模式之后，第一次投递就有拿不到 ack 的观测（40 次里 1 次量级）。
             * 测具自己不满足前置就该明说，不能让产品断言去背这个锅。 */
            bool pipeReady = false;
            for (int i = 0; i < 60 && !pipeReady; ++i) {
                QLocalSocket g;
                g.connectToServer(ipcServerName());
                pipeReady = g.waitForConnected(200);
                if (!pipeReady) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(50);
                }
                g.abort();
            }
            if (!pipeReady) {
                printf("[FAIL] 前置不成立：3 秒内 IPC 管道一直没开始监听（测具问题，不是产品断言）\n");
                return 1;
            }

            TaskListModel* model = w.findChild<TaskListModel*>();
            rowsBefore = model ? model->rowCount() : -1;
            {
                int ids0[64] = {0};
                engBefore = dlmgr_list(ids0, 64);
            }

            /* 在另一条线程上问一次 activate，主线程只管泵事件 ——
               同线程既当服务端又当客户端会让"字节能不能推进去"取决于测具自己的时序。
               wantReply=false 时故意不等回话就 abort，模拟超时走掉的客户端。 */
            auto askActivate = [&](bool wantReply, qint64* msOut, bool* ackOut) {
                std::atomic<bool> done { false };
                QThread* t = QThread::create([&] {
                    QElapsedTimer et; et.start();
                    {
                        QLocalSocket s;
                        s.connectToServer(ipcServerName());
                        if (s.waitForConnected(2000)) {
                            QJsonObject req;
                            req[QStringLiteral("command")] = QStringLiteral("activate");
                            req[QStringLiteral("t0")] =
                                static_cast<double>(QDateTime::currentMSecsSinceEpoch());
                            s.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
                            s.flush();
                            if (wantReply) {
                                /* ⚠️ 这里必须起一个**本线程的**事件循环，不能用
                                   waitForBytesWritten + waitForReadyRead 了事：
                                   Windows 上 QLocalSocket 的写出走 QWindowsPipeWriter
                                   （异步，需要事件循环才真正把字节推进管道）。
                                   这条线程没有循环 ⇒ 实测约每隔几轮第一条请求的字节
                                   根本没出去，5 秒后 ack=0/1 —— 红的是测具，不是产品。 */
                                QEventLoop loop;
                                QTimer deadline;
                                deadline.setSingleShot(true);
                                QObject::connect(&s, &QLocalSocket::readyRead,
                                                 &loop, &QEventLoop::quit);
                                QObject::connect(&deadline, &QTimer::timeout,
                                                 &loop, &QEventLoop::quit);
                                deadline.start(5000);
                                loop.exec();
                                const QJsonObject resp =
                                    QJsonDocument::fromJson(s.readAll()).object();
                                if (ackOut)
                                    *ackOut = resp.value(QStringLiteral("success")).toBool(false);
                                s.disconnectFromServer();
                            } else {
                                s.abort();      // 故意不等回话就走（模拟超时离开的客户端）
                            }
                        } else if (ackOut) {
                            *ackOut = false;
                        }
                    }
                    if (msOut) *msOut = et.elapsed();
                    done = true;
                });
                t->start();
                while (!done.load()) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(5);
                }
                t->wait(8000);
                delete t;
                for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
            };

            /* ── A 段：两次都要拿到 success，才谈得上"处理了两遍" ── */
            bool ack1 = false, ack2 = false;
            askActivate(true,  nullptr, &ack1);
            askActivate(true,  nullptr, &ack2);
            ok1 = ack1 && ack2;

            rowsAfter = model ? model->rowCount() : -2;
            {
                int ids[64] = {0};
                engAfter = dlmgr_list(ids, 64);
            }
            /* 判据取前后之差，不是绝对值：隔离数据目录里本来就可能存着上轮恢复的
               任务（并发探针会留 5 个），断绝对值测的就不是"重复投递"这件事。 */
            ok2 = (rowsAfter == rowsBefore && engAfter == engBefore);

            /* 两次激活之后窗口仍是"可见 + 正常态"：证明第二遍没有把它改成
               最小化/最大化/隐藏之类的别的状态。 */
            ok3 = w.isVisible() && !w.isMinimized() && !w.isMaximized();

            /* 服务端没被搞坏：还能连（也没人会去起第二套实例） */
            QLocalSocket probe;
            probe.connectToServer(ipcServerName());
            ok4 = probe.waitForConnected(1500);
            probe.disconnectFromServer();

            /* ── B 段：先留一条"不等回话"的连接，再量正常请求的往返 ── */
            askActivate(false, nullptr, &firstAbandonedAck);
            askActivate(true, &roundTripMs, &ok5);

            /* ── C 段：连续投递 N 次必须全部拿到 ack ──
             * 丢回话是概率性的（CI 上几轮碰上一次），投两次量不出来；
             * 这一段把它变成一个可数的比率。默认 50 次，IDM_ACT_STRESS 可调大做 A/B。 */
            stressTotal = qEnvironmentVariable("IDM_ACT_STRESS", "50").toInt();
            for (int k = 0; k < stressTotal; ++k) {
                bool a = false;
                askActivate(true, nullptr, &a);
                if (a) ++stressAcks;
            }
            ok6 = (stressAcks == stressTotal);

            /* ── D 段：一条"连上但一个字节都不发、也不断开"的客户端 ──
             * 这是 handleClient 那个 10×100ms 有界读循环的直接测量。循环跑在 GUI 线程上，
             * 而 onNewConnection 里是 `while (hasPendingConnections()) handleClient(...)`
             * ⇒ 一条空转的连接能让 GUI 线程整秒不回事件循环，后面每条连接都跟着排队。
             * 真实场景：浏览器扩展的原生消息轮询、被杀掉的副实例留下的半开连接。
             *
             * ⚠️ 第一版我把"空转连接之后再问一次、量它的往返"，测出来 14ms —— 那是测具错了：
             * 阻塞整个发生在某一次 processEvents() 调用**内部**，我那 150ms 的等待循环
             * 正好把这一秒吞在自己的计时之外，量到的是"事后"。改测直接量：
             * 事件循环单轮的最大耗时（= GUI 线程被独占多久）。 */
            std::atomic<bool> idleClosed { false };
            std::atomic<bool> idleAbort { false };   // 主线程采完之后再收摊（窗口内不起/不灭线程）
            /* 排空 + 静默（入参第 1 条）：D 段之前 C 段刚跑完 50 次往返。
             * 若 accept 队列里还有残留，D 段量到的高 N / 高耗时就可能是**上一段造的**，
             * 而不是产品行为。判据：连续两轮 150ms 静置里「取尽最久一次」不增长 ⇒ 认为已排空。 */
            qint64 quietMax = IpcServer::drainStat().maxMs;
            int quietRounds = 0;
            for (int i = 0; i < 20 && quietRounds < 2; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(150);
                const qint64 nowMax = IpcServer::drainStat().maxMs;
                if (nowMax == quietMax) ++quietRounds;
                else { quietRounds = 0; quietMax = nowMax; }
            }
            const bool drained = (quietRounds >= 2);

            /* ── D 段窗口：事件驱动 + 测具开销排除（§7-57 C）──
             * ⚠ `QThread::create(...)->start()` 在 Windows 上是**同步**的（CreateThread＋句柄就绪），
             *   机器忙的时候它自己就能占掉 GUI 线程一整轮。独立判别式实测（同一台机、同样泵法）：
             *     A 面 每轮新建并启动一条线程 worst=32ms ｜B 面 预热一条复用 worst=0ms
             *   ⇒ 干净面上那些"要么 0 个慢轮、要么 1~2 个 300/1700ms 慢轮"的双峰，
             *     **是测具造的，不是产品造的**。所以：
             *       ① 对端线程提前起好、连接保持打开，窗口内不再起任何线程；
             *       ② 先排到 newConnection 已被服务端看到（drainStat().connections>0），
             *          再开窗采样 ⇒ accept 那一轮也不记在产品占用里。 */
            QThread* idle = QThread::create([&] {
                QLocalSocket s;
                s.connectToServer(ipcServerName());
                if (s.waitForConnected(3000)) {
                    while (!idleAbort.load())     // 由主线程决定何时收摊（窗口跑完之后再关）
                        QThread::msleep(20);
                    s.abort();
                }
                idleClosed = true;
            });
            idle->start();

            /* 排到空转事件真的被服务端看到——这一步在**采样窗口之外**。
             * ⚠ 这里泵掉的每一轮都不许进分布：它是测具自己的握手成本。
             * ⚠⚠ 但产品定时器（状态栏 250ms / flush 100ms / …）在这段里会被**饿掉**：
             *   这段最长可达 4s，其间没有任何轮次去烧它们的到期回调。canary/变异如果
             *   在这段里才 start，它的第一个超时就会被"推后到开窗之后"⇒ 采满 N 轮就退出时
             *   它一次都没跑（实测形状：触发=否、超预算=0 ⇒ 夹具没牙的门）。
             *   所以计时器必须**先挂上**，让握手那段等待也给它派发超时。 */
            QTimer* canary = nullptr;
            const int canaryMs = qEnvironmentVariableIntValue("IDM_CANARY_MS");
            canaryRequestedOut = canaryMs > 0;

            /* ⚠⚠ Qt6 的 `processEvents(flags, QDeadlineTimer(20))` 是**预算制**：
             *   到点就返回，哪怕事件已经派发出去、回调还在里面睡。
             *   于是"每轮睡 80ms"的注入在这个口径下**永远量不到**——
             *   实测形状：变异面跑过 30 轮、非零 0 轮、max=0ms（一条打不着靶的门）。
             *   要让它可达，窗口必须用**无时限**泵（AllEvents 不带 deadline），
             *   一轮跑到自然收束为止 ⇒ 占用真的落进这一轮的计时里。
             * 默认仍是旧口径（不影响干净面的低占用读数）；只要本轮存在注入面（canary 或变异），
             * 就自动切无时限口径——改的是"能不能看见"，不是"看见后算不算红"。 */
            const bool unboundedRounds =
                canaryMs > 0 || qEnvironmentVariableIsSet("IDM_ACTIVATE_MUTATE")
                || qEnvironmentVariableIsSet("IDM_UNBOUNDED_ROUNDS");

            QTimer* mutator = nullptr;
            const int mSleep = qEnvironmentVariableIntValue("IDM_MUTATE_SLEEP_MS") > 0
                             ? qEnvironmentVariableIntValue("IDM_MUTATE_SLEEP_MS") : 80;
            const int mTick  = qEnvironmentVariableIntValue("IDM_MUTATE_TICK_MS") > 0
                             ? qEnvironmentVariableIntValue("IDM_MUTATE_TICK_MS") : 40;

            qint64 canaryAtTurn = -1;
            if (canaryMs > 0) {
                canary = new QTimer(&w);
                QObject::connect(canary, &QTimer::timeout, [&, canaryMs] {
                    QThread::msleep(canaryMs);               // 占住 GUI 线程的一个整轮
                    w.setProperty("_idmCanaryFired", true);
                });
                canary->start(120);
            }
            if (qEnvironmentVariableIsSet("IDM_ACTIVATE_MUTATE")) {
                mutator = new QTimer(&w);
                QObject::connect(mutator, &QTimer::timeout, [&, mSleep] {
                    QThread::msleep(mSleep);          // 反复占住整轮 ⇒ 这就是缺陷形状
                });
                mutator->start(mTick);
                printf("[mutate] 变异面已开：每 %d ms 睡 %d ms（测具侧定时器，不碰产品代码）\n",
                       mTick, mSleep);
            }

            bool idleSeen = false;
            {
                QElapsedTimer waitIdle; waitIdle.start();
                while (waitIdle.elapsed() < 4000 && !idleSeen) {
                    if (unboundedRounds)
                        QCoreApplication::processEvents(QEventLoop::AllEvents);
                    else
                        QCoreApplication::processEvents(QEventLoop::AllEvents, QDeadlineTimer(20));
                    if (IpcServer::drainStat().connections > 0) idleSeen = true;
                }
            }

            /* ── 变异面（IDM_ACTIVATE_MUTATE=1）：缺陷形状＝"每轮都被占住" ──
             * ⚠ 只在 IDM_ACTIVATE_MUTATE=1 时存在；不设＝产品与判据零变化。 */
            QElapsedTimer idleWindow;
            idleWindow.start();
            qint64 worstLoopMs = 0;
            int peakN = 0;               // 窗口内「一次取尽」的最大连接数
            qint64 peakDrainMs = 0;      // 窗口内「一次取尽」的最大自身耗时
            const qint64 sampleStart = idleWindow.elapsed();
            /* ⚠ 偶发红的归因（2026-10-02 实测：单轮最长 2510ms，而 IPC 自身只占 8%）：
             *   `processEvents(AllEvents, 20)` 的第二参数在 Qt6 是**墙钟上限**，不是事件条数。
             *   这一轮被 Windows 消息队列里**不属于本探针**的事件占着时（同机别的会话在前台、
             *   系统级广播），它会一直等到 20ms 用尽或队列有动静 ⇒ 计时把"别人的阻塞"
             *   记成我的 GUI 线程占用。判据要的是"我们的活儿让 GUI 线程转不动多久"，
             *   所以改成 QDeadlineTimer 给每轮一个硬预算；撞预算的次数单独印，
             *   **不进判据分子**（它不是产品缺陷，也不许被读成通过）。 */
            // truncatedTurns 在外层声明（判据行要读它）
            QVector<qint64> turnSamples;   // C 证据：只留**非零**轮（纯空转的 0ms 轮会把分位数稀释成假分布）
            int turnsRun = 0;              // B 判据的分母：**跑过的轮数**（含 0ms 轮）
            qint64 distSum = 0; int distNonZero = 0;
            /* ⚠ 循环条件不能带 !idleClosed.load()：对端 2.5s 才关连接，那一发 newConnection
             *   恰好落在 idleClosed 置位之后 —— 提前退出就把要量的那轮切掉了
             *   （实测 max 恒 ≤7ms、变异 100ms 也测不出＝假绿门，第一版栽在这）。 */
            const int distWindowMs = qEnvironmentVariableIntValue("IDM_DIST_WINDOW_MS") > 0
                                   ? qEnvironmentVariableIntValue("IDM_DIST_WINDOW_MS") : 3000;
            /* C 证据（§7-57）：**窗口长度由墙钟决定，不由轮数决定**。
             * ⚠ 这里我连着错过两次，形状记下来防重犯：
             *   ① 只看墙钟（旧版 2s）⇒ 空转那一发落在第 1.5~2.5s，有时被切掉 ⇒ max 恒 ≤7ms；
             *   ② 改成"采满 N 轮就走"⇒ 空闲时一轮只花 0~1ms，30 轮加起来只有几十毫秒墙钟，
             *      **比产品最短的节拍（flushTimer 100ms）还短**，于是任何定时器都来不及 fire，
             *      "非零轮=0"读的是我的采样区间，不是产品（实测：墙钟 413ms、跑过 20 万轮、非零 0）。
             *   现在：墙钟到点才收（默认 3000ms，覆盖各节拍多轮），
             *   并且只在已经看到空转事件之后退出 ⇒ 被测的那一轮一定在账本里。
             *   样本数只印出来当读数，不当闸口——两面（干净/变异）的分母都由同一段时长给出。 */
            /* ⚠ 窗口**不能只靠"采满 N 轮"退出**：产品空闲时一轮可以只花 0~1ms，
             *   N 轮加起来墙钟只有几十毫秒——比产品最短的那个节拍（flushTimer 100ms）还短，
             *   于是窗口里**任何定时器都来不及 fire**，"非零轮=0"只是我的采样区间太短。
             *   （实测形状：墙钟 413ms、跑过 20 万轮、非零 0 轮。）
             *   所以退出条件是：**墙钟到点为止**（默认 3000ms：覆盖 flushTimer 100ms 约 30 次、
             *   状态栏 250ms 约 12 次；只 1.5s 的话 5000ms 那个节拍一次都不 fire，
             *   普查就会把"没轮到它"误读成"它不占"），
             *   样本数只作读数印出来，不当闸口——分母由时长决定，两面才有可比性。 */
            /* ⚠ 墙钟到点即收，但**不能早于空转事件**：那一轮正是本段被测对象，
             *   切掉它就是第一版"max 恒 ≤7ms、变异测不出"的假绿门（:2308 那条教训）。 */
            bool sawIdleEvent = false;
            bool roundsCaliberPrinted = false;
            while (idleWindow.elapsed() - sampleStart < distWindowMs) {
                QElapsedTimer one;
                one.start();
                /* 口径开关（只为取对照数据，**不改判据、不改阈值**）：
                 *   IDM_ROUND_CRITERION=deadline（默认/缺省）→ 每轮 QDeadlineTimer(20)（新口径）
                 *   IDM_ROUND_CRITERION=count               → processEvents(flags, 20)（旧口径原样）
                 * ⚠ Qt6 里第二参数是 `int maxtime`（墙钟上限），我当初误当成"事件条数"——
                 *    两种写法在源码上是两个形状，所以必须各自跑同一枚二进制才能比。 */
                if (roundCriterionCount)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                else if (unboundedRounds)
                    QCoreApplication::processEvents(QEventLoop::AllEvents);
                else
                    QCoreApplication::processEvents(QEventLoop::AllEvents, QDeadlineTimer(20));
                /* ⚠ 口径旗子必须**如实反映这一轮真的用了哪种泵**。
                 * 之前它印的是 roundCriterionCount 一个布尔，第三种（无时限）被印成"deadline"，
                 * 于是我拿着一行写着 deadline(20) 的读数去问"为什么变异不可达"——
                 * 那条问题根本问错对象了（读数与代码状态对不上＝假归因）。 */
                if (unboundedRounds && !roundsCaliberPrinted) {
                    roundsCaliberPrinted = true;
                    printf("[caliber] D 段本轮实际口径=%s\n",
                           roundCriterionCount ? "count(第二参当墙钟上限)"
                                               : "AllEvents(无时限：一轮跑到自然收束)");
                }
                const qint64 took = one.elapsed();
                if (took >= 20) ++truncatedTurns;
                /* canary 落在第几轮：本轮**之前**的样本数 +1 ⇒ 本轮就是它那一轮。
                 * 必须在 append 之前取，否则 off-by-one（第一版把注入点记成了"下一轮"）。 */
                if (canaryMs > 0 && canaryAtTurn < 0 && w.property("_idmCanaryFired").toBool())
                    canaryAtTurn = turnSamples.size() + 1;
                /* ⚠ 分母必须是**跑过的轮数**，不是"非零轮数"。
                 * 空闲产品上一轮 ≈0ms 是常态；用非零数当分母会出现 N=0 ⇒ 门自己红，
                 * 而那条红没有归属（产品没变，只是机器安静）——那是一条会撒谎的门。
                 * "只留非零"的口径留给分布打印用（那里 0 会把分位数稀释成假分布）。 */
                ++turnsRun;
                if (took > 0) { turnSamples.append(took); distSum += took; ++distNonZero; }
                worstLoopMs = qMax(worstLoopMs, took);
                /* C 归因的读数面：慢轮（>50ms）单独打时间戳与当时状态。
                 * 不打印就永远只能猜"是谁占的这一轮"。 */
                if (took > 50 && qEnvironmentVariableIsSet("IDM_ROUND_ATTR"))
                    printf("[slowturn] t=%lldms 窗口第%lldms peakN=%d\n",
                           (long long)took, (long long)(idleWindow.elapsed() - sampleStart),
                           IpcServer::drainStat().connections);
                const IpcServer::DrainStat ds = IpcServer::drainStat();
                peakN = qMax(peakN, ds.connections);
                peakDrainMs = qMax(peakDrainMs, ds.lastMs);
                if (ds.connections > 0) sawIdleEvent = true;
                /* 退出只看墙钟（见上面那段：轮数当闸口会让窗口比产品任何节拍都短）。
                 * ⚠ 但**不能早于空转事件**：那一轮正是本段的被测对象，
                 *   切掉它就是第一版"max 恒 ≤7ms、变异测不出"的假绿门。 */
                if (sawIdleEvent && idleWindow.elapsed() - sampleStart >= distWindowMs)
                    break;
            }
            /* 采完之后才让对端收摊：连接在整个窗口里保持打开 ⇒
             * 窗口内一次线程起灭都没有（起线程的同步开销不属于产品）。 */
            idleAbort = true;

            /* ⚠ 收摊循环**不许再泵事件**（旧版这里是 processEvents + msleep(5)）：
             *   空转连接的 disconnected 要到 GUI 线程下一次回到事件循环才被处理，
             *   而 `idleClosed` 是对端线程置的——用"泵事件"去等一个"只有泵事件才会发生"的旗子，
             *   等于把几百毫秒到几十秒的事件循环**记在窗口账本之外**。
             *   实测形状：窗口墙钟 15ms、跑过 30 轮，但产品自己的 250/100ms 定时器按这个长度
             *   根本来不及 fire ⇒ "非零=0"不是产品空闲，是**我的采样区间比任何节拍都短**。
             *   改成只 join 线程（不碰事件循环），随后立刻普查，读数才有归属。 */
            idle->wait(8000);
            delete idle;
            if (mutator) { mutator->stop(); delete mutator; mutator = nullptr; }
            okIdleAck = (turnsRun > 0);   // 真的跑过轮次才算这条测试跑过（不是"非零轮>0"）
            worstEventLoopTurnMs = worstLoopMs;

            /* ── 窗口普查（IDM_ROUND_ATTR=1）：这一窗口的读数**归谁**──
             * ⚠ 干净面出现"跑过 N 轮、非零 0 轮"时，光看这个 0 分不清两种解释：
             *   产品真没占用 / 我的量具看不见。所以这里必须给出可核对的分母：
             *     · 开窗时刻活动定时器数与清单（应当 ≥4：状态栏 250 / flush 100 / 队列 1000 / 定时 5000）
             *     · 窗口墙钟长度（对照"这些节拍各该 fire 几次"）
             *   清单非空而非零轮=0 ⇒ **测具在此平台上不派发 Qt 定时器**，
             *   于是"单轮最长=0ms"是量具读数、不是产品读数 ⇒ 不许当证据（印明，不静默）。 */
            if (qEnvironmentVariableIsSet("IDM_ROUND_ATTR")) {
                int actCount = 0, totalCount = 0;
                QStringList census;
                const auto objsW = w.findChildren<QObject*>();
                for (QObject* o : objsW) {
                    if (auto* tm = qobject_cast<QTimer*>(o)) {
                        ++totalCount;
                        if (!tm->isActive()) continue;
                        ++actCount;
                        census << QStringLiteral("%1ms@%2")
                                    .arg(tm->interval())
                                    .arg(QString::fromLatin1(
                                             tm->parent() ? tm->parent()->metaObject()->className() : "-"));
                    }
                }
                const qint64 winMs = idleWindow.elapsed() - sampleStart;
                printf("[census] D 段窗口=%lldms｜活动定时器 %d/%d 个 [%s]｜跑过=%d 轮 非零=%d 轮\n",
                       (long long)winMs, actCount, totalCount,
                       census.join(QStringLiteral(", ")).toUtf8().constData(),
                       turnsRun, distNonZero);
                printf("[census] 判读：%s\n",
                       (actCount > 0 && distNonZero == 0)
                           ? "有活动定时器却一轮都不耗时 ⇒ **本平台的 GUI 计时看不见定时器回调**，"
                             "D 段的 max/超预算计数只反映 Windows 消息到达，不是产品占用 ⇒ 不作通过证据"
                           : (actCount == 0 ? "窗口内没有活动定时器 ⇒ 这个 0 无人证得过"
                                            : "非零轮>0 ⇒ 计时抓到了动静，读数可用"));
            }

            /* ── B 判据：预算命中数，不是单轮最长（§7-57）──
             * 为什么不用 max 当门（实测出来的形状）：干净面在**同一枚二进制、同一条命令**下
             * 会同时出现两种读数——绝大多数轮 ≈0~1ms，偶发一轮 ~300ms。
             * 拿 max 配任何有限阈值，都会让"产品没变、机器忙了一下"也红；
             * 而那条红既不可复算、也不指向任何代码状态 ⇒ 是一条没有归属的旗子。
             * 换成计数：**一段固定时长里有多少轮超过 T**。
             * ⚠ T=50ms 的来源（不许是拍脑袋数，写清楚怎么来的）：
             *   本轮采到的干净面分布里，非零轮绝大多数是 **1ms 量级的 Windows 消息到达**，
             *   只有每窗口 ≤2 发是 ~300ms 的那一类（见 [dist] 与 IDM_DIST_FILE）。
             *   所以"产品自己的活儿让一轮转不动"这个形状，只能从**有负载的面**上标定：
             *     IDM_ATTR_SLEEP=30 → p95≈45ms、超预算 0 次（低于 50 时门不该响）
             *     IDM_ATTR_SLEEP=60 → p95≈74ms、超预算 22~26 次（门红）
             *   ⇒ T 取两者之间、且高于干净面常态（1ms）一个量级 = **50ms**。
             *   K=1：允许一窗内一发无归属的系统抖动（就是那类 ~300ms），第二发才算反复占用。
             *   旧门的 500ms 是当初照着"IPC 阻塞常数 216ms"拍的，与现在这个量纲无关。 */
            const int budgetMs = qEnvironmentVariableIntValue("IDM_BUDGET_MS") > 0
                               ? qEnvironmentVariableIntValue("IDM_BUDGET_MS") : 50;
            int overBudget = 0;
            for (qint64 v : turnSamples) if (v > budgetMs) ++overBudget;
            overBudgetTurns = overBudget;
            budgetSamples = turnsRun;
            budgetMsOut = budgetMs;

            /* canary 判读：注入的那枚定时器**是否真的被这个窗口采到**。
             * 采到 ⇒ 窗口有牙（能看见"一整轮被占住"的形状），此时的低读数才是证据；
             * 没采到 ⇒ 这个窗口的 0/低值是"量具到不了"的 0，不许当通过。
             * ⚠ 这一枚是**B 门的夹具**：干净产品面上 overBudget=0，"0"本身给不了护栏任何牙
             *   （期望值为 0 的夹具证不了它会拦）。canary 用**产品自己的定时器通道**注入一次
             *   已知超预算占用 ⇒ 同一个计数器必须 +1。它红了才证明这扇门认得出违规。 */
            bool canaryCounted = false;
            if (canary) {
                const bool fired = w.property("_idmCanaryFired").toBool();
                /* 证人要落在**被采样的那一轮**上：注入点之后的那一轮必须进 turnSamples，
                 * 否则"已触发"只证明它跑了、不证明门看得见它。 */
                const bool inSample = fired && canaryAtTurn >= 0
                                   && canaryAtTurn <= turnSamples.size();
                canaryCounted = (inSample && worstLoopMs >= canaryMs);
                canaryCountedOut = canaryCounted;
                printf("[canary] 注入 %dms 占用：触发=%s（第 %lld 轮）｜样本内=%s ｜窗口 max=%lldms（应 ≥%d）⇒ %s\n",
                       canaryMs, fired ? "是" : "否", (long long)canaryAtTurn,
                       inSample ? "是" : "否",
                       (long long)worstLoopMs, canaryMs,
                       canaryCounted
                           ? "窗口有牙：低读数可归因于产品本身"
                           : "窗口没采到已知占用 ⇒ 本轮 D 段读数不作数");
                /* ⚠ stop 再 delete：canary 不是 singleShot，回调里 msleep 中时删对象＝段错误。 */
                canary->stop();
                delete canary;
                canary = nullptr;
            }

            /* ── 第二个具名读数：被空转连接压在后面的**那条真请求**，真实往返 ──
             * 它必须**自己开一份事件**（另起一轮 idle 连接 + 一条真请求），不能塞进上面那个 D 段窗口。
             * 理由不是风格，是实测：把第二条连接放进同一个窗口后，`IpcServer` 的
             * `s_drainStat.lastMs` 会被**后一次快取尽**覆盖（src/app/ipc_server.cpp:101-104 写的是
             * "最近一次取尽"），于是 D 段的 `取尽自身峰值` 从 213ms 掉到 0ms、`IPC占比` 从 100% 掉到 0%——
             * 等于把我自己做归因要用那个证人打瞎（0% 正是"别的东西占大头"的形状）。
             * 同理 :1349 那条教训：消融必须重开一份被测事件，不能拿事件已经过去了的空窗口当对照。
             * 形状复用 askActivate（独立 QThread + 本线程 QEventLoop），但**不用它的等待循环**——
             * 那个循环在 GUI 线程上 processEvents，会把"我的等待"混进被测量。 */
            {
                std::atomic<bool> rDone { false }, rAck { false };
                std::atomic<bool> bClosed { false };
                std::atomic<qint64> rMs { -1 };
                QThread* idle2 = QThread::create([&] {
                    QLocalSocket s;
                    s.connectToServer(ipcServerName());
                    if (s.waitForConnected(3000)) {
                        QThread::msleep(2500);      // 与 D 段同一条空转连接：连上、一个字不发
                        s.abort();
                    }
                    bClosed = true;
                });
                QThread* req2 = QThread::create([&] {
                    QElapsedTimer et;
                    et.start();
                    QLocalSocket s;
                    s.connectToServer(ipcServerName());
                    if (s.waitForConnected(2000)) {
                        QJsonObject req;
                        req[QStringLiteral("command")] = QStringLiteral("activate");
                        req[QStringLiteral("t0")] =
                            static_cast<double>(QDateTime::currentMSecsSinceEpoch());
                        s.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
                        s.flush();
                        QEventLoop loop;
                        QTimer deadline;
                        deadline.setSingleShot(true);
                        QObject::connect(&s, &QLocalSocket::readyRead, &loop, &QEventLoop::quit);
                        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
                        deadline.start(5000);
                        loop.exec();
                        const QJsonObject resp = QJsonDocument::fromJson(s.readAll()).object();
                        rAck = resp.value(QStringLiteral("success")).toBool(false);
                        s.disconnectFromServer();
                    }
                    rMs = et.elapsed();
                    rDone = true;
                });
                idle2->start();
                QThread::msleep(50);   // 让两条连接都到达 accept 队列，第一次取尽才会"空转在前"
                req2->start();
                QElapsedTimer w2;
                w2.start();
                const qint64 s2 = w2.elapsed();
                while (w2.elapsed() - s2 < 3000 && !rDone.load()) {
                    QElapsedTimer one;
                    one.start();
                    /* 与 D 段同一个理由：第二参数是墙钟上限，不是事件条数。给每轮硬预算，
                     * 免得队列里不属于本探针的事件把"别人的阻塞"记成我的轮次。口径开关同 D 段。 */
                    if (roundCriterionCount)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    else
                        QCoreApplication::processEvents(QEventLoop::AllEvents, QDeadlineTimer(20));
                    turnBehindMs = qMax(turnBehindMs, one.elapsed());
                    const IpcServer::DrainStat ds = IpcServer::drainStat();
                    nBehind = qMax(nBehind, ds.connections);
                }
                req2->wait(9000);
                idle2->wait(6000);
                for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
                idleBehindReplyMs = rMs.load();
                okIdleReply = rAck.load();
                delete req2;
                delete idle2;
            }

            /* C 证据的读数面：把 D 段窗口的逐轮样本原样写盘，阈值由采集脚本从分布里取，
             * 不在源码里拍。未设 IDM_DIST_FILE 时零副作用。 */
            std::sort(turnSamples.begin(), turnSamples.end());
            /* 分母写清楚：turnsRun＝跑过的轮数（含 0ms 空转轮），非零只是其中一部分。
             * 只印"非零轮"会让读者把 N=30/非零=1 误读成"样本太少、结论不算数"，
             * 而那恰恰是产品空闲时的**正常形状**。 */
            printf("[dist] 跑过=%d 轮（非零=%d，其余为 0ms 空转轮）mean=%.1fms p50=%lld p95=%lld p99=%lld max=%lld\n",
                   turnsRun, distNonZero,
                   distNonZero ? double(distSum) / distNonZero : 0.0,
                   (long long)(turnSamples.size() ? turnSamples[turnSamples.size() * 50 / 100] : 0),
                   (long long)(turnSamples.size()
                                   ? turnSamples[qMin(turnSamples.size() * 95 / 100,
                                                       turnSamples.size() - 1)] : 0),
                   (long long)(turnSamples.size()
                                   ? turnSamples[qMin(turnSamples.size() * 99 / 100,
                                                       turnSamples.size() - 1)] : 0),
                   (long long)worstLoopMs);
            if (!qEnvironmentVariable("IDM_DIST_FILE").isEmpty()) {
                QFile df(qEnvironmentVariable("IDM_DIST_FILE"));
                if (df.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    for (qint64 v : turnSamples)
                        df.write(QByteArray::number(v) + "\n");
                    df.close();
                }
            }

            /* 归因（入参第 2 条）：把「这一轮的时间是谁花的」拆开印出来，不再靠猜。
             *   peakN>1  ⇒ 串行宽限解释得通；peakN==1 而单轮仍高 ⇒ 别的东西占大头
             *   IPC 占比 = 取尽自身耗时峰值 / 单轮最长
             * 只量、不改判据、不放宽 500。 */
            const double ipcShare = worstLoopMs > 0
                                  ? (100.0 * double(peakDrainMs) / double(worstLoopMs)) : 0.0;
            printf("[act] D 段归因：排空静默=%s peakN=%d 取尽自身峰值=%lldms "
                   "单轮最长=%lldms IPC占比=%.0f%%\n",
                   drained ? "成" : "未成", peakN, (long long)peakDrainMs,
                   (long long)worstLoopMs, ipcShare);

            /* ── 轮内归因消融（IDM_ROUND_ATTR=1，只量不改判据）──
             * IPC 只占 10~13%，剩下 ~1.5s 是谁？先做二分：把 MainWindow 子树里所有 QTimer
             * 停掉再采一轮。掉回 ~215ms ⇒ 时间花在定时器回调上（再逐个消融定位是哪一个）；
             * 不掉 ⇒ 与定时器无关（socket/重绘/外部事件），换一条路查。
             * 顺带把定时器清单打出来：间隔、是否活动、宿主 objectName/类名。 */
            if (qEnvironmentVariableIsSet("IDM_ROUND_ATTR")) {
                /* 范围＝**两路并集**：qApp 全对象树 + MainWindow `w` 自己的子树。
                 * ⚠ 单靠 qApp 那一路是**实测错过的形状**：本探针里它连着几轮都印「0 个」，
                 *   而产品侧 m_statusTimer(250ms)/flushTimer(100ms)/queueScheduler(1000ms)
                 *   按源码必须活着（main_window.cpp:385 / :285、download_manager.cpp:43）。
                 *   "扫描能抓到我自己刚挂的 canary"只证明**那条路有眼**，不证明**产品那棵树在它看得见的位置**。
                 *   所以这里两条路各数一遍、取并集，并把两路各自的计数印出来——
                 *   谁漏了立刻能从两个数上看见，而不是让归因拿着 0 当结论。 */
                QVector<QTimer*> timers;
                auto collect = [&](QObject* root, int* total, QSet<QTimer*>* into) {
                    if (!root) return;
                    const auto kids = root->findChildren<QObject*>();
                    for (QObject* o : kids) {
                        if (auto* tm = qobject_cast<QTimer*>(o)) {
                            ++(*total);
                            if (tm->isActive()) into->insert(tm);
                        }
                    }
                };
                int totalApp = 0, totalWin = 0;
                QSet<QTimer*> unionActive;
                collect(QCoreApplication::instance(), &totalApp, &unionActive);
                collect(&w, &totalWin, &unionActive);
                timers = QVector<QTimer*>(unionActive.begin(), unionActive.end());
                /* ⚠ 顺序承重：QSet 迭代序在进程间不稳定，而 IDM_TIMER_INDEX=N 是"停第 N 个"。
                 *   不排序的话同一个 N 两次跑会指向**不同的定时器**（实测 idx1 有一次直接段错误），
                 *   逐定时器消融的结论就不可复算。按 (间隔, 宿主类名) 定序，编号才有含义。 */
                std::sort(timers.begin(), timers.end(), [](QTimer* a, QTimer* b) {
                    if (!a || !b) return !a;
                    if (a->interval() != b->interval()) return a->interval() < b->interval();
                    const char* ca = a->parent() ? a->parent()->metaObject()->className() : "";
                    const char* cb = b->parent() ? b->parent()->metaObject()->className() : "";
                    return QString::fromLatin1(ca) < QString::fromLatin1(cb);
                });
                printf("[attr] 定时器：qApp 树 %d 个 / MainWindow 树 %d 个 / 活动并集 %d 个\n",
                       totalApp, totalWin, int(timers.size()));
                /* 量具自检（canary）：临时挂一枚**我确定存在、确定活动**的定时器，
                 * 再用**同一条两路扫描**去找它。找不到 ⇒ 计数是量具瞎了，不是产品没东西跑，
                 * 本段清单/消融一律作废（教训：把"我的量具瞎"读成"产品没有定时器"）。 */
                {
                    auto* c1 = new QTimer(qApp);      // 走 qApp 那一路
                    auto* c2 = new QTimer(&w);        // 走 MainWindow 那一路
                    c1->setObjectName(QStringLiteral("__idm_attr_canary_app__"));
                    c2->setObjectName(QStringLiteral("__idm_attr_canary_win__"));
                    c1->start(9999);
                    c2->start(9998);
                    int t2App = 0, t2Win = 0;
                    QSet<QTimer*> probe2;
                    collect(QCoreApplication::instance(), &t2App, &probe2);
                    collect(&w, &t2Win, &probe2);
                    const bool seenApp = probe2.contains(c1);
                    const bool seenWin = probe2.contains(c2);
                    printf("[attr] 扫描自检：临时挂 2 枚已知活动定时器（qApp 侧 / MainWindow 侧各 1），"
                           "重扫 qApp=%d MainWindow=%d，命中=%s/%s ⇒ %s\n",
                           t2App, t2Win, seenApp ? "能" : "不能", seenWin ? "能" : "不能",
                           (seenApp && seenWin) ? "两路都有眼，前面的计数可信"
                                                : "至少一路看不见自己刚挂上的东西 —— 本段消融/清单一律作废");
                    c1->stop(); c2->stop();
                    delete c1; delete c2;
                }
                for (QTimer* tm : timers) {
                    if (!tm) continue;
                    QObject* par = tm->parent();
                    printf("     interval=%-6d active=%d 宿主=%s[%s]\n",
                           tm->interval(), int(tm->isActive()),
                           par ? par->metaObject()->className() : "-",
                           par ? qUtf8Printable(par->objectName()) : "-");
                }
                /* ⚠ 第一版消融是错的，记在这里防重犯：我把「停掉定时器再采样」放在 D 段之后，
                 * 而 D 段那条空转连接早被处理完了 ⇒ 之后无论停谁都是 0ms，
                 * 4 个定时器全被判成"元凶"。消融必须**重开一份被测事件**，
                 * 不能拿"事件已经过去了"的空窗口当对照。 */
                /* 逐个消融开关（C 证据用）：IDM_TIMER_INDEX=N 只停第 N 个；
                 * 未设时按原样"全停/不停"。停谁由清单行号决定，不靠猜。 */
                const int onlyIdx = qEnvironmentVariable("IDM_TIMER_INDEX").toInt();
                /* 消融窗口必须自己采得到东西：对端 2.5s 后才关连接，窗口只跑 2s ⇒
                 * newConnection 落在窗口外，onA 恒 0（第一版 bisect 全 0ms 就是这个形状）。
                 * 3s > 2.5s 保证空转事件必在窗口内；主 D 段的 2s 不动。 */
                const qint64 ablationWindowMs = 3000;
                auto sampleWithIdle = [&](bool timersOff) -> qint64 {
                    QVector<bool> wasActive;
                    if (timersOff) {
                        for (int ti = 0; ti < timers.size(); ++ti) {
                            QTimer* tm = timers[ti];
                            wasActive.append(tm ? tm->isActive() : false);
                            if (!tm) continue;
                            if (onlyIdx > 0 && ti != onlyIdx - 1) continue;
                            tm->stop();
                        }
                    }
                    /* ⚠ 消融窗口自己也要有牙：它印「有定时器=0ms」有两种解释——
                     *   (a) 这个窗口里确实没有占用（产品空闲）
                     *   (b) **这个窗口看不见占用**（量具到不了）
                     * 分不清就会把"停不停都 0"读成"与定时器无关"——那是拿我的瞎当证据。
                     * IDM_CANARY_MS>0 时往这个窗口注入一枚已知时长的占用：
                     *   采到 ⇒ 该窗口的低读数可信；采不到 ⇒ 该面作废、不进对照结论。 */
                    const int cMs = qEnvironmentVariableIntValue("IDM_CANARY_MS");
                    QTimer* cTimer = nullptr;
                    bool cFired = false;
                    if (cMs > 0) {
                        /* ⚠ 不能 setSingleShot(true)：Qt6 的 `start(int)` 不清 singleShot，
                         *   而这里要的是"整个窗口里反复 fire"（单次的话烧完就自灭）。 */
                        cTimer = new QTimer(&w);
                        QObject::connect(cTimer, &QTimer::timeout, [&] {
                            QThread::msleep(cMs);
                            cFired = true;
                        });
                        cTimer->start(qMax(30, int(ablationWindowMs / 8)));
                    }
                    std::atomic<bool> abortTh { false };
                    QThread* th = QThread::create([&] {
                        QLocalSocket s;
                        s.connectToServer(ipcServerName());
                        if (s.waitForConnected(3000)) {
                            while (!abortTh.load()) QThread::msleep(20);   // 窗口内保持连接
                            s.abort();
                        }
                    });
                    th->start();
                    QElapsedTimer win; win.start();
                    const qint64 start = win.elapsed();
                    qint64 worst = 0;
                    /* ⚠ 这里也不能带 !closed.load()：对端 2.5s 后才关，newConnection 落在 closed
                     *   置位之后 ⇒ 带上它就把要量的那轮切掉（主 D 段 :2308 同一条坑，第一版 bisect
                     *   恒 0ms、清单恒「定时器 0 个」都是它）。窗口必须自己采到事件。 */
                    while (win.elapsed() - start < ablationWindowMs) {
                        QElapsedTimer one; one.start();
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        worst = qMax(worst, one.elapsed());
                    }
                    /* 收摊不许再泵事件：与 D 段同一个理由（泵事件＝把产品节拍烧在账本之外，
                     * 于是"停不停都低"这种形状会被当成归因结论）。 */
                    abortTh = true;
                    th->wait(8000);
                    delete th;
                    const bool canaryOk = cTimer && cFired && worst >= cMs;
                    if (cTimer) {
                        printf("     （消融面 canary=%dms：触发=%s max=%lldms ⇒ %s）\n",
                               cMs, cFired ? "是" : "否", (long long)worst,
                               canaryOk ? "此窗口有牙" : "此窗口采不到已知占用 ⇒ 它的 0ms 不算证据");
                        /* ⚠ 必须先 stop 再 delete：这个定时器**不是 singleShot**（要的就是反复 fire），
                         *   而回调里会 `QThread::msleep(cMs)`。不 stop 就 delete，正在派发的回调
                         *   拿着已释放的对象 ⇒ 段错误（实测形状：带 IDM_ROUND_ATTR+IDM_CANARY_MS 必崩）。 */
                        cTimer->stop();
                        delete cTimer;
                    }
                    if (timersOff)
                        for (int i = 0; i < timers.size(); ++i)
                            if (timers[i] && wasActive.value(i)) timers[i]->start();
                    return canaryOk ? worst : -1;   // 负数＝这一面没牙，不参与结论
                };
                if (onlyIdx > 0) {
                    const qint64 zA = sampleWithIdle(false);
                    const qint64 zB = sampleWithIdle(true);
                    const qint64 zC = sampleWithIdle(false);
                    /* ⚠ 判据不能写 `zB*3 < zA`：那要求"停掉它之后只剩噪声"。
                     *   当窗口里还有**别的**占用源时（变异面就有两路：注入器 + 真路径上的 sleep），
                     *   停掉其中一路只会让读数减半，永远不会塌到三分之一——
                     *   于是每个候选都印"不是它"，而事实是"两个都在贡献"。
                     *   改成配对比较：停掉这一路 ⇒ 相对不停掉**降了一半以上**就算数，
                     *   并且用第三面（再有）排除单调漂移。 */
                    const bool dropped = (zA > 0 && zC > 0 && zB > 0
                                          && zB * 2 < qMin(zA, zC));
                    printf("[attr2] 只停 #%d：有=%lldms → 停=%lldms → 再有=%lldms ⇒ %s\n",
                           onlyIdx, (long long)zA, (long long)zB, (long long)zC,
                           (zA < 0 || zB < 0 || zC < 0)
                               ? "某一面没牙（canary 采不到）⇒ 本面对比作废"
                               : (dropped ? "停掉它就减半 ⇒ 它是主要占用源之一"
                                          : "停掉它没减半 ⇒ 这一路不是主因（可能有多路并存）"));
                }
                const qint64 onA  = sampleWithIdle(false);
                const qint64 offB = sampleWithIdle(true);
                const qint64 onC  = sampleWithIdle(false);   // 排除单调漂移
                /* C 证据（§7-57）：旧版"本轮未复现(<500)就跳过消融"在阈值改小后会自废——
                 * 340ms 常态下永远拿不到归因。改成**无条件跑**，判读看"停不停有没有差"，
                 * 不再看是否越过某个阈值。 */
                printf("[attr] 同一测具两份窗口对照：有定时器=%lldms → 停定时器=%lldms → 再有定时器=%lldms"
                       "（IPC 自身峰值=%lldms）⇒ %s\n",
                       (long long)onA, (long long)offB, (long long)onC, (long long)peakDrainMs,
                       (onA < 0 || offB < 0 || onC < 0)
                           ? "至少一面没牙（canary 采不到）⇒ 本段对照作废，不读成结论"
                           : (offB < onA / 2 && offB < onC / 2)
                               ? "时间花在定时器回调上（对照成立：两边都塌才算数）"
                               : "对照不成立 —— 停不停都高或都低，这条归因作废，换路查");
            }

            printf("[act] A 段两次 ack=%d/%d 模型行数 %d→%d 引擎任务数 %d→%d | "
                   "B 段放弃后往返=%lldms（需 <1500） | C 段 ack %d/%d | "
                   "D 段 事件循环单轮最长=%lldms（需 <500）\n",
                   ack1, ack2, rowsBefore, rowsAfter, engBefore, engAfter,
                   (long long)roundTripMs, stressAcks, stressTotal,
                   (long long)worstLoopMs);

            /* ── E 段：空转连接不得阻塞主线程（§7-56 P0 的夹具）──
             * 旧形状：handleClient 在 GUI 线程上 waitForReadyRead(100)×2 ⇒ 一条"连上一个字节
             * 都不发"的连接让 newConnection 那一轮 ≥200ms；扩展 checkConnection 轮询就是这种
             * 客户端 ⇒ 用户每点一次插件，界面卡一下。
             * ⚠ 第一版用"压在空转后面的真请求往返"当证人——**无效**：Windows 命名管道允许
             *   写先于 accept 完成，waitForConnected 返回时字节常常已可读，往返量不到那两轮
             *   sleep（变异实测 17ms < 150 照样绿）。证人换成**直接量这一轮本身**：
             *   同一进程内自起一个 IpcServer（自己的 pid 管道，不碰产品实例），
             *   空转连接建立后连续泵事件 300ms，取最长一轮 ——
             *   修复前 ≥200（sleep 全落在这一轮里），修复后 ≈0~20。 */
            {
                const QString epipe = QStringLiteral("idm-idle-block-probe-%1")
                                          .arg(QCoreApplication::applicationPid());
                QLocalServer::removeServer(epipe);
                IpcServer srv;
                bool listened = false;
                {   // 临时把服务名指到探针私有管道（ipcServerName() 尊重 IDM_TEST_PIPE）
                    qputenv("IDM_TEST_PIPE", epipe.toUtf8());
                    listened = srv.start();
                }
                if (!listened) {
                    printf("[FAIL] E 段前置不成立：探针私有 IPC 服务起不来\n");
                    ++okIdleBlockMs;      // -1 → 0：判据行会读成"没跑过"⇒ 红
                } else {
                    QLocalSocket idleS;
                    idleS.connectToServer(epipe);
                    const bool idleConn = idleS.waitForConnected(2000);

                    /* ⚠ 读数必须是**包含 newConnection 的那一整轮**，而且要停掉 D 段那批产品定时器再量：
                     *   同一进程里状态栏 250ms / flush 100ms 等节拍会继续落进 E 段的轮次，
                     *   于是 ok8 量到的是"这一轮恰好有别的东西"而不是"空转连接阻塞了它"
                     *   （实测形状：干净面偶发 383~415ms＝没有归属的红）。
                     *   证人时刻＝服务端真的取尽这条连接的那一刻；用 drainStat().connections 认它，
                     *   它是 IpcServer 自己记的数，不是我另造的量。 */
                    QVector<QTimer*> eTimers;
                    const auto eObjs = w.findChildren<QObject*>();
                    for (QObject* o : eObjs)
                        if (auto* tm = qobject_cast<QTimer*>(o))
                            if (tm->isActive()) eTimers.append(tm);
                    for (QTimer* tm : eTimers) tm->stop();

                    qint64 worstE = -1;
                    bool eSawAccept = false;
                    QElapsedTimer win; win.start();
                    while (win.elapsed() < 3000) {
                        QElapsedTimer one; one.start();
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        const qint64 took = one.elapsed();
                        const int conns = IpcServer::drainStat().connections;
                        if (!eSawAccept && conns > 0) {
                            eSawAccept = true;
                            worstE = took;              // 就是这一轮在服务端处理连接
                            break;
                        }
                    }
                    for (QTimer* tm : eTimers) tm->start();
                    if (!eSawAccept) {
                        printf("[FAIL] E 段前置不成立：3s 内 newConnection 从未被服务端看到\n");
                        okIdleBlockMs = 9999;           // 没跑过＝红，不许静默绿
                        okIdleAckE = false;
                    } else {
                        okIdleBlockMs = worstE;
                        okIdleAckE = idleConn;          // 正例对照：连接真建立起来，读数才有归属
                        printf("[E] accept 轮次读数=%lldms（窗口第 %lldms 处命中，含该轮即止；"
                               "已先停 %d 个产品定时器排除节拍混淆）\n",
                               (long long)worstE, (long long)win.elapsed(), int(eTimers.size()));
                    }
                    idleS.abort();
                    qunsetenv("IDM_TEST_PIPE");   // 交还产品名（本探针后续不再起产品服务）
                }
            }
        }

        printf("%s activate 连投两次都被服务端确认处理（success=true×2，"
               "否则下条「没双份副作用」就是假的）\n", ok1 ? "[PASS]" : "[FAIL]");
        printf("%s 重复投递不产生第二次副作用（任务列表行数与引擎任务数都不变）\n",
               ok2 ? "[PASS]" : "[FAIL]");
        printf("%s 第二遍投递没有把窗口改成别的状态（仍可见、正常态）\n",
               ok3 ? "[PASS]" : "[FAIL]");
        printf("%s 两次投递之后 IPC 服务仍可用（没有第二套实例、服务没坏）\n",
               ok4 ? "[PASS]" : "[FAIL]");
        printf("%s 有一条不等回话的死连接在前，后续请求的往返仍 <1500ms（服务端没被拖住）\n",
               (ok5 && roundTripMs >= 0 && roundTripMs < 1500) ? "[PASS]" : "[FAIL]");
        printf("%s 连续投递 %d 次全部拿到回话（%d/%d；回话被丢掉就在这里红）\n",
               ok6 ? "[PASS]" : "[FAIL]", stressTotal, stressAcks, stressTotal);
        /* ── B 门（§7-57）：预算命中数，取代"单轮最长 <500ms"──
         * 三面都要成立，缺一个就是没有牙的门：
         *   ① 采到了样本（okIdleAck + budgetSamples>0）；
         *   ② 超过 T 的轮数 ≤ K（默认 K=1）——这是产品面；
         *   ③ canary 设了就必须**被这个计数器数进去**（overBudget ≥ 2：canary 那一轮 + 违规轮）。
         * ③ 是关键：干净面 overBudget 天然为 0，光看它红不了，那是一条期望值为 0、
         *    因此没有证人的护栏。canary 走产品自己的定时器通道注入一次已知超预算占用，
         *    只有当计数真的把它算进去，才证明这扇门认得出违规形状。
         * ⚠ 但 canary **不能白名单化**：第一版我写成 `overBudget<=1` 且 canary 只要求"已计入"，
         *    于是带负载时 canary(1)+违规若干里只要恰好只漏进 1 个就 PASS ——
         *    等于给门加了一条能吞掉真违规的路。现在反过来：canary 在场时它自己占 1 个名额，
         *    第 2 个才是产品的违规，所以 ≥2 必须红。 */
        const bool budgetCounted = (budgetSamples > 0);
        ok7 = (okIdleAck && budgetCounted && overBudgetTurns <= 1
               && (!canaryRequestedOut || (canaryCountedOut && overBudgetTurns >= 2)));
        /* E 段判据（§7-56）：探针私有 IpcServer 上，一条连上不发消息的连接所触发的
         * newConnection 轮次本身必须 <100ms。修复前 ≥200（两轮 waitForReadyRead(100)），
         * 修复后 ≈0~20。ackE＝连接真的建立起来（正例对照：没连上的 0ms 不算证据）。 */
        const bool ok8 = (okIdleAckE && okIdleBlockMs >= 0 && okIdleBlockMs < 100);
        printf("%s 空转连接（连上不发字节）不再阻塞主线程：它触发的那一轮最长=%lldms（需 <100）连接成立=%d\n",
               ok8 ? "[PASS]" : "[FAIL]", (long long)okIdleBlockMs, int(okIdleAckE));
        /* ⚠ 口径旗子必须印出来：第一版对照我切了 IDM_ROUND_CRITERION=count，两面读数几乎重叠
         *   （219~228 vs 203~227），当时无法区分"两种写法等价"还是"开关根本没生效"。
         *   没有这一行，那份对照就是不可解释的数据。 */
        printf("%s GUI 线程单轮占用：N=%d 轮里超 %dms 的轮数=%d（需 ≤1）｜最长一轮=%lldms "
               "撞每轮预算次数=%d 口径=%s%s\n",
               ok7 ? "[PASS]" : "[FAIL]", budgetSamples, budgetMsOut, overBudgetTurns,
               (long long)worstEventLoopTurnMs, truncatedTurns,
               roundCriterionCount ? "count(旧:第二参当墙钟上限)" : "deadline(新:QDeadlineTimer(20))",
               canaryRequestedOut ? (canaryCountedOut ? " ｜canary=已计入" : " ｜canary=未计入⇒门作废")
                                  : " ｜canary=未设（本轮无夹具，只量不证牙）");
        /* 第二个具名读数：本轮首次测量、无基线 ⇒ 只进报告行，不进退码、不配阈值。
         * 攒到能看出抖动振幅的轮数之后，再谈它是否进判据（阈值要从读数里取，不许拍）。 */
        printf("[report] 压在空转连接后面的那条真请求：往返=%lldms ack=%d"
               "（同窗口最长轮次=%lldms 取尽连接数 peakN=%d；本轮首次测量、无基线⇒不进判据、不配阈值）\n",
               (long long)idleBehindReplyMs, int(okIdleReply),
               (long long)turnBehindMs, nBehind);
        return (ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8
                && roundTripMs >= 0 && roundTripMs < 1500) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_OFFTHREAD_PROBE=1 ──
     * runOffThread 是组件自愈不再冻住 GUI 线程的唯一依赖，所以它自己要能被证伪。
     * 四条各管一半，缺一条就可能"看着对其实同步跑完了"：
     *   ① job 真的跑在**另一条**线程上；
     *   ② done 回到 context 所在线程（UI 与回调只能在那儿做）；
     *   ③ job 睡着的这 400ms 里，GUI 线程的事件循环**仍在走**（计时器有 tick）——
     *      这条才是"用户还点得动"的代理量，只看①②不足以证明没冻结；
     *   ④ done 恰好被调一次（不多不少，防止既同步调一次又投递一次）。 */
    if (qEnvironmentVariableIsSet("IDM_OFFTHREAD_PROBE")) {
        QThread* guiThread = QThread::currentThread();
        QThread* jobThread = nullptr;
        QThread* doneThread = nullptr;
        int doneCalls = 0;
        int ticks = 0;
        qint64 maxGapMs = 0;

        /* 判据必须是「相邻两次 tick 的最大间隔」，不是 tick 总数：
           总数在"先冻 400ms 再继续转"的场景下照样能攒到十几次，
           我第一版就是这么写错了 —— 同步执行的变异只红了①，③照样绿。 */
        QTimer pulse;
        QElapsedTimer sinceLast;
        QObject::connect(&pulse, &QTimer::timeout, [&] {
            ++ticks;
            if (sinceLast.isValid()) maxGapMs = qMax(maxGapMs, sinceLast.elapsed());
            sinceLast.restart();
        });
        pulse.start(40);
        sinceLast.start();

        QElapsedTimer wait;
        wait.start();
        runOffThread(
            &pulse,
            [&jobThread] {
                jobThread = QThread::currentThread();
                QThread::msleep(400);
            },
            [&doneThread, &doneCalls] {
                doneThread = QThread::currentThread();
                ++doneCalls;
            });

        while (doneCalls == 0 && wait.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        /* done 之后再多转一会儿，确认没有第二条重复投递 */
        for (int i = 0; i < 15; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(20);
        }
        pulse.stop();

        const bool okJob = (jobThread && jobThread != guiThread);
        const bool okDone = (doneThread == guiThread);
        const bool okLoop = (maxGapMs < 150);     // 40ms 心跳；阻塞 400ms 会直接顶到 ~400
        const bool okOnce = (doneCalls == 1);

        printf("[offthread] job=%p gui=%p done=%p ticks=%d maxGap=%lldms doneCalls=%d 用时=%lldms\n",
               (void*)jobThread, (void*)guiThread, (void*)doneThread,
               ticks, (long long)maxGapMs, doneCalls, (long long)wait.elapsed());
        printf("%s job 跑在工作线程，不在 GUI 线程上\n", okJob ? "[PASS]" : "[FAIL]");
        printf("%s done 回到 context 所在线程（UI 只能在那儿做）\n", okDone ? "[PASS]" : "[FAIL]");
        printf("%s GUI 事件循环没被打断（tick 最大间隔=%lldms，需 <150）\n",
               okLoop ? "[PASS]" : "[FAIL]", (long long)maxGapMs);
        printf("%s done 恰好一次\n", okOnce ? "[PASS]" : "[FAIL]");
        return (okJob && okDone && okLoop && okOnce) ? 0 : 1;
    }

    /* ── 诊断模式：IDM_FLUSH_PROBE=1 ──
     * 双向证明"回话必须推完再销毁"这条修复到底管不管用 —— 不是靠"连投 50 次没丢"
     * 那种只能证明正常情况正常的断言。
     *
     * 场景：服务端要发一份 2MB 的回话，对端故意晚 250ms 才开始读 ⇒ 写必然被背压挡住，
     * 于是出现"还有字节没发出去"的状态。然后两个分支只做一件不同的事，
     * 最后**都立刻 delete 那个 socket**：
     *   旧写法：write → flush → waitForBytesWritten(2000) 一次 → 销毁
     *           （Qt 的 waitForBytesWritten 只保证"这段时间内写出去过字节"，
     *             不保证推完 ⇒ 剩余字节随 socket 一起没了）
     *   新写法：write → flush → drainSocketBeforeClose(2000) 推到 bytesToWrite()==0 → 销毁
     * 判据：旧分支客户端收到的字节 **少于** 2MB（丢，必红），新分支 **正好等于** 2MB（必绿）。
     *
     * ⚠️ 与生产的差别要说清楚：生产里销毁是 deleteLater()（下一轮事件循环才真删），
     * 所以只有那一轮被别的事占住时才会真丢 —— 这正是它在 CI 上表现为间歇红、
     * 而本机连投 600 次都不丢的原因。本探针把销毁改成即时，把同一个机制变成
     * **可重复触发**，用来证明"先推完再销毁"确实是止损的那一步。
     * 生产侧的现行信号是日志字段 `送达=0/1`（drain 的返回值）：
     * CI 再看到 `送达=0` 就是这条路；若全是 `送达=1` 而客户端仍超时，就得换方向查。 */
    if (qEnvironmentVariableIsSet("IDM_FLUSH_PROBE")) {
        const int PAYLOAD = 2 * 1024 * 1024;
        const QByteArray payload(PAYLOAD, 'x');
        const QString pipe = QStringLiteral("idm-flush-probe-%1")
                                 .arg(QCoreApplication::applicationPid());
        qint64 drainElapsedMs = -1;

        auto runCase = [&](bool useDrain, int readerDelayMs) -> qint64 {
            QLocalServer::removeServer(pipe);
            QLocalServer srv;
            if (!srv.listen(pipe)) return -1;
            QLocalSocket* serverSide = nullptr;
            QObject::connect(&srv, &QLocalServer::newConnection, [&srv, &serverSide] {
                serverSide = srv.nextPendingConnection();
            });

            std::atomic<qint64> received { 0 };
            std::atomic<bool> clientDone { false };
            QElapsedTimer spentHere;
            spentHere.start();
            QThread* client = QThread::create([&] {
                QLocalSocket s;
                s.connectToServer(pipe);
                if (s.waitForConnected(3000)) {
                    if (readerDelayMs >= 0)
                        QThread::msleep(readerDelayMs);      // 晚读：制造/缓解背压
                    while (s.state() == QLocalSocket::ConnectedState) {
                        s.waitForReadyRead(200);
                        received += s.readAll().size();
                    }
                    received += s.readAll().size();          // 断开后缓冲里的残余也要读完
                }
                clientDone = true;
            });
            client->start();

            for (int i = 0; i < 300 && !serverSide; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
            qint64 pendingAfter = -1;
            if (serverSide) {
                serverSide->write(payload);
                serverSide->flush();
                bool waitRet = false;
                bool drained = false;
                if (useDrain) {
                    drained = drainSocketBeforeClose(serverSide, 2000);
                } else {
                    waitRet = serverSide->waitForBytesWritten(2000);   // 旧写法：等一次就算数
                    drained = (serverSide->bytesToWrite() == 0);
                }
                pendingAfter = serverSide->bytesToWrite();
                if (useDrain) drainElapsedMs = spentHere.elapsed();
                printf("[flush] %s(delay=%d) 单次等待返回=%d 销毁前待发=%lld 推完=%d 用时=%lldms\n",
                       useDrain ? "drain" : "旧(只等一次)", readerDelayMs,
                       waitRet ? 1 : 0, (long long)pendingAfter, drained ? 1 : 0,
                       (long long)spentHere.elapsed());
                delete serverSide;
                serverSide = nullptr;
            }
            while (!clientDone.load()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
            client->wait(5000);
            delete client;
            return received.load();
        };

        /* 两组情形都跑了，结论是**负结果**，照实写在这儿：
             · 对端完全不读 ⇒ 单次 waitForBytesWritten 之后 bytesToWrite() 也已经是 0
               （写对端已断开的管道会被直接丢弃，不会留下"待发字节"）；
             · 对端晚 300ms 读 ⇒ 旧写法（只等一次）与新写法（drain 到空）都把 2MB 完整送到。
           也就是说：**这台机器上构造不出"返回时仍有待发字节"的状态**，
           因此 drainSocketBeforeClose 的必要性在本地无法证明，也无法证伪 ——
           它的判据只能来自生产的 `送达=0/1` 字段（见 docs/ARCHITECTURE.md §7）。
           本探针保留的是这个 helper 本身可证的两条契约： */
        const qint64 oldRx = runCase(false, 300);
        const qint64 newRx = runCase(true, 300);
        QLocalServer::removeServer(pipe);

        /* ① 契约：推完再销毁 ⇒ 晚读的对端也必须收到完整 2MB（少一字节就是 drain 写坏了） */
        const bool okDeliver = (newRx == PAYLOAD);
        /* ② 契约：预算必须封顶 —— 这个函数跑在 GUI 线程上，它不能把人钉死。 */
        const bool okBounded = (drainElapsedMs >= 0 && drainElapsedMs < 2600);
        printf("[flush] 本机负结果：旧写法收到 %lld/%d，新写法收到 %lld/%d ⇒ 本地无法复现丢字节\n",
               (long long)oldRx, PAYLOAD, (long long)newRx, PAYLOAD);
        printf("%s 推完再销毁：对端晚读 300ms 仍收到完整 2MB\n",
               okDeliver ? "[PASS]" : "[FAIL]");
        printf("%s drain 的等待被预算封顶（实测 %dms，需 <2600ms，跑在 GUI 线程上）\n",
               okBounded ? "[PASS]" : "[FAIL]", (int)drainElapsedMs);
        return (okDeliver && okBounded) ? 0 : 1;
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
