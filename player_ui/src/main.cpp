#include <QSettings>
#include <QAbstractItemModel>
#include <QDir>
#include <cmath>
#include <mutex>
#include <map>
#include <atomic>
#include <algorithm>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTimer>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QFileInfo>
#include <QFile>
#include <QPainter>

#include "cover_provider.h"
#include "library_service.h"
#include "models.h"
#include "player_controller.h"
#include "playlist_service.h"
#include "playlist_importer.h"
#include "decrypt_service.h"
#include "cover_service.h"
#include "settings_service.h"
#include "decryptor.hpp"
#include "window_helper.h"
#include "library_tools.h"
#include "taskbar_helper.h"
#include "frame_diag.h"
#include "render_guard.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#endif
#include <QIcon>
#include <QThreadPool>
#include <QThread>

// 滚动基准用:记录界面线程上每个事件的处理耗时 (只在 KP_TEST_SCROLLBENCH 录制期间开启)
namespace bench {
std::atomic<bool> on{false};
std::map<QString, std::pair<double, int>> cost; // 事件类型 + 接收者 → (总毫秒, 次数)
std::vector<std::pair<double, QString>> slow;   // 单次 ≥ 4ms 的事件
}
class TimedApp : public QGuiApplication {
public:
    using QGuiApplication::QGuiApplication;
    bool notify(QObject* r, QEvent* e) override {
        if (!bench::on.load() || depth_ > 0 || QThread::currentThread() != thread()) return QGuiApplication::notify(r, e);
        ++depth_;
        QElapsedTimer t;
        t.start();
        // 接收者在处理后可能已被删除 (DeferredDelete 等):名字先取好
        const int type = e->type();
        QString who = QString::fromLatin1(r->metaObject()->className());
        if (r->parent() && (type == QEvent::Timer || type == QEvent::MetaCall))
            who += QStringLiteral("<") + QString::fromLatin1(r->parent()->metaObject()->className());
        if (!r->objectName().isEmpty()) who += QStringLiteral("#") + r->objectName();
        const bool res = QGuiApplication::notify(r, e);
        const double ms = t.nsecsElapsed() / 1e6;
        --depth_;
        const QString key = QString::number(type) + QStringLiteral(" ") + who;
        auto& c = bench::cost[key];
        c.first += ms;
        c.second += 1;
        if (ms >= 4.0) bench::slow.push_back({ms, key});
        return res;
    }
private:
    int depth_ = 0;
};

