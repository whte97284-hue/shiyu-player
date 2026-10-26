#include "frame_diag.h"

#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScreen>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QSysInfo>
#include <QSettings>
#include <QLibraryInfo>
#include <QVersionNumber>
#include <QThread>
#include <QCoreApplication>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
QStringList g_graphicsLines;
// Qt 动画驱动的模式:vsync (每帧按一个刷新周期推进动画时间,最顺) / timer (按挂钟时间推进,会抖)。
// 连续几帧超过 1.25 个刷新周期 (按主屏刷新率算) 它就切到 timer,要连续 10 帧准时才切回来
// 线程化渲染循环里有两个驱动:界面线程的 (NumberAnimation、Behavior、FrameAnimation 等) 和
// 渲染线程的 (OpacityAnimator / ScaleAnimator / UniformAnimator 等 Animator)。日志不带线程信息,按发出日志的线程区分
std::atomic<int> g_driverTimer{0};     // 界面线程驱动
std::atomic<int> g_rtDriverTimer{0};   // 渲染线程驱动
std::atomic<int> g_driverSwitches{0};
std::atomic<int> g_rtDriverSwitches{0};
bool OnGuiThread() { return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread(); }
QtMessageHandler g_prevHandler = nullptr;

// Qt 自己的逐帧耗时日志 (qt.scenegraph.time.renderloop,只在诊断开着时打开),整毫秒精度:
//  界面线程:polish (布局 / 建代理) · animations (推进动画、跑绑定) · blockedForSync (等渲染线程同步)
//  渲染线程:sync (把场景同步进渲染树、上传纹理) · render (录制命令) · swap (present,含等垂直同步)
struct FrameParts {
    std::mutex m;
    std::vector<int> polish, anim, blocked, sync, render, swap;
} g_parts;
const QRegularExpression& GuiRe() {
    static const QRegularExpression re(QStringLiteral("polish=(\\d+) ms.*blockedForSync=(\\d+) ms, animations=(\\d+)"));
    return re;
}
const QRegularExpression& RtRe() {
    static const QRegularExpression re(QStringLiteral("frame rendered in \\d+ ?ms, sync=(\\d+), render=(\\d+), swap=(\\d+)"));
    return re;
}

void CaptureHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (ctx.category && qstrcmp(ctx.category, "qt.scenegraph.time.renderloop") == 0) {
        if (msg.contains(QLatin1String("Frame prepared"))) {
            const auto m = GuiRe().match(msg);
            if (m.hasMatch()) {
                std::lock_guard<std::mutex> l(g_parts.m);
                g_parts.polish.push_back(m.captured(1).toInt());
                g_parts.blocked.push_back(m.captured(2).toInt());
                g_parts.anim.push_back(m.captured(3).toInt());
            }
        } else if (msg.contains(QLatin1String("frame rendered"))) {
            const auto m = RtRe().match(msg);
            if (m.hasMatch()) {
                std::lock_guard<std::mutex> l(g_parts.m);
                g_parts.sync.push_back(m.captured(1).toInt());
                g_parts.render.push_back(m.captured(2).toInt());
                g_parts.swap.push_back(m.captured(3).toInt());
            }
        }
        return;
    }
    if (ctx.category && qstrcmp(ctx.category, "qt.scenegraph.general") == 0) {
        const bool gui = OnGuiThread();
        if (msg.contains(QLatin1String("switched to timer mode"))) {
            (gui ? g_driverTimer : g_rtDriverTimer) = 1;
            ++(gui ? g_driverSwitches : g_rtDriverSwitches);
            return;
        }
        if (msg.contains(QLatin1String("switched to vsync mode"))) {
            (gui ? g_driverTimer : g_rtDriverTimer) = 0;
            ++(gui ? g_driverSwitches : g_rtDriverSwitches);
            return;
        }
        if (msg.contains(QLatin1String("using walltime"))) g_driverTimer = 1;
        if (g_graphicsLines.size() < 40) g_graphicsLines << msg.trimmed();
        return; // 启动信息只收进诊断,不往控制台刷
    }
    if (g_prevHandler) g_prevHandler(type, ctx, msg);
}

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
}

QString ApiName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
        case QSGRendererInterface::Software: return QStringLiteral("Software");
        case QSGRendererInterface::OpenGL: return QStringLiteral("OpenGL");
        case QSGRendererInterface::Direct3D11: return QStringLiteral("Direct3D 11");
        case QSGRendererInterface::Vulkan: return QStringLiteral("Vulkan");
        case QSGRendererInterface::Metal: return QStringLiteral("Metal");
        default: return QStringLiteral("未知");
    }
}
} // namespace

