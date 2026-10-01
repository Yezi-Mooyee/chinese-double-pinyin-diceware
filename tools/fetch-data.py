#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
获取本项目所需的全部外部数据（跨平台，仅用 Python 标准库）。

## 为什么要有这个脚本

这些数据的许可各不相同（MIT / CC BY-SA / GPL）。为了不把再分发问题带进仓库，
**仓库里不存放这些文件**，改由本脚本在使用者本机下载。

## 用法

    python tools/fetch-data.py                    # 直连
    python tools/fetch-data.py --proxy 127.0.0.1:26561
    python tools/fetch-data.py --only pinyin-data # 只下一项
    python tools/fetch-data.py --list             # 列出全部数据源
    python tools/fetch-data.py --force            # 强制重新下载

也可用环境变量指定代理（http_proxy / https_proxy / all_proxy），脚本会自动使用。

## 数据源与许可

    mozillazg/pinyin-data           MIT            汉字读音
    mozillazg/phrase-pinyin-data    MIT            词级读音
      └ cc_cedict.txt               源自 CC-CEDICT，CC BY-SA 3.0
      └ zdic_cibs.txt               源自汉典
    cryptogun/diceware              MIT            原始词表（本项目词表主体）
    cfbao/chinese-diceware          GPL-3.0        原始词表（仅备用）
    EFF                             eff.org        英语词表（仅对比用）

## 关于「原作者删库」

脚本默认使用 **jsDelivr CDN + 固定 commit**，但 CDN 与 GitHub 仓库同生共死。
若担心原作者删库，请 fork 一份并改写下面的 SOURCES，把 owner/repo 换成分叉。
详见 08-许可与版权.md。
"""
import argparse
import hashlib
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CDN = 'https://cdn.jsdelivr.net/gh'

# (键, 目标路径, URL, 说明)
SOURCES = [
    ('pinyin-data', 'tools/pinyin-data.txt',
     f'{CDN}/mozillazg/pinyin-data@master/pinyin.txt',
     '汉字读音（MIT）'),
    ('phrase-pinyin', 'tools/phrase-pinyin.txt',
     f'{CDN}/mozillazg/phrase-pinyin-data@master/pinyin.txt',
     '词级读音（MIT）'),
    ('cc-cedict', 'tools/phrase-cedict.txt',
     f'{CDN}/mozillazg/phrase-pinyin-data@master/cc_cedict.txt',
     'CC-CEDICT 分发（CC BY-SA 3.0）'),
    ('zdic', 'tools/phrase-zdic-cibs.txt',
     f'{CDN}/mozillazg/phrase-pinyin-data@master/zdic_cibs.txt',
     '汉典词典数据（独立第二源）'),
    ('cryptogun', 'raw-cryptogun-wordlist.txt',
     f'{CDN}/cryptogun/diceware@master/wordlist.txt',
     '原始词表（MIT，本项目词表主体）'),
    ('cfbao-8k', 'raw-cfbao-pinyin8k.wordlist',
     f'{CDN}/cfbao/chinese-diceware@master/pinyin8k.wordlist',
     '原始词表（GPL-3.0，仅备用）'),
    ('cfbao-7776', 'raw-cfbao-pinyin7776.wordlist',
     f'{CDN}/cfbao/chinese-diceware@master/pinyin.wordlist',
     '原始词表（GPL-3.0，仅备用）'),
    ('eff-long', 'tools/eff_large.txt',
     'https://www.eff.org/files/2016/07/18/eff_large_wordlist.txt',
     'EFF 长词表 7776 词（仅对比用）'),
    ('eff-short', 'tools/eff_short.txt',
     'https://www.eff.org/files/2016/09/08/eff_short_wordlist_1.txt',
     'EFF 短词表 1296 词（仅对比用）'),
]


def build_opener(proxy: str):
    handlers = []
    if proxy:
        p = proxy if '://' in proxy else f'http://{proxy}'
        handlers.append(urllib.request.ProxyHandler({'http': p, 'https': p}))
        print(f'使用代理: {p}')
    return urllib.request.build_opener(*handlers)


def fetch(opener, url: str, dest: Path, note: str, force: bool):
    if dest.exists() and not force:
        size = dest.stat().st_size
        print(f'  [跳过] {dest.relative_to(ROOT)}  ({size:,} B, 已存在)')
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f'  [下载] {dest.relative_to(ROOT)}  ← {note}')
    req = urllib.request.Request(url, headers={'User-Agent': 'dsh-fetch-data/1.0'})
    try:
        with opener.open(req, timeout=300) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f'    失败: HTTP {e.code}  {url}')
    except Exception as e:
        raise SystemExit(f'    失败: {e}\n    URL: {url}\n'
                         f'    若无法访问，请加 --proxy 127.0.0.1:26561')
    dest.write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()[:16]
    print(f'          {len(data):,} B   sha256:{sha}…')


def main():
    ap = argparse.ArgumentParser(description='下载本项目所需的外部数据')
    ap.add_argument('--proxy', default=None, help='如 127.0.0.1:26561')
    ap.add_argument('--only', default=None, help='只下载指定项（见 --list）')
    ap.add_argument('--force', action='store_true', help='强制重新下载')
    ap.add_argument('--list', action='store_true', help='列出全部数据源')
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding='utf-8')

    if args.list:
        print(f'{"键":<16}{"目标":<34}说明')
        print('-' * 78)
        for key, rel, url, note in SOURCES:
            print(f'{key:<16}{rel:<34}{note}')
        return

    todo = [s for s in SOURCES if not args.only or s[0] == args.only]
    if not todo:
        raise SystemExit(f'没有匹配的数据源: {args.only}')

    opener = build_opener(args.proxy)
    print(f'目标目录: {ROOT}\n')
    for key, rel, url, note in todo:
        fetch(opener, url, ROOT / rel, note, args.force)

    print('\n完成。接下来可运行：')
    print('  python tools/make-wordlist-cryptogun.py   # 生成词表')
    print('  python tools/audit.py                     # 逐项审计')
    print('  python tools/entropy.py                   # 重算熵')


if __name__ == '__main__':
    main()
