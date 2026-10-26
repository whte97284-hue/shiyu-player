#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <QVariantList>
#include <mutex>
#include <vector>

// 帧诊断 (Ctrl+Shift+P):弄清「滚动时流畅不流畅」到底跟什么挂钩
//  · 帧间隔:渲染线程每次 present 完成 (frameSwapped) 记一笔,算中位 / 最慢 / 掉帧数,最近 120 帧画成柱
//  · 界面线程阻塞:每 2ms 一次的定时器,记它迟到了多久 —— 迟到就是界面线程被别的事占住 (JS、建代理、解码)
//  · 渲染线程每帧录制命令的耗时 (beforeRendering → afterRendering)
//  · 显卡后端 / 适配器 (scenegraph 启动日志)、系统报告的刷新率 vs 实测出帧节拍
//  · 每秒一行写进日志,附上当时的状态 (滚动中、播放中、减弱动效、桌面歌词、窗口前台……),
//    卡的时候和顺的时候对比,就能看出是哪一项在变
class FrameDiag : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY statsChanged)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY statsChanged)   // 最近 120 帧的间隔 (ms)
    Q_PROPERTY(QString graphics READ graphics NOTIFY statsChanged)
    Q_PROPERTY(QString logPath READ logPath CONSTANT)
    // 由 QML 绑定进来的状态,只用于记日志
    Q_PROPERTY(QVariantMap state READ state WRITE setState)
    // 启动参数 (下次启动生效,便于对比):图形接口 (空 = 默认 / d3d11 / d3d12 / opengl / vulkan)、
    // Qt 的「简单动画驱动」(始终按挂钟推进,不在两种模式间切换)
    Q_PROPERTY(QString rhiChoice READ rhiChoice WRITE setRhiChoice NOTIFY renderOptionsChanged)
    Q_PROPERTY(bool simpleDriver READ simpleDriver WRITE setSimpleDriver NOTIFY renderOptionsChanged)
    // 渲染循环 (空 = 默认 threaded / basic)、没选图形接口时实际用的默认值
    Q_PROPERTY(QString renderLoop READ renderLoop WRITE setRenderLoop NOTIFY renderOptionsChanged)
    Q_PROPERTY(QString defaultRhi READ DefaultRhi CONSTANT)
public:
    explicit FrameDiag(QObject* parent = nullptr);
    // 在 QGuiApplication 创建之后、窗口创建之前调用:打开 scenegraph 启动信息并截获它
    static void CaptureGraphicsInfo();
    // 在 QGuiApplication 创建之前调用:按保存的启动参数设置环境变量 (用户自己设了环境变量时不覆盖)
    static void ApplyStartupOptions();
    QString rhiChoice() const;
    void setRhiChoice(const QString& v);
    bool simpleDriver() const;
    QString renderLoop() const;
    void setRenderLoop(const QString& v);
    static QString DefaultRhi();
    void setSimpleDriver(bool v);

    void attach(QQuickWindow* window);
    bool enabled() const { return enabled_; }
    void setEnabled(bool on);
    QVariantMap stats() const { return stats_; }
    QVariantList recent() const { return recent_; }
    QString graphics() const;
    QString logPath() const { return logPath_; }
    QVariantMap state() const { return state_; }
    void setState(const QVariantMap& s) { state_ = s; }

signals:
    void enabledChanged();
    void statsChanged();
    void renderOptionsChanged();

private:
    void tick();        // 2ms:界面线程迟到量
    void publish();     // 250ms:汇总给界面;每 4 次写一行日志

    QPointer<QQuickWindow> window_;
    bool enabled_ = false;
    QElapsedTimer clock_;
    QTimer probe_;
    QTimer report_;
    qint64 lastProbe_ = 0;
    double stallMax_ = 0;       // 本秒内界面线程最长阻塞 (ms)
    double stallWindowMax_ = 0; // 本 250ms 内
    int reports_ = 0;

    std::mutex mtx_;            // 以下由渲染线程写入
    std::vector<qint64> swaps_;
    std::vector<double> renderMs_;
    qint64 renderStart_ = 0;

    std::vector<double> second_;   // 本秒的帧间隔
    std::vector<double> secondRender_;
    double secondStall_ = 0;
    qint64 lastSwap_ = 0;
    QVariantMap stats_;
    QVariantList recent_;
    QVariantMap state_;
    QMap<QString, int> stateOn_;   // 本秒内各状态为真的采样数 (每 2ms 采一次)
    int stateSamples_ = 0;
    QString logPath_;
    QFile log_;
};
