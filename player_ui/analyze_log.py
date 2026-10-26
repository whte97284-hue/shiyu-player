#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 frame_diag.log 里抽取最近 5 个会话, 只看稳态滚动段, 做对比表。"""
import re

LOG = r'C:\Users\33011\AppData\Local\KugouPlayer\KugouPlayer\frame_diag.log'

d = open(LOG, 'rb').read().replace(b'\x00', b'').decode('utf-8', 'replace').replace('\r\n', '\n')
segs = re.split(r'^==== ', d, flags=re.M)[1:][-5:]
names = ['D3D11 歌词关', 'D3D12 歌词关', 'D3D12 歌词开', 'D3D11 歌词开', 'D3D12 歌词开+简单']

ROW = re.compile(r'^(\d\d:\d\d:\d\d)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+\[(.*?)\]\s*(.*)$')
PART = re.compile(r'(\d+)\s+(\d+)\s*\|\s*(\d+)\s+(\d+)\s+(\d+)')


def med(v):
    v = sorted(v)
    return v[len(v) // 2] if v else 0


print('%-18s %-4s %-5s %-5s %-6s %-7s %-9s %-14s %s' % (
    '配置', '窗', 'p50', 'p99', 'max', 'janks', 'GUI计时器', '驱动', '环节 polish/anim|sync/render/swap'))
print('-' * 108)

for nm, s in zip(names, segs):
    nwin = re.search(r'windows: (\d+) visible', s)
    drvs = re.findall(r'Animation Driver: (.*)', s)
    drvtype = 'QElapsedTimer' if 'QElapsedTimer' in s else 'vsync 6.94ms'

    rows = []
    for l in s.splitlines():
        m = ROW.match(l)
        if m:
            rows.append(m.groups())

    steady = [r for r in rows if int(r[1]) >= 100]      # 真在滚动
    if not steady:
        print('%-18s 无稳态数据 (总行 %d)' % (nm, len(rows)))
        continue

    timer = 100.0 * sum(1 for r in steady if 'GUI_DRIVER=timer' in r[9]) / len(steady)
    print('%-18s %-4s %-5s %-5s %-5s %-7d %-9s %-14s %s' % (
        nm,
        nwin.group(1) if nwin else '?',
        med([float(r[2]) for r in steady]),
        med([float(r[3]) for r in steady]),
        med([float(r[4]) for r in steady]),
        sum(int(r[5]) for r in steady),
        '%.0f%%' % timer,
        '%d个/%s' % (len(drvs), drvtype),
        med([r[8].strip() for r in steady])))
    print('%18s 稳态 %d 秒;  该会话总行 %d' % ('', len(steady), len(rows)))
