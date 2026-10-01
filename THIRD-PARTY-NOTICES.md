# Third-Party Notices

## License identifiers (SPDX)

| Subject | SPDX identifier |
|---|---|
| This repository's own code (`tools/`) and documentation | `MIT` |
| `wordlist-zh-cg.txt`, `wordlist-shuangpin-cg.txt` (derived wordlists) | `MIT` |
| `raw-cryptogun-wordlist.txt`, cryptogun/diceware | `MIT` |
| `tools/pinyin-data.txt`, mozillazg/pinyin-data | `MIT` |

### A note on "versions"

The MIT License **has no version numbers**, unlike, say, the GPL (v2 / v3).
What it does have is **variants**, which can be confused with each other:

| Variant | SPDX | Distinguishing feature |
|---|---|---|
| **MIT (Expat)** | `MIT` | the plain, most common text, **used throughout this repository** |
| MIT No Attribution | `MIT-0` | drops the attribution requirement |
| X11 | `X11` | adds a clause forbidding use of the author's name for promotion |
| ISC | `ISC` | functionally equivalent to MIT, shorter wording |

Every license text reproduced below and the `LICENSE` file of this repository
are the **plain Expat variant**, i.e. SPDX `MIT`. Neither the X11 clause nor the
MIT-0 waiver is present.

The bulk of this repository is either MIT-licensed or public domain.
**No GPL-licensed material is included.**

---

## 1. cryptogun/diceware, the wordlist

- Source: <https://github.com/cryptogun/diceware>
- License: **MIT**
- What it provides: the 8192-entry wordlist `raw-cryptogun-wordlist.txt`,
  and, through it, the final wordlists
  `wordlist-zh-cg.txt` and `wordlist-shuangpin-cg.txt`.
- Note: the pronunciation column of that wordlist is the **sole source of
  readings** used by this project.

```
MIT License

Copyright (c) 2017

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. mozillazg/pinyin-data, character readings

- Source: <https://github.com/mozillazg/pinyin-data>
- License: **MIT**
- What it provides: `tools/pinyin-data.txt`, used **only** to validate syllable
  segmentation (deciding whether a written pinyin string can be cut into legal
  syllables). It does not determine which reading a word takes.

```
The MIT License (MIT)

Copyright (c) 2016 mozillazg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 3. Ziranma double-pinyin layout (自然码双拼)

The key mapping implemented in `tools/shuangpin.py`, which initial or final
goes on which key, is the **Ziranma** input scheme, a publicly documented
method invented by **Zhou Zhinong (周志农)** in the 1980s.

Per the inventor's own statement (2006), the scheme is copyrighted but
**freely licensed for use**, which is how Microsoft came to adopt it.

This project expresses the mapping as a **plain data table** written from the
published scheme. It contains no code or text copied from any implementation.
The mapping was cross-checked against two independent implementations
(Rime and Fcitx5 libime) during development; neither is redistributed here.

---

## 4. Pronunciation standards

Pronunciation judgements in this project refer to public standards:

- **《普通话异读词审音表》 (1985)**, issued by the State Language Commission,
  the State Education Commission and the Ministry of Radio and Television.
  Under **Article 5 of the PRC Copyright Law**, documents of this kind
  (administrative documents of state organs) are **not subject to copyright**,
  i.e. they are in the public domain.
- **《现代汉语词典》 (7th edition, 2016)**, a commercial publication that
  **is** copyrighted. **No text from it is reproduced here.** Only individual
  readings are used, as facts; a single reading is a "mere item of factual
  information" under Article 5(2) of the same law.

The per-word reading corrections applied by this project are listed explicitly
in `tools/make-wordlist-cryptogun.py` (`MANUAL_READING_FIX`, `MANUAL_DROP`),
each with its reason, so they can be reviewed one by one.
