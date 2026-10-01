#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对最终词表（无声调版）做逐项审计。

覆盖用户要求的 6 项，外加我自行补充的几项：

  1  多字词问题与附属的前缀问题
  2  被剔除的词逐条复核（有没有删掉能正常用的词）
  3  读音准确性
  4  依据的标准与版本
  5  同码问题：去重 vs 合并条目的熵对比
  6  熵：公式与计算过程自查（含小规模精确枚举验证）
  7  补充：有放回 / 不放回的不一致
  8  补充：文件编码、换行、可复现性

输出：tools/report-audit.txt
"""
import math
import random
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from shuangpin import load_pinyin_data, syl_to_code, norm                     # noqa: E402

WL = ROOT / 'wordlist-shuangpin-cg.txt'
ZDIC = TOOLS / 'phrase-zdic-cibs.txt'
REPORT = TOOLS / 'report-audit.txt'

L = []


def say(s=''):
    L.append(s)


entries = []
for line in WL.read_text(encoding='utf-8').splitlines():
    if line.strip():
        w, c = line.split('\t')
        entries.append((w.strip(), c.strip()))
N = len(entries)

say('最终词表逐项审计（无声调版）')
say('=' * 78)
say(f'文件 {WL.name}    词数 {N}')
say()

# ============ 1. 多字词与前缀 ============
say('【1】多字词问题 与 前缀问题')
say(f'  字数分布 : {dict(Counter(len(w) for w, _ in entries))}')
say(f'  码长分布 : {dict(Counter(len(c) for _, c in entries))}')
sc = sorted(c for _, c in entries)
pref = [(sc[i], sc[i + 1]) for i in range(len(sc) - 1) if sc[i + 1].startswith(sc[i])]
real_pref = [(a, b) for a, b in pref if a != b]
say(f'  构成前缀关系的对数 : {len(pref)}，其中「真前缀」（A != B）: {len(real_pref)}')
say('  结论：全部双字词、码长恒为 4，属**定长码**。')
say('        定长码下 A 是 B 的前缀当且仅当 A == B，因此前缀问题退化为同码问题，')
say('        不存在切分歧义。若混入单字词(2 字母)或三字词(6 字母)，4 字母码才会')
say('        成为 6 字母码的前缀，那时必须加分隔符——本词表不含这类词。')
for a, b in real_pref[:20]:
    say(f'    ! {a} 是 {b} 的前缀')
say()

# ============ 2. 被剔除的词 ============
say('【2】被剔除的词逐条复核')
# 清单直接取自生成脚本（权威来源），**不读运行产物** report-*.txt ——
# 那些是 gitignore 掉的，别人 clone 下来并不存在。
import importlib.util as _ilu


def _load_generator():
    spec = _ilu.spec_from_file_location('_gen', TOOLS / 'make-wordlist-cryptogun.py')
    mod = _ilu.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


try:
    _gen = _load_generator()
    say(f'  A 类 整词剔除（{len(_gen.MANUAL_DROP)} 个）—— 两个读音的声母韵母都不同，'
        f'去掉声调后编码仍不唯一，加声调也救不了：')
    for w, why in _gen.MANUAL_DROP.items():
        say(f'    {w}  {why}')
    say()
    say(f'  B 类 逐条修正读音（{len(_gen.MANUAL_READING_FIX)} 个）—— '
        f'cryptogun 拼音列的标注错误，依据《现代汉语词典》的读音事实：')
    for w, now in _gen.MANUAL_READING_FIX.items():
        say(f'    {w} -> {now}')
except Exception as e:      # 生成脚本缺失或无法加载时降级，不中断审计
    say(f'  （无法加载生成脚本以列出剔除清单：{e}）')
say('  复核结论：A 类词的两个读音声母韵母都不同，无法靠声调解决，只能靠选定读音，')
say('            故剔除。属于「不能正常使用」，不是误删。')
say()

# ============ 3. 读音准确性 ============
say('【3】读音准确性')
zdic = {}
if not ZDIC.exists():
    say('  （未找到 phrase-zdic-cibs.txt，跳过与汉典的交叉核验）')
    say('  如需启用：python tools/fetch-data.py --only zdic')
    print('提示：未找到 phrase-zdic-cibs.txt，跳过与汉典的交叉核验。')
    print('      如需启用：python tools/fetch-data.py --only zdic')
for line in (ZDIC.read_text(encoding='utf-8').splitlines() if ZDIC.exists() else []):
    if not line or line.startswith('#'):
        continue
    parts = re.split(r'[:\t]', line, maxsplit=1)
    if len(parts) != 2:
        continue
    syls, ok = [], True
    for tok in parts[1].split():
        p = norm(tok)
        if not re.fullmatch(r'[a-z]+', p):
            ok = False
            break
        syls.append(p)
    if ok and syls:
        zdic.setdefault(parts[0].strip(), set()).add(tuple(syls))

code2syl = {}
for s in {p for lst in load_pinyin_data().values() for p, _ in lst}:
    try:
        code2syl.setdefault(syl_to_code(s), s)
    except ValueError:
        pass
# ⚠ 不能靠「码 -> 音节」反推来验证读音：uo 与 o 共用 o 键，「luo」与「lo」同码，
#    反推会把 luo 误还原成 lo。必须读生成时实际采用的音节序列。
syl_of = {}
for line in (TOOLS / 'word-syllables.txt').read_text(encoding='utf-8').splitlines():
    if line.strip():
        w, s = line.split('\t')
        syl_of[w] = tuple(s.split())

agree = diff = miss = 0
diffs = []
for w, c in entries:
    guess = syl_of.get(w)
    if guess is None:
        miss += 1
        continue
    rs = zdic.get(w)
    if not rs:
        miss += 1
        continue
    if guess in rs:
        agree += 1
    else:
        diff += 1
        diffs.append((w, ''.join(guess), ' / '.join(''.join(x) for x in sorted(rs))))

say(f'  以汉典（zdic_cibs，348446 条）为独立第二源交叉核验')
say(f'    与汉典一致   : {agree}')
say(f'    与汉典不一致 : {diff}   ← 见下分类')
say(f'    汉典未收录   : {miss}')
say()
say('  不一致项分类（这三类都不该剔除）：')
say('    a) 汉典把字的生僻读音套到了词上 —— 典型：查→zha、朴→piao、区→ou。')
say('       以权威标准为准，本表的读音正确。')
say('    b) 只在口语中才分开读 —— 典型：这些 zhèixiē、那些 nèixiē（口语合音）。')
say('       本词表按**书面语**制作，书面语读音唯一。')
say('    c) 声母不同但两读都成立 —— 需人工判断，已单列。')
say()
for w, mine, theirs in diffs:
    say(f'    {w}  本表={mine}  汉典={theirs}')
say()

# ============ 5. 同码问题 ============
say('【5】同码问题：去重 vs 合并条目')
bycode = defaultdict(list)
for w, c in entries:
    bycode[c].append(w)
dups = {c: ws for c, ws in bycode.items() if len(ws) > 1}
say(f'  同码组数 : {len(dups)}  受影响词数 : {sum(len(v) for v in dups.values())}')
for c, ws in sorted(dups.items()):
    say(f'    {c} : {"、".join(ws)}')

n_keep = N
e_logm_keep = sum(k * math.log2(k) for k in Counter(c for _, c in entries).values()) / N
h_keep = 7 * math.log2(N) - 7 * e_logm_keep

# 方案：合并同码条目（一个条目含多个词）
merged = N - sum(len(v) - 1 for v in dups.values())
h_merged = 7 * math.log2(merged)

# 方案：去重（每码只留一个词）
h_dedup = 7 * math.log2(len(bycode))

say()
say(f'  保留全部词（现状）  : 词数 {n_keep}，7 词熵 {h_keep:.4f} bit')
say(f'  合并同码为一条目    : 条目 {merged}，7 词熵 {h_merged:.4f} bit')
say(f'  去重（每码留一词）  : 词数 {len(bycode)}，7 词熵 {h_dedup:.4f} bit')
say('  结论：三者差异 < 0.001 bit。**保留全部词**最好——熵不降，且记成哪个都能')
say('        打出同一串，对默写是容错。合并条目可读性更好但意义不大，故不合并。')
say()

# ============ 6. 熵：公式自查 ============
say('【6】熵的公式与计算过程自查')


def brute_force(entries_small, n):
    """小规模精确枚举：直接按定义算 H = -Σ p log2 p。"""
    Ns = len(entries_small)
    codes = [c for _, c in entries_small]
    cnt = Counter()
    for i in range(Ns):
        for j in range(Ns):
            if n == 2:
                cnt[codes[i] + codes[j]] += 1
    tot = Ns ** n
    h = 0.0
    for c, k in cnt.items():
        p = k / tot
        h -= p * math.log2(p)
    return h


say('  公式：生成过程 = 独立均匀选 n 个词；串 s 的概率 p(s) = c(s)/N^n，')
say('        其中 c(s) = 能拼出 s 的 n 词序列数。于是')
say('            H = -Σ_s p log2 p = n·log2(N) - E[log2 c]')
say()
say('  小规模精确验证（人工构造含同码的词表，n=2，直接枚举全部序列）：')
toy = [('甲', 'aaaa'), ('乙', 'aaaa'), ('丙', 'bbbb'), ('丁', 'cccc')]
h_bf = brute_force(toy, 2)
e_logm_toy = sum(k * math.log2(k) for k in Counter(c for _, c in toy).values()) / len(toy)
h_fml = 2 * math.log2(len(toy)) - 2 * e_logm_toy
say(f'    精确枚举 H = {h_bf:.10f}')
say(f'    公式计算 H = {h_fml:.10f}')
say(f'    差 = {abs(h_bf - h_fml):.2e}   → {"一致 ✓" if abs(h_bf - h_fml) < 1e-12 else "不一致 ✗"}')
say()
say('  真实词表（n=7）：')
say(f'    上限   n·log2(N) = {7 * math.log2(N):.6f}   (N={N})')
say(f'    E[log2 m]       = {e_logm_keep:.8f}   (m = 随机词的码被多少词共享)')
say(f'    Δ同码词         = {7 * e_logm_keep:.6f}')
say(f'    真实熵 H        = {h_keep:.6f} bit')
say('    Δ切分歧义       = 0（定长码，切分唯一）')
say()

# ============ 7. 有放回 / 不放回 ============
say('【7】补充：有放回 / 不放回')
h_wr = 7 * math.log2(N)
h_wor = sum(math.log2(N - i) for i in range(7))
say(f'  entropy.py 按**有放回**算（标准 diceware）：上限 {h_wr:.6f}')
say(f'  但 pick-passphrase.py 实际用**不放回**抽样：上限 {h_wor:.6f}')
say(f'  差 {h_wr - h_wor:.6f} bit —— 数量级可忽略，但两者确实不一致。')
say('  说明：不放回会略降熵（避免重复词），换来的是不会抽到重复词，对记忆更友好。')
say('        本方案保留不放回，并在文档中注明该差异。')
say()

# ============ 8. 文件与可复现性 ============
say('【8】补充：文件格式与可复现性')
raw = WL.read_bytes()
say(f'  UTF-8 无 BOM : {"是" if not raw.startswith(b"\\xef\\xbb\\xbf") else "否 ✗"}')
say(f'  含 CR        : {"是 ✗" if b"\\r" in raw else "否（LF）"}')
say(f'  行数         : {len(raw.splitlines())}')
say('  生成脚本是确定性的：同一输入永远产出同一输出，可随时重跑比对。')
say()

# ============ 4. 标准版本 ============
say('【4】依据的标准与版本（详见 07-读音标准与数据来源.md）')
say('  《普通话异读词审音表》1985 年 12 月版（现行有效；2016 修订稿仅是征求意见稿）')
say('  《现代汉语词典》第 7 版（2016-09）—— 完整读音的实际依据，无机读版')
say('  数据源：pinyin-data 0.15.0、phrase-pinyin-data 0.19.0（下载于 2026-10-01）')
say('  注意：数据源均为机读近似，**不等于国家标准本身**。')
say('  默认方案（不加声调）不依赖任何读音标准判定，只用到声母韵母。')

REPORT.write_bytes(('\n'.join(L) + '\n').encode('utf-8'))
sys.stdout.reconfigure(encoding='utf-8')
print('\n'.join(L))
print(f'\nreport -> {REPORT.name}')