// 离屏截图驱动 (开发验证用):--screenshot <输出目录>
// 依次调用 QML 根对象上的自动化函数并截图,用于在无显示器环境下检查排版与配色
// flowDir 非空时改为验证「首次使用」流程:空曲库引导 → 添加文件夹 → 识别进度 → 专辑墙 → 打开专辑点歌播放
static void RunScreenshots(QQmlApplicationEngine& engine, LibraryService& lib, PlayerController& player, const QString& outDir,
                           const QString& flowDir) {
    QDir().mkpath(outDir);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
    if (!window) return;

    struct Step {
        QString name;
        std::function<void()> action;
        int waitMs;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    auto call = [window](const char* fn, QVariant arg = {}) {
        if (arg.isValid()) QMetaObject::invokeMethod(window, fn, Q_ARG(QVariant, arg));
        else QMetaObject::invokeMethod(window, fn);
    };
    if (qEnvironmentVariableIsSet("KP_TEST_ORGANIZE")) {
        // 回归:整理 (移动) 后立即点播放、切歌、查重处理后再播放,不应崩溃
        auto tools = [&engine]() { return qobject_cast<LibraryTools*>(engine.rootContext()->contextProperty("libtools").value<QObject*>()); };
        steps->push_back({"o1_playing", [&player] { player.playAlbum(0, 0); }, 800});
        steps->push_back({"o2_planned", [tools, outDir] {
            tools()->setTargetUrl(QDir(outDir).absoluteFilePath("organized"));
            tools()->buildPlan();
        }, 3000});
        steps->push_back({"o3_executed", [tools] { tools()->execute(); }, 8000});
        steps->push_back({"o4_play_after", [&player] { player.playAlbum(2, 1); }, 1500});
        steps->push_back({"o5_next", [&player] { player.next(); player.next(); }, 1500});
        steps->push_back({"o6_dedupe", [tools, call] { call("automationGo", QStringLiteral("dedupe")); tools()->findDuplicates(); }, 6000});
        steps->push_back({"o7_applied", [tools] { tools()->applyDedupe(1); }, 6000});
        steps->push_back({"o8_play_after", [&player] { player.playAlbum(1, 0); player.next(); }, 2000});
    } else if (qEnvironmentVariableIsSet("KP_TEST_PICK")) {
        // 点歌设置的四种组合 + 右键菜单的「播放」(now) 不受设置影响;歌词查找与网易云 JSON 行
        auto dump = [&player](const char* tag) {
            auto* m = qobject_cast<QAbstractItemModel*>(player.queueModel());
            QStringList t;
            for (int i = 0; i < m->rowCount() && i < 14; ++i) t << m->index(i, 0).data(Qt::UserRole + 2).toString();
            qInfo().noquote() << "qml PICK" << tag << "pos" << player.queuePosition() << "now" << player.title() << "|" << t.join(" / ");
        };
        auto setMode = [](int click, int queue) { QSettings().setValue("player/clickAction", click); QSettings().setValue("player/queueMode", queue); };
        auto lyr = [&player](const char* tag) {
            QStringList t;
            const QVariantList l = player.lyrics();
            for (int i = 0; i < l.size() && i < 4; ++i) t << QString::number(l[i].toMap().value("time").toDouble()) + ":" + l[i].toMap().value("text").toString();
            qInfo().noquote() << "qml LYR" << tag << player.title() << "n" << l.size() << "|" << t.join(" / ");
        };
        steps->push_back({"p00", [&player, setMode] { setMode(0, 0); player.playAlbum(0, 0); }, 900});
        steps->push_back({"p01_now_append", [&player, setMode, dump] { setMode(0, 1); qInfo() << "qml PICK r" << player.pickAlbum(1, 2); dump("now+append"); }, 600});
        steps->push_back({"p02_next_append", [&player, setMode, dump] { setMode(0, 0); player.playAlbum(0, 0); setMode(1, 1); qInfo() << "qml PICK r" << player.pickAlbum(1, 2); dump("next+append"); }, 600});
        steps->push_back({"p03_next_append_whole", [&player, dump] { qInfo() << "qml PICK r" << player.pickAlbum(2, 1, true); dump("next+append whole"); }, 600});
        steps->push_back({"p04_next_replace", [&player, setMode, dump] { setMode(0, 0); player.playAlbum(0, 0); setMode(1, 0); qInfo() << "qml PICK r" << player.pickAlbum(1, 2); dump("next+replace"); }, 600});
        steps->push_back({"p05_menu_now", [&player, dump] { qInfo() << "qml PICK r" << player.pickAlbum(1, 3, false, true); dump("menu now"); }, 600});
        steps->push_back({"p06_reset", [&player, setMode, call] { setMode(0, 0); call("automationPlayTitle", QStringLiteral("叶惠美")); }, 1200});
        steps->push_back({"p07_json_vip", [&player, lyr] { player.next(); player.next(); }, 1500});
        steps->push_back({"p08_pure_json", [&player, lyr] { lyr("vip"); player.next(); player.next(); }, 1500});
        steps->push_back({"p09_zh", [&player, lyr, call] { lyr("purejson"); call("automationPlayTitle", QStringLiteral("THE BOOK")); }, 1500});
        steps->push_back({"p10_done", [&player, lyr] { lyr("zh"); }, 300});
    } else if (qEnvironmentVariableIsSet("KP_TEST_NOTES")) {
        // 文件夹备注 + 导入列表滚动 (配合 KP_TEST_PLAYLIST_FAKE=1)
        steps->push_back({"n01_panel", [call, &lib] {
            if (lib.folders().size() > 0) lib.setFolderNote(lib.folders()[0], QStringLiteral("无损收藏"));
            call("automationLibraryPanel", true);
        }, 1000});
        steps->push_back({"n02_import", [call, &lib] {
            call("automationLibraryPanel", false);
            if (lib.folders().size() > 0) lib.setFolderNote(lib.folders()[0], QString());
            call("automationImport", true);
            call("automationImportGo", QStringLiteral("kugou:1067062"));
        }, 2600});
        steps->push_back({"n03_scrolled", [call] { call("automationImportScroll", 100000); }, 120});
        steps->push_back({"n04_scrolled_late", [] {}, 600});
    } else if (qEnvironmentVariableIsSet("KP_TEST_RESCAN")) {
        // 重扫没有变化时不刷新:播放中打开沉浸页 → 手动重扫 → 文件夹里写一个歌词文件 (触发监控) → 看曲库代数是否不变
        auto gen0 = std::make_shared<int>(0);
        steps->push_back({"r01_play", [call, &player] { player.playAlbum(0, 0); call("automationNowPlaying", true); }, 2500});
        steps->push_back({"r02_rescan", [&lib, gen0] { *gen0 = lib.generation(); lib.rescan(); }, 4000});
        steps->push_back({"r03_touch", [&lib, gen0] {
            std::fprintf(stderr, "KP_RESCAN manual: gen %d -> %d unchanged=%d\n", *gen0, lib.generation(),
                         lib.lastScan().value("unchanged").toBool());
            *gen0 = lib.generation();
            const QString f = lib.folders().value(0) + "/kp_watch_probe.lrc";
            QFile probe(f);
            if (probe.open(QIODevice::WriteOnly)) probe.write("[00:00.00]probe\n");
        }, 6000});
        steps->push_back({"r04_after", [&lib, gen0] {
            std::fprintf(stderr, "KP_RESCAN watch: gen %d -> %d unchanged=%d shown=%d\n", *gen0, lib.generation(),
                         lib.lastScan().value("unchanged").toBool(), lib.lastScan().value("shown").toBool());
            QFile::remove(lib.folders().value(0) + "/kp_watch_probe.lrc");
            *gen0 = lib.generation();
            // 真有新歌:复制一首进来,应当刷新
            auto snap = lib.snapshot();
            if (!snap->songs.empty())
                QFile::copy(QString::fromStdWString(snap->songs[0]->file_path), lib.folders().value(0) + "/kp_watch_probe.flac");
        }, 7000});
        steps->push_back({"r05_added", [&lib, gen0] {
            std::fprintf(stderr, "KP_RESCAN added: gen %d -> %d unchanged=%d\n", *gen0, lib.generation(),
                         lib.lastScan().value("unchanged").toBool());
            QFile::remove(lib.folders().value(0) + "/kp_watch_probe.flac");
        }, 6000});
        steps->push_back({"r06_removed", [&lib, gen0] {
            std::fprintf(stderr, "KP_RESCAN removed: gen %d\n", lib.generation());
        }, 300});
    } else if (qEnvironmentVariableIsSet("KP_TEST_IMPORT")) {
        // 导入歌单:歌单墙的「导入」格 → 卡片 (空 / 识别出链接 / 纯数字选平台 / 读取中 / 结果 / 筛选) → 存为歌单
        // 配合 KP_TEST_PLAYLIST_FAKE=1 使用 (用当前曲库拼的假歌单)
        steps->push_back({"i01_wall", [call] { call("automationGo", QStringLiteral("playlists")); }, 1600});
        steps->push_back({"i02_open_100", [call] { call("automationImport", true); }, 100});
        steps->push_back({"i03_open", [] {}, 900});
        steps->push_back({"i04_detect", [call] { call("automationImportLink", QStringLiteral("https://www.kugou.com/yy/special/single/1067062.html")); }, 500});
        steps->push_back({"i05_platform", [call] { call("automationImportGo", QStringLiteral("1067062")); }, 600});
        steps->push_back({"i06_loading", [call] { call("automationImportGo", QStringLiteral("kugou:1067062")); }, 300});
        steps->push_back({"i07_ready_200", [] {}, 600});
        steps->push_back({"i08_ready", [] {}, 1400});
        steps->push_back({"i09_missing", [call] { call("automationImportFilter", 2); }, 900});
        steps->push_back({"i10_local", [call] { call("automationImportFilter", 1); }, 900});
        steps->push_back({"i11_small", [window, call] { call("automationImportFilter", 0); window->resize(980, 640); }, 1400});
        steps->push_back({"i12_saved", [window, call] { window->resize(1600, 1000); call("automationImportSave"); }, 1800});
        steps->push_back({"i13_err", [call] { call("automationImport", true); call("automationImportGo", QStringLiteral("https://example.com/abc")); }, 900});
    } else if (qEnvironmentVariableIsSet("KP_TEST_COVER")) {
        // 封面面板:专辑墙 → 真实点一下横幅上的封面按钮 → 选一张 → 开始导入
        // 用合成鼠标事件点 objectName=coverBtn,验证的是「点击 → 信号 → 面板」整条链路
        auto clickCoverBtn = [window]() {
            auto* it = window->findChild<QQuickItem*>("coverBtn");
            if (!it) { std::fprintf(stderr, "KP_COVER_BTN not found\n"); return; }
            const QPointF c = it->mapToScene(QPointF(it->width() / 2, it->height() / 2));
            std::fprintf(stderr, "KP_COVER_BTN at %.0f,%.0f size %.0fx%.0f\n", c.x(), c.y(),
                         it->width(), it->height());
            QMouseEvent mv(QEvent::MouseMove, c, window->mapToGlobal(c), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &mv);
            QMouseEvent pr(QEvent::MouseButtonPress, c, window->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &pr);
            QMouseEvent rl(QEvent::MouseButtonRelease, c, window->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &rl);
        };
        // 等首次扫描完成(真实使用里用户不会在扫描中开面板)
        steps->push_back({"v01_wall", [call] { call("automationGo", QStringLiteral("albums")); call("automationWallSort", 3); }, 12000});
        steps->push_back({"v02_click", clickCoverBtn, 1400});
        // 矮窗口:底部按钮必须还在视野内且可点
        steps->push_back({"v02b_short", [window] { window->resize(1180, 620); }, 1500});
        steps->push_back({"v03_runall", [call] { call("automationCoverRunAll"); }, 45000});
        steps->push_back({"v04_report", [call] { call("automationCoverLog"); }, 2500});
        steps->push_back({"v04_running", [call] { call("automationCoverRun"); }, 3000});
    } else if (qEnvironmentVariableIsSet("KP_TEST_BURST")) {
        // 封面墙(测试页):入场炸开 → 落定 → 滚动 → 停下,同时统计帧间隔
        auto swaps = std::make_shared<std::vector<qint64>>();
        auto mtx = std::make_shared<std::mutex>();
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto rec = std::make_shared<std::atomic<bool>>(false);
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [=]() {
            if (!rec->load()) return;
            std::lock_guard<std::mutex> l(*mtx);
            swaps->push_back(clock->nsecsElapsed());
        }, Qt::DirectConnection);
        auto dump = [=](const char* tag) {
            std::vector<qint64> v;
            { std::lock_guard<std::mutex> l(*mtx); v = *swaps; swaps->clear(); }
            std::vector<double> d;
            for (size_t i = 1; i < v.size(); ++i) d.push_back((v[i] - v[i - 1]) / 1e6);
            std::sort(d.begin(), d.end());
            auto pct = [&](double p) { return d.empty() ? 0.0 : d[std::min(d.size() - 1, size_t(p * d.size()))]; };
            int jank = 0;
            for (double x : d) jank += x > 25.0;
            std::fprintf(stderr, "KP_BURST %-12s frames=%zu p50=%.1f p90=%.1f p99=%.1f max=%.1f >25ms=%d\n",
                         tag, v.size(), pct(0.5), pct(0.9), pct(0.99), d.empty() ? 0.0 : d.back(), jank);
        };
        auto wheel = [window](int n) {
            const QPointF at(window->width() / 2.0, window->height() / 2.0);
            // 先让鼠标"进入"内容区:平滑滚动组件要看到 hover 才会接管滚轮
            QMouseEvent mv(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &mv);
            for (int i = 0; i < n; ++i) {
                QWheelEvent we(at, window->mapToGlobal(at), QPoint(), QPoint(0, -120),
                               Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &we);
            }
        };
        QSize burstSize(1700, 1050);
        if (const QByteArray s = qgetenv("KP_BENCH_SIZE"); !s.isEmpty()) {
            const QList<QByteArray> wh = s.split('x');
            if (wh.size() == 2) burstSize = QSize(wh[0].toInt(), wh[1].toInt());
        }
        steps->push_back({"x00_big", [window, burstSize] { window->resize(burstSize); }, 1200});
        steps->push_back({"x01_enter", [call, rec] { rec->store(true); call("automationGo", QStringLiteral("burst")); }, 300});
        steps->push_back({"x02_mid", [] {}, 420});
        steps->push_back({"x03_done", [dump, rec] { rec->store(false); dump("intro"); }, 300});
        steps->push_back({"x04_scroll", [wheel, rec] { rec->store(true); wheel(14); }, 900});
        steps->push_back({"x05_scrolled", [dump, rec] { rec->store(false); dump("scroll-v"); }, 300});
        // 直接驱动画布:滚轮事件链在无窗口焦点时不可靠,这里走 automation 入口
        steps->push_back({"x06_pan_h", [call, rec] {
            rec->store(true);
            call("automationBurstPan", QVariantList{900, 400});
        }, 900});
        steps->push_back({"x07_panned", [dump, rec] { rec->store(false); dump("pan"); }, 300});
        // 推到右下角极限:验证边界不会露出没有封面的空白
        steps->push_back({"x07b_corner", [call] {
            call("automationBurstPan", QVariantList{99999, 99999});
        }, 900});
        steps->push_back({"x07c_at_corner", [] {}, 400});
        steps->push_back({"x08_zoom_in", [call, rec] {
            rec->store(true);
            call("automationBurstZoom", 1.5);
        }, 900});
        steps->push_back({"x09_zoomed", [dump, rec] { rec->store(false); dump("zoom-in"); }, 300});
        steps->push_back({"x10_zoom_out", [call, rec] {
            rec->store(true);
            call("automationBurstZoom", 0.7);
        }, 900});
        steps->push_back({"x11_done", [dump, rec] { rec->store(false); dump("zoom-out"); }, 300});
        // 鼠标横扫:验证悬停路径(格子放大 / 边缘感应)会不会把帧拖垮。
        // 用定时器按 ~8ms 分发,模拟真实鼠标移动;一次循环发完的话全挤在一帧里,测不出东西
        steps->push_back({"x12_hover_sweep", [window, rec] {
            rec->store(true);
            auto* t = new QTimer(window);
            auto* k = new int(0);
            t->setInterval(8);
            QObject::connect(t, &QTimer::timeout, window, [=]() {
                if (*k >= 180) {
                    t->stop();
                    t->deleteLater();
                    delete k;
                    return;
                }
                const int i = (*k)++;
                const QPointF at(120 + (i % 60) * 22, 260 + (i / 60) * 90);
                QMouseEvent mv(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &mv);
            });
            t->start();
        }, 2200});
        steps->push_back({"x13_hovered", [dump, rec] { rec->store(false); dump("hover-sweep"); }, 300});
        // 点开一个封面:验证飞向详情页的动画起点是否正确(不应出现硬切)
        steps->push_back({"x14_fly", [call, rec] { rec->store(true); call("automationBurstOpen", 10); }, 90});
        steps->push_back({"x15_flying", [] {}, 120});
        steps->push_back({"x16_landed", [dump, rec] { rec->store(false); dump("open-album"); }, 900});
    } else if (qEnvironmentVariableIsSet("KP_TEST_SWITCH")) {
        // 页面切换开销:对比"从专辑墙切到歌手页"和"从封面墙切到歌手页"。
        // KP_SWITCH_FROM=burst|albums 指定起点
        auto swaps = std::make_shared<std::vector<qint64>>();
        auto mtx = std::make_shared<std::mutex>();
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto rec = std::make_shared<std::atomic<bool>>(false);
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [=]() {
            if (!rec->load()) return;
            std::lock_guard<std::mutex> l(*mtx);
            swaps->push_back(clock->nsecsElapsed());
        }, Qt::DirectConnection);
        auto dump = [=](const char* tag) {
            std::vector<qint64> v;
            { std::lock_guard<std::mutex> l(*mtx); v = *swaps; swaps->clear(); }
            std::vector<double> d;
            for (size_t i = 1; i < v.size(); ++i) d.push_back((v[i] - v[i - 1]) / 1e6);
            std::sort(d.begin(), d.end());
            auto pct = [&](double p) { return d.empty() ? 0.0 : d[std::min(d.size() - 1, size_t(p * d.size()))]; };
            int jank = 0;
            for (double x : d) jank += x > 25.0;
            std::fprintf(stderr, "KP_SWITCH %-14s frames=%zu p50=%.1f p90=%.1f p99=%.1f max=%.1f >25ms=%d\n",
                         tag, v.size(), pct(0.5), pct(0.9), pct(0.99), d.empty() ? 0.0 : d.back(), jank);
        };
        const bool fromBurst = qgetenv("KP_SWITCH_FROM") == "burst";
        QSize swSize(1700, 1050);
        if (const QByteArray s = qgetenv("KP_BENCH_SIZE"); !s.isEmpty()) {
            const QList<QByteArray> wh = s.split('x');
            if (wh.size() == 2) swSize = QSize(wh[0].toInt(), wh[1].toInt());
        }
        steps->push_back({"t0_resize", [window, swSize] { window->resize(swSize); }, 1400});
        steps->push_back({"t1_from", [call, fromBurst] {
            call("automationGo", fromBurst ? QStringLiteral("burst") : QStringLiteral("albums"));
        }, 3000});
        steps->push_back({"t2_to_artists", [call, rec] {
            rec->store(true);
            call("automationGo", QStringLiteral("artists"));
        }, 2200});
        steps->push_back({"t3_settled", [dump, rec] { rec->store(false); dump("to-artists"); }, 1600});
        steps->push_back({"t4_back", [call, fromBurst] { call("automationGo", fromBurst ? QStringLiteral("burst") : QStringLiteral("albums")); }, 2200});
        steps->push_back({"t5_back_settled", [] {}, 1200});
        // 第二次进歌手页:页面已常驻,用来区分"首次创建成本"和"页面本身持续开销"
        steps->push_back({"t5b_artists_again", [call, rec] {
            rec->store(true);
            call("automationGo", QStringLiteral("artists"));
        }, 2200});
        steps->push_back({"t5c_again_done", [dump, rec] { rec->store(false); dump("artists-2nd"); }, 1200});
        steps->push_back({"t6_to_songs", [call, rec] {
            rec->store(true);
            call("automationGo", QStringLiteral("songs"));
        }, 2200});
        steps->push_back({"t7_done", [dump, rec] { rec->store(false); dump("to-songs"); }, 1200});
    } else if (qEnvironmentVariableIsSet("KP_TEST_DESKLYR")) {
        // 桌面歌词逐字染色:播一首歌 → 打开桌面歌词 → 停住等外部截屏
        const QString song = qEnvironmentVariable("KP_DESKLYR_SONG", QStringLiteral("钟无艳"));
        steps->push_back({"d1_resize", [window] { window->resize(1400, 900); }, 1200});
        steps->push_back({"d2_play", [call, song] {
            call("automationPlayTitle", song);
        }, 3000});
        steps->push_back({"d3_open", [call] {
            call("automationDesktopLyrics", true);
        }, 4000});
        steps->push_back({"d4_hold", [] {}, 90000});
    } else if (qEnvironmentVariableIsSet("KP_TEST_SORTUI")) {
        // 排序入口与横幅对齐:专辑墙 (默认 / 菜单开着 / 播放中) 与歌曲页 (默认 / 改过排序),再看矮窗口
        steps->push_back({"s01_wall", [call] { call("automationGo", QStringLiteral("albums")); }, 1600});
        steps->push_back({"s02_wall_menu", [call] { call("automationWallSortMenu", true); }, 800});
        steps->push_back({"s03_wall_playing", [call, &player] { call("automationWallSortMenu", false); player.playAlbum(0, 0); }, 1800});
        steps->push_back({"s04_songs", [call] { call("automationGo", QStringLiteral("songs")); }, 1600});
        steps->push_back({"s05_songs_sorted", [call] { call("automationSongsSort", 1); }, 1000});
        steps->push_back({"s06_wall_short", [window, call] { call("automationSongsSort", 0); window->resize(1500, 760); call("automationGo", QStringLiteral("albums")); }, 1800});
    } else if (qEnvironmentVariableIsSet("KP_TEST_SCROLLBENCH")) {
        // 滚动基准:在歌曲页 / 专辑墙用合成的滚轮事件连续滚动,逐帧记录帧间隔,对比
        // 暂停 / 播放、减弱动效开 / 关、鼠标停在列表上 / 侧栏上
        auto* st = qobject_cast<SettingsService*>(engine.rootContext()->contextProperty("settings").value<QObject*>());
        auto swaps = std::make_shared<std::vector<qint64>>();
        auto mtx = std::make_shared<std::mutex>();
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto recording = std::make_shared<std::atomic<bool>>(false);
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [=]() {
            if (!recording->load()) return;
            std::lock_guard<std::mutex> l(*mtx);
            swaps->push_back(clock->nsecsElapsed());
        }, Qt::DirectConnection);
        auto bench = [=](const QString& label, QPointF hoverAt) {
            QMouseEvent mv(QEvent::MouseMove, hoverAt, window->mapToGlobal(hoverAt), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &mv);
            { std::lock_guard<std::mutex> l(*mtx); swaps->clear(); }
            bench::cost.clear();
            bench::slow.clear();
            bench::on.store(true);
            recording->store(true);
            std::fprintf(stderr, "KP_BENCH begin %s\n", qPrintable(label));
            auto ticks = std::make_shared<int>(0);
            auto* t = new QTimer(window);
            t->setInterval(45);
            QObject::connect(t, &QTimer::timeout, window, [=]() {
                const int k = (*ticks)++;
                if (k >= 66) {
                    t->stop();
                    t->deleteLater();
                    QTimer::singleShot(500, window, [=]() {
                        recording->store(false);
                        bench::on.store(false);
                        std::vector<std::pair<double, QString>> top;
                        double total = 0;
                        for (auto& [k, c] : bench::cost) { top.push_back({c.first, k + QString(" x%1").arg(c.second)}); total += c.first; }
                        std::sort(top.rbegin(), top.rend());
                        std::fprintf(stderr, "KP_COST %s total=%.0fms\n", qPrintable(label), total);
                        for (size_t i = 0; i < std::min<size_t>(8, top.size()); ++i)
                            std::fprintf(stderr, "KP_COST   %7.1fms  %s\n", top[i].first, qPrintable(top[i].second));
                        std::sort(bench::slow.rbegin(), bench::slow.rend());
                        for (size_t i = 0; i < std::min<size_t>(5, bench::slow.size()); ++i)
                            std::fprintf(stderr, "KP_SLOW   %7.1fms  %s\n", bench::slow[i].first, qPrintable(bench::slow[i].second));
                        std::vector<qint64> v;
                        { std::lock_guard<std::mutex> l(*mtx); v = *swaps; }
                        std::vector<double> d;
                        for (size_t i = 1; i < v.size(); ++i) d.push_back((v[i] - v[i - 1]) / 1e6);
                        std::sort(d.begin(), d.end());
                        auto pct = [&](double p) { return d.empty() ? 0.0 : d[std::min(d.size() - 1, size_t(p * d.size()))]; };
                        int janky = 0;
                        for (double x : d) janky += x > 25.0;
                        std::fprintf(stderr, "KP_BENCH %-28s frames=%zu p50=%.1f p90=%.1f p99=%.1f max=%.1f >25ms=%d\n",
                                     qPrintable(label), v.size(), pct(0.5), pct(0.9), pct(0.99), d.empty() ? 0.0 : d.back(), janky);
                    });
                    return;
                }
                // 每 22 格换一次方向,不撞到底
                const int dir = (k / 22) % 2 ? 1 : -1;
                const QPointF at(900, 500);
                QWheelEvent we(at, window->mapToGlobal(at), QPoint(), QPoint(0, 120 * dir), Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &we);
            });
            t->start();
        };
        const QPointF overList(900, 500), overNav(36, 640);
        auto setup = [=, &player](bool play, bool reduced) {
            if (st) st->setReducedMotion(reduced);
            if (play && !player.playing()) player.toggle();
            if (!play && player.playing()) player.toggle();
        };
        steps->push_back({"b00_songs", [call, &player, window, &engine] {
            // 基准窗口默认 1400x900(逻辑)。高 DPI 屏上真正吃 GPU 的是全屏尺寸,
            // 用 KP_BENCH_SIZE=2844x1600 可以按逻辑尺寸铺满屏幕来测。
            QSize benchSize(1400, 900);
            if (const QByteArray s = qgetenv("KP_BENCH_SIZE"); !s.isEmpty()) {
                const QList<QByteArray> wh = s.split('x');
                if (wh.size() == 2) benchSize = QSize(wh[0].toInt(), wh[1].toInt());
            }
            if (benchSize.width() > 320 && benchSize.height() > 240) window->resize(benchSize);
            if (auto* fd = engine.rootContext()->contextProperty("frameDiag").value<QObject*>()) fd->setProperty("enabled", true);
            player.playAlbum(0, 0);
            player.toggle();
            call("automationGo", QStringLiteral("songs"));
        }, 2500});
        struct Case { const char* name; bool play; bool reduced; bool hoverList; };
        const Case cases[] = {
            {"songs paused hover", false, false, true},
            {"songs paused nav", false, false, false},
            {"songs playing hover", true, false, true},
            {"songs playing nav", true, false, false},
            {"songs playing reduced", true, true, true},
            {"songs paused reduced", false, true, true},
        };
        for (const Case& c : cases) {
            if (qEnvironmentVariableIsSet("KP_BENCH_WALL")) break;   // 只测专辑墙
            steps->push_back({QStringLiteral("b_%1").arg(QString(c.name).replace(' ', '_')), [=] {
                setup(c.play, c.reduced);
                QTimer::singleShot(400, window, [=] { bench(c.name, c.hoverList ? overList : overNav); });
            }, 4600});
        }
        steps->push_back({"b10_wall", [call, st] { if (st) st->setReducedMotion(false); call("automationGo", QStringLiteral("albums")); }, 2000});
        const Case wall[] = {
            {"wall paused hover", false, false, true},
            {"wall playing hover", true, false, true},
            {"wall playing nav", true, false, false},
            {"wall playing reduced", true, true, true},
        };
        for (const Case& c : wall) {
            steps->push_back({QStringLiteral("b_%1").arg(QString(c.name).replace(' ', '_')), [=] {
                setup(c.play, c.reduced);
                QTimer::singleShot(400, window, [=] { bench(c.name, c.hoverList ? overList : overNav); });
            }, 4600});
        }
        steps->push_back({"b99_end", [st] { if (st) st->setReducedMotion(false); }, 200});
    } else if (qEnvironmentVariableIsSet("KP_TEST_EQ")) {
        // 均衡器:播放页右下角打开面板 → 关闭状态 → 选预设 (曲线过渡) → 拖一段成自定义 → 关掉 → 小窗口
        steps->push_back({"e01_playing", [call, &player] {
            player.applyEqPreset(QStringLiteral("flat"));
            player.setEqEnabled(false);
            player.playAlbum(0, 0);
            call("automationNowPlaying", true);
        }, 2600});
        steps->push_back({"e02_open_anim", [call] { call("automationEqPanel", true); }, 140});
        steps->push_back({"e03_open_off", [] {}, 900});
        steps->push_back({"e04_rock_anim", [&player] { player.applyEqPreset(QStringLiteral("rock")); }, 160});
        steps->push_back({"e05_rock", [&player] {
            std::fprintf(stderr, "KP_EQ chain: %s\n", qPrintable(player.chainSummary()));
        }, 900});
        steps->push_back({"e06_custom_hover", [call, &player] {
            player.setEqGain(5, 6.0);
            call("automationEqHover", 5);
        }, 900});
        steps->push_back({"e07_off", [call, &player] { call("automationEqHover", -1); player.setEqEnabled(false); }, 900});
        steps->push_back({"e08_small", [window, call, &player] {
            player.applyEqPreset(QStringLiteral("vocal"));
            call("automationEqPanel", false);
            window->resize(980, 640);
        }, 1200});
        steps->push_back({"e09_small_panel", [call] { call("automationEqPanel", true); }, 1400});
    } else if (qEnvironmentVariableIsSet("KP_TEST_LYRSRC")) {
        // 歌词来源:自动找到的在线歌词 → 打开来源面板 (自动搜索) → 换一份 → 保存到本地;以及没有歌词时的样子
        // 配合 KP_TEST_LYRIC_FAKE=1 使用 (假的歌词接口)
        const QString album = qEnvironmentVariable("KP_LYR_ALBUM", QStringLiteral("async"));
        steps->push_back({"l01_auto_online", [call, album] { call("automationPlayTitle", album); call("automationNowPlaying", true); }, 2600});
        steps->push_back({"l02_panel_open_80", [call] { call("automationLyricPanel", true); }, 120});
        steps->push_back({"l03_panel_searching", [] {}, 160});
        steps->push_back({"l04_panel_results", [] {}, 1400});
        steps->push_back({"l05_hover_row", [call] { call("automationLyricHover", 2); }, 500});
        steps->push_back({"l06_picked", [call] { call("automationLyricHover", -1); call("automationLyricPick", 2); }, 1400});
        steps->push_back({"l07_saved", [call] { call("automationLyricSave"); }, 1400});
        steps->push_back({"l08_closed", [call] { call("automationLyricPanel", false); }, 900});
        steps->push_back({"l09_reopen", [call] { call("automationLyricPanel", true); }, 900});
        steps->push_back({"l10_small", [window, call] { call("automationLyricPanel", false); window->resize(980, 640); }, 1200});
        steps->push_back({"l11_small_panel", [call] { call("automationLyricPanel", true); }, 1400});
    } else if (qEnvironmentVariableIsSet("KP_TEST_LYRTR")) {
        // 外语歌补翻译:本地只有原文的歌 → 自动换成带翻译的在线版 → 来源面板 (候选预取、「翻译」标签、只看带翻译) → 用本地歌词
        const QString title = qEnvironmentVariable("KP_LYR_TITLE", QStringLiteral("aLIEz"));
        auto lyr = [&player](const char* tag) {
            int tr = 0;
            const QVariantList l = player.lyrics();
            for (const QVariant& v : l) tr += v.toMap().contains("translation");
            qInfo().noquote() << "qml LYRTR" << tag << player.title() << "source" << player.lyricSource().value("label").toString()
                              << "local" << player.lyricSource().value("local").toString() << "lines" << l.size() << "translated" << tr
                              << "tags" << QVariant(player.lyricCandidateTags()).toStringList().join(",");
        };
        steps->push_back({"t01_auto", [call, title] { call("automationPlayTrackTitle", title); call("automationNowPlaying", true); }, 7000});
        steps->push_back({"t02_panel", [call, lyr] { lyr("auto"); call("automationLyricPanel", true); }, 7000});
        steps->push_back({"t03_only_tr", [call, lyr] { lyr("panel"); call("automationLyricOnlyTranslated", true); }, 1200});
        steps->push_back({"t04_local", [&player, call] { call("automationLyricOnlyTranslated", false); player.useLocalLyrics(); }, 1200});
        steps->push_back({"t05_done", [lyr] { lyr("local"); }, 300});
    } else if (qEnvironmentVariableIsSet("KP_TEST_FLIGHT")) {
        // 打开 / 收起播放页的过程:封面从底栏黑胶飞出、投影与内容的出场时机
        steps->push_back({"f00_playing", [call] { call("automationPlayTitle", QStringLiteral("async")); }, 1800});
        steps->push_back({"f01_open_60", [call] { call("automationNowPlaying", true); }, 60});
        steps->push_back({"f02_open_mid", [] {}, 120});
        steps->push_back({"f03_open_late", [] {}, 160});
        steps->push_back({"f04_open_land", [] {}, 200});
        steps->push_back({"f05_open_done", [] {}, 900});
        steps->push_back({"f06_close_60", [call] { call("automationNowPlaying", false); }, 60});
        steps->push_back({"f07_close_mid", [] {}, 120});
        steps->push_back({"f08_close_late", [] {}, 160});
        steps->push_back({"f09_close_done", [] {}, 900});
    } else if (qEnvironmentVariableIsSet("KP_TEST_BACKFLY")) {
        // 小窗下返回专辑墙:飞行途中滚动 → 副本就地淡出,不跟着飞到旧位置;详情页滚下去后返回 → 不飞
        steps->push_back({"b00_wall", [window, call] { window->resize(980, 640); call("automationGo", QStringLiteral("albums")); }, 1500});
        steps->push_back({"b01_detail", [call] { call("automationOpenAlbum", 2); }, 1400});
        steps->push_back({"b02_back_scroll", [call] { call("automationBack"); call("automationWallScroll", 260); }, 90});
        steps->push_back({"b03_mid", [] {}, 300});
        steps->push_back({"b04_done", [] {}, 900});
        steps->push_back({"b05_detail_scrolled", [call] { call("automationWallScroll", 0); call("automationOpenAlbum", 2); }, 1400});
        steps->push_back({"b06_scrolled", [call] { call("automationDetailScroll", 500); }, 900});
        steps->push_back({"b07_back_mid", [call] { call("automationBack"); }, 150});
        steps->push_back({"b08_back_done", [] {}, 900});
    } else if (qEnvironmentVariableIsSet("KP_TEST_ALBUM")) {
        // 专辑页:详情 / 播放中 / 滚动后的精简栏 / 页尾 / 专辑菜单 / 封面大图 / 多碟 / 宽窗与小窗
        const QString hires = qEnvironmentVariable("KP_ALBUM_A", QStringLiteral("async"));
        const QString multi = qEnvironmentVariable("KP_ALBUM_B", QStringLiteral("Joe Hisaishi Best"));
        steps->push_back({"a01_detail", [call, hires] { call("automationOpenAlbumTitle", hires); }, 1600});
        steps->push_back({"a02_playing", [call, hires] { call("automationPlayTitle", hires); }, 1800});
        steps->push_back({"a03_scrolled", [call] { call("automationDetailScroll", 420); }, 900});
        steps->push_back({"a04_footer", [call] { call("automationDetailScroll", -1); }, 1200});
        steps->push_back({"a05_menu", [call] { call("automationDetailScroll", 0); call("automationAlbumMenu"); }, 900});
        steps->push_back({"a06_cover", [call] { call("automationCloseMenu"); call("automationViewCover", true); }, 1200});
        steps->push_back({"a07_multidisc", [call, multi] {
            call("automationViewCover", false);
            call("automationBack");
            call("automationOpenAlbumTitle", multi);
        }, 1800});
        steps->push_back({"a07b_multidisc_scroll", [call] { call("automationDetailScroll", 300); }, 900});
        steps->push_back({"a08_wide", [window, call, hires] {
            window->resize(1920, 1080);
            call("automationBack");
            call("automationOpenAlbumTitle", hires);
        }, 2000});
        steps->push_back({"a09_wall_wide", [call] { call("automationBack"); }, 1600});
        steps->push_back({"a10_small", [window, call, hires] {
            window->resize(980, 640);
            call("automationOpenAlbumTitle", hires);
        }, 2000});
        steps->push_back({"a11_wall_small", [call] { call("automationBack"); }, 1600});
    } else if (qEnvironmentVariableIsSet("KP_TEST_WALL")) {
        // 专辑墙:今日推荐 / 排序菜单 / 按年份 (年代索引) / 筛选 / 播放与暂停 / 卡片菜单 / 重扫后位置 / 封面大小 / 小窗
        steps->push_back({"w01_wall", [call] { call("automationGo", QStringLiteral("albums")); }, 1600});
        steps->push_back({"w02_sortmenu", [call] { call("automationWallSortMenu", true); }, 800});
        steps->push_back({"w03_year", [call] { call("automationWallSortMenu", false); call("automationWallSort", 2); }, 1400});
        steps->push_back({"w04_scrolled", [call] { call("automationWallScroll", 520); }, 900});
        steps->push_back({"w05_lossy", [call] { call("automationWallScroll", 0); call("automationWallFilter", 3); }, 1400});
        steps->push_back({"w06_playing", [call, &player] {
            call("automationWallFilter", 0);
            call("automationWallSort", 0);
            player.playAlbum(0, 0);
        }, 1800});
        steps->push_back({"w07_paused", [&player] { player.toggle(); }, 1200});
        steps->push_back({"w08_card_menu", [call] { call("automationWallMenu", 1); }, 900});
        steps->push_back({"w09_rescan_kept", [call, &lib] {
            call("automationCloseMenu");
            call("automationWallScroll", 600);
            lib.rescan();
        }, 6000});
        steps->push_back({"w10_small_cells", [call] { call("automationWallScroll", 0); call("automationAlbumCell", 128); }, 1400});
        steps->push_back({"w11_big_cells", [call] { call("automationAlbumCell", 272); }, 1400});
        steps->push_back({"w12_small_window", [window, call] {
            call("automationAlbumCell", 176);
            call("automationWallSort", 1);
            window->resize(980, 640);
        }, 1600});
        steps->push_back({"w13_wide", [window] { window->resize(1920, 1080); }, 1600});
    } else if (qEnvironmentVariableIsSet("KP_TEST_PAGES")) {
        // 歌手 / 歌单 / 歌曲 / 搜索页:播放中的暂停键、滚动后的精简栏、整组「更多」菜单、搜索最佳匹配与无结果
        const QString artist = qEnvironmentVariable("KP_PAGES_ARTIST", QStringLiteral("久石譲"));
        // 页面上的自动化函数 (按 objectName 找到页面根对象)
        auto pageCall = [window](const char* page, const char* fn, QVariant arg = {}) {
            QObject* o = window->findChild<QObject*>(QLatin1String(page));
            if (QObject* root = window->property("contentItem").value<QObject*>(); !o && root) o = root->findChild<QObject*>(QLatin1String(page));
            if (!o) return;
            if (arg.isValid()) QMetaObject::invokeMethod(o, fn, Q_ARG(QVariant, arg));
            else QMetaObject::invokeMethod(o, fn);
        };
        steps->push_back({"g01_artists", [call] { call("automationGo", QStringLiteral("artists")); }, 1600});
        steps->push_back({"g02_artist", [call, artist] { call("automationOpenArtist", artist); }, 1600});
        steps->push_back({"g03_artist_playing", [pageCall] { pageCall("artistDetailPage", "playOrToggle"); }, 1800});
        steps->push_back({"g04_artist_bar", [pageCall] { pageCall("artistDetailPage", "automationScroll", 760); }, 900});
        steps->push_back({"g05_artist_menu", [pageCall] {
            pageCall("artistDetailPage", "automationScroll", 0);
            pageCall("artistDetailPage", "automationMenu");
        }, 900});
        steps->push_back({"g06_artists_back", [call] { call("automationCloseMenu"); call("automationBack"); }, 1600});
        steps->push_back({"g07_playlists", [&engine, &lib, call] {
            auto* pl = qobject_cast<PlaylistService*>(engine.rootContext()->contextProperty("playlists").value<QObject*>());
            auto snap = lib.snapshot();
            if (pl && pl->playlists().size() <= 1 && snap->songs.size() > 40) {
                QVariantList a;
                for (size_t i = 0; i < 68; ++i) a.append(double(snap->songs[i * 3]->id));
                pl->addTracks(pl->create(QStringLiteral("深夜循环")), a);
                pl->create(QStringLiteral("空歌单"));
            }
            call("automationGo", QStringLiteral("playlists"));
        }, 1400});
        steps->push_back({"g08_playlist", [&engine, call] {
            auto* pl = qobject_cast<PlaylistService*>(engine.rootContext()->contextProperty("playlists").value<QObject*>());
            if (pl && pl->playlists().size() > 1) call("automationOpenPlaylist", pl->playlists()[1].toMap()["id"]);
        }, 1400});
        steps->push_back({"g09_playlist_bar", [pageCall] { pageCall("playlistDetailPage", "automationScroll", 700); }, 900});
        steps->push_back({"g10_playlist_menu", [pageCall] {
            pageCall("playlistDetailPage", "automationScroll", 0);
            pageCall("playlistDetailPage", "automationMenu");
        }, 900});
        steps->push_back({"g11_empty_playlist", [&engine, call] {
            call("automationCloseMenu");
            auto* pl = qobject_cast<PlaylistService*>(engine.rootContext()->contextProperty("playlists").value<QObject*>());
            if (!pl) return;
            for (const QVariant& v : pl->playlists())
                if (v.toMap()["count"].toInt() == 0) { call("automationOpenPlaylist", v.toMap()["id"]); break; }
        }, 1400});
        steps->push_back({"g12_songs", [call] { call("automationGo", QStringLiteral("songs")); }, 1600});
        steps->push_back({"g13_search_artist", [call, artist] { call("automationSearch", artist); }, 1400});
        steps->push_back({"g14_search_track", [call] { call("automationSearch", QStringLiteral("summer")); }, 1400});
        steps->push_back({"g15_search_none", [call] { call("automationSearch", QStringLiteral("qqxxzz")); }, 1400});
        steps->push_back({"g16_small_artist", [window, call, artist] {
            window->resize(980, 640);
            call("automationOpenArtist", artist);
        }, 1800});
    } else if (qEnvironmentVariableIsSet("KP_TEST_CHROME")) {
        // 播放队列抽屉 / 队列右键菜单与键盘操作 / 清空后撤销 / 沉浸页 / 歌曲信息 / 定时关闭与音量 / 窄窗播放条 / 顶部磨砂条
        auto key = [window](int k) {
            QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
        };
        auto wheel = [window](int dy) {
            const QPointF pos(window->width() * 0.55, window->height() * 0.45);
            QWheelEvent ev(pos, window->mapToGlobal(pos), QPoint(), QPoint(0, dy), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &ev);
        };
        steps->push_back({"c01_queue", [call, &player, &lib] {
            auto snap = lib.snapshot();
            QVariantList ids;
            for (size_t i = 0; i < snap->songs.size() && i < 400; ++i) ids.append(double(snap->songs[i]->id));
            player.playTracks(ids, 12, QStringLiteral("songs"));
            call("automationQueue", true);
        }, 1600});
        steps->push_back({"c02_queue_scrolled", [call] { call("automationQueueScroll", 120); }, 900});
        steps->push_back({"c03_queue_menu", [call] { call("automationQueueScroll", 0); call("automationQueueMenu", 16); }, 900});
        steps->push_back({"c04_menu_keys", [key, window] {
            window->requestActivate();
            QTimer::singleShot(150, [key] { key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Right); key(Qt::Key_Down); });
        }, 900});
        steps->push_back({"c05_cleared", [call] { call("automationCloseMenu"); call("automationClearQueue"); }, 900});
        steps->push_back({"c06_now_queue", [call, &player] {
            player.undoClearQueue();
            call("automationQueue", false);
            call("automationNowPlaying", true);
            call("automationNowTab", QStringLiteral("queue"));
        }, 1800});
        steps->push_back({"c07_now_lyrics", [call] { call("automationNowTab", QStringLiteral("lyrics")); }, 1000});
        steps->push_back({"c08_track_info", [call] { call("automationNowPlaying", false); call("automationTrackInfo", true); }, 1300});
        steps->push_back({"c09_sleep_compact", [call, window, &player] {
            call("automationTrackInfo", false);
            call("automationSleep", 30);
            window->resize(960, 640);
            player.changeVolume(-0.1);
        }, 500});
        steps->push_back({"c10_songs_band", [call, window, wheel] {
            window->resize(1280, 800);
            call("automationGo", QStringLiteral("songs"));
            QTimer::singleShot(900, [wheel] { wheel(-360); });
        }, 2400});
        steps->push_back({"c10b_songs_quality", [call] { call("automationSongsSort", 5); }, 1400});
        steps->push_back({"c10c_songs_artist_keys", [call, key, window] {
            call("automationSongsSort", 1);
            window->requestActivate();
            QTimer::singleShot(600, [key] { key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Down); });
        }, 1600});
        steps->push_back({"c11_big_now", [call, window] {
            call("automationSongsSort", 0);
            call("automationSleep", 0);
            window->resize(1920, 1080);
            call("automationNowPlaying", true);
        }, 2000});
    } else if (qEnvironmentVariableIsSet("KP_TEST_FINDPLAY")) {
        // 回归:只查找重复、只预览整理 (不动文件),然后点播放
        auto tools = [&engine]() { return qobject_cast<LibraryTools*>(engine.rootContext()->contextProperty("libtools").value<QObject*>()); };
        // 用户报告的崩溃:暂停状态下重新扫描 (曲库换了新快照),再点一首别的歌
        steps->push_back({"p0_play", [&player] { player.playAlbum(1, 0); }, 1200});
        steps->push_back({"p0_pause_rescan", [&player, &lib] { if (player.playing()) player.toggle(); lib.rescan(); }, 6000});
        steps->push_back({"p0_play_after_scan", [&player] { player.playAlbum(2, 0); }, 1500});
        steps->push_back({"p1_dedupe", [tools, call] { call("automationGo", QStringLiteral("dedupe")); tools()->findDuplicates(); }, 8000});
        steps->push_back({"p2_organize", [tools, call, outDir] {
            call("automationGo", QStringLiteral("organize"));
            tools()->setTargetUrl(QDir(outDir).absoluteFilePath("organized"));
            tools()->buildPlan();
        }, 4000});
        steps->push_back({"p3_play", [&player, call] { call("automationGo", QStringLiteral("albums")); player.playAlbum(0, 0); }, 1500});
        steps->push_back({"p4_next", [&player] { player.next(); player.playAlbum(3, 2); }, 1500});
        steps->push_back({"p5_dedupe_again", [tools, call] { call("automationGo", QStringLiteral("dedupe")); tools()->autoSelect(1); }, 1500});
        steps->push_back({"p6_play_again", [&player] { player.playAlbum(5, 0); }, 1500});
    } else if (!flowDir.isEmpty()) {
        steps->push_back({"f1_empty", [] {}, 1200});
        steps->push_back({"f2_importing", [&lib, flowDir] { lib.addFolder(flowDir); }, 1100});
        steps->push_back({"f3_imported", [] {}, 4000});
        steps->push_back({"f4_folders", [call] { call("automationLibraryPanel", true); }, 900});
        steps->push_back({"f5_album", [call] {
            call("automationLibraryPanel", false);
            call("automationOpenAlbum", 1);
        }, 1400});
        steps->push_back({"f6_playing", [&player] { player.playAlbum(1, 2); }, 2200});
        steps->push_back({"f7_removed", [&lib, call] {
            call("automationBack");
            if (!lib.folders().isEmpty()) lib.removeFolder(lib.folders().first());
        }, 2500});
    } else {
    steps->push_back({"00_decrypt_prompt", [] {}, 900});
    steps->push_back({"01_albums", [call] { call("automationDecryptPrompt", false); }, 1500});
    steps->push_back({"02_album_detail", [call] { call("automationOpenAlbum", 0); }, 1500});
    // 封面飞回专辑墙的中途与落地 (检查飞行副本与卡片是否对齐、有无重影)
    steps->push_back({"02a_wall", [call] { call("automationBack"); }, 900});
    steps->push_back({"02a_card_album", [call] { call("automationOpenAlbum", 3); }, 1300});
    steps->push_back({"02b_close_mid", [call] { call("automationBack"); }, 260});
    steps->push_back({"02c_closed", [] {}, 900});
    steps->push_back({"02d_reopen", [call] { call("automationOpenAlbum", 0); }, 1300});
    steps->push_back({"03_playing", [&player, call] {
        player.playAlbum(0, 0);
    }, 2000});
    steps->push_back({"04_menu", [window] {
        QMetaObject::invokeMethod(window, "automationTrackMenu", Q_ARG(QVariant, 760), Q_ARG(QVariant, 420));
    }, 900});
    steps->push_back({"04b_track_info", [call] { call("automationTrackInfo", true); }, 900});
    steps->push_back({"05_artists", [window, call] {
        QMetaObject::invokeMethod(window, "automationTrackInfo", Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(window, "automationBack");
        call("automationGo", QStringLiteral("artists"));
    }, 1600});
    steps->push_back({"06_artist_detail", [call] { call("automationOpenArtist", QStringLiteral("久石譲")); }, 1600});
    steps->push_back({"07_songs", [call] { call("automationGo", QStringLiteral("songs")); }, 1600});
    steps->push_back({"08_playlists", [&engine, &lib, call] {
        // 演示数据:两个歌单 + 几首收藏
        auto* pl = qobject_cast<PlaylistService*>(engine.rootContext()->contextProperty("playlists").value<QObject*>());
        auto snap = lib.snapshot();
        if (pl && pl->playlists().size() <= 1 && snap->songs.size() > 40) {
            QVariantList a, b;
            for (size_t i = 0; i < snap->songs.size(); i += 3) a.append(double(snap->songs[i]->id));
            for (size_t i = 1; i < 14; ++i) b.append(double(snap->songs[i * 2]->id));
            pl->addTracks(pl->create(QStringLiteral("深夜循环")), a);
            pl->addTracks(pl->create(QStringLiteral("通勤路上")), b);
            pl->create(QStringLiteral("空歌单"));
            for (int i = 5; i < 11; ++i) pl->toggleFavorite(double(snap->songs[i]->id));
        }
        call("automationGo", QStringLiteral("playlists"));
    }, 1400});
    steps->push_back({"08b_playlist_detail", [&engine, call] {
        auto* pl = qobject_cast<PlaylistService*>(engine.rootContext()->contextProperty("playlists").value<QObject*>());
        if (pl && pl->playlists().size() > 1) call("automationOpenPlaylist", pl->playlists()[1].toMap()["id"]);
    }, 1400});
    steps->push_back({"09_search", [call] { call("automationSearch", QStringLiteral("sen")); }, 1400});
    steps->push_back({"10_search_cn", [call] { call("automationSearch", QStringLiteral("陈奕迅")); }, 1400});
    steps->push_back({"11_now_playing", [call] {
        call("automationGo", QStringLiteral("albums"));
        call("automationPlayTitle", QStringLiteral("森林物语"));
        call("automationNowPlaying", true);
    }, 2400});
    steps->push_back({"12_wall", [call] { call("automationNowPlaying", false); }, 1400});
    steps->push_back({"13_wall_end", [call] { call("automationScrollEnd"); }, 1500});
    // 文件夹面板:打开途中与落定,以及面板里进入的整理 / 查重页
    steps->push_back({"20_panel_mid", [call] { call("automationLibraryPanel", true); }, 90});
    steps->push_back({"20b_panel", [] {}, 800});
    steps->push_back({"21_organize", [call] {
        call("automationLibraryPanel", false);
        call("automationGo", QStringLiteral("organize"));
    }, 1200});
    steps->push_back({"21b_organize_plan", [&engine, outDir] {
        auto* t = qobject_cast<LibraryTools*>(engine.rootContext()->contextProperty("libtools").value<QObject*>());
        if (!t) return;
        t->setTargetUrl(QDir(outDir).absoluteFilePath("organized"));
        t->buildPlan();
    }, 3000});
    steps->push_back({"22_dedupe", [&engine, call] {
        call("automationGo", QStringLiteral("dedupe"));
        auto* t = qobject_cast<LibraryTools*>(engine.rootContext()->contextProperty("libtools").value<QObject*>());
        if (t) t->findDuplicates();
    }, 400});
    steps->push_back({"22b_dedupe_done", [] {}, 14000});
    steps->push_back({"23_desktop_lyrics", [&engine, &player] {
        // 从头播放,给这首写一份短 LRC (测试曲目只有 2 秒);锁定以显示不悬停时的样子
        player.playAlbum(0, 0);
        // 同一目录的每首都写 (曲目很短,截图时可能已经播到后面几首);同时间戳的第二行是翻译
        const QString path = player.currentPath();
        const QDir dir = QFileInfo(path).dir();
        for (const QFileInfo& fi : dir.entryInfoList({"*.flac", "*.mp3", "*.wav"}, QDir::Files)) {
            QFile f(fi.path() + "/" + fi.completeBaseName() + ".lrc");
            if (!f.open(QIODevice::WriteOnly)) continue;
            f.write(QStringLiteral("[00:00.00]風が吹いている\n[00:00.00]风在吹\n[00:00.60]遠くの空へ 届くように\n[00:00.60]愿能传到远方的天空\n"
                                   "[00:01.20]夢の続きを 歩いていく\n[00:01.20]继续走在梦的路上\n[00:01.90]下一句\n").toUtf8());
            f.close();
        }
        auto* st = qobject_cast<SettingsService*>(engine.rootContext()->contextProperty("settings").value<QObject*>());
        if (st) { st->setLyricsLocked(true); st->setDesktopLyrics(true); }
        player.reloadLyrics();
    }, 1450});
    steps->push_back({"24_desktop_lyrics_off", [&engine, outDir] {
        // 桌面歌词是独立窗口:单独截下来,垫一张深色和一张浅色底看描边效果
        for (QWindow* w : QGuiApplication::topLevelWindows()) {
            auto* q = qobject_cast<QQuickWindow*>(w);
            if (!q || q->title() != QStringLiteral("桌面歌词") || !q->isVisible()) continue;
            // 离屏截图拿不到透明底:临时换成深 / 浅底色各截一张
            q->setColor(QColor(30, 34, 40));
            const QImage dark = q->grabWindow();
            q->setColor(QColor(225, 232, 238));
            const QImage light = q->grabWindow();
            q->setColor(Qt::transparent);
            QImage out(dark.width(), dark.height() * 2, QImage::Format_ARGB32_Premultiplied);
            QPainter p(&out);
            p.drawImage(0, 0, dark);
            p.drawImage(0, dark.height(), light);
            p.end();
            out.save(outDir + "/23b_desktop_lyrics_window.png");
        }
        auto* st = qobject_cast<SettingsService*>(engine.rootContext()->contextProperty("settings").value<QObject*>());
        if (st) { st->setDesktopLyrics(false); st->setLyricsLocked(false); }
    }, 600});
    steps->push_back({"15_tools", [call] { call("automationGo", QStringLiteral("tools")); }, 1400});
    steps->push_back({"16_tools_decrypting", [&engine] {
        auto* d = qobject_cast<DecryptService*>(engine.rootContext()->contextProperty("decrypt").value<QObject*>());
        if (d) d->decryptPending();
    }, 60});
    steps->push_back({"17_tools_done", [] {}, 3000});
    steps->push_back({"18_songs_after_decrypt", [call] { call("automationGo", QStringLiteral("songs")); }, 1400});
    steps->push_back({"19_settings", [window, call] {
        window->resize(1320, 1180);
        call("automationGo", QStringLiteral("settings"));
    }, 1600});
    // 设置页往下翻两屏 (检查各分组的对齐)
    for (int k = 1; k <= 3; ++k)
        steps->push_back({QStringLiteral("19%1_settings_scroll").arg(QChar('a' + k)), [window, k] {
            if (auto* f = window->findChild<QObject*>("settingsFlick")) {
                const double maxY = std::max(0.0, f->property("contentHeight").toDouble() - f->property("height").toDouble());
                f->setProperty("contentY", std::min(maxY, 900.0 * k));
            }
        }, 700});
    steps->push_back({"14_small_window", [window, call] {
        call("automationHideChain");
        window->resize(980, 640);
    }, 1500});
    }

    auto run = std::make_shared<std::function<void(size_t)>>();
    *run = [=](size_t i) {
        if (i >= steps->size()) {
            QCoreApplication::quit();
            return;
        }
        (*steps)[i].action();
        QTimer::singleShot((*steps)[i].waitMs, window, [=]() {
            QImage img = window->grabWindow();
            img.save(outDir + "/" + (*steps)[i].name + ".png");
            (*run)(i + 1);
        });
    };
    auto start = [run]() { (*run)(0); };
    // 只在首次扫描完成后启动一次 (之后的重扫,例如解密完成触发的,不能再次启动截图序列)
    auto started = std::make_shared<bool>(false);
    auto once = [start, started]() {
        if (*started) return;
        *started = true;
        start();
    };
    if (!flowDir.isEmpty()) QTimer::singleShot(800, once);
    else if (lib.scanning()) QObject::connect(&lib, &LibraryService::scanningChanged, window, [&lib, once]() {
        if (!lib.scanning()) QTimer::singleShot(800, once);
    });
    else QTimer::singleShot(800, once);
}

