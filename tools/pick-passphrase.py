#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从双拼词表随机选词，生成候选根密码。

随机源用 secrets 模块（操作系统 CSPRNG），选词是均匀的（randbelow 无取模偏置）。

用法：
    python tools/pick-passphrase.py                    默认 3 组、每组 7 词
    python tools/pick-passphrase.py -g 5 -n 8          5 组、每组 8 词
    python tools/pick-passphrase.py --show-codes       同时显示每词的双拼码
    python tools/pick-passphrase.py --csv              只输出机器可读表格
"""
import argparse
import secrets
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# 默认用 cryptogun 版（MIT 链路）。cfbao 版（wordlist-shuangpin-2syl.txt）是 GPL-3.0，
# 见 08-许可与版权.md，故不作默认。
DEFAULT_LIST = ROOT / 'wordlist-shuangpin-cg.txt'


def load_wordlist(path: Path):
    entries = []
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        parts = line.split('\t')
        if len(parts) >= 2:
            entries.append((parts[0], parts[1].strip()))
    if not entries:
        raise SystemExit(f'词表为空: {path}')
    return entries


def pick(entries, n, rng=secrets):
    """均匀不放回地抽 n 个词。"""
    if n > len(entries):
        raise SystemExit(f'每组词数 {n} 超过词表大小 {len(entries)}')
    chosen = []
    pool = entries[:]
    for _ in range(n):
        i = rng.randbelow(len(pool))
        chosen.append(pool.pop(i))
    return chosen


def main():
    ap = argparse.ArgumentParser(description='从双拼词表随机生成候选根密码')
    ap.add_argument('-g', '--groups', type=int, default=3, help='生成几组候选（默认 3）')
    ap.add_argument('-n', '--words', type=int, default=7, help='每组几个词（默认 7）')
    ap.add_argument('-l', '--list', type=Path, default=DEFAULT_LIST, help='词表路径')
    ap.add_argument('-p', '--pool', type=int, default=None,
                    help='只用词表**前 N 个**词。词表按词频降序排列，前 N 个即最常用的 N 个；'
                         '取 2 的幂（如 4096）可免去拒绝采样')
    ap.add_argument('--show-codes', action='store_true', help='逐词显示双拼码')
    ap.add_argument('--csv', action='store_true', help='输出 TSV，便于脚本处理')
    args = ap.parse_args()

    entries_all = load_wordlist(args.list)
    pool_note = ''
    if args.pool is not None:
        if not 2 <= args.pool <= len(entries_all):
            raise SystemExit(f'--pool 必须在 2..{len(entries_all)} 之间')
        entries = entries_all[:args.pool]
        is_pow2 = (args.pool & (args.pool - 1)) == 0
        pool_note = (f'词池        : 前 {args.pool} 词（词表按词频降序，即最常用的 '
                     f'{args.pool} 个）{"；是 2 的幂，可免拒绝采样" if is_pow2 else ""}')
    else:
        entries = entries_all

    import math
    bits_per_word = math.log2(len(entries))
    total_bits = bits_per_word * args.words

    if args.csv:
        sys.stdout.reconfigure(encoding='utf-8')
        print('group\twords\tcodes\tpassphrase')
        for g in range(1, args.groups + 1):
            ch = pick(entries, args.words)
            words = ' '.join(w for w, c in ch)
            codes = ' '.join(c for w, c in ch)
            print(f'{g}\t{words}\t{codes}\t{"".join(c for w, c in ch)}')
        return

    sys.stdout.reconfigure(encoding='utf-8')
    print(f'词表        : {args.list.name}  (共 {len(entries_all)} 词)')
    if pool_note:
        print(pool_note)
    print(f'实际词池    : {len(entries)} 词，每词 {bits_per_word:.4f} bit')
    print(f'每组        : {args.words} 词 = {total_bits:.3f} bit')
    print(f'输出长度    : {args.words * 4} 个小写字母')
    print(f'随机源      : Python secrets (操作系统 CSPRNG)，不放回抽样')
    print()
    for g in range(1, args.groups + 1):
        ch = pick(entries, args.words)
        print(f'【候选 {g}】')
        print('  词  : ' + ' '.join(w for w, c in ch))
        if args.show_codes:
            print('  码  : ' + ' '.join(c for w, c in ch))
        print('  密码: ' + ''.join(c for w, c in ch))
        print()
    print('提示：从"能编出一个具体、反常的画面"的候选里挑一组记熟；其余丢弃。')


if __name__ == '__main__':
    main()
