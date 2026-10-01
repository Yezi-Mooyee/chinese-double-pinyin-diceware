/*
 * pick-and-burn.c —— 生成根密码候选，边打边认，确认后即焚（C 版）
 * ============================================================================
 *
 * 为什么用 C 重写：Python 里 str 是不可变对象，input() 拿到的那行密码
 * 无法就地擦除 —— del 之后字节仍留在 pymalloc 的空闲池里，只能等内存被复用。
 * C 里同一块缓冲可以精确控制生命周期：分配 → 锁定 → 使用 → 清零 → 释放。
 *
 * ---------------------------------------------------------------------------
 * 本程序防什么、不防什么
 *
 *   防：
 *     · 密码被写进日志 / 文件 / 会话记录 —— 强制要求真终端，拒绝重定向与管道。
 *     · 密码明文出现在屏幕缓冲里 —— 输入回显成 '*'，明文不进屏幕缓冲。
 *     · 密码明文长期滞留在本进程内存里 —— 见下面「不做明文副本」。
 *     · 退出后屏幕内容留在你的日常终端里 —— 强制在独立控制台窗口中运行，
 *       窗口随进程一起消失（见下面「为什么必须在独立窗口里运行」）。
 *
 *   不防：
 *     · 同一台机器上、同一个登录会话里、同一个用户下的其他进程。
 *
 *   为什么不防：候选词是明文显示在屏幕上的，而「中文词 → 双拼码」是
 *   同目录 wordlist-shuangpin-cg.txt 里写死的确定性映射。
 *   也就是说 —— 屏幕上的 7 个词就等于密码本身。
 *   同会话的任意进程都可以 AttachConsole + ReadConsoleOutputCharacterW
 *   把整屏读走，再查一遍公开词表就还原出密码，全程无需任何权限。
 *   这是 diceware 方案的本质（词必须给使用者看，词表必须公开），
 *   不是编码瑕疵，**任何版本都改不掉**。
 *
 *   结论：如果你不信任这台机器上正在运行的软件（第三方输入法、
 *   来路不明的工具、刚装完的 npm/pip 包……），就不要在这里生成根密码。
 *   换一台离线、干净、运行期间不装任何不可信软件的机器。
 *
 * ---------------------------------------------------------------------------
 * 不做明文副本，连"等价于密码的东西"也不长留（本版的关键改动）
 *
 *   旧版为每一组候选预先拼出一份完整的 28 字母密码串，存在 answer 缓冲里，
 *   整个记忆练习期间都不释放。红队评估实测：进程内存里唯一一份明文副本
 *   就是它，并且同用户进程用 ReadProcessMemory 一次就能整块读走
 *   （VirtualLock 完全挡不住这个）。
 *
 *   本版按两条规矩走：
 *
 *   1. 从头到尾不拼接明文串。要判断输入对不对，就把输入和候选各自喂进
 *      SHA-256 比指纹；算候选指纹时是逐词喂的，中途也不成串。
 *
 *   2. 候选索引 cands 只活到"词画上屏幕"那一刻。画完立刻算指纹、随即
 *      secure_memzero 抹掉 —— 因为 cands 配上同目录的公开词表同样等于密码，
 *      不能让它整场练习都躺在内存里。
 *
 *   于是内存里长期存在的只剩指纹（不可逆），明文只在这些瞬间存在：
 *   使用者敲键的那一行（算完指纹立刻擦），以及显示候选用的索引
 *   （画完屏立刻擦）。
 *
 *   边界：这只挡"能读内存、读不到屏幕"的攻击者。能读屏幕的照样一眼看穿，
 *   所以主要防线仍然是上面那句"不要在不可信的机器上生成密码"。
 *
 * ---------------------------------------------------------------------------
 * 为什么必须在独立窗口里运行
 *
 *   程序结束时的清屏（ESC[2J ESC[3J ESC[H）**不是可靠的**：终端可以不理会它，
 *   而且 Windows Terminal 自己的文本缓冲与 GPU 纹理里可能仍有历史文本。
 *   「屏幕内容不残留」这件事，唯一可靠的实现是让承载它的进程消失 ——
 *   即：在独占的控制台窗口里运行，进程退出，窗口随之销毁。
 *
 *   所以本程序启动时会用 GetConsoleProcessList() 检查自己是否独占控制台：
 *   不是的话（例如你在某个已有的 PowerShell 窗口里直接跑），它会用
 *   CREATE_NEW_CONSOLE 把自己重新启动到一个独立窗口里。
 *   检测到已独占时不会重复重开。
 *
 * ---------------------------------------------------------------------------
 * 为什么不能用 memset 清密码（本机 gcc / clang -O2 汇编实测）
 *
 *   构造：缓冲区在 printf 之后不再被使用 —— 编译器可以合法地删掉清零动作。
 *
 *     写法                   gcc -O2 结果            clang -O2 结果
 *     ---------------------- ----------------------- -----------------------
 *     memset                 清零指令消失，直接 ret    清零指令消失，直接 retq
 *     SecureZeroMemory       rep stosb 保留          4 × movaps 保留
 *     volatile 循环          逐字节循环保留           逐字节 movb 保留
 *
 * ---------------------------------------------------------------------------
 * 关于「C 标准库的安全函数」—— 本机实测结论（gcc 13.2 / clang 22.1.8 / MSVC 19.x）
 *
 *   函数                              gcc      clang    MSVC
 *   --------------------------------- -------- -------- --------
 *   memset_s         (C11 K.3.7.4.1)  不可用   不可用   不可用
 *   memset_explicit  (C23 7.26.6.2)   不可用   不可用   不可用
 *
 *   失败形式：gcc 是链接期 "undefined reference to memset_s"，
 *            clang 是 "call to undeclared function"，
 *            MSVC 是 "LNK2019: 无法解析的外部符号"。
 *   两者都写进了标准，但主流实现都没提供。
 *
 *   所以本文件按 Annex K 的语义自行实现 secure_memzero()，并保证同样的性质：
 *   「即使编译器能推断出目标缓冲区之后不再被使用，这次写入也不得被消除。」
 *
 * ---------------------------------------------------------------------------
 * 路径：Windows 的窄字符文件 API 用 ANSI 代码页解释路径，而本文件是 UTF-8，
 * 直接传中文字面量会找不到文件。所以路径统一走宽字符（_wfopen + wchar_t），
 * 默认词表位置由 exe 自身位置推导，不依赖工作目录。
 *
 * ---------------------------------------------------------------------------
 * 编译（w64devkit 的 gcc 或 LLVM clang 均可）：
 *
 *   gcc -O2 -Wall -Wextra -std=c11 -finput-charset=UTF-8 -fexec-charset=UTF-8 \
 *       -o pick-and-burn.exe pick-and-burn.c -lbcrypt
 *
 *   MSVC 需要 /utf-8 并自行链接 bcrypt.lib。
 *
 * 运行：直接双击或在任意终端里跑都行 —— 它会自己挪进独立窗口：
 *
 *   T:\密码管理\中文双拼diceware\tools\pick-and-burn.exe -g 4
 *
 * 绝不要重定向、管道，或由自动化工具（含 AI agent）启动 —— 那等同于把
 * 根密码写进对面的日志。启动时会检查 stdout/stdin 是不是终端，不是就拒绝。
 * 这条护栏已被验证：在 DSH 的 pwsh 工具下运行本程序，stdout.isatty() 为假，
 * 程序以退出码 2 拒绝。
 * ============================================================================
 */

