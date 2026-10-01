#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
计算根密码的「真实熵」——不是 n*log2(N) 那个上限。

## 一、为什么上限不等于真实熵

生成过程是「独立均匀地从词表选 n 个词，把它们的编码拼起来」。
密码串是这个过程的结果，所以密码串的分布**不是**均匀的：

  * **同码词**：两个不同的词共享同一个码（同音词，如「略为」/「略微」→ `ltwz`）。
  * **跨词边界切分歧义**：码若变长且**不加分隔符**，串的切分可能不唯一。

两者都让「不同密码串的个数」少于「词序列的个数」，从而压低熵。

## 二、公式

设序列 S 产出的串为 s，令 c(s) = 「能拼出 s 的 n 词序列个数」。
每个序列等概率 1/N^n，故串 s 的概率是 c(s)/N^n：

    H = -Σ_s (c_s/N^n)·log2(c_s/N^n)
      = n·log2(N) - (1/N^n)·Σ_s c_s·log2(c_s)
      = n·log2(N) - E[log2 c]

即 **真实熵 = 上限 − 密码串平均「歧义度」的对数**。

对随机序列令
    c0 = 沿**原始切分**的同码词数乘积（只反映同码词）
    c  = 串的全部解码方案数（含重新切分，同码词加权）
则逐样本 log2(c) = log2(c0) + log2(c/c0)，于是两类损失可以分开报告。

## 三、关于「加声调」

**加声调不增加熵。** 声调是词读音的一部分，由词唯一确定，
「词序列 → 密码串」仍是确定性映射，熵由词表大小和词数决定，与编码形态无关。

加声调唯一的熵收益是消除同音词撞码（如 lüèwéi / lüèwēi），实测约 **+0.002 bit**，
代价是输出长度从 28 字符涨到约 41。带声调词表由主链路的
`tools/make-wordlist-cryptogun.py` 生成（`wordlist-shuangpin-cg-tone.txt`），
用 `-l` 指定即可比较。

## 四、用法

    python tools/entropy.py                          # 默认算 cryptogun 版两份词表
    python tools/entropy.py -l wordlist-shuangpin-cg-tone.txt
    python tools/entropy.py -n 7 --samples 200000
    python tools/entropy.py --no-replacement         # 不放回抽样
