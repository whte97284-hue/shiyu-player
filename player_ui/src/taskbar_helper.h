#pragma once
#include <QAbstractNativeEventFilter>
#include <QImage>
#include <QObject>
#include <QPointer>

class QQuickWindow;
class PlayerController;
class PlaylistService;

// Windows 任务栏:鼠标悬停在任务栏图标上时
//  · 缩略图显示当前歌曲的封面 (而不是缩小的整个窗口),标题为「歌名 — 歌手」(即窗口标题)
//  · 缩略图下方一排按钮:上一首 / 播放·暂停 / 下一首 / 收藏 / 桌面歌词
//  · 「速览」(悬停缩略图) 时显示真实窗口
// 其他平台为空操作。
class TaskbarHelper : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    TaskbarHelper(PlayerController* player, PlaylistService* playlists, QObject* parent = nullptr);
    ~TaskbarHelper() override;

    void attach(QQuickWindow* window);
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void toggleDesktopLyrics();

private:
    void createButtons();
    void updateButtons();
    void invalidateThumbnail();
    void sendThumbnail(int maxWidth, int maxHeight);
    void sendLivePreview();
    // 渲染 24 视口的线条图标为任务栏按钮用的小图标 (按任务栏深浅色着色)
    QImage renderIcon(const QString& name) const;

    PlayerController* player_;
    PlaylistService* playlists_;
    QPointer<QQuickWindow> window_;
    quintptr hwnd_ = 0;
    void* taskbar_ = nullptr;   // ITaskbarList3*
    unsigned buttonMsg_ = 0;    // RegisterWindowMessage("TaskbarButtonCreated")
    bool buttonsAdded_ = false;
    bool lightTaskbar_ = false;
    void* icons_[7] = {};       // HICON:prev play pause next heart heart-filled lyrics
};