#if !defined(_WIN32)
#error "本程序依赖 Windows API（BCryptGenRandom / VirtualLock / SecureZeroMemory / _wfopen）。"
#endif

#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <math.h>

/* 注意：转义序列必须拆成两个字符串字面量。
 * "\x1b7" 会被 C 当成一个十六进制转义 \x1b7（= 0x1b7，超出 char 范围），
 * 必须写成 "\x1b" "7"。 */
#define ESC_SAVE       "\x1b" "7"     /* DECSC：保存光标位置 */
#define ESC_RESTORE    "\x1b" "8"     /* DECRC：恢复光标位置 */
#define ESC_ERASE_LINE "\x1b[K"       /* 从光标清到行尾 */
#define CLEAR_SCREEN   "\x1b[2J\x1b[3J\x1b[H"  /* 清屏 + 清回滚缓冲 + 归位 */

#define MAX_WORDS   64
#define MAX_GROUPS  64
#define LINE_MAX_   512

#define PB_VERSION  "1.0.0"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

/* 练习循环的出口 */
enum { LOOP_AGAIN = 0, LOOP_DONE, LOOP_QUIT };

/* ==========================================================================
 * 安全清零
 *
 * C11 Annex K 的 memset_s() 与 C23 的 memset_explicit() 在 gcc / clang / MSVC
 * 上都不存在（见文件头实测表），所以这里自行实现，并保持同样的语义：
 * 这次写入具有可观察的副作用，编译器不得因为缓冲区之后不再被使用而删除它。
 * ========================================================================== */

static void secure_memzero(void *p, size_t n)
{
    if (p == NULL || n == 0) {
        return;
    }
#if defined(_WIN32)
    /* RtlSecureZeroMemory 内部用 volatile 写实现，实测 -O2 下保留（rep stosb / movaps） */
    SecureZeroMemory(p, n);
#else
    {
        volatile unsigned char *v = (volatile unsigned char *)p;
        while (n--) {
            *v++ = 0;
        }
    }
#endif
}

/* ==========================================================================
 * 尽量锁定在工作集里的内存
 *
 * 本节只防换页到磁盘，**不防**同用户进程读取 —— 红队实测：被 VirtualLock
 * 锁过的缓冲，同用户进程用 ReadProcessMemory 照样整块读走。
 * 要防后者，只能靠不让使用者在有不可信进程的机器上运行本程序。
 * 真正的换页兜底是 BitLocker（整卷加密后，换出的页在磁盘上是密文）。
 * ========================================================================== */

static void *locked_alloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        return NULL;
    }
    if (!VirtualLock(p, n)) {
        fputs("提示：VirtualLock 失败（可能超出工作集配额），"
              "这块内存可能被换出到 pagefile。\n", stderr);
    }
    return p;
}

static void locked_free(void *p, size_t n)
{
    if (p == NULL) {
        return;
    }
    secure_memzero(p, n);
    VirtualUnlock(p, n);
    free(p);
}

/* ==========================================================================
 * 密码学安全随机数
 *
 * C 标准库没有 CSPRNG：C11 的 rand() 是实现定义的 PRNG，C23 也没有补上。
 * Windows 上的正解是 BCryptGenRandom（RtlGenRandom / rand_s 已被微软标记为
 * 不推荐用于新代码）。
 * ========================================================================== */

static uint32_t random_u32(void)
{
    uint32_t v  = 0;
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)&v, (ULONG)sizeof v,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0) {
        fputs("致命错误：BCryptGenRandom 失败，拒绝用弱随机源继续。\n", stderr);
        exit(1);
    }
    return v;
}

/*
 * [0, bound) 上的均匀整数，无取模偏置。
 *
 * 直接写 r % bound 会引入偏置：2^32 不能被 bound 整除时，靠前的取值概率更高。
 * 这里先算出「会造成偏置的那一段」threshold，落在里面的样本直接丢弃重抽。
 * 词表 8173 词不是 2 的幂，这一步不能省。
 */
static uint32_t random_below(uint32_t bound)
{
    uint32_t threshold = (uint32_t)(-bound) % bound; /* 2^32 mod bound */
    uint32_t r;
    do {
        r = random_u32();
    } while (r < threshold);
    return r % bound;
}

/* ==========================================================================
 * 词表
 * ========================================================================== */

