#include "taskbar_helper.h"

#include <QPainter>
#include <QTimer>
#include <algorithm>
#include <iterator>
#include <string>
#include <QQuickWindow>
#include <QSettings>
#include <QSvgRenderer>

#include "player_controller.h"
#include "playlist_service.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>
#endif

namespace {
enum Button { kPrev = 0, kPlay, kNext, kFav, kLyrics, kButtonCount };
enum Icon { kIcoPrev = 0, kIcoPlay, kIcoPause, kIcoNext, kIcoHeart, kIcoHeartFilled, kIcoLyrics };
const char* const kIconNames[] = {"prev", "play", "pause", "next", "heart", "heart-filled", "lyrics"};
// 按钮 id 从这里起,避开菜单命令常用的小数值
const unsigned kIdBase = 0xB800;

#ifdef Q_OS_WIN
// 不同 SDK 头文件里这些值的可见性取决于 _WIN32_WINNT,直接写数值
const UINT kMsgSendIconicThumbnail = 0x0323;        // WM_DWMSENDICONICTHUMBNAIL
const UINT kMsgSendIconicLivePreview = 0x0326;      // WM_DWMSENDICONICLIVEPREVIEWBITMAP
const DWORD kAttrForceIconic = 7;                   // DWMWA_FORCE_ICONIC_REPRESENTATION
const DWORD kAttrHasIconicBitmap = 10;              // DWMWA_HAS_ICONIC_BITMAP

// 32 位预乘 Alpha 自上而下 DIB (DWM 缩略图要求的格式)
HBITMAP ToDib(const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.width();
    bi.bmiHeader.biHeight = -img.height();
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    const int stride = img.width() * 4;
    for (int y = 0; y < img.height(); ++y)
        memcpy(static_cast<uchar*>(bits) + y * stride, img.constScanLine(y), stride);
    return bmp;
}

ITaskbarList3* Taskbar(void* p) { return static_cast<ITaskbarList3*>(p); }
#endif
}  // namespace

TaskbarHelper::TaskbarHelper(PlayerController* player, PlaylistService* playlists, QObject* parent)
    : QObject(parent), player_(player), playlists_(playlists) {
#ifdef Q_OS_WIN
    buttonMsg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // 任务栏是深色还是浅色 (决定按钮图标的颜色)
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                  QSettings::NativeFormat);
    lightTaskbar_ = reg.value(QStringLiteral("SystemUsesLightTheme"), 0).toInt() != 0;
#endif
}

TaskbarHelper::~TaskbarHelper() {
#ifdef Q_OS_WIN
    for (void*& ico : icons_)
        if (ico) {
            DestroyIcon(static_cast<HICON>(ico));
            ico = nullptr;
        }
    if (taskbar_) Taskbar(taskbar_)->Release();
#endif
}

void TaskbarHelper::attach(QQuickWindow* window) {
#ifdef Q_OS_WIN
    if (!window || hwnd_) return;
    window_ = window;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    hwnd_ = reinterpret_cast<quintptr>(hwnd);
    // 以管理员身份运行时,Explorer 发来的 TaskbarButtonCreated 会被 UIPI 拦掉
    ChangeWindowMessageFilterEx(hwnd, buttonMsg_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);

    // 缩略图与速览由程序自己提供:缩略图 = 封面,速览 = 窗口截图
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, kAttrForceIconic, &on, sizeof(on));
    DwmSetWindowAttribute(hwnd, kAttrHasIconicBitmap, &on, sizeof(on));

    ITaskbarList3* tb = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tb)))) {
        if (SUCCEEDED(tb->HrInit())) taskbar_ = tb;
        else tb->Release();
    }
    const int edge = qRound(16 * window->devicePixelRatio());
    for (int i = 0; i < 7; ++i) {
        QImage img = renderIcon(QString::fromLatin1(kIconNames[i])).scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        icons_[i] = img.toHICON();
    }
    // 窗口在过滤器装好之前就已显示时,任务栏按钮已经建好,TaskbarButtonCreated 不会再来:直接试一次
    createButtons();

    connect(player_, &PlayerController::playingChanged, this, &TaskbarHelper::updateButtons);
    connect(player_, &PlayerController::trackChanged, this, &TaskbarHelper::updateButtons);
    connect(playlists_, &PlaylistService::playlistsChanged, this, &TaskbarHelper::updateButtons);
    connect(player_, &PlayerController::coverImageChanged, this, &TaskbarHelper::invalidateThumbnail);
    connect(player_, &PlayerController::trackChanged, this, &TaskbarHelper::invalidateThumbnail);
#else
    Q_UNUSED(window);
#endif
}

