#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""看每个会话「滚动刚开始」那几秒 —— 验证是否存在预热停顿。"""
import re, os

LOG = r'C:\Users\33011\AppData\Local\KugouPlayer\KugouPlayer\frame_diag.log'
ROW = re.compile(r'^(\d\d:\d\d:\d\d)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+\[(.*?)\]\s*(.*)$')

d = open(LOG, 'rb').read().replace(b'\x00', b'').decode('utf-8', 'replace').replace('\r\n', '\n')
segs = re.split(r'^==== ', d, flags=re.M)[1:][-5:]
names = ['D3D11 歌词关', 'D3D12 歌词关', 'D3D12 歌词开', 'D3D11 歌词开', 'D3D12 开+简单']

for nm, s in zip(names, segs):
    rows = []
    for l in s.splitlines():
        m = ROW.match(l)
        if m:
            rows.append(m.groups())
    print('===== %s =====' % nm)
    for r in rows[:10]:
        print('  %s fps=%-4s p50=%-7s max=%-8s janks=%-3s | %s'
              % (r[0], r[1], r[2], r[4], r[5], r[9].strip()[:64]))
    print()

print('\n=== 管线缓存目录 ===')
cache = r'C:\Users\33011\AppData\Local\KugouPlayer\KugouPlayer\cache'
if os.path.isdir(cache):
    for root, dirs, files in os.walk(cache):
        for f in files:
            p = os.path.join(root, f)
            print('  %8d B  %s' % (os.path.getsize(p), p))
else:
    print('  缓存目录不存在')
