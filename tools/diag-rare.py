#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生僻词检查。

没有现成的词频表可用，改用**字频**作代理指标：
从汉典词典（34.8 万词条）统计每个字出现在多少个词条中，
再取每个词中「最生僻的那个字」的频次作为该词的「生僻度」。
频次越低，越可能是不常用的词。

输出：tools/report-rare.txt
"""
import re
import sys
from collections import Counter
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent

ZDIC = TOOLS / 'phrase-zdic-cibs.txt'
WL = ROOT / 'wordlist-zh-cg.txt'
REPORT = TOOLS / 'report-rare.txt'

if not ZDIC.exists():
    raise SystemExit(
        f'缺少 {ZDIC.name}。\n'
        f'本脚本用汉典词条的字频作为「生僻度」的代理指标，必须要有该文件。\n'
        f'请先运行：python tools/fetch-data.py --only zdic')

char_freq = Counter()
for line in ZDIC.read_text(encoding='utf-8').splitlines():
    if not line or line.startswith('#'):
        continue
    parts = re.split(r'[:\t]', line, maxsplit=1)
    if len(parts) != 2:
        continue
    for ch in parts[0].strip():
        if '\u4e00' <= ch <= '\u9fff':
            char_freq[ch] += 1

words = [w.strip() for w in WL.read_text(encoding='utf-8').splitlines() if w.strip()]

rows = []
for w in words:
    freqs = [char_freq.get(ch, 0) for ch in w]
    rows.append((min(freqs) if freqs else 0, w, freqs))

rows.sort()

L = []
L.append('生僻词检查（以「词中最生僻的那个字」的汉典词条频次为指标）')
L.append('=' * 74)
L.append(f'词表 {len(words)} 词；字频来自汉典 {len(char_freq)} 个不同的字')
L.append('')
L.append('频次分布（每个词的 min 字频）：')
buckets = Counter()
for f, w, _ in rows:
    if f >= 5000:
        buckets['>=5000（很常用）'] += 1
    elif f >= 1000:
        buckets['1000-4999'] += 1
    elif f >= 300:
        buckets['300-999'] += 1
    elif f >= 100:
        buckets['100-299'] += 1
    elif f >= 30:
        buckets['30-99'] += 1
    else:
        buckets['<30（最可疑）'] += 1
for k in ['>=5000（很常用）', '1000-4999', '300-999', '100-299', '30-99', '<30（最可疑）']:
    L.append(f'  {k:16s} {buckets.get(k, 0):5d}  ({buckets.get(k, 0) / len(words) * 100:.1f}%)')
L.append('')
L.append('--- 最生僻的 120 个词 ---')
for f, w, freqs in rows[:120]:
    L.append(f'  {w}  min频次={f}  各字频次={freqs}')

REPORT.write_bytes(('\n'.join(L) + '\n').encode('utf-8'))
sys.stdout.reconfigure(encoding='utf-8')
for k in ['>=5000（很常用）', '1000-4999', '300-999', '100-299', '30-99', '<30（最可疑）']:
    print(f'  {k:16s} {buckets.get(k, 0):5d}  ({buckets.get(k, 0) / len(words) * 100:.1f}%)')
print()
print('最生僻的 40 个词：')
for f, w, freqs in rows[:40]:
    print(f'  {w}  min={f}  {freqs}')
print(f'\nreport -> {REPORT.name}')
