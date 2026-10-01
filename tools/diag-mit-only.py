#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
评估：能否把整个链路做成「纯 MIT」——即完全不依赖 CC-CEDICT（CC BY-SA 3.0）。

## 背景

主链路目前读了这些读音来源：
    raw-cryptogun-wordlist.txt   MIT     词 + 拼音列（读音主来源）
    phrase-cedict.txt            CC BY-SA 3.0   修正 cryptogun 的标注错误
    phrase-zdic-cibs.txt         汉典    仅交叉核验（不参与输出）
    pinyin-data.txt              MIT     音节合法性判定

若去掉 CC-CEDICT，改用同样 MIT 的 `phrase-pinyin.txt`，链路就全是 MIT，
产物的权利状态也就干净了（可依 MIT 直接复制分发）。

## 本脚本回答两个问题

1. cryptogun 的拼音列到底错在哪、错多少？
2. 只用 MIT 的 `phrase-pinyin.txt` 能修正其中多少？
"""
import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from shuangpin import norm  # noqa: E402

CG = ROOT / 'raw-cryptogun-wordlist.txt'
PH_MIT = TOOLS / 'phrase-pinyin.txt'
PH_CC = TOOLS / 'phrase-cedict.txt'


def load(path):
    d = {}
    if not path.exists():
        return d
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        parts = re.split(r'[:\t]', line, maxsplit=1)
        if len(parts) != 2:
            continue
        syls = []
        ok = True
        for tok in parts[1].split():
            p = norm(tok)
            if not re.fullmatch(r'[a-z]+', p):
                ok = False
                break
            syls.append(p)
        if ok and syls:
            d.setdefault(parts[0].strip(), set()).add(tuple(syls))
    return d


# cryptogun 词表：词 -> 自带拼音
cg = {}
for line in CG.read_text(encoding='utf-8').splitlines():
    if not line.strip():
        continue
    parts = line.split('\t')
    if len(parts) < 4:
        continue
    py = parts[2].strip()
    ex = re.sub(r'^例:\s*', '', parts[3].strip())
    first = ex.split()[0] if ex.split() else ''
    if first:
        cg[first] = py

mit = load(PH_MIT)
cc = load(PH_CC)

sys.stdout.reconfigure(encoding='utf-8')
print('读音来源对比')
print('=' * 76)
print(f'  cryptogun 词表      : {len(cg)} 词（自带拼音，MIT）')
print(f'  phrase-pinyin.txt   : {len(mit)} 条目（MIT）')
print(f'  phrase-cedict.txt   : {len(cc)} 条目（CC-CEDICT，CC BY-SA 3.0）')

# 找 cryptogun 拼音与两个源都不一致、或与其中之一不一致的词
only_mit = only_cc = both = neither = agree_all = 0
fix_by_mit, fix_by_cc_only = [], []

for w, py in cg.items():
    n = len(w)
    m = {c for c in mit.get(w, ()) if len(c) == n}
    c = {c for c in cc.get(w, ()) if len(c) == n}
    hit_m = py in {''.join(x) for x in m} if m else False
    hit_c = py in {''.join(x) for x in c} if c else False

    if hit_m and hit_c:
        agree_all += 1
    elif hit_m and not hit_c:
        only_mit += 1
    elif hit_c and not hit_m:
        only_cc += 1
    elif not m and not c:
        neither += 1
    else:
        # 两个源都不认同 cryptogun 的标注
        if m:
            fix_by_mit.append((w, py, sorted(''.join(x) for x in m)))
        elif c:
            fix_by_cc_only.append((w, py, sorted(''.join(x) for x in c)))

print()
print('cryptogun 自带拼音的比对结果：')
print(f'  与两个源都一致        : {agree_all}')
print(f'  只有 MIT 源能确认一致 : {only_mit}')
print(f'  只有 CC 源能确认一致  : {only_cc}')
print(f'  两个源都未收录        : {neither}')
print(f'  两个源都不认同（需修正）: {len(fix_by_mit)}')
print()
print('--- 需修正的词：MIT 源给出的读音 ---')
for w, py, fix in fix_by_mit[:60]:
    print(f'  {w:<6} cryptogun={py:<10} MIT源={"/".join(fix)}')
if len(fix_by_mit) > 60:
    print(f'  ...（共 {len(fix_by_mit)} 个）')

print()
print('--- 仅 CC 源（CC BY-SA）才能修正的词 ---')
for w, py, fix in fix_by_cc_only[:60]:
    print(f'  {w:<6} cryptogun={py:<10} CC源={"/".join(fix)}')
if not fix_by_cc_only:
    print('  （无）')

print()
print('结论：')
if not fix_by_cc_only:
    print('  ✓ 所有需修正的词都能由 MIT 源（phrase-pinyin.txt）覆盖，')
    print('    因此可以去掉 CC-CEDICT，把整个链路做成纯 MIT。')
else:
    print(f'  ⚠ 有 {len(fix_by_cc_only)} 个词只能靠 CC-CEDICT 修正；')
    print('    若要坚持纯 MIT，这些词需要另行处理（剔除或按 cryptogun 原样保留）。')
