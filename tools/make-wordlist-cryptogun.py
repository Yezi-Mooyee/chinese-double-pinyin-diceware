#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用 cryptogun 词表生成纯中文词表与自然码双拼词表。

## 许可（重要）

本脚本的**全部输入只有两个 MIT 文件**，因此输出也是 MIT，可直接复制分发：

    raw-cryptogun-wordlist.txt   cryptogun/diceware     MIT   词 + 拼音（读音的唯一来源）
    tools/pinyin-data.txt        mozillazg/pinyin-data  MIT   字级读音，仅用于音节切分校验

**本脚本不读取任何第三方修正文件**——连作交叉核验的都不读。
理由是：读音是**普遍事实**，cryptogun 拼音列中约 21 处标注错误直接**逐条列出**即可
（见 `MANUAL_READING_FIX` / `MANUAL_DROP`），无需引入任何外部数据源来"修正"。

需要拿第三方数据做交叉核验时，请用**独立的核验脚本**
（`tools/audit.py`、`tools/verify-wordlist.py`），它们只报告、不改产物。

## 为什么读音以 cryptogun 自带拼音列为准

实测（`tools/diag-mit-only.py`）两份词级读音数据都会把**正确**的标注改错：

    cc_cedict       「大夫」→ dà fū（士大夫义，独立成词时其实读 dàifu）
                    「便宜」→ biànyí（便宜行事，常用义其实是 piányi）
    phrase-pinyin   「古朴」→ gǔpiào、「质朴」→ zhìpiào（取了「朴」的姓音 piáo），
                    凭空造出 3 组重码

而 cryptogun 自带的拼音列质量更好，且同为 MIT。它自身约 21 处标注错误
（绝大多数是多音字，如把「部长」标成 buchang）已在 `MANUAL_READING_FIX`
中**逐条列出并注明理由**，可复核。

## 输出（全部 UTF-8 无 BOM + LF）

    wordlist-zh-cg.txt              纯中文词表，每行一个词
    wordlist-shuangpin-cg.txt       词 <TAB> 4 字母双拼码（定长）
    tools/word-syllables.txt        中间产物：实际采用的音节序列（供审计）
    tools/report-cg-build.txt       构建报告

