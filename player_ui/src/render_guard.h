#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <atomic>

class QQuickWindow;

// 渲染看门狗。
//
// 解决什么问题:
//   显示器休眠 / 电视切输入源 / 拔插 HDMI 之后,vsync 信号会消失。
//   Qt 的 threaded 渲染线程这时候会卡在 Present 上等一个永远不来的信号,
//   唤醒后**不会自己恢复**。表现是:画面冻在休眠前那一帧,
//   但点击仍有响应、音乐照常播放(GUI 线程和音频线程都是好的)。
//
// 怎么判断"卡住"而不是"只是静止":
//   QML 静止时不出帧是正常的,不能拿"多久没出帧"当判据。
//   这里主动 requestUpdate() 请求一帧,300ms 后检查 frameSwapped 有没有发生 ——
//   正常时必定立刻出帧,卡住时则不会。
//
// 恢复手段分两级:
//   1) 第一次失败:再 update() 催一次(温和,不闪)
//   2) 连续两次失败:最小化再恢复窗口,强制销毁重建 swapchain 与图形资源
class RenderGuard : public QObject {
    Q_OBJECT
public:
    explicit RenderGuard(QObject* parent = nullptr);

    void attach(QQuickWindow* window);
    int recoveries() const { return recoveries_; }

signals:
    void recovered();

private:
    void probe();
    void check(qint64 requestedAt);
    void rebuildSwapchain();

    QQuickWindow* window_ = nullptr;
    QTimer probe_;
    QElapsedTimer clock_;
    std::atomic<qint64> lastSwap_{0};
    int fails_ = 0;
    int recoveries_ = 0;
};
