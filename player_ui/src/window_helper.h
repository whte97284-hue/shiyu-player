#pragma once
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPoint>
#include <QWindow>

// 无边框窗口的平台增强:Windows 11 圆角与系统阴影 (DWM),其他平台为空操作。
// 另负责全局多媒体键:窗口不在前台时,键盘上的播放/暂停、下一首、上一首也能控制播放器
class WindowHelper : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    using QObject::QObject;
    ~WindowHelper() override;
    Q_INVOKABLE void applyNativeFrame(QWindow* window);
    Q_INVOKABLE bool isWindows() const;
    // 鼠标在屏幕上的位置 (桌面歌词锁定后窗口不接收鼠标,靠它判断悬停)
    Q_INVOKABLE QPoint cursorPos() const;
    // 窗口所在显示器的刷新率 (Hz);平滑滚动按它的整数倍推进,画面步长才均匀
    Q_INVOKABLE double refreshRate(QWindow* window) const;

    // 向系统注册多媒体键 (被其他程序占用的键会注册失败,此时只在窗口前台时生效)
    void registerMediaKeys(QWindow* window);
    // 自定义全局热键:字符串形如 "Ctrl+Alt+Space" / "MediaPlayPause" / "F9"
    // 空字符串表示禁用该动作。注册失败(被别的程序占用)时对应的位不会置上
    Q_INVOKABLE void setHotkeys(const QString& playPause, const QString& next, const QString& prev);
    // 位掩码:1=播放暂停 2=下一首 4=上一首,置上表示注册成功
    Q_INVOKABLE int registeredMask() const { return registered_; }
    // 把 "Ctrl+Alt+Space" 变成人类可读的样子,顺便校验格式
    Q_INVOKABLE bool isValidHotkey(const QString& seq) const;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void mediaPlayPause();
    void mediaNext();
    void mediaPrevious();

private:
    void applyHotkeys();

    quintptr hwnd_ = 0;
    int registered_ = 0; // 位掩码:成功注册的热键
    QString hotkeys_[3] = {QStringLiteral("MediaPlayPause"), QStringLiteral("MediaNext"),
                           QStringLiteral("MediaPrev")};
};
