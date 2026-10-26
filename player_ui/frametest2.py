#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""帧诊断对比 v2:显式指定图形接口 + 桌面歌词开关

v1 的缺陷:未设 QSG_RHI_BACKEND 时会回落到 QSettings 里的 render/rhi(=d3d12),
导致"默认"和"D3D12"其实是同一组配置。v2 一律显式指定,并支持切换桌面歌词。

关键背景(Claude 分析):
- Qt Quick 动画驱动:某帧超过 1.25×刷新周期(144Hz -> 8.7ms)就记一次,累计够多切"计时器模式";
  计时器模式按挂钟推进动画 -> 一顿一顿;要切回垂直同步需连续 10 帧 < 8.7ms。
- 线程化渲染循环只在【单窗口】时用垂直同步驱动动画;两个及以上可见窗口 -> 改由系统计时器驱动。
  桌面歌词就是第二个窗口。
"""
import ctypes, os, sys, time, subprocess, re, json, winreg
from ctypes import wintypes

u32 = ctypes.WinDLL('user32', use_last_error=True)
INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
KEYEVENTF_KEYUP = 0x0002
MOUSEEVENTF_WHEEL = 0x0800
VK_CONTROL, VK_SHIFT = 0x11, 0x10

REG_PATH = r'Software\KugouPlayer\KugouPlayer\lyrics'
EXE = r'F:\kpgui\b\player_ui\KugouPlayer.exe'
LOG = r'C:\Users\33011\AppData\Local\KugouPlayer\KugouPlayer\frame_diag.log'


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class _IU(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _IU)]


def _send(i):
    u32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def key(vk, up=False):
    i = INPUT(type=INPUT_KEYBOARD)
    i.u.ki = KEYBDINPUT(wVk=vk, wScan=0, dwFlags=KEYEVENTF_KEYUP if up else 0,
                        time=0, dwExtraInfo=None)
    _send(i)


def wheel(delta):
    i = INPUT(type=INPUT_MOUSE)
    i.u.mi = MOUSEINPUT(0, 0, ctypes.c_uint32(delta & 0xFFFFFFFF),
                        MOUSEEVENTF_WHEEL, 0, None)
    _send(i)


def windows_of(pid):
    out = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        wpid = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and u32.IsWindowVisible(hwnd):
            r = wintypes.RECT()
            u32.GetWindowRect(hwnd, ctypes.byref(r))
            n = u32.GetWindowTextLengthW(hwnd)
            b = ctypes.create_unicode_buffer(n + 1)
            u32.GetWindowTextW(hwnd, b, n + 1)
            out.append({'hwnd': hwnd, 'title': b.value,
                        'w': r.right - r.left, 'h': r.bottom - r.top,
                        'rect': (r.left, r.top, r.right, r.bottom)})
        return True
    u32.EnumWindows(cb, 0)
    return out


def focus_main(pid):
    ws = [w for w in windows_of(pid) if w['w'] > 400 and w['h'] > 300]
    if not ws:
        return None
    m = max(ws, key=lambda w: w['w'] * w['h'])
    u32.ShowWindow(m['hwnd'], 9)
    u32.SetForegroundWindow(m['hwnd'])
    time.sleep(0.4)
    u32.SetCursorPos((m['rect'][0] + m['rect'][2]) // 2,
                     (m['rect'][1] + m['rect'][3]) // 2)
    return m


def read_log():
    if not os.path.exists(LOG):
        return ''
    return open(LOG, 'rb').read().replace(b'\x00', b'').decode('utf-8', 'replace')


def set_lyrics(on):
    """直接改 QSettings 的 lyrics/desktop (注册表)"""
    k = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_SET_VALUE)
    winreg.SetValueEx(k, 'desktop', 0, winreg.REG_SZ, 'true' if on else 'false')
    k.Close()


def get_lyrics():
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_READ)
        v = winreg.QueryValueEx(k, 'desktop')[0]
        k.Close()
        return v == 'true'
    except Exception:
        return False


def run_case(name, backend, simple, lyrics, scroll_secs=20):
    set_lyrics(lyrics)
    env = dict(os.environ)
    env['QSG_RHI_BACKEND'] = backend
    if simple:
        env['QSG_USE_SIMPLE_ANIMATION_DRIVER'] = '1'
    else:
        env.pop('QSG_USE_SIMPLE_ANIMATION_DRIVER', None)

    mark = len(read_log())
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                         creationflags=0x00000008 | 0x00000200, close_fds=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    nwin = 0
    try:
        time.sleep(5.5)
        if not focus_main(p.pid):
            return name, None, '找不到主窗口'
        nwin = len([w for w in windows_of(p.pid) if w['w'] > 200])
        key(VK_CONTROL); key(VK_SHIFT); key(0x50); key(0x50, True)
        key(VK_SHIFT, True); key(VK_CONTROL, True)
        time.sleep(1.5)

        t0 = time.time(); dt = 1.0 / 25; d = -120; n = 0
        while time.time() - t0 < scroll_secs:
            wheel(d); n += 1
            if n % 100 == 0:
                d = -d
            time.sleep(dt)
        time.sleep(1.5)
    finally:
        try:
            p.terminate()
        except Exception:
            pass
        time.sleep(1.0)
        subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)

    seg = read_log()[mark:]
    return name, (seg, nwin), None


def parse(seg, nwin):
    lines = seg.splitlines()
    rows, header = [], {}
    for ln in lines:
        if ln.startswith('graphics:'):
            header['graphics'] = ln.split(':', 1)[1].strip()
        elif ln.startswith('windows:'):
            header['windows'] = ln.split(':', 1)[1].strip()
        m = re.match(r'^(\d\d:\d\d:\d\d)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+\[(.*?)\]\s*(.*)$', ln)
        if m:
            rows.append({'fps': int(m.group(2)), 'p50': float(m.group(3)),
                         'p99': float(m.group(4)), 'max': float(m.group(5)),
                         'janks': int(m.group(6)), 'parts': m.group(9).strip(),
                         'state': m.group(10).strip()})
    if not rows:
        return None
    active = [r for r in rows if r['p50'] < 60 and r['fps'] >= 15]
    if not active:
        return None
    def med(v):
        v = sorted(v); return v[len(v) // 2] if v else 0.0
    TH = 1000.0 / 144.0 * 1.25
    parts = {'polish': [], 'anim': [], 'sync': [], 'render': [], 'swap': []}
    for r in active:
        mm = re.match(r'(\d+)\s+(\d+)\s*\|\s*(\d+)\s+(\d+)\s+(\d+)', r['parts'])
        if mm:
            for k, v in zip(parts, mm.groups()):
                parts[k].append(int(v))
    return {
        'nwin': nwin, 'active': len(active),
        'graphics': header.get('graphics'),
        'fps': med([r['fps'] for r in active]),
        'p50': med([r['p50'] for r in active]),
        'p99': med([r['p99'] for r in active]),
        'max': med([r['max'] for r in active]),
        'janks': sum(r['janks'] for r in active),
        'gui_timer': 100.0 * sum(1 for r in active if 'GUI_DRIVER=timer' in r['state']) / len(active),
        'rt_timer': 100.0 * sum(1 for r in active if 'RT_DRIVER=timer' in r['state']) / len(active),
        'over125': 100.0 * sum(1 for r in active if r['max'] > TH) / len(active),
        'parts': {k: med(v) for k, v in parts.items()},
    }


CASES = [
    # name,            backend,  simple, lyrics
    ('D3D11 歌词关',    'd3d11',  False, False),
    ('D3D12 歌词关',    'd3d12',  False, False),
    ('D3D12 歌词开',    'd3d12',  False, True),   # 用户当前问题场景
    ('D3D11 歌词开',    'd3d11',  False, True),
    ('D3D12 歌词开+简单', 'd3d12', True,  True),   # 候选修复
]

if __name__ == '__main__':
    orig = get_lyrics()
    results = []
    try:
        for name, be, si, ly in CASES:
            print('=== %s  (backend=%s simple=%s lyrics=%s)' % (name, be, si, ly), flush=True)
            nm, res, err = run_case(name, be, si, ly)
            if err:
                print('    失败:', err, flush=True); results.append((nm, None)); continue
            seg, nwin = res
            st = parse(seg, nwin)
            results.append((nm, st))
            if st:
                print('    活跃秒=%d 窗口=%d  fps=%s p50=%s p99=%s max=%s | janks=%d | GUI_timer=%.0f%% RT_timer=%.0f%% | >1.25x=%.0f%%'
                      % (st['active'], st['nwin'], st['fps'], st['p50'], st['p99'], st['max'],
                         st['janks'], st['gui_timer'], st['rt_timer'], st['over125']), flush=True)
                print('    环节(中位): %s' % st['parts'], flush=True)
            else:
                print('    无有效数据', flush=True)
    finally:
        set_lyrics(orig)
        print('\n(已恢复 lyrics/desktop = %s)' % orig)

    print('\n########## 汇总 ##########')
    print('%-20s %-4s %-6s %-6s %-6s %-6s %-8s %-9s %s' %
          ('配置', '窗口', 'p50', 'p99', 'max', 'janks', 'GUI计时器', 'RT计时器', '环节(polish/anim|sync/render/swap)'))
    for nm, st in results:
        if not st:
            print('%-20s 无数据' % nm); continue
        p = st['parts']
        print('%-20s %-4d %-6s %-6s %-6s %-6d %-8.0f%% %-9.0f%% %s' %
              (nm, st['nwin'], st['p50'], st['p99'], st['max'], st['janks'],
               st['gui_timer'], st['rt_timer'],
               '%d/%d | %d/%d/%d' % (p['polish'], p['anim'], p['sync'], p['render'], p['swap'])))
