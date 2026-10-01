# Design notes (中文)

本目录是方案的完整设计记录，语言为中文。面向使用者的说明见根目录 `README.md`。

## 编辑约定

中文段落写成整段一行，不要在句中折行。Markdown 会把段落内的换行渲染成空格，英文靠空格分词，这样正好；中文不用空格分词，折行处就凭空多出一个空格。CommonMark 把软换行的处理方式留给了实现，GitHub 选了空格，写作者只能迁就。

## 索引

| 文件 | 内容 |
|---|---|
| `00-原始项目说明.md` | 项目总览：状态、约束、已排除的方案 |
| `01-方案决策记录.md` | 为什么这样设计、需求溯源 |
| `02-词表资源.md` | 两份上游词表的实测格式与质量对比 |
| `03-自然码双拼.md` | 完整键位表、定长码性质、验证样本 |
| `04-根密码策略.md` | 熵要求、词数、保管与自检 |
| `05-密码管理器背景.md` | KeePassXC / Keepass2Android 的已查证事实 |
| `06-如何生成密码.md` | 操作手册：从选词到定稿 |
| `07-读音标准与数据来源.md` | 国家标准清单与版本钉死 |
| `08-许可与版权.md` | 各来源协议、GPL/CC-BY-SA 传染性分析、发布方案 |

## 关于 cfbao 链路（GPL-3.0）

设计过程中评估过第二份上游词表：[cfbao/chinese-diceware](https://github.com/cfbao/chinese-diceware)，许可证是 GPL-3.0。最终没有采用，理由见 `02`：它的拼音构成前缀码，代价是砍掉大量常用词（26 个常用探针词命中 0 个）；而本方案用双拼定长码，本就不需要前缀码。

GPL 有传染性，所以该链路的脚本与产物都不放在本仓库里：

- 未包含的脚本：`make-zh-wordlist.py`、`make-shuangpin-wordlist.py`、`build-tone-dict.py`、`make-tone-wordlist.py`、`diag-rime-algebra.py`、`diag-keymap-crosscheck.py`
- 未包含的产物：`wordlist-zh.txt`、`wordlist-zh-2syl.txt`、`wordlist-shuangpin-2syl.txt`、`wordlist-shuangpin-all.txt`、`wordlist-shuangpin-tone-2syl.txt`、`tools/tone-dict.txt`
- 未包含的上游数据：`raw-cfbao-*.wordlist`
- 文档中关于它的讨论保留，作为设计记录与取舍依据

`tools/fetch-data.py` 仍列出这些上游的下载地址，仅供自行复核使用；仓库本身不含它们的任何内容。

同理，两份带许可争议的词级读音数据（CC-CEDICT 的 CC BY-SA 3.0、汉典）也不在仓库中，只在需要时由使用者自行下载用于交叉核验。详见 `08`。

## 与开发目录的差异

本仓库只是开发目录对外发布的一部分，保留主链路（纯 MIT）所需的一切。
文档中提到、但仓库里找不到的文件，都属于上述未发布的链路。
