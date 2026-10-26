#include "window_helper.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

void WindowHelper::applyNativeFrame(QWindow* window) {
#ifdef Q_OS_WIN
    if (!window) return;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    // Windows 11:系统圆角 (DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2);旧系统上调用无害地失败
    const DWORD kCornerPref = 33;
    int round = 2;
    DwmSetWindowAttribute(hwnd, kCornerPref, &round, sizeof(round));
    // 将 DWM 边框扩展 1px 进客户区,无边框窗口即可获得系统原生阴影
    MARGINS m = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    // 深色模式标题栏/系统菜单 (DWMWA_USE_IMMERSIVE_DARK_MODE = 20)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
#else
    Q_UNUSED(window);
#endif
}

namespace {
// 热键 id,只需在本进程内唯一
enum { kHotPlayPause = 0xB701, kHotNext = 0xB702, kHotPrev = 0xB703 };
}

void WindowHelper::registerMediaKeys(QWindow* window) {
#ifdef Q_OS_WIN
    if (!window || hwnd_) return;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    hwnd_ = reinterpret_cast<quintptr>(hwnd);
    if (RegisterHotKey(hwnd, kHotPlayPause, 0, VK_MEDIA_PLAY_PAUSE)) registered_ |= 1;
    if (RegisterHotKey(hwnd, kHotNext, 0, VK_MEDIA_NEXT_TRACK)) registered_ |= 2;
    if (RegisterHotKey(hwnd, kHotPrev, 0, VK_MEDIA_PREV_TRACK)) registered_ |= 4;
#else
    Q_UNUSED(window);
#endif
}

WindowHelper::~WindowHelper() {
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    if (registered_ & 1) UnregisterHotKey(hwnd, kHotPlayPause);
    if (registered_ & 2) UnregisterHotKey(hwnd, kHotNext);
    if (registered_ & 4) UnregisterHotKey(hwnd, kHotPrev);
#endif
}

bool WindowHelper::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG") return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_HOTKEY || reinterpret_cast<quintptr>(msg->hwnd) != hwnd_) return false;
    switch (msg->wParam) {
    case kHotPlayPause: emit mediaPlayPause(); break;
    case kHotNext: emit mediaNext(); break;
    case kHotPrev: emit mediaPrevious(); break;
    default: return false;
    }
    if (result) *result = 0;
    return true;
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}

bool WindowHelper::isWindows() const {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QPoint WindowHelper::cursorPos() const { return QCursor::pos(); }

double WindowHelper::refreshRate(QWindow* window) const {
    QScreen* s = window && window->screen() ? window->screen() : QGuiApplication::primaryScreen();
    const double hz = s ? s->refreshRate() : 0.0;
    return hz >= 24.0 && hz <= 500.0 ? hz : 60.0;
}
