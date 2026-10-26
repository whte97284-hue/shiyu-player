#include "render_guard.h"

#include <QQuickWindow>

RenderGuard::RenderGuard(QObject* parent) : QObject(parent) {
    clock_.start();
    // 5 秒探一次:太频繁会白白增加开销,太慢则用户要盯着冻结的画面等太久
    probe_.setInterval(5000);
    connect(&probe_, &QTimer::timeout, this, &RenderGuard::probe);
}

void RenderGuard::attach(QQuickWindow* window) {
    if (!window) return;
    window_ = window;
    // 直接连接:这个信号在渲染线程发出,只记一个时间戳
    connect(window, &QQuickWindow::frameSwapped, this, [this]() {
        lastSwap_.store(clock_.elapsed());
    }, Qt::DirectConnection);
    probe_.start();
}

void RenderGuard::probe() {
    if (!window_) return;
    // 最小化 / 隐藏时不出帧是正常的,不检查
    const auto v = window_->visibility();
    if (!window_->isVisible() || v == QWindow::Minimized || v == QWindow::Hidden) return;
    // 被完全遮挡时 Qt 也不会出帧(Windows 会省掉不可见窗口的绘制)
    if (!window_->isExposed()) return;

    const qint64 at = clock_.elapsed();
    window_->requestUpdate();
    QTimer::singleShot(300, this, [this, at]() { check(at); });
}

void RenderGuard::check(qint64 requestedAt) {
    if (!window_) return;
    // 请求之后出过帧 -> 渲染正常
    if (lastSwap_.load() >= requestedAt) {
        fails_ = 0;
        return;
    }
    ++fails_;
    if (fails_ == 1) {
        // 第一次先温和地再催一下,不闪屏
        window_->update();
        return;
    }
    // 连续两次都没出帧:认为渲染线程卡死,重建交换链
    rebuildSwapchain();
    fails_ = 0;
}

void RenderGuard::rebuildSwapchain() {
    if (!window_) return;
    const auto v = window_->visibility();
    ++recoveries_;
    // 最小化再恢复:窗口尺寸变化会强制销毁并重建 swapchain 与图形资源,
    // 卡在 Present 上的渲染线程也随之解开
    window_->setVisibility(QWindow::Minimized);
    QTimer::singleShot(150, this, [this, v]() {
        if (!window_) return;
        window_->setVisibility(v == QWindow::Minimized ? QWindow::Windowed : v);
        window_->requestUpdate();
        emit recovered();
    });
}
