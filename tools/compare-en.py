#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
与英语 diceware 词表做对比：熵 与 输出长度。

英语词表是直接下载 EFF 官方文件后数行数得到的，不是凭印象。
"""
import math
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent


def load_eff(path, col=1):
    if not path.exists():
        return None
    words = []
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.strip():
            parts = line.split()
            if len(parts) > col:
                words.append(parts[col])
    return words


def load_cn(path):
    return [l.split('\t')[0] for l in path.read_text(encoding='utf-8').splitlines() if l.strip()]


rows = []
for name, words, sep, out_len in [
    # out_len = 每个词在密码里占的字符数：
    #   英语词表直接用单词本身（平均词长），本方案输出的是**双拼码**，每词固定 4 字母
    ('EFF long（英语）', load_eff(TOOLS / 'eff_large.txt'), ' ', None),
    ('EFF short（英语）', load_eff(TOOLS / 'eff_short.txt'), ' ', None),
    ('本方案（双拼）', load_cn(ROOT / 'wordlist-shuangpin-cg.txt'), '', 4.0),
]:
    if not words:
        continue
    n = len(words)
    avg = sum(len(w) for w in words) / n
    per_word = out_len if out_len is not None else avg
    rows.append((name, n, avg, sep, per_word))

sys.stdout.reconfigure(encoding='utf-8')
print(f'{"词表":<20}{"词数":>7}{"每词熵":>10}{"7词熵":>10}{"平均词长":>10}{"密码长度":>10}')
print('-' * 74)
for name, n, avg, sep, per_word in rows:
    out = 7 * per_word + 6 * len(sep)
    print(f'{name:<20}{n:>7}{math.log2(n):>10.3f}{7 * math.log2(n):>10.2f}{avg:>10.2f}{out:>10.1f}')
print()
print('结论：')
print('  * 熵值：本方案与 EFF long 基本持平（差 < 0.5 bit），因为它只取决于「词数」。')
print('  * 长度：本方案 28 字符，EFF long 约 55 字符 —— 约为一半。')
print('  * 所以双拼的优势在**长度**，不在熵；熵要靠词数去换。')