#ifdef Q_OS_WIN
// 崩溃诊断:未处理的异常 (访问越界等) 发生时,在数据目录 crashes\ 下写一个 minidump,
// 用调试器打开即可看到崩溃时每个线程的调用栈
static wchar_t g_dumpDir[MAX_PATH] = {0};

static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* ep) {
    if (!g_dumpDir[0]) return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH + 64];
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\crash-%04d%02d%02d-%02d%02d%02d.dmp", g_dumpDir, st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mi;
        mi.ThreadId = GetCurrentThreadId();
        mi.ExceptionPointers = ep;
        mi.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &mi, nullptr,
                          nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallCrashDump() {
    const QString dir = LibraryService::dataDir() + "/crashes";
    QDir().mkpath(dir);
    const std::wstring w = QDir::toNativeSeparators(dir).toStdWString();
    if (w.size() >= MAX_PATH) return;
    wcsncpy(g_dumpDir, w.c_str(), MAX_PATH - 1);
    SetUnhandledExceptionFilter(WriteCrashDump);
}
#endif

int main(int argc, char* argv[]) {
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QCoreApplication::setOrganizationName("KugouPlayer");
    QCoreApplication::setApplicationName("KugouPlayer"); // 数据目录名,不能改(改了曲库/设置全丢)
    QGuiApplication::setApplicationDisplayName(QStringLiteral("拾羽")); // 只影响界面显示
    // 界面缩放 (设置页):必须在创建应用对象之前生效
    const double scale = SettingsService::StartupScale();
    if (scale > 0.5 && scale < 3.0 && std::abs(scale - 1.0) > 0.01 && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR"))
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'f', 2));
    FrameDiag::ApplyStartupOptions();
    TimedApp app(argc, argv);
    FrameDiag::CaptureGraphicsInfo();
    // 全局线程池只跑提取封面主色、加载歌词、统计缓存这类零星后台任务,默认按逻辑核数 (可达 20) 铺线程没有必要
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    app.setWindowIcon(QIcon(QStringLiteral(":/images/app_icon_256.png")));
#ifdef Q_OS_WIN
    InstallCrashDump();