QImage TaskbarHelper::renderIcon(const QString& name) const {
    QImage img(64, 64, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QSvgRenderer svg(QStringLiteral(":/icons/%1.svg").arg(name));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    svg.render(&p, QRectF(4, 4, 56, 56));
    // 线条图标原本是白色,浅色任务栏上改为深灰
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(img.rect(), lightTaskbar_ ? QColor(0x20, 0x20, 0x20) : QColor(0xff, 0xff, 0xff));
    return img;
}

void TaskbarHelper::createButtons() {
#ifdef Q_OS_WIN
    if (!taskbar_ || buttonsAdded_ || !hwnd_) return;
    THUMBBUTTON b[kButtonCount] = {};
    for (int i = 0; i < kButtonCount; ++i) {
        b[i].dwMask = THUMBBUTTONMASK(THB_ICON | THB_TOOLTIP | THB_FLAGS);
        b[i].iId = kIdBase + i;
        b[i].dwFlags = THBF_ENABLED;
    }
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    // 先放占位图标,真正的状态 (播放/暂停、是否收藏) 由 updateButtons 填
    b[kPrev].hIcon = static_cast<HICON>(icons_[kIcoPrev]);
    b[kPlay].hIcon = static_cast<HICON>(icons_[kIcoPlay]);
    b[kNext].hIcon = static_cast<HICON>(icons_[kIcoNext]);
    b[kFav].hIcon = static_cast<HICON>(icons_[kIcoHeart]);
    b[kLyrics].hIcon = static_cast<HICON>(icons_[kIcoLyrics]);
    if (FAILED(Taskbar(taskbar_)->ThumbBarAddButtons(hwnd, kButtonCount, b))) return;
    buttonsAdded_ = true;
    updateButtons();
#endif
}

void TaskbarHelper::updateButtons() {
#ifdef Q_OS_WIN
    if (!taskbar_ || !buttonsAdded_) return;
    const bool has = player_->hasTrack();
    const bool fav = has && playlists_->isFavorite(player_->trackId());
    struct Spec {
        int icon;
        QString tip;
        bool enabled;
    };
    const Spec specs[kButtonCount] = {
        {kIcoPrev, tr("上一首"), has},
        {player_->playing() ? kIcoPause : kIcoPlay, player_->playing() ? tr("暂停") : tr("播放"), has},
        {kIcoNext, tr("下一首"), has},
        {fav ? kIcoHeartFilled : kIcoHeart, fav ? tr("取消收藏") : tr("收藏"), has},
        {kIcoLyrics, tr("桌面歌词"), true},
    };
    THUMBBUTTON b[kButtonCount] = {};
    for (int i = 0; i < kButtonCount; ++i) {
        b[i].dwMask = THUMBBUTTONMASK(THB_ICON | THB_TOOLTIP | THB_FLAGS);
        b[i].iId = kIdBase + i;
        b[i].hIcon = static_cast<HICON>(icons_[specs[i].icon]);
        b[i].dwFlags = specs[i].enabled ? THBF_ENABLED : THBF_DISABLED;
        const std::wstring tip = specs[i].tip.toStdWString();
        const size_t n = std::min(tip.size(), std::size(b[i].szTip) - 1);
        std::copy_n(tip.c_str(), n, b[i].szTip);
        b[i].szTip[n] = L'\0';
    }
    Taskbar(taskbar_)->ThumbBarUpdateButtons(reinterpret_cast<HWND>(hwnd_), kButtonCount, b);
#endif
}

void TaskbarHelper::invalidateThumbnail() {
#ifdef Q_OS_WIN
    if (hwnd_) DwmInvalidateIconicBitmaps(reinterpret_cast<HWND>(hwnd_));
#endif
}

void TaskbarHelper::sendThumbnail(int maxWidth, int maxHeight) {
#ifdef Q_OS_WIN
    if (!hwnd_ || maxWidth <= 0 || maxHeight <= 0) return;
    // 封面铺满正方形缩略图;没有在播放的歌时用程序图标
    QImage cover = player_->hasTrack() ? player_->coverImage() : QImage();
    if (cover.isNull()) cover = QImage(QStringLiteral(":/images/app_icon_256.png"));
    const int edge = std::min(maxWidth, maxHeight);
    QImage out(edge, edge, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (!cover.isNull()) {
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QImage scaled = cover.scaled(edge, edge, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawImage((edge - scaled.width()) / 2, (edge - scaled.height()) / 2, scaled);
    }
    HBITMAP bmp = ToDib(out);
    if (!bmp) return;
    DwmSetIconicThumbnail(reinterpret_cast<HWND>(hwnd_), bmp, 0);
    DeleteObject(bmp);
#else
    Q_UNUSED(maxWidth);
    Q_UNUSED(maxHeight);
#endif
}

void TaskbarHelper::sendLivePreview() {
#ifdef Q_OS_WIN
    if (!hwnd_ || !window_) return;
    const QImage shot = window_->grabWindow();
    if (shot.isNull()) return;
    HBITMAP bmp = ToDib(shot);
    if (!bmp) return;
    DwmSetIconicLivePreviewBitmap(reinterpret_cast<HWND>(hwnd_), bmp, nullptr, 0);
    DeleteObject(bmp);
#endif
}

bool TaskbarHelper::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" || !hwnd_) return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (reinterpret_cast<quintptr>(msg->hwnd) != hwnd_) return false;
    if (msg->message == buttonMsg_ && buttonMsg_) {
        // Explorer 重启后按钮需要重新添加
        buttonsAdded_ = false;
        createButtons();
        return false;
    }
    switch (msg->message) {
    case WM_COMMAND:
        if (HIWORD(msg->wParam) == THBN_CLICKED) {
            switch (static_cast<int>(LOWORD(msg->wParam)) - static_cast<int>(kIdBase)) {
            case kPrev: player_->previous(); break;
            case kPlay: player_->toggle(); break;
            case kNext: player_->next(); break;
            case kFav:
                if (player_->hasTrack()) playlists_->toggleFavorite(player_->trackId());
                break;
            case kLyrics: emit toggleDesktopLyrics(); break;
            default: return false;
            }
            if (result) *result = 0;
            return true;
        }
        return false;
    case kMsgSendIconicThumbnail:
        sendThumbnail(HIWORD(msg->lParam), LOWORD(msg->lParam));
        if (result) *result = 0;
        return true;
    case kMsgSendIconicLivePreview:
        // 截图要等渲染线程出一帧,不在 DWM 的同步消息里做
        QTimer::singleShot(0, this, &TaskbarHelper::sendLivePreview);
        if (result) *result = 0;
        return true;
    default:
        return false;
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}