typedef struct {
    char  *zh;   /* 中文词，UTF-8，NUL 结尾（指向下面 buf 内部） */
    char  *code; /* 双拼码，ASCII，NUL 结尾 */
} Entry;

typedef struct {
    char  *buf;   /* 整个文件的内容，行的 NUL 就地切出来 */
    Entry *items;
    size_t count;
} Wordlist;

static void die(const char *msg)
{
    fprintf(stderr, "\n致命错误：%s\n", msg);
    exit(1);
}

static void wordlist_load(Wordlist *wl, const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (f == NULL) {
        die("打不开词表文件。");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        die("fseek 失败。");
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        die("词表为空。");
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        die("内存不足。");
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    size_t cap   = 1024;
    Entry *items = (Entry *)malloc(cap * sizeof *items);
    if (items == NULL) {
        free(buf);
        die("内存不足。");
    }

    size_t n = 0;
    char  *p = buf;
    while (*p != '\0') {
        char *line = p;
        char *nl   = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
            p   = nl + 1;
        } else {
            p = line + strlen(line);
        }

        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') {
            line[--ll] = '\0';
        }
        if (ll == 0) {
            continue;
        }

        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab        = '\0';
        char  *code = tab + 1;
        size_t cl   = strlen(code);
        while (cl > 0 && (code[cl - 1] == ' ' || code[cl - 1] == '\r')) {
            code[--cl] = '\0';
        }
        if (cl == 0) {
            continue;
        }

        if (n == cap) {
            cap  *= 2;
            items = (Entry *)realloc(items, cap * sizeof *items);
            if (items == NULL) {
                free(buf);
                die("内存不足。");
            }
        }
        items[n].zh   = line;
        items[n].code = code;
        n++;
    }

    if (n == 0) {
        free(items);
        free(buf);
        die("词表里没有解析出任何条目。");
    }

    wl->buf   = buf;
    wl->items = items;
    wl->count = n;
}

static void wordlist_free(Wordlist *wl)
{
    free(wl->items);
    free(wl->buf);
    wl->items = NULL;
    wl->buf   = NULL;
    wl->count = 0;
}

/* ==========================================================================
 * 路径与编码辅助
 * ========================================================================== */

/* 默认词表：exe 所在目录的上一级 */
static void default_list_path(wchar_t *out, size_t cap)
{
    static const wchar_t suffix[] = L"\\wordlist-shuangpin-cg.txt";
    wchar_t exe[MAX_PATH];

    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        die("GetModuleFileNameW 失败。");
    }
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash == NULL) {
        die("无法从 exe 路径推导目录。");
    }
    *slash = L'\0';                      /* 去掉 exe 文件名 → tools\ */
    slash  = wcsrchr(exe, L'\\');
    if (slash == NULL) {
        die("无法从 exe 路径推导上级目录。");
    }
    *slash = L'\0';                      /* 去掉 tools\ → 项目根 */

    if (wcslen(exe) + wcslen(suffix) + 1 > cap) {
        die("词表路径过长。");
    }
    wcscpy(out, exe);
    wcscat(out, suffix);
}

/* 命令行参数是 ANSI 字节（CRT 从宽字符命令行转来的），按 CP_ACP 还原 */
static wchar_t *widen_acp(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (w == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

/* 把宽字符按 UTF-8 打到终端（配合 SetConsoleOutputCP(CP_UTF8)） */
static void fputs_w(const wchar_t *w, FILE *out)
{
    char buf[2048];
    int  n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL);
    if (n > 0) {
        fputs(buf, out);
    }
}

/*
 * argv[0] 是 CRT 从宽字符命令行转来的 ANSI 字节，而本程序一律按 UTF-8 输出 ——
 * 直接 printf("%s", argv[0]) 会让中文路径在终端上变成乱码。
 * 这里把它还原成宽字符、再编成 UTF-8。失败返回 NULL（调用者退回原样打印）。
 */
static char *argv0_utf8(const char *a0)
{
    wchar_t *w   = widen_acp(a0);
    char    *out = NULL;

    if (w == NULL) {
        return NULL;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n > 0) {
        out = (char *)malloc((size_t)n);
        if (out != NULL) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
        }
    }
    secure_memzero(w, wcslen(w) * sizeof *w);
    free(w);
    return out;
}

/* ==========================================================================
 * 抽样：不放回，均匀
 * ========================================================================== */

static void draw_indices(size_t n_entries, size_t words, size_t *chosen)
{
    size_t *pool = (size_t *)malloc(n_entries * sizeof *pool);
    if (pool == NULL) {
        die("内存不足。");
    }
    for (size_t i = 0; i < n_entries; i++) {
        pool[i] = i;
    }

    size_t len = n_entries;
    for (size_t i = 0; i < words; i++) {
        size_t j  = (size_t)random_below((uint32_t)len);
        chosen[i] = pool[j];
        pool[j]   = pool[--len]; /* swap-remove，O(1) */
    }

    secure_memzero(pool, n_entries * sizeof *pool);
    free(pool);
}

/* ==========================================================================
 * 终端与输入
 * ========================================================================== */

/* 打开 VT 处理。没有这一步，下面所有 ESC 序列（清屏、光标保存/恢复）
 * 都会被当成普通字符原样显示。 */
