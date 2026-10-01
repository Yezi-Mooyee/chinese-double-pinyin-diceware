#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
诊断：多音词会不会影响**不加声调**的方案？

关键区分——两类多音词的风险完全不同：

  A. 各读音**声母韵母相同、只有声调不同**（「摆设」bǎishè / bǎishe）
     → 去掉声调后双拼码**唯一** ⇒ 不加声调的方案**零风险**。

  B. 各读音**声母韵母也不同**（「大夫」dà fū→`dafu` / dài fu→`dlfu`）
     → 连双拼码本身都不唯一 ⇒ 这才是真风险，**加声调也救不了**
       （必须靠选定一个读音来解决）。

输出：tools/report-multi-reading.txt
"""
import re
import sys
from collections import Counter
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from shuangpin import norm, tone_of, load_pinyin_data, build_syllable_set, cut_word  # noqa: E402

CEDICT = TOOLS / 'phrase-cedict.txt'
WORDLIST = ROOT / 'wordlist-zh-cg.txt'
REPORT = TOOLS / 'report-multi-reading.txt'


def read_readings():
    """{词: set((音节tuple, 调号tuple))}"""
    if not CEDICT.exists():
        raise SystemExit(
            f'缺少 {CEDICT.name}。\n'
            f'本脚本需要词级读音数据来判断多音词，必须要有该文件。\n'
            f'请先运行：python tools/fetch-data.py --only cc-cedict')
    d = {}
    for line in CEDICT.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        parts = re.split(r'[:\t]', line, maxsplit=1)
        if len(parts) != 2:
            continue
        w, r = parts[0].strip(), parts[1].strip()
        syls, tones, ok = [], [], True
        for tok in r.split():
            p = norm(tok)
            if not re.fullmatch(r'[a-z]+', p):
                ok = False
                break
            syls.append(p)
            tones.append(tone_of(tok))
        if ok and syls:
            d.setdefault(w, set()).add((tuple(syls), tuple(tones)))
    return d


def main():
    readings = read_readings()
    words = [w.strip() for w in WORDLIST.read_text(encoding='utf-8').splitlines() if w.strip()]

    a_words, b_words = [], []
    for w in words:
        rs = {c for c in readings.get(w, ()) if len(c[0]) == len(w)}
        if len(rs) <= 1:
            continue
        syl_variants = {c[0] for c in rs}
        if len(syl_variants) == 1:
            a_words.append((w, sorted(rs)))
        else:
            b_words.append((w, sorted(rs)))

    L = []
    L.append('多音词风险诊断（针对「不加声调」方案）')
    L.append('=' * 72)
    L.append(f'词表大小                            : {len(words)}')
    L.append(f'A 类 仅声调不同（不加声调无风险）  : {len(a_words)}')
    L.append(f'B 类 声母韵母也不同（真风险）      : {len(b_words)}')
    L.append('')
    L.append('--- B 类：双拼码本身不唯一，必须靠词表选定读音 ---')
    for w, rs in b_words:
        variants = '  |  '.join(
            ''.join(s for s in syls) + ' [' + ''.join(t for t in tones) + ']' for syls, tones in rs)
        L.append(f'  {w}   {variants}')
    L.append('')
    L.append(f'--- A 类：只是声调不同，去掉声调后码唯一（{len(a_words)} 个）---')
    for w, rs in a_words:
        syls = rs[0][0]
        L.append(f'  {w}   {"".join(syls)}   声调组合: '
                 + ' | '.join(''.join(t for t in tones) for _, tones in rs))
    REPORT.write_bytes(('\n'.join(L) + '\n').encode('utf-8'))

    sys.stdout.reconfigure(encoding='utf-8')
    print(f'词表 {len(words)} 词')
    print(f'A 类 仅声调不同（不加声调无风险）: {len(a_words)}')
    print(f'B 类 声母韵母也不同（真风险）    : {len(b_words)}')
    print()
    print('B 类清单：')
    for w, rs in b_words:
        print(f'  {w}: ' + ' | '.join(
            ''.join(syls) + '[' + ''.join(tones) + ']' for syls, tones in rs))
    print(f'\nreport -> {REPORT.name}')


if __name__ == '__main__':
    main()