#endif
    QQuickStyle::setStyle("Basic");

    QString screenshotDir, libraryArg, flowArg;
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--screenshot") screenshotDir = args[i + 1];
        if (args[i] == "--library") libraryArg = args[i + 1];
        if (args[i] == "--flow") flowArg = args[i + 1];
    }

    // 解密工具的外部资源 (kgm.mask / kgg-dec.exe) 与程序放在一起
    DecryptorRegistry::Instance().SetToolsDir((QCoreApplication::applicationDirPath() + "/tools").toStdWString());
    LibraryService library;
    PlayerController player(&library);
    // 播放历史先于专辑墙建立:曲库变化时它先更新,专辑墙「最近播放」排序用到它
    PlaylistService playlists(&library);
    AlbumModel albums(&library, &playlists);
    TrackListModel albumTracks(&library);
    SearchModel search(&library);
    DecryptService decrypt(&library);
    CoverService covers(&library);
    SettingsService settings(&library);
    ArtistModel artists(&library);
    TrackCollectionModel songs(&library, &playlists);
    TrackCollectionModel artistTracks(&library, &playlists);
    TrackCollectionModel playlistTracks(&library, &playlists);
    artistTracks.setSource("artist");
    playlistTracks.setSource("playlist");
    TrackCollectionModel historyTracks(&library, &playlists);
    historyTracks.setSource("history");
    // 真正开始播放一首歌时记入历史 (恢复上次会话时停在暂停状态的那首,等按下播放才算)
    auto recordPlay = [&player, &playlists]() {
        if (player.playing() && player.trackId() >= 0) playlists.recordPlay(player.trackId());
    };
    QObject::connect(&player, &PlayerController::trackChanged, &playlists, recordPlay);
    QObject::connect(&player, &PlayerController::playingChanged, &playlists, recordPlay);
    WindowHelper windowHelper;
    FrameDiag frameDiag; // 先于引擎创建、晚于引擎销毁:QML 绑定在退出时还会读它
    // 窗口不可见时释放场景图,可见时重建。
    // 显示器休眠 / 拔插 HDMI 之后 vsync 会消失,threaded 渲染线程可能卡在 Present 上,
    // 这个开关让 Qt 在窗口状态变化时走"销毁-重建"路径,而不是复用可能已经失效的交换链。
    RenderGuard renderGuard;
    LibraryTools libtools(&library, &playlists, &player);
    PlaylistImporter importer(&library, &playlists);

    QQmlApplicationEngine engine;
    engine.addImageProvider("cover", new CoverProvider);
    QQmlContext* ctx = engine.rootContext();
    ctx->setContextProperty("library", &library);
    ctx->setContextProperty("player", &player);
    ctx->setContextProperty("albumModel", &albums);
    ctx->setContextProperty("albumTracks", &albumTracks);
    ctx->setContextProperty("searchModel", &search);
    ctx->setContextProperty("playlists", &playlists);
    ctx->setContextProperty("decrypt", &decrypt);
    ctx->setContextProperty("covers", &covers);
    ctx->setContextProperty("settings", &settings);
    ctx->setContextProperty("artistModel", &artists);
    ctx->setContextProperty("songsModel", &songs);
    ctx->setContextProperty("artistTracks", &artistTracks);
    ctx->setContextProperty("playlistTracks", &playlistTracks);
    ctx->setContextProperty("historyTracks", &historyTracks);
    ctx->setContextProperty("windowHelper", &windowHelper);
    ctx->setContextProperty("frameDiag", &frameDiag);
    ctx->setContextProperty("libtools", &libtools);
    ctx->setContextProperty("importer", &importer);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    // 兼容 Qt 6.4 (loadFromModule 自 6.5 起才有):资源前缀在 CMake 中显式固定为 /qt/qml
    engine.addImportPath(QStringLiteral("qrc:/qt/qml"));
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/PlayerUi/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    auto* mainWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
    player.attachWindow(mainWindow);
    // 窗口不可见时释放场景图,可见时重建 —— 休眠唤醒后走重建路径而不是复用失效的交换链。
    // (这两个都是实例方法,只能在窗口建好之后调;设得晚一点也仍然生效)
    if (mainWindow) {
        mainWindow->setPersistentSceneGraph(false);
        mainWindow->setPersistentGraphics(false);
    }
    // 渲染看门狗:显示器休眠唤醒后画面冻住时自动恢复(见 render_guard.h)
    renderGuard.attach(mainWindow);
    // 全局多媒体键
    frameDiag.attach(qobject_cast<QQuickWindow*>(engine.rootObjects().value(0)));
    app.installNativeEventFilter(&windowHelper);
    windowHelper.registerMediaKeys(qobject_cast<QWindow*>(engine.rootObjects().value(0)));
    QObject::connect(&windowHelper, &WindowHelper::mediaPlayPause, &player, &PlayerController::toggle);
    QObject::connect(&windowHelper, &WindowHelper::mediaNext, &player, &PlayerController::next);
    QObject::connect(&windowHelper, &WindowHelper::mediaPrevious, &player, &PlayerController::previous);
    // 任务栏缩略图 (封面) 与缩略图按钮
    TaskbarHelper taskbar(&player, &playlists);
    app.installNativeEventFilter(&taskbar);
    taskbar.attach(qobject_cast<QQuickWindow*>(engine.rootObjects().value(0)));
    QObject::connect(&taskbar, &TaskbarHelper::toggleDesktopLyrics, &settings,
                     [&settings]() { settings.setDesktopLyrics(!settings.desktopLyrics()); });
#ifdef Q_OS_WIN
    // 播放时不让电脑自动睡眠 (显示器照常关闭);暂停或关掉这项后恢复
    auto keepAwake = [&player, &settings]() {
        SetThreadExecutionState(player.playing() && settings.keepAwake() ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
    };
    QObject::connect(&player, &PlayerController::playingChanged, &settings, keepAwake);
    QObject::connect(&settings, &SettingsService::changed, &settings, keepAwake);
#endif

    if (!libraryArg.isEmpty()) library.addFolder(libraryArg);
    else library.rescan();

    if (!screenshotDir.isEmpty()) RunScreenshots(engine, library, player, screenshotDir, flowArg);
    return app.exec();
}
