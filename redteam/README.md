# 红队评估

`tools/pick-and-burn.c` 经过三轮评估。三轮用的是同一个威胁模型：和目标同一个
用户、同一个登录会话、Medium 完整性级别的普通进程。没有管理员权限，不用
SeDebug，不注入线程。

评估用的工具由 WMI 服务启动，和目标进程没有派生关系，不继承句柄。

## 三轮结果

| 轮次 | 报告 | 打到什么 |
|---|---|---|
| 一 | `report-redteam.md` | 三条路径全部成功：读控制台屏幕、屏幕加词表直接算出密码、读进程内存 |
| 二 | `report-round2.md` | 输入缓冲把使用者敲的字符攒到按回车才清 |
| 三 | `report-round3.md` | 链式哈希保留了历史，历史可以逐步枚举反推 |

三轮下来有两个结论没有变：

1. 候选词必须显示给使用者看，而词到码的映射写在公开词表里。屏幕上的词就是密码。
2. 使用者敲的字符必须被程序逐字符接收，而逐字符处理会在内存里留下足以唯一
   确定明文的可枚举状态。

这两条都是模型的固有性质，不是实现里写漏了哪一行。工具能做的只有缩短窗口，
做不到消除。

## 文件

- `report-redteam.md`、`report-round2.md`、`report-round3.md`：三轮报告原文
- `attacker.c`：评估用的工具

报告里出现的 28 个字母的串，是评估时随机生成的测试串，全部替换成了
`<REDACTED>`。

## 编译攻击者工具

    gcc -O2 -Wall -Wextra -std=c11 -o attacker.exe attacker.c -luser32

## 子命令

    attacker.exe find   pick-and-burn.exe     找目标的 PID
    attacker.exe spawn  <程序> "<参数>"        启动目标
    attacker.exe screen <pid>                 读控制台屏幕
    attacker.exe solve  <pid> [词表]           屏幕加词表算出密码
    attacker.exe mem    <pid> [最短长度]       扫描内存找明文
    attacker.exe crack  <pid>                 从链式哈希历史枚举反推输入
    attacker.exe peek   <pid> [秒数] [busy]   偷窥控制台输入队列
    attacker.exe keys   <pid> "<文本>"         注入按键
    attacker.exe il                            显示自己的完整性级别
    attacker.exe low    <子命令...>            降为 Low 完整性后重跑自己

最后一条是对照组。把评估者降为 Low 完整性之后，读屏和读内存都会返回
`ERROR_ACCESS_DENIED`，说明 Windows 的完整性边界本身是有效的。问题在于用户
登录之后跑起来的程序默认都是 Medium 完整性，这道边界挡不住它们。
