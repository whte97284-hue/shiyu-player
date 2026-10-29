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
const int kHotIds[3] = {kHotPlayPause, kHotNext, kHotPrev};

struct HotkeySpec {
    UINT mods = 0;
    UINT vk = 0;
    bool ok = false;
};

// 单个键名 -> 虚拟键码;isModifier 为 true 时 vk 无意义
UINT VkFromName(const QString& raw, bool& isModifier) {
    isModifier = false;
    const QString n = raw.trimmed();
    if (n.isEmpty()) return 0;
    const QString l = n.toLower();
    if (l == "ctrl" || l == "control" || l == "alt" || l == "shift" || l == "win" || l == "meta") {
        isModifier = true;
        return 0;
    }
    if (l == "space") return VK_SPACE;
    if (l == "enter" || l == "return") return VK_RETURN;
    if (l == "tab") return VK_TAB;
    if (l == "esc" || l == "escape") return VK_ESCAPE;
    if (l == "left") return VK_LEFT;
    if (l == "right") return VK_RIGHT;
    if (l == "up") return VK_UP;
    if (l == "down") return VK_DOWN;
    if (l == "home") return VK_HOME;
    if (l == "end") return VK_END;
    if (l == "pgup" || l == "pageup") return VK_PRIOR;
    if (l == "pgdn" || l == "pagedown") return VK_NEXT;
    if (l == "ins" || l == "insert") return VK_INSERT;
    if (l == "del" || l == "delete") return VK_DELETE;
    if (l == "mediaplaypause" || l == "mediaplay") return VK_MEDIA_PLAY_PAUSE;
    if (l == "medianext") return VK_MEDIA_NEXT_TRACK;
    if (l == "mediaprev" || l == "mediaprevious") return VK_MEDIA_PREV_TRACK;
    if (l == "mediastop") return VK_MEDIA_STOP;
    if (l.size() >= 2 && l[0] == QLatin1Char('f')) {
        bool numOk = false;
        const int num = l.mid(1).toInt(&numOk);
        if (numOk && num >= 1 && num <= 24) return VK_F1 + static_cast<UINT>(num - 1);
    }
    if (n.size() == 1) {
        const QChar c = n.toUpper()[0];
        if (c.isLetterOrNumber()) return static_cast<UINT>(c.unicode());
    }
    return 0;
}

HotkeySpec ParseHotkey(const QString& seq) {
    HotkeySpec out;
    const QStringList parts = seq.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return out;
    for (const QString& part : parts) {
        bool isMod = false;
        const UINT vk = VkFromName(part, isMod);
        const QString l = part.trimmed().toLower();
        if (isMod) {
            if (l == "ctrl" || l == "control") out.mods |= MOD_CONTROL;
            else if (l == "alt") out.mods |= MOD_ALT;
            else if (l == "shift") out.mods |= MOD_SHIFT;
            else if (l == "win" || l == "meta") out.mods |= MOD_WIN;
            continue;
        }
        if (vk == 0 || out.vk != 0) return out; // 键名无法识别,或出现了两个主键
        out.vk = vk;
    }
    out.ok = out.vk != 0;
    return out;
}
}

void WindowHelper::registerMediaKeys(QWindow* window) {
#ifdef Q_OS_WIN
    if (!window || hwnd_) return;
    hwnd_ = static_cast<quintptr>(window->winId()); // WId 在 Qt6 已经是 quintptr
    applyHotkeys();
#else
    Q_UNUSED(window);
#endif
}

void WindowHelper::setHotkeys(const QString& playPause, const QString& next, const QString& prev) {
    hotkeys_[0] = playPause;
    hotkeys_[1] = next;
    hotkeys_[2] = prev;
    applyHotkeys();
}

bool WindowHelper::isValidHotkey(const QString& seq) const {
    if (seq.trimmed().isEmpty()) return true; // 空 = 禁用,也算合法
    return ParseHotkey(seq).ok;
}

void WindowHelper::applyHotkeys() {
#ifdef Q_OS_WIN
    if (!hwnd_) return;
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    for (int i = 0; i < 3; ++i) {
        if (registered_ & (1 << i)) {
            UnregisterHotKey(hwnd, kHotIds[i]);
            registered_ &= ~(1 << i);
        }
    }
    for (int i = 0; i < 3; ++i) {
        const HotkeySpec spec = ParseHotkey(hotkeys_[i]);
        if (!spec.ok) continue;
        // 被别的程序占用时 RegisterHotKey 返回 false,这一位就不会置上,
        // 界面用 registeredMask() 就能提示用户换个键
        if (RegisterHotKey(hwnd, kHotIds[i], spec.mods, spec.vk)) registered_ |= (1 << i);
    }
    emit hotkeysRegistered();
#endif
}

WindowHelper::~WindowHelper() {
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    for (int i = 0; i < 3; ++i)
        if (registered_ & (1 << i)) UnregisterHotKey(hwnd, kHotIds[i]);
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
