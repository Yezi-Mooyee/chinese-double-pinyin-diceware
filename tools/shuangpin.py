#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
自然码双拼编码核心库：音节切分、双拼转换、声调提取。

被 entropy.py 等脚本复用。键位规则取自自然码方案（周志农，1980 年代），并已与
Rime `rime/rime-double-pinyin` 和 fcitx/libime 的 `SPMap_*_Ziranma` 两个相互
独立的实现交叉核验。本文件不含这两者的代码或文本。

对外接口：
    load_pinyin_data()            -> {汉字: [(无声调读音, 声调), ...]}
    build_syllable_set(reads)     -> 合法音节集合
    cut_word(han, pinyin, ...)    -> 音节元组 或 None
    syl_to_code(syl)              -> 2 字母双拼码
    word_tones(han, syls, reads)  -> 声调串 或 None（声调有歧义）
"""
import re
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PYDATA = ROOT / 'tools' / 'pinyin-data.txt'

# 成音节鼻音等非常规音节，不作为词的构成单位
BAD_SYL = {'n', 'ng', 'm', 'hm', 'hng', 'ê'}
# 词表对 üe 有两种写法（lve/nve 与 lue/nue），二者同码，都收作合法音节
EXTRA_SYL = {'lue', 'nue'}

# 声调组合符 -> 调号
TONE_MARKS = {'\u0304': '1', '\u0301': '2', '\u030c': '3', '\u0300': '4'}
# 所有需要剥掉的组合符（声调 + 其它修饰）
STRIP_MARKS = '\u0304\u0301\u030c\u0300\u0306\u0302'

MAX_SYL = 6

# ============================================================================
# 自然码双拼键位表（以**数据表**形式表达）
#
# 说明：声母/韵母到键位的映射是「自然码」这一公开输入方案的标准内容（1980 年代
# 方案），属于事实性信息，不是任何一家实现的独创表达。本表按该标准独立整理，
# 并与两个相互独立的实现交叉验证过（见 tools/diag-rime-algebra.py）：
#     * Rime  rime/rime-double-pinyin 的 double_pinyin.schema.yaml (GPL-3.0)
#     * Fcitx5 fcitx/libime 的 SPMap_C_Ziranma / SPMap_S_Ziranma
# 本文件不含上述项目的代码或文本。
# ============================================================================

# 声母键位：只有 zh / ch / sh 三个变键，其余一律用字母本身
INITIAL_KEY = {'zh': 'v', 'ch': 'i', 'sh': 'u'}

# 声母表（按长度降序，保证 zh/ch/sh 优先匹配）
INITIALS = ('zh', 'ch', 'sh',
            'b', 'p', 'm', 'f', 'd', 't', 'n', 'l', 'g', 'k', 'h',
            'j', 'q', 'x', 'r', 'z', 'c', 's', 'y', 'w')

# 韵母键位
FINAL_KEY = {
    # 单韵母
    'a': 'a', 'o': 'o', 'e': 'e', 'i': 'i', 'u': 'u', 'v': 'v',
    # 复韵母
    'ai': 'l', 'ei': 'z', 'ao': 'k', 'ou': 'b',
    'ia': 'w', 'ie': 'x', 'iao': 'c', 'iu': 'q',
    'ua': 'w', 'uo': 'o', 'uai': 'y', 'ui': 'v',
    've': 't', 'ue': 't',          # üe：lüe 写作 lve；jue 写作 jue，两者同码
    # 鼻韵母
    'an': 'j', 'en': 'f', 'ang': 'h', 'eng': 'g', 'er': 'r',
    'ian': 'm', 'in': 'n', 'iang': 'd', 'ing': 'y', 'iong': 's',
    'uan': 'r', 'un': 'p', 'uang': 'd', 'ong': 's',
    'van': 'r', 'vn': 'p',         # üan / ün：juan→jr、jun→jp、lüan 同码
}

# 零声母音节：整体给出，不拆声母韵母。
#   单韵母 a/o/e 双写；ai/ei/ao/ou/an/en/er 照写；ang/eng 用「首字母 + 韵母键」。
ZERO_INITIAL = {
    'a': 'aa', 'o': 'oo', 'e': 'ee',
    'ai': 'ai', 'ei': 'ei', 'ao': 'ao', 'ou': 'ou',
    'an': 'an', 'en': 'en', 'er': 'er',
    'ang': 'ah', 'eng': 'eg',
}


def syl_to_code(syl: str) -> str:
    """音节 -> 自然码双拼码（恰好 2 字母）。"""
    if syl in ZERO_INITIAL:
        return ZERO_INITIAL[syl]

    if syl[0] in 'aoe':
        # 其余 a/o/e 起头的零声母音节：本身就是两字母，照写
        if len(syl) == 2:
            return syl
        raise ValueError(f'未收录的零声母音节 {syl!r}')

    for ini in INITIALS:
        if syl.startswith(ini):
            fin = syl[len(ini):]
            key = FINAL_KEY.get(fin)
            if key is not None:
                return INITIAL_KEY.get(ini, ini) + key

    raise ValueError(f'音节 {syl!r} 无法按自然码键位表编码')


def _strip_marks(s: str) -> str:
    d = unicodedata.normalize('NFD', s)
    return unicodedata.normalize('NFC', ''.join(c for c in d if c not in STRIP_MARKS))


def norm(s: str) -> str:
    """归一化读音：剥声调、ü→v、小写。"""
    return _strip_marks(s).replace('ü', 'v').lower()


def tone_of(s: str) -> str:
    """从带声调拼音取调号；无声调符（轻声/中性）返回 '0'。"""
    d = unicodedata.normalize('NFD', s)
    ts = sorted({TONE_MARKS[c] for c in d if c in TONE_MARKS})
    return ts[0] if ts else '0'


def load_pinyin_data(path: Path = PYDATA):
    """解析 pinyin-data，返回 {汉字: [(无声调读音, 调号), ...]}。

    词表把 lve/nve 也写成 lue/nue，故为二者都生成条目。
    """
    out = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        m = re.match(r'^U\+([0-9A-Fa-f]+):\s*([^#]*)#', line)
        if not m:
            continue
        ch = chr(int(m.group(1), 16))
        lst = []
        for x in m.group(2).split(','):
            x = x.strip()
            if not x:
                continue
            plain = norm(x)
            if not re.fullmatch(r'[a-z]+', plain) or plain in BAD_SYL:
                continue
            lst.append((plain, tone_of(x)))
            mm = re.fullmatch(r'([ln])ve', plain)
            if mm:
                lst.append((mm.group(1) + 'ue', tone_of(x)))
        if lst:
            out[ch] = lst
    return out


def build_syllable_set(reads) -> set:
    s = set(EXTRA_SYL)
    for lst in reads.values():
        for plain, _ in lst:
            s.add(plain)
    return s


def segment(py: str, n: int, sylset: set):
    """把连写拼音切成恰好 n 个音节，返回全部方案。"""
    out = []
    L = len(py)

    def rec(i, acc):
        if len(acc) == n:
            if i == L:
                out.append(tuple(acc))
            return
        if i >= L:
            return
        for j in range(i + 1, min(i + MAX_SYL, L) + 1):
            s = py[i:j]
            if s in sylset:
                acc.append(s)
                rec(j, acc)
                acc.pop()

    rec(0, [])
    return out


def cut_word(han: str, pinyin: str, reads, sylset):
    """切分并校验读音一致性，返回音节元组；不唯一或无解返回 None。"""
    n = len(han)
    if not n:
        return None
    cands = segment(pinyin, n, sylset)
    if not cands:
        return None
    good = []
    for c in cands:
        ok = True
        for ch, s in zip(han, c):
            rs = reads.get(ch)
            if not rs or not any(p == s for p, _ in rs):
                ok = False
                break
        if ok:
            good.append(c)
    return good[0] if len(good) == 1 else None


def word_tones(han: str, syls, reads, strict: bool = True):
    """确定每个字的调号。

    同一个字的同韵母读音若有多个不同调号（如「为」wéi / wèi），
    字级字典无法判断它在**这个具体的词**里读哪个：

      strict=True  -> 返回 (None, True)，调用方通常据此排除该词；
      strict=False -> 取字典里的首选读音，返回 (调号串, True)，
                      由调用方标记出来供人核对。

    ⚠ 这只是**字级**字典的保守判断，会大幅高估歧义：
    在「词」的层面读音通常已经确定（「中奖」必读 zhòng、「中国」必读 zhōng）。
    密码词表需要的是**使用者自己稳定的读法**，不是词典的规范注音，
    因此生产具体密码时应使用 strict=False，并只对多候选词做提示。
    """
    ts = []
    amb = False
    for ch, s in zip(han, syls):
        cand = [t for p, t in reads.get(ch, []) if p == s]
        if not cand:
            return None, True
        if len(set(cand)) > 1:
            amb = True
        ts.append(cand[0])
    return ''.join(ts), amb