// 没选过图形接口时的默认值。
//
// 原来是 D3D12(当时实测桌面歌词场景掉帧更少)。但全屏 4K 下页面切换实测差距巨大:
//   切换专辑墙 → 歌手页,2.2 秒窗口内 >25ms 的帧数:
//     D3D12  p99 60~105ms  max 210~260ms  17~26 帧
//     D3D11  p99  9~12ms   max  29~36ms    1~2 帧
// 差了 10 倍以上。原因是 D3D12 每个 frame slot 只有 16MB staging 缓冲,
// 新页面一屏几十张封面/头像同时上传纹理就会溢出到逐资源慢路径。
// 所以默认改回 D3D11 —— 它是 Windows 上 Qt 的默认后端,也是被优化得最充分的那个。
// 想用 D3D12 的仍可在设置页手动选(用户显式选择优先于这里)。
QString FrameDiag::DefaultRhi() {
#ifdef Q_OS_WIN
    return QStringLiteral("d3d11");
#endif
    return {};
}

void FrameDiag::ApplyStartupOptions() {
    const QSettings s;
    QString rhi = s.value(QStringLiteral("render/rhi")).toString();
    if (rhi.isEmpty()) rhi = DefaultRhi();
    if (!rhi.isEmpty() && qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) qputenv("QSG_RHI_BACKEND", rhi.toLatin1());
    const QString loop = s.value(QStringLiteral("render/loop")).toString();
    if (!loop.isEmpty() && qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) qputenv("QSG_RENDER_LOOP", loop.toLatin1());
    if (s.value(QStringLiteral("render/simpleDriver"), false).toBool() && qEnvironmentVariableIsEmpty("QSG_USE_SIMPLE_ANIMATION_DRIVER"))
        qputenv("QSG_USE_SIMPLE_ANIMATION_DRIVER", "1");
}

QString FrameDiag::rhiChoice() const { return QSettings().value(QStringLiteral("render/rhi")).toString(); }
void FrameDiag::setRhiChoice(const QString& v) {
    if (v == rhiChoice()) return;
    QSettings().setValue(QStringLiteral("render/rhi"), v);
    emit renderOptionsChanged();
}
QString FrameDiag::renderLoop() const { return QSettings().value(QStringLiteral("render/loop")).toString(); }
void FrameDiag::setRenderLoop(const QString& v) {
    if (v == renderLoop()) return;
    QSettings().setValue(QStringLiteral("render/loop"), v);
    emit renderOptionsChanged();
}
bool FrameDiag::simpleDriver() const { return QSettings().value(QStringLiteral("render/simpleDriver"), false).toBool(); }
void FrameDiag::setSimpleDriver(bool v) {
    if (v == simpleDriver()) return;
    QSettings().setValue(QStringLiteral("render/simpleDriver"), v);
    emit renderOptionsChanged();
}

void FrameDiag::CaptureGraphicsInfo() {
    QLoggingCategory::setFilterRules(QStringLiteral("qt.scenegraph.general=true"));
    g_prevHandler = qInstallMessageHandler(CaptureHandler);
}

FrameDiag::FrameDiag(QObject* parent) : QObject(parent) {
    clock_.start();
    probe_.setTimerType(Qt::PreciseTimer);
    probe_.setInterval(2);
    connect(&probe_, &QTimer::timeout, this, &FrameDiag::tick);
    report_.setInterval(250);
    connect(&report_, &QTimer::timeout, this, &FrameDiag::publish);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    logPath_ = QDir(dir).filePath(QStringLiteral("frame_diag.log"));
}

