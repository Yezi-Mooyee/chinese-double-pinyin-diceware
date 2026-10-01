#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
逐项核验最终词表（无声调版）：

  1. 读音准确性 —— 用**独立于生成过程**的汉典数据（zdic）交叉验证
  2. 重码       —— 同音词撞码清单
  3. 前缀问题   —— 码之间是否存在前缀关系（含「是否有超过两字的词」）
  4. 有无分隔符 —— 定长码下两者的熵差

输出：tools/report-verify.txt
"""
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from shuangpin import (load_pinyin_data, build_syllable_set, syl_to_code,   # noqa: E402
                       norm, tone_of)

WL = ROOT / 'wordlist-shuangpin-cg.txt'
# 汉典词典数据（zdic_cibs，34.8 万条，含普通词语）。
# 注意 zdic_cybs 那份是成语词典，不含普通词，不能用。
ZDIC = TOOLS / 'phrase-zdic-cibs.txt'
CEDICT = TOOLS / 'phrase-cedict.txt'
REPORT = TOOLS / 'report-verify.txt'

L = []


def say(s=''):
    L.append(s)


# ---------- 载入词表 ----------
entries = []
for line in WL.read_text(encoding='utf-8').splitlines():
    if line.strip():
        w, c = line.split('\t')
        entries.append((w.strip(), c.strip()))
N = len(entries)
say('最终词表核验报告（无声调版）')
say('=' * 74)
say(f'文件      : {WL.name}')
say(f'词数      : {N}')
say()

# ---------- 1. 字数与码长 ----------
say('【1】字数分布与码长分布')
len_dist = Counter(len(w) for w, _ in entries)
for k in sorted(len_dist):
    say(f'  {k} 字词 : {len_dist[k]}')
code_len = Counter(len(c) for _, c in entries)
for k in sorted(code_len):
    say(f'  码长 {k} : {code_len[k]}')
say(f'  → 全部为 {set(len_dist)} 字词，全部码长 {set(code_len)}')
say()

# ---------- 2. 前缀关系 ----------
say('【2】前缀问题')
codes = [c for _, c in entries]
sc = sorted(codes)
pref = []
for i in range(len(sc) - 1):
    if sc[i + 1].startswith(sc[i]):
        pref.append((sc[i], sc[i + 1]))
say(f'  码之间构成前缀关系的对数 : {len(pref)}')
if len(code_len) == 1:
    say('  说明：全部码等长（定长码）时，A 是 B 的前缀当且仅当 A == B，')
    say('        因此「前缀问题」退化为「同码问题」，不会产生切分歧义。')
    say('        若词表混有单字词(2 字母)或三字词(6 字母)，4 字母码才会成为')
    say('        6 字母码的前缀，那时必须加分隔符。本词表不含这类词。')
for a, b in pref[:10]:
    say(f'    ! {a} 是 {b} 的前缀')
say()

# ---------- 3. 重码 ----------
say('【3】重码（同音词撞码）')
bycode = defaultdict(list)
for w, c in entries:
    bycode[c].append(w)
dup = {c: ws for c, ws in bycode.items() if len(ws) > 1}
say(f'  重码组数 : {len(dup)}')
for c, ws in sorted(dup.items()):
    say(f'    {c} : {"、".join(ws)}')
say(f'  受影响词数 : {sum(len(ws) for ws in dup.values())}')
say()

# ---------- 4. 读音准确性（用汉典独立验证） ----------
say('【4】读音准确性 —— 用汉典数据独立交叉验证')


def load_source(path):
    d = {}
    if not path.exists():
        return d
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        parts = re.split(r'[:\t]', line, maxsplit=1)
        if len(parts) != 2:
            continue
        w, r = parts[0].strip(), parts[1].strip()
        syls = []
        ok = True
        for tok in r.split():
            p = norm(tok)
            if not re.fullmatch(r'[a-z]+', p):
                ok = False
                break
            syls.append(p)
        if ok and syls:
            d.setdefault(w, set()).add(tuple(syls))
    return d


zdic = load_source(ZDIC)
say(f'  汉典条目数 : {len(zdic)}')

# 反推：4 字母码 -> 2 个音节
code2syl = {}
for s in sorted({p for lst in load_pinyin_data().values() for p, _ in lst}):
    try:
        code2syl[syl_to_code(s)] = s
    except ValueError:
        pass
say(f'  可反解的双拼码数 : {len(code2syl)}')

ok = mismatch = nocover = 0
bad = []
for w, c in entries:
    syl_guess = tuple(code2syl.get(c[i:i + 2], '?') for i in range(0, len(c), 2))
    rs = zdic.get(w)
    if not rs:
        nocover += 1
        continue
    if syl_guess in rs:
        ok += 1
    else:
        mismatch += 1
        if len(bad) < 40:
            bad.append((w, ''.join(syl_guess), sorted('+'.join(x) for x in rs)))
say(f'  与汉典一致    : {ok}')
say(f'  与汉典不一致  : {mismatch}')
say(f'  汉典未收录    : {nocover}')
say('  不一致明细：')
for w, got, exp in bad:
    say(f'    {w}  本表={got}  汉典={exp}')
say()

REPORT.write_bytes(('\n'.join(L) + '\n').encode('utf-8'))
sys.stdout.reconfigure(encoding='utf-8')
print('\n'.join(L))
print(f'\nreport -> {REPORT.name}')