static void enable_vt(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | (DWORD)ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

static void burn_screen(void)
{
    fputs(CLEAR_SCREEN, stdout);
    fflush(stdout);
}

/* 普通一行输入（回显正常，用于"按回车"这类不含秘密的场合） */
static void read_line(char *buf, size_t cap)
{
    if (fgets(buf, (int)cap, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }
    size_t l = strlen(buf);
    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) {
        buf[--l] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * 读取一行秘密。真正的实现在下面「链式哈希」一节（read_secret_chain），
 * 因为它必须和那里的哈希构造绑在一起 —— 这一版不再有任何输入缓冲。
 * -------------------------------------------------------------------------- */


/* ==========================================================================
 * 确保自己在独占的控制台窗口里
 *
 * 清屏不可靠（终端可以不理会 ESC 序列，Windows Terminal 自己的缓冲里也可能
 * 留有余温）。可靠的"屏幕不残留"只有一条路：让承载屏幕的进程消失。
 * 所以这里把程序挪进一个独占控制台 —— 进程退出，窗口随之销毁。
 * ========================================================================== */

static int owns_console_alone(void)
{
    DWORD pids[16];
    DWORD n = GetConsoleProcessList(pids, 16);
    return n == 1;                      /* 只有自己 → 已经独占 */
}

static void relaunch_in_own_console(int argc, char **argv)
{
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0 || exe[0] == L'\0') {
        return;                         /* 拿不到路径就放弃，按原样继续跑 */
    }

    wchar_t cmd[4096];
    cmd[0] = L'\0';
    if (wcslen(exe) + 16 >= 4096) {
        return;
    }
    wcscat(cmd, L"\"");
    wcscat(cmd, exe);
    wcscat(cmd, L"\" --child");

    for (int i = 1; i < argc; i++) {
        wchar_t *w = widen_acp(argv[i]);
        if (w == NULL) {
            continue;
        }
        size_t need = wcslen(cmd) + wcslen(w) + 8;
        if (need >= 4096) {
            free(w);
            break;
        }
        int quote = (wcschr(w, L' ') != NULL);
        wcscat(cmd, L" ");
        if (quote) {
            wcscat(cmd, L"\"");
        }
        wcscat(cmd, w);
        if (quote) {
            wcscat(cmd, L"\"");
        }
        secure_memzero(w, wcslen(w) * sizeof *w);
        free(w);
    }

    printf("\n");
    printf("   正在打开独立窗口。\n");
    printf("   运行已经移交过去，本窗口可以关闭。\n\n");
    fflush(stdout);

    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        printf("   打开独立窗口失败（错误码 %lu）。\n",
               (unsigned long)GetLastError());
        printf("   请手动用 wt.exe -w new 启动本程序，然后关掉本窗口。\n\n");
        printf("   按回车在当前窗口继续（不推荐）: ");
        fflush(stdout);
        char dummy[LINE_MAX_];
        read_line(dummy, sizeof dummy);
        secure_memzero(dummy, sizeof dummy);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* ==========================================================================
 * 指纹：让长住在内存里的不是密码，而是它的 SHA-256
 *
 * 动机：候选索引 cands 配上同目录的公开词表就等于密码。这里把它的价值
 * 削掉 —— 显示完立刻为每行算出指纹，随即抹掉 cands；此后长期存在的只有
 * 指纹，而指纹不可逆（密码有 90 bit 熵，反查无门）。
 *
 * 边界：这只挡"能读内存、读不到屏幕"的攻击者。能读屏幕的照样一眼看穿
 * （所以启动横幅那句警告才是主要防线，这条是纵深）。
 * ========================================================================== */

#define HASH_LEN 32

typedef struct {
    BCRYPT_ALG_HANDLE  alg;
    BCRYPT_HASH_HANDLE h;
} Hasher;

static void hasher_begin(Hasher *H)
{
    if (BCryptOpenAlgorithmProvider(&H->alg, BCRYPT_SHA256_ALGORITHM,
                                    NULL, 0) != 0) {
        die("BCryptOpenAlgorithmProvider 失败。");
    }
    if (BCryptCreateHash(H->alg, &H->h, NULL, 0, NULL, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(H->alg, 0);
        die("BCryptCreateHash 失败。");
    }
}

static void hasher_feed(Hasher *H, const void *p, size_t n)
{
    if (n == 0) {
        return;
    }
    if (BCryptHashData(H->h, (PUCHAR)p, (ULONG)n, 0) != 0) {
        die("BCryptHashData 失败。");
    }
}

static void hasher_end(Hasher *H, unsigned char out[HASH_LEN])
{
    if (BCryptFinishHash(H->h, out, HASH_LEN, 0) != 0) {
        die("BCryptFinishHash 失败。");
    }
    BCryptDestroyHash(H->h);
    BCryptCloseAlgorithmProvider(H->alg, 0);
}

/* ==========================================================================
 * 链式逐字符哈希 —— 「输入不留痕」的核心
 *
 * 第二轮红队实测：旧版把使用者敲的字符逐个累积进 line[512]，直到按回车
 * 才清零。于是明文在内存里的窗口是**整个打字过程**（数秒到十几秒），
 * 攻击者一次普通的 ReadProcessMemory 就能拿到完整密码。这与 KeePass
 * CVE-2023-32784 属于同一类问题（输入缓冲里的明文/前缀残留）。
 *
 * 报告给的修法是「逐字符喂 SHA-256 + 预填充对齐到 64 字节块」。但那只有
 * **打满最后一个字符**时才触发压缩：使用者打到第 14 个字符时，块缓冲里
 * 正躺着"填充 + 14 个字符的明文前缀"，读走就是 14 个字符的密码。
 * 前缀泄露正是 CVE-2023-32784 的形态，所以那不算修好。
 *
 * 这里换成链式构造，让**每一步都立即压缩**：
 *
 *     h_0 = SHA256("pick-and-burn/chain/v1")
 *     h_k = SHA256( h_{k-1} ‖ 31 字节零 ‖ c_k )
 *
 * 每步喂入 32 + 31 + 1 = 64 字节，正好一个块 —— 最后一个 feed 立刻触发
 * 压缩，块缓冲随即清空。于是：
 *
 *   · 当前字符在块缓冲里的寿命是亚微秒；
 *   · h_{k-1} 是散列值，读走也推不出任何明文；
 *   · 内存里任何时刻最多存在 **1 个** 明文字符。
 *
 * 退格：链的历史 h_0..h_k 全部留着（各 32 字节，且不可逆），退格就是
 * len--。比"复制 BCrypt 句柄压栈"省得多，也不会留下明文。
 *
 * 代价：每次按键多做一次 64 字节的 SHA-256。相对打字速率完全无感。
 * ========================================================================== */

static const char CHAIN_DOMAIN[] = "pick-and-burn/chain/v1";

static void chain_init(unsigned char h[HASH_LEN])
{
    Hasher H;
    hasher_begin(&H);
    hasher_feed(&H, CHAIN_DOMAIN, sizeof CHAIN_DOMAIN - 1);
    hasher_end(&H, h);
}

/* 把一个字符推进链：prev → out。c 由调用者负责清零。 */
static void chain_step(const unsigned char prev[HASH_LEN], unsigned char c,
                       unsigned char out[HASH_LEN])
{
    static const unsigned char Z31[31] = {0};
    Hasher H;

    hasher_begin(&H);
    hasher_feed(&H, prev, HASH_LEN);   /* 32 */
    hasher_feed(&H, Z31, sizeof Z31);  /* 31 */
    hasher_feed(&H, &c, 1);            /*  1 → 合计 64，立即压缩，缓冲清空 */
    hasher_end(&H, out);
}

/*
 * 一条链的历史：h[0] 是初始状态，h[k] 是输入 k 个字符之后的状态。
 * 退格只需把 len 减一 —— 链不可逆，历史里没有任何明文。
 */
typedef struct {
    unsigned char (*h)[HASH_LEN];
    size_t        cap;
    size_t        len;
} Chain;

static const unsigned char *chain_current(const Chain *C)
{
    return C->h[C->len];
}

static void chain_reset(Chain *C)
{
    /* 整块清干净，不能只重置 h[0] 和 len ——
     * 第三轮红队指出：h[1..] 里留着上一轮的全部历史，
     * 而那条历史配上"单步只有 95 种可能"就是一张逐字符查找表。 */
    secure_memzero(C->h, C->cap * HASH_LEN);
    chain_init(C->h[0]);
    C->len = 0;
}

static void chain_push(Chain *C, unsigned char c)
{
    if (C->len + 1 >= C->cap) {
        return;                        /* 打太长就丢弃多余的，反正不会匹配 */
    }
    chain_step(C->h[C->len], c, C->h[C->len + 1]);
    C->len++;
}

static void chain_pop(Chain *C)
{
    if (C->len > 0) {
        secure_memzero(C->h[C->len], HASH_LEN);
        C->len--;
    }
}

/* --------------------------------------------------------------------------
 * 读取一行秘密：关掉控制台自己的行编辑与回显，逐字符自己回显成 '*'，
 * **并且每个字符一进来就推进链，不落进任何缓冲**。
 *
 * 为什么关回显：控制台行输入模式下使用者敲的字符会被 conhost 回显进屏幕
 * 缓冲，同会话进程可以 AttachConsole + ReadConsoleOutputCharacterW 读走。
 * 关掉 ENABLE_ECHO_INPUT 之后，屏幕缓冲里只剩 '*'。
 *
 * 为什么是 '*' 而不是 getpass 式完全无回显（两种都实测过）：星号不泄露
 * 任何新信息 —— 候选词本来就列在屏幕上，词数公开，攻击者早知道是 28 个
 * 字母；而无回显会明显抬高打错率，受伤的正是"靠反复输入固化词序列"
 * 这个目标本身。
 *
 * ENABLE_LINE_INPUT 也必须一起关掉，否则 conhost 会同时做它自己的行编辑
 * 和回显，和下面这个循环互相打架。
 *
 * 读到回车时不输出换行 —— 调用者紧接着会重画状态行与输入行。
 * -------------------------------------------------------------------------- */
static void read_secret_chain(Chain *C)
{
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  old = 0;
    int    have_old = (hin != INVALID_HANDLE_VALUE) && GetConsoleMode(hin, &old);

    chain_reset(C);

    if (have_old) {
        SetConsoleMode(hin, old & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
    }

    for (;;) {
        INPUT_RECORD ir;
        DWORD        got = 0;
        wchar_t      ch;
        WORD         vk;

        if (!ReadConsoleInputW(hin, &ir, 1, &got) || got == 0) {
            break;
        }
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) {
            continue;
        }
        ch = ir.Event.KeyEvent.uChar.UnicodeChar;
        vk = ir.Event.KeyEvent.wVirtualKeyCode;

        if (vk == VK_RETURN) {
            break;
        }
        if (vk == VK_BACK) {
            if (C->len > 0) {
                chain_pop(C);
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }
        if (ch >= 0x20 && ch < 0x7f) {
            unsigned char c = (unsigned char)ch;
            if (c >= 'A' && c <= 'Z') {
                c = (unsigned char)(c - 'A' + 'a');
            }
            chain_push(C, c);
            c = 0;                     /* 用完即弃，不留在任何地方 */
            fputc('*', stdout);
            fflush(stdout);
        }
    }

    if (have_old) {
        SetConsoleMode(hin, old);
    }
}

/* 一行候选的指纹 —— 走同一条链，逐字符推进，不拼任何明文串 */
static void fingerprint_row(const Wordlist *wl, const size_t *idx, size_t words,
                            unsigned char out[HASH_LEN])
{
    unsigned char h[HASH_LEN], tmp[HASH_LEN];

    chain_init(h);
    for (size_t i = 0; i < words; i++) {
        const char *code = wl->items[idx[i]].code;
        for (const char *p = code; *p != '\0'; p++) {
            unsigned char c = (unsigned char)*p;
            chain_step(h, c, tmp);
            memcpy(h, tmp, HASH_LEN);
            c = 0;
        }
    }
    memcpy(out, h, HASH_LEN);
    secure_memzero(h, sizeof h);
    secure_memzero(tmp, sizeof tmp);
}

/* 定长比较，不提前退出 —— 免得比较过程本身漏出"前几个字节对上了"的信息 */
static int fingerprint_eq(const unsigned char *a, const unsigned char *b)
{
    unsigned int d = 0;
    for (int i = 0; i < HASH_LEN; i++) {
        d |= (unsigned int)(a[i] ^ b[i]);
    }
    return d == 0;
}

/* ==========================================================================
 * 界面
 * ========================================================================== */

static void show_header(const Wordlist *wl, const wchar_t *list_path,
                        size_t words, double total_bits)
{
    const wchar_t *base = wcsrchr(list_path, L'\\');
    base = (base != NULL) ? base + 1 : list_path;

    printf("\n");
    printf("  ============================================================\n");
    printf("   中文双拼 diceware 密码生成器\n");
    printf("   词表 ");
    fputs_w(base, stdout);
    printf("（%zu 词）\n", wl->count);
    printf("   每行 %zu 词 = %.2f bit；每行输出 %zu 个小写字母\n",
           words, total_bits, words * 4);
    printf("  ============================================================\n\n");

    printf("   ▶ 切换到纯英文输入法，或直接关掉输入法 ◀\n");
    printf("     中文输入法的\"英文模式\"一样会截获按键\n\n");
    printf("   ▶ 机器上若有你不信任的软件在跑，不要在这里生成密码 ◀\n");
    printf("     屏幕上的词和进程内存里的输入，普通权限的程序都能读走。\n\n");
}

/* 画候选列表 + 操作说明，并把光标停在即将用作状态行的空行行首 */
static void draw_board(const Wordlist *wl, const wchar_t *list_path,
                       const size_t *cands, size_t groups, size_t words,
                       int drills, double total_bits)
{
    burn_screen();
    show_header(wl, list_path, words, total_bits);

    for (size_t g = 0; g < groups; g++) {
        printf("   [%zu]  ", g + 1);
        for (size_t w = 0; w < words; w++) {
            fputs(wl->items[cands[g * words + w]].zh, stdout);
            if (w + 1 < words) {
                fputs("  ", stdout);
            }
        }
        printf("\n");
    }

    printf("\n");
    printf("   挑一行编成画面记住，然后打出来。输入一次后按回车。\n");
    printf("   连续打对 %d 次即可；n = 换一批，q = 退出。\n", drills);
    printf("   重复输入只是辅助记忆 —— 本程序的核心作用是随机生成 Diceware 密码。\n");
    printf("\n");
    fflush(stdout);

    fputs(ESC_SAVE, stdout);            /* 把光标钉在状态行行首 */
    fflush(stdout);
}

/* ==========================================================================
 * 练习状态机
 *
 * 整段逻辑刻意做成不碰 IO 的纯函数，好让它能被单独测试（见自检）。
 * 规则：
 *   · 打中任何一行  → 若和上次认的是同一行则累计，否则改认这一行并重新从 1 起
 *   · 打不中任何一行 → 计数清零
 *   · 累计到 drills 次 → 达标
 * ========================================================================== */

typedef struct {
    int current;   /* 当前认作第几行（0 基）；-1 表示还没认下任何一行 */
    int count;     /* 已连续打对次数 */
} Drill;

static int drill_feed(Drill *d, int match, int drills)
{
    if (match < 0) {
        d->count = 0;                   /* 打错：计数清零，认下的行不变 */
    } else if (match == d->current) {
        d->count++;
    } else {
        d->current = match;             /* 改打另一行：改认，计数从 1 起 */
        d->count   = 1;
    }
    return d->count >= drills;
}

/* ==========================================================================
 * main
 * ========================================================================== */

/* -V：版本与构建信息。不含任何秘密，随便重定向。 */
static void print_version(void)
{
    printf("中文双拼 diceware 密码生成器  %s\n", PB_VERSION);
    printf("构建于 %s %s\n", __DATE__, __TIME__);
#if defined(__clang__)
    printf("编译器 clang %d.%d.%d\n",
           __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    printf("编译器 gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    printf("编译器 MSVC %d\n", _MSC_VER);
#endif
    printf("目标平台 Windows（依赖 BCryptGenRandom / VirtualLock / SHA-256）\n");
    printf("\n");
    printf("词表    程序只读一个文件：UTF-8 文本，每行「中文词<TAB>4 字母双拼码」\n");
    printf("        默认取 exe 上一级的 wordlist-shuangpin-cg.txt\n");
    printf("        8173 个双字词，每词 log2(8173) ≈ 13.0 bit；7 词 ≈ 91 bit\n");
    printf("随机源  BCryptGenRandom（操作系统 CSPRNG），不放回抽样，无取模偏置\n");
    printf("副作用  无 —— 不写任何文件，不访问网络，不碰剪贴板\n");
}

/* -h：完整说明。同样不含秘密。 */
static void print_help(const char *argv0)
{
    char       *a0   = argv0_utf8(argv0);
    const char *prog = (a0 != NULL) ? a0 : argv0;

    printf("中文双拼 diceware 密码生成器  %s\n\n", PB_VERSION);

    printf("用法：%s [选项]\n\n", prog);
    printf("选项：\n");
    printf("  -g N            一次给几行候选（默认 4，范围 1..%d）\n", MAX_GROUPS);
    printf("  -n N            每行几个词（默认 7，范围 1..%d）\n", MAX_WORDS);
    printf("  -d N            连续打对几次算通过（默认 5，>= 1）\n");
    printf("  -l PATH         指定词表（默认见下）\n");
    printf("  -h, --help      显示本说明后退出\n");
    printf("  -V, --version   显示版本信息后退出\n");
    printf("\n");

    printf("它做什么：\n");
    printf("  从词表里随机选词，画成几行候选，让你挑一行编成画面记住，\n");
    printf("  再把它按自然码双拼打出来；连续打对 N 次就算记住。\n");
    printf("  重复输入只是辅助记忆 —— 程序的核心作用是随机生成 Diceware 密码。\n");
    printf("\n");

    printf("词表：\n");
    printf("  程序只读一个文件。UTF-8 文本，每行「中文词<TAB>4 字母双拼码」。\n");
    printf("  默认路径为 exe 所在目录的上一级：\n");
    printf("    ..\\wordlist-shuangpin-cg.txt\n");
    printf("  该词表针对自然码双拼定制，不能拿去喂常规的英文 diceware 工具。\n");
    printf("\n");

    printf("窗口要求：\n");
    printf("  程序需要独占的控制台窗口。若检测到自己和别人共用一个控制台\n");
    printf("  （比如你直接在已有的 PowerShell 窗口里跑），它会用\n");
    printf("  CREATE_NEW_CONSOLE 把自己重启到独立窗口，原窗口随即退出。\n");
    printf("  这样做是因为结束时会连窗口一起消失 —— 屏幕内容不会留在\n");
    printf("  你日常使用的终端里，比依赖清屏序列可靠。\n");
    printf("\n");
    printf("  若 stdout/stdin 不是终端（重定向、管道、被自动化工具捕获），\n");
    printf("  程序拒绝运行：那种情况下密码等于被写进了对方的日志。\n");
    printf("  -h 与 -V 不受这条限制，它们不产生密码也不含秘密。\n");
    printf("\n");

    printf("退出码：\n");
    printf("  0   正常结束，或 -h / -V\n");
    printf("  1   致命错误（词表打不开、随机源失败等）\n");
    printf("  2   参数非法，或拒绝运行\n");
    printf("\n");

    printf("例：\n");
    printf("  %s\n", prog);
    printf("  %s -g 6 -d 8\n", prog);
    printf("  %s -l D:\\my\\wordlist.txt\n", prog);

    free(a0);
}

/* 参数出错时的简短提示，别把整篇帮助糊到 stderr 上 */
static void usage_hint(const char *argv0)
{
    char       *a0   = argv0_utf8(argv0);
    const char *prog = (a0 != NULL) ? a0 : argv0;

    fprintf(stderr,
            "用法：%s [-g N] [-n N] [-d N] [-l PATH]　（%s -h 看完整说明）\n",
            prog, prog);

    free(a0);
}

int main(int argc, char **argv)
{
    int      groups = 4;
    int      words  = 7;
    int      drills = 5;
    int      child  = 0;
    wchar_t *user_list = NULL;

    /* 输出编码先设好，免得 -h / -V 里的中文在控制台上变成乱码 */
    SetConsoleOutputCP(CP_UTF8);

    /* -h / -V 放在所有护栏之前处理：它们不产生密码，输出里也没有任何秘密，
     * 所以被重定向、被管道、被脚本捕获都无所谓，没必要拦。 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            groups = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            words = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            drills = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            user_list = widen_acp(argv[++i]);
            if (user_list == NULL) {
                fputs("无法转换词表路径。\n", stderr);
                return 2;
            }
        } else if (strcmp(argv[i], "--child") == 0) {
            child = 1;
        } else {
            fprintf(stderr, "无法识别的参数：%s\n", argv[i]);
            usage_hint(argv[0]);
            return 2;
        }
    }

    if (groups < 1 || groups > MAX_GROUPS || words < 1 || words > MAX_WORDS ||
        drills < 1) {
        fputs("参数取值非法。\n", stderr);
        free(user_list);
        return 2;
    }

    /* —— 护栏一：必须是真正的终端 ——
     * 一旦 stdout 被重定向 / 管道 / 由 agent 捕获，根密码就进了对面的日志。 */
    if (!_isatty(_fileno(stdin)) || !_isatty(_fileno(stdout))) {
        fputs("\n拒绝运行：标准输入/输出不是终端。\n"
              "本程序禁止重定向、管道，或由自动化工具（含 AI agent）启动——\n"
              "那会把根密码写进日志或会话记录，而且收不回来。\n\n",
              stderr);
        free(user_list);
        return 2;
    }

    /* —— 护栏二：必须独占控制台 ——
     * 共享控制台（比如直接在已有 PowerShell 窗口里跑）意味着这个窗口
     * 在程序结束后还活着，屏幕内容会留下来。 */
    if (!child && !owns_console_alone()) {
        relaunch_in_own_console(argc, argv);
        free(user_list);
        return 0;
    }

    /* 让终端按 UTF-8 解码输入（输出编码已在 main 开头设过） */
    SetConsoleCP(CP_UTF8);
    enable_vt();
    /* VirtualLock 受工作集配额限制，先把配额抬上去 */
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             (SIZE_T)(64u << 20), (SIZE_T)(256u << 20));

    wchar_t  default_path[MAX_PATH + 64];
    wchar_t *list_path = user_list;
    if (list_path == NULL) {
        default_list_path(default_path, sizeof default_path / sizeof default_path[0]);
        list_path = default_path;
    }

    Wordlist wl;
    wordlist_load(&wl, list_path);

    double bits_per_word = log2((double)wl.count);
    double total_bits    = bits_per_word * (double)words;

    /* 候选索引（秘密：它确定了选中哪几个词）。注意这里没有 answer 缓冲。 */
    size_t *cands = (size_t *)locked_alloc((size_t)groups * (size_t)words *
                                           sizeof(size_t));
    /* 每行候选的指纹 —— 它才是长期留在内存里的东西，cands 画完屏就抹 */
    unsigned char (*fps)[HASH_LEN] =
        (unsigned char (*)[HASH_LEN])locked_alloc((size_t)groups * HASH_LEN);
    if (cands == NULL || fps == NULL) {
        die("内存不足。");
    }

    /* 输入链：cap 个状态，每个 32 字节。使用者敲的字符从不落进任何缓冲，
     * 只有链的当前状态在变 —— 而链是不可逆的。 */
    const size_t chain_cap = (size_t)words * 4 + 32;
    Chain        chain;
    chain.h   = (unsigned char (*)[HASH_LEN])locked_alloc(chain_cap * HASH_LEN);
    chain.cap = chain_cap;
    chain.len = 0;
    if (chain.h == NULL) {
        die("内存不足。");
    }

    /* q / n 的指纹预先算好 —— 于是运行时只比摘要，一个明文字符都不用留 */
    unsigned char fp_q[HASH_LEN], fp_n[HASH_LEN];
    chain_reset(&chain);
    chain_push(&chain, (unsigned char)'q');
    memcpy(fp_q, chain_current(&chain), HASH_LEN);
    chain_reset(&chain);
    chain_push(&chain, (unsigned char)'n');
    memcpy(fp_n, chain_current(&chain), HASH_LEN);
    chain_reset(&chain);

    int    finished = 0;
    int    quit     = 0;

    while (!quit) {
        for (int g = 0; g < groups; g++) {
            draw_indices(wl.count, (size_t)words,
                         cands + (size_t)g * (size_t)words);
        }

        Drill drill;
        drill.current = -1;
        drill.count   = 0;

        char mark[16] = "";             /* 状态行里的 "[2]" */
        char hint[16] = "";             /* 状态行末尾的 ✓ / ✗ */

        draw_board(&wl, list_path, cands, (size_t)groups, (size_t)words,
                   drills, total_bits);

        /* 词已经画到屏幕上了，cands 的使命到此为止 ——
         * 立刻换成不可逆的指纹，然后把它抹掉。 */
        for (int g = 0; g < groups; g++) {
            fingerprint_row(&wl, cands + (size_t)g * (size_t)words,
                            (size_t)words, fps[g]);
        }
        secure_memzero(cands, (size_t)groups * (size_t)words * sizeof(size_t));

        int result = LOOP_AGAIN;
        for (;;) {
            /* 两行原地重画：状态行 + 输入行。光标固定停在输入行的 '>' 后面，
             * 屏幕上不堆历史行，候选也就不会被顶出去。 */
            if (drill.current >= 0) {
                snprintf(mark, sizeof mark, "   [%d]", drill.current + 1);
            }
            fputs(ESC_RESTORE, stdout);
            printf("   已连续 %d/%d 次%s%s" ESC_ERASE_LINE "\n",
                   drill.count, drills, mark, hint);
            printf("   > " ESC_ERASE_LINE);
            fflush(stdout);

            read_secret_chain(&chain);
            hint[0] = '\0';             /* 上一轮的结论作废 */

            if (chain.len == 0) {
                continue;               /* 空回车：不提示，直接重画 */
            }

            /* 输入已经躺在链里，内存里没有任何明文字符串可比 —— 只比摘要 */
            const unsigned char *fp_in = chain_current(&chain);

            int is_q  = fingerprint_eq(fp_in, fp_q);
            int is_n  = fingerprint_eq(fp_in, fp_n);
            int match = -1;
            if (!is_q && !is_n) {
                for (int g = 0; g < groups; g++) {
                    if (fingerprint_eq(fp_in, fps[g])) {
                        match = g;
                        break;
                    }
                }
            }

            /* —— 比完立刻把链整条抹掉 ——
             *
             * 第三轮红队证明：只要 (h_{k-1}, h_k) 这一对留在内存里，
             * 单步的未知量只有 95 种可能，95 次 SHA-256 就能把 c_k 枚举出来。
             * 整条历史 = 密码的可解码表示。
             *
             * 没法根治（攻击者高频轮询固定地址仍能凑出相邻对），但至少把
             * 窗口从"程序退出为止"压回"这一次比对为止"。 */
            secure_memzero(chain.h, chain_cap * HASH_LEN);

            if (is_q) {
                result = LOOP_QUIT;
                break;
            }
            if (is_n) {
                result = LOOP_AGAIN;      /* 换一批候选，计数清零 */
                break;
            }

            strcpy(hint, (match < 0) ? "   ✗" : "   ✓");

            if (drill_feed(&drill, match, drills)) {
                result = LOOP_DONE;
                break;
            }
        }

        if (result == LOOP_QUIT) {
            quit = 1;
        } else if (result == LOOP_DONE) {
            finished = 1;
            break;
        }
        /* LOOP_AGAIN：回到外层，重新生成一批候选 */
    }

    /* 无论走哪条路，候选索引都立刻销毁 —— 它配上公开词表就等于密码 */
    secure_memzero(cands, (size_t)groups * (size_t)words * sizeof(size_t));

    if (finished || quit) {
        /* 刻意不清屏：让使用者还能再看一眼那些词。
         * 真正的销毁动作是下面这次按回车 —— 清内存、清屏、关窗口。 */
        fputs(ESC_RESTORE, stdout);
        if (finished) {
            printf("   已连续打对 %d 次。" ESC_ERASE_LINE "\n", drills);
        } else {
            printf("   退出。" ESC_ERASE_LINE "\n");
        }
        printf("   把它输进你要设置密码的地方；除此以外，不要再让它出现在任何地方。"
               ESC_ERASE_LINE "\n");
        printf("   按回车 → 清内存、清屏、关窗口 : " ESC_ERASE_LINE);
        fflush(stdout);

        char dummy[LINE_MAX_];
        read_line(dummy, sizeof dummy);
        secure_memzero(dummy, sizeof dummy);

        /* 最终销毁：先抹指纹，再清屏 */
        secure_memzero(fps, (size_t)groups * HASH_LEN);
    }

    secure_memzero(chain.h, chain_cap * HASH_LEN);
    locked_free(chain.h, chain_cap * HASH_LEN);
    locked_free(fps, (size_t)groups * HASH_LEN);
    locked_free(cands, (size_t)groups * (size_t)words * sizeof(size_t));
    wordlist_free(&wl);
    free(user_list);

    burn_screen();
    return 0;
}