void FrameDiag::attach(QQuickWindow* window) {
    window_ = window;
    if (!window) return;
    // 渲染线程上的信号:直接连接,只记时间戳
    connect(window, &QQuickWindow::frameSwapped, this, [this]() {
        if (!enabled_) return;
        std::lock_guard<std::mutex> l(mtx_);
        swaps_.push_back(clock_.nsecsElapsed());
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::beforeRendering, this, [this]() {
        if (enabled_) renderStart_ = clock_.nsecsElapsed();
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterRendering, this, [this]() {
        if (!enabled_ || !renderStart_) return;
        std::lock_guard<std::mutex> l(mtx_);
        renderMs_.push_back((clock_.nsecsElapsed() - renderStart_) / 1e6);
    }, Qt::DirectConnection);
}

QString FrameDiag::graphics() const {
    QString api = window_ && window_->rendererInterface() ? ApiName(window_->rendererInterface()->graphicsApi()) : QString();
    // 适配器名在 scenegraph 启动日志里 (如 "Adapter 0: 'NVIDIA GeForce RTX 3060' (vendor 0x10de ...) active swapchain")
    QString adapter;
    for (const QString& l : g_graphicsLines) {
        if (l.contains(QStringLiteral("(using)")) || l.contains(QStringLiteral("active")) || l.startsWith(QStringLiteral("GL_RENDERER"))) {
            adapter = l;
            break;
        }
    }
    if (adapter.isEmpty()) {
        for (const QString& l : g_graphicsLines)
            if (l.contains(QStringLiteral("Adapter")) || l.contains(QStringLiteral("GL_RENDERER"))) { adapter = l; break; }
    }
    return adapter.isEmpty() ? api : api + QStringLiteral(" · ") + adapter;
}

void FrameDiag::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    {
        std::lock_guard<std::mutex> l(mtx_);
        swaps_.clear();
        renderMs_.clear();
    }
    second_.clear();
    secondRender_.clear();
    recent_.clear();
    lastSwap_ = 0;
    stallMax_ = stallWindowMax_ = secondStall_ = 0;
    QLoggingCategory::setFilterRules(on ? QStringLiteral("qt.scenegraph.general=true\nqt.scenegraph.time.renderloop=true")
                                        : QStringLiteral("qt.scenegraph.general=true"));
    {
        std::lock_guard<std::mutex> l(g_parts.m);
        g_parts.polish.clear(); g_parts.anim.clear(); g_parts.blocked.clear();
        g_parts.sync.clear(); g_parts.render.clear(); g_parts.swap.clear();
    }
    if (on) {
        lastProbe_ = clock_.nsecsElapsed();
        probe_.start();
        report_.start();
        log_.setFileName(logPath_);
        if (log_.open(QIODevice::Append | QIODevice::Text)) {
            QString head = QStringLiteral("\n==== %1  %2  Qt %3\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                                                                        QSysInfo::prettyProductName(), QString::fromLatin1(qVersion()));
            head += QStringLiteral("graphics: %1\n").arg(graphics());
            head += QStringLiteral("env: QSG_RHI_BACKEND=%1 QSG_USE_SIMPLE_ANIMATION_DRIVER=%2 QSG_RENDER_LOOP=%3\n")
                        .arg(qEnvironmentVariable("QSG_RHI_BACKEND"), qEnvironmentVariable("QSG_USE_SIMPLE_ANIMATION_DRIVER"),
                             qEnvironmentVariable("QSG_RENDER_LOOP"));
            head += QStringLiteral("windows: %1 visible\n").arg([] {
                int n = 0;
                for (QWindow* w : QGuiApplication::topLevelWindows()) n += w->isVisible();
                return n;
            }());
            for (const QString& l : g_graphicsLines) head += QStringLiteral("  sg: %1\n").arg(l);
            if (window_ && window_->screen())
                head += QStringLiteral("screen: %1  %2 Hz  dpr %3\n").arg(window_->screen()->name())
                            .arg(window_->screen()->refreshRate()).arg(window_->devicePixelRatio());
            for (const QScreen* sc : QGuiApplication::screens())
                head += QStringLiteral("  monitor %1%2: %3 Hz %4x%5\n").arg(sc->name(), sc == QGuiApplication::primaryScreen() ? " (primary)" : "")
                            .arg(sc->refreshRate()).arg(sc->size().width()).arg(sc->size().height());
            head += QStringLiteral("time      fps  p50   p99   max   janks render gui_stall  [max ms: polish anim | sync render swap]  state\n");
            log_.write(head.toUtf8());
            log_.flush();
        }
    } else {
        probe_.stop();
        report_.stop();
        log_.close();
    }
    emit enabledChanged();
    emit statsChanged();
}

void FrameDiag::tick() {
    const qint64 now = clock_.nsecsElapsed();
    const double late = (now - lastProbe_) / 1e6 - probe_.interval();
    lastProbe_ = now;
    if (late > stallWindowMax_) stallWindowMax_ = late;
    if (late > secondStall_) secondStall_ = late;
    ++stateSamples_;
    for (auto it = state_.cbegin(); it != state_.cend(); ++it)
        if (it.value().typeId() == QMetaType::Bool && it.value().toBool()) ++stateOn_[it.key()];
}

void FrameDiag::publish() {
    std::vector<qint64> swaps;
    std::vector<double> render;
    {
        std::lock_guard<std::mutex> l(mtx_);
        swaps.swap(swaps_);
        render.swap(renderMs_);
    }
    for (qint64 t : swaps) {
        if (lastSwap_) {
            const double d = (t - lastSwap_) / 1e6;
            // 静止时不出帧是正常的:超过 250ms 的间隔视为空闲,不计入
            if (d < 250) {
                second_.push_back(d);
                recent_.append(d);
            }
        }
        lastSwap_ = t;
    }
    while (recent_.size() > 120) recent_.removeFirst();
    secondRender_.insert(secondRender_.end(), render.begin(), render.end());

    const double hz = window_ && window_->screen() ? window_->screen()->refreshRate() : 60.0;
    const double frame = 1000.0 / (hz > 1 ? hz : 60.0);
    std::vector<double> recentVals;
    for (const QVariant& v : recent_) recentVals.push_back(v.toDouble());
    int janks = 0;
    for (double d : recentVals) janks += d > frame * 1.5;
    QVariantMap s;
    s["fps"] = recentVals.empty() ? 0 : qRound(1000.0 / std::max(1.0, Pct(recentVals, 0.5)));
    s["p50"] = Pct(recentVals, 0.5);
    s["p99"] = Pct(recentVals, 0.99);
    s["max"] = recentVals.empty() ? 0.0 : *std::max_element(recentVals.begin(), recentVals.end());
    s["janks"] = janks;
    s["hz"] = hz;
    s["frame"] = frame;
    s["stall"] = stallWindowMax_;
    s["render"] = render.empty() ? 0.0 : Pct(render, 0.9);
    s["timerMode"] = g_driverTimer.load() != 0;
    s["switches"] = g_driverSwitches.load();
    s["rtTimerMode"] = g_rtDriverTimer.load() != 0;
    s["rtSwitches"] = g_rtDriverSwitches.load();
    const QScreen* primary = QGuiApplication::primaryScreen();
    s["primaryHz"] = primary ? primary->refreshRate() : 0.0;
    stats_ = s;
    stallWindowMax_ = 0;
    emit statsChanged();

    if (++reports_ % 4 != 0) return;
    // 每秒一行日志
    if (log_.isOpen() && !second_.empty()) {
        int j = 0;
        for (double d : second_) j += d > frame * 1.5;
        QStringList st;
        // 布尔状态按本秒内为真的比例写:全程为真只写名字,部分时间为真写百分比
        for (auto it = state_.cbegin(); it != state_.cend(); ++it) {
            const QVariant& v = it.value();
            if (v.typeId() == QMetaType::Bool) {
                const int on = stateOn_.value(it.key());
                if (!on || !stateSamples_) continue;
                const int pct = qRound(100.0 * on / stateSamples_);
                st << (pct >= 98 ? it.key() : QStringLiteral("%1:%2%").arg(it.key()).arg(pct));
            } else if (!v.toString().isEmpty()) {
                st << it.key() + "=" + v.toString();
            }
        }
        if (window_ && !window_->isActive()) st << QStringLiteral("inactive");
        if (g_driverTimer.load()) st << QStringLiteral("GUI_DRIVER=timer");
        if (g_rtDriverTimer.load()) st << QStringLiteral("RT_DRIVER=timer");
        QString parts;
        {
            std::lock_guard<std::mutex> l(g_parts.m);
            auto mx = [](const std::vector<int>& v) { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); };
            parts = QStringLiteral("[%1 %2 | %3 %4 %5]").arg(mx(g_parts.polish), 2).arg(mx(g_parts.anim), 2)
                        .arg(mx(g_parts.sync), 2).arg(mx(g_parts.render), 2).arg(mx(g_parts.swap), 2);
            g_parts.polish.clear(); g_parts.anim.clear(); g_parts.blocked.clear();
            g_parts.sync.clear(); g_parts.render.clear(); g_parts.swap.clear();
        }
        const QString line = QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8  %10  %9\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
            .arg(second_.size(), 4)
            .arg(Pct(second_, 0.5), 5, 'f', 1)
            .arg(Pct(second_, 0.99), 5, 'f', 1)
            .arg(*std::max_element(second_.begin(), second_.end()), 5, 'f', 1)
            .arg(j, 5)
            .arg(Pct(secondRender_, 0.9), 6, 'f', 1)
            .arg(secondStall_, 8, 'f', 1)
            .arg(st.join(' '), parts);
        log_.write(line.toUtf8());
        log_.flush();
    }
    second_.clear();
    secondRender_.clear();
    secondStall_ = 0;
    stateOn_.clear();
    stateSamples_ = 0;
}