"""
import argparse
import math
import random
import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent

def entries_from_file(path: Path):
    out = []
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        parts = line.split('\t')
        if len(parts) >= 2:
            out.append((parts[0], parts[1].strip()))
    return out, {}


# ---------------------------------------------------------------- 熵的计算

def build_code_table(entries):
    by_len = {}
    for _, c in entries:
        d = by_len.setdefault(len(c), {})
        d[c] = d.get(c, 0) + 1
    return by_len


def count_decodings(s, n, by_len, lens):
    """串 s 能被多少个「恰好 n 个码」的方案拼出（同码词按词数加权）。"""
    L = len(s)
    f = [[0] * (L + 1) for _ in range(n + 1)]
    f[0][0] = 1
    for k in range(n):
        row, nxt = f[k], f[k + 1]
        for i in range(L + 1):
            v = row[i]
            if not v:
                continue
            for l in lens:
                j = i + l
                if j > L:
                    continue
                cnt = by_len[l].get(s[i:j], 0)
                if cnt:
                    nxt[j] += v * cnt
    return f[n][L]


def mc_stats(codes, by_len, lens, n, samples, rng, replacement):
    N = len(codes)
    tot = 0.0
    reuse = collided = 0
    for _ in range(samples):
        if replacement:
            seq = [codes[rng.randrange(N)] for _ in range(n)]
        else:
            seq = rng.sample(codes, n)
        base = 1
        for x in seq:
            base *= by_len[len(x)][x]
        c = count_decodings(''.join(seq), n, by_len, lens)
        tot += math.log2(c)
        if c > base:
            reuse += 1
        if c > 1:
            collided += 1
    return {'delta': tot / samples, 'reparse_rate': reuse / samples,
            'collide_rate': collided / samples}


def analyze(entries, ns, samples, replacement, rng, label, skip=None):
    codes = [c for _, c in entries]
    N = len(entries)
    by_len = build_code_table(entries)
    lens = sorted(by_len)
    fixed = len(lens) == 1

    sum_k_logk = sum(k * math.log2(k) for m in by_len.values() for k in m.values())
    E_logm = sum_k_logk / N
    dup_groups = sum(1 for m in by_len.values() for k in m.values() if k > 1)
    dup_words = sum(k for m in by_len.values() for k in m.values() if k > 1)

    rows = []
    for n in ns:
        upper = (n * math.log2(N) if replacement
                 else sum(math.log2(N - i) for i in range(n)))
        d_same = n * E_logm
        if fixed:
            d_nosep, d_extra, how, reparse = d_same, 0.0, '精确', 0.0
        else:
            st = mc_stats(codes, by_len, lens, n, samples, rng, replacement)
            d_nosep, d_extra = st['delta'], st['delta'] - d_same
            how, reparse = f'MC/{samples}', st['reparse_rate']
        rows.append({'n': n, 'upper': upper, 'nosep': upper - d_nosep,
                     'sep': upper - d_same, 'd_same': d_same, 'd_extra': d_extra,
                     'how': how, 'reparse': reparse,
                     'out_len': n * lens[0] if fixed else None})

    return {'label': label, 'N': N, 'lens': lens, 'fixed': fixed,
            'E_logm': E_logm, 'dup_groups': dup_groups, 'dup_words': dup_words,
            'rows': rows, 'replacement': replacement, 'skip': skip or {}}


def render(res, out=sys.stdout):
    w = out.write
    w('=' * 100 + '\n')
    w(f'{res["label"]}\n')
    code_kind = (f'定长 {res["lens"][0]}' if res['fixed']
                 else '变长 ' + '/'.join(map(str, res['lens'])))
    w(f'词数 {res["N"]}   码长 {code_kind}   '
      f'重码 {res["dup_groups"]} 组/{res["dup_words"]} 词   '
      f'E[log2 重码数]={res["E_logm"]:.6f}   '
      f'抽样 {"有放回" if res["replacement"] else "不放回"}\n')
    if res['skip']:
        w(f'排除：非汉字 {res["skip"].get("nothanzi",0)}  '
          f'切分失败 {res["skip"].get("cut",0)}  '
          f'声调歧义 {res["skip"].get("tone",0)}\n')
    w('\n')
    w(' 词数   理论上限     无分隔符     有分隔符   Δ同码词  Δ切分歧义   输出长度\n')
    w(' ' + '-' * 84 + '\n')
    for r in res['rows']:
        L = f'{r["out_len"]} 字符' if r['out_len'] else '变长'
        w(f' {r["n"]:>3}   {r["upper"]:>9.3f}   {r["nosep"]:>9.3f}   {r["sep"]:>8.3f}'
          f'   {r["d_same"]:>7.4f}  {r["d_extra"]:>8.4f}   {L}\n')
    w('\n')


def main():
    ap = argparse.ArgumentParser(description='计算双拼根密码的真实熵')
    ap.add_argument('-l', '--list', type=Path, action='append', default=None,
                    help='双拼词表文件（不指定则用两份默认词表）')
    ap.add_argument('-n', '--words', default='4,5,6,7,8,9,10')
    ap.add_argument('--samples', type=int, default=200000)
    ap.add_argument('--no-replacement', dest='replacement', action='store_false',
                    default=True)
    ap.add_argument('--seed', type=int, default=20260214)
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding='utf-8')
    ns = [int(x) for x in args.words.split(',')]
    rng = random.Random(args.seed)
    print(f'随机种子 {args.seed}\n')


    # 默认只算 cryptogun 版（纯 MIT 链路）。cfbao 版是 GPL-3.0，
    # 要用需显式 -l 指定，见 08-许可与版权.md
    lists = args.list or [ROOT / 'wordlist-shuangpin-cg.txt']
    for p in lists:
        entries, _ = entries_from_file(p)
        rng = random.Random(args.seed)
        # 按码里有没有数字判断是否含声调（先前一律标成「无声调」，是错的）
        has_tone = any(ch.isdigit() for _, c in entries for ch in c)
        tag = '含声调' if has_tone else '无声调'
        res = analyze(entries, ns, args.samples, args.replacement, rng,
                      f'自然码双拼，{tag}  ← {p.name}')
        render(res)


if __name__ == '__main__':
    main()