注：不再生成带声调版本 —— 调号需要额外的词级读音数据（均带 CC BY-SA 或质量存疑），
按「声调收益仅 +0.002 bit」的评估，不值得为它牺牲链路的许可干净度。
"""
import re
import sys
from collections import Counter
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from shuangpin import (load_pinyin_data, build_syllable_set, cut_word,   # noqa: E402
                       syl_to_code)

CG = ROOT / 'raw-cryptogun-wordlist.txt'
PYDATA = TOOLS / 'pinyin-data.txt'

OUT_ZH = ROOT / 'wordlist-zh-cg.txt'
OUT_SP = ROOT / 'wordlist-shuangpin-cg.txt'
OUT_SYL = TOOLS / 'word-syllables.txt'
REPORT = TOOLS / 'report-cg-build.txt'

# ---------------------------------------------------------------------------
# cryptogun 拼音列的逐条修正。
# 判据：《现代汉语词典》第 7 版的读音（事实性内容，依《著作权法》第五条不受保护）。
# 每条都注明该字的多音情况，便于复核。
# ---------------------------------------------------------------------------
MANUAL_READING_FIX = {
    # 「长」读 zhǎng 而非 cháng
    '部长': 'buzhang', '队长': 'duizhang', '会长': 'huizhang',
    '处长': 'chuzhang', '组长': 'zuzhang', '班长': 'banzhang',
    '村长': 'cunzhang', '长大': 'zhangda',
    # 其它多音字误标
    '西藏': 'xizang',     # 藏 zàng（西藏）/ cáng（收藏）
    '成都': 'chengdu',    # 都 dū（成都）/ dōu（都是）
    '不觉': 'bujue',      # 觉 jué（觉得）/ jiào（睡觉）
    '睡着': 'shuizhao',   # 着 zháo（睡着）/ zhe（助词）/ zhuó（着陆）
    '重返': 'chongfan',   # 重 chóng（重逢）/ zhòng（重要）
    '完了': 'wanle',      # 了 le（完了）/ liǎo（了解）
    '咋办': 'zaban',      # 咋 zǎ（咋办）/ zhā
    '明朝': 'mingchao',   # 朝 cháo（朝代）/ zhāo（朝阳）
    '差事': 'chashi',     # 差 chāi（差事）/ chā（差别）
    '泄露': 'xielu',      # 露 lù（书面）/ lòu（口语）
    '调配': 'diaopei',    # 调 diào（调动）/ tiáo（调整）
    '下调': 'xiadiao',    # 同上
    '出血': 'chuxue',     # 血 xuè（书面）/ xiě（口语）
    '颜色': 'yanse',      # 色 sè（书面）/ shǎi（口语）
    '变色': 'bianse',     # 同上
    '着地': 'zhaodi',     # 着 zháo
}

# ---------------------------------------------------------------------------
# 需要**整词剔除**的词：两个读音的声母韵母都不同，去掉声调后编码仍不唯一，
# 加声调也救不了，所以直接剔除。依据《现代汉语词典》的读音事实。
# ---------------------------------------------------------------------------
MANUAL_DROP = {
    '得了': 'déle（行了）/ déliǎo（了结）',
    '大夫': 'dàfū（士大夫）/ dàifu（医生）',
    '便宜': 'biànyí（便宜行事）/ piányi（价钱低）',
    '罢了': 'bàle（助词）/ bàliǎo（算了）',
    '同行': 'tóngháng（同业）/ tóngxíng（一起走）',
    '本色': 'běnsè（本来面貌）/ běnshǎi（口语）',
    '温和': 'wēnhé（和气）/ wēnhuo（口语，使暖和）',
    '大王': 'dàwáng（君王）/ dàiwang（山大王）',
    '琢磨': 'zhuómó（雕琢）/ zuómo（思考）',
    '调配': 'diàopèi（调动分配）/ tiáopèi（调和）',
    '下调': 'xiàdiào（往下调整）/ xiàtiáo（口语）',
    '朝阳': 'cháoyáng（向着太阳）/ zhāoyáng（早晨的太阳）',
    '差事': 'chāishì（差使）/ chàshi（不合标准）',
    '调拨': 'diàobō（调动拨付）/ tiáobō（调拨）',
    # 书面语中两个读音都成立
    '随行': 'suíxíng（随行人员）/ suíháng（随行就市）',
    '朝日': 'zhāorì（早晨的太阳）/ cháorì',
    '中的': 'zhòngdì（射中靶心）/ zhōngde',
    '都会': 'dūhuì（大城市）/ dōu huì',
}


def main():
    reads = load_pinyin_data(PYDATA)
    sylset = build_syllable_set(reads)

    rows = []
    for line in CG.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        parts = line.split('\t')
        if len(parts) < 4:
            continue
        pinyin = parts[2].strip()
        ex = re.sub(r'^例:\s*', '', parts[3].strip())
        first = ex.split()[0] if ex.split() else ''
        if first:
            rows.append((pinyin, first))

    stat = Counter()
    seen = set()
    out = []
    syl_out = []
    fixed = []
    dropped = []

    for pinyin, w in rows:
        if w in seen:
            stat['dup_word'] += 1
            continue
        seen.add(w)
        n = len(w)

        if w in MANUAL_DROP:
            stat['manual_drop'] += 1
            dropped.append((w, MANUAL_DROP[w]))
            continue

        src = pinyin
        if w in MANUAL_READING_FIX:
            src = MANUAL_READING_FIX[w]
            stat['manual_fix'] += 1
            fixed.append((w, pinyin, src))
        else:
            stat['from_wordlist'] += 1

        syls = cut_word(w, src, reads, sylset)
        if syls is None:
            stat['cut_fail'] += 1
            continue

        out.append((w, ''.join(syl_to_code(s) for s in syls)))
        syl_out.append((w, syls))

    OUT_ZH.write_bytes(''.join(w + '\n' for w, _ in out).encode('utf-8'))
    OUT_SP.write_bytes(''.join(f'{w}\t{c}\n' for w, c in out).encode('utf-8'))
    OUT_SYL.write_bytes((''.join(f'{w}\t{" ".join(syls)}\n' for w, syls in syl_out))
                        .encode('utf-8'))

    codes = Counter(c for _, c in out)
    dup = {c: [w for w, cc in out if cc == c] for c, v in codes.items() if v > 1}

    L = []
    L.append('cryptogun 词表构建报告（纯 MIT 链路）')
    L.append('=' * 74)
    L.append(f'源行数            : {len(rows)}')
    L.append(f'产出词数          : {len(out)}')
    L.append('')
    L.append('读音来源统计：')
    for k, v in stat.most_common():
        L.append(f'  {k:16s} {v}')
    L.append('')
    L.append(f'--- 逐条修正的读音（{len(fixed)} 个）---')
    for w, was, now in fixed:
        L.append(f'  {w}  {was} -> {now}')
    L.append('')
    L.append(f'--- 整词剔除（{len(dropped)} 个，两个读音的声母韵母都不同）---')
    for w, why in dropped:
        L.append(f'  {w}  {why}')
    L.append('')
    L.append(f'--- 双拼码重复（{len(dup)} 组）---')
    for c, ws in sorted(dup.items()):
        L.append(f'  {c} : {"、".join(ws)}')
    REPORT.write_bytes(('\n'.join(L) + '\n').encode('utf-8'))

    sys.stdout.reconfigure(encoding='utf-8')
    print(f'产出 {len(out)} 词')
    for k, v in stat.most_common():
        print(f'  {k:16s} {v}')
    print(f'双拼码重复 {len(dup)} 组')
    for c, ws in sorted(dup.items()):
        print(f'  ! {c}: {"、".join(ws)}')
    print(f'-> {OUT_ZH.name} / {OUT_SP.name}')


if __name__ == '__main__':
    main()
