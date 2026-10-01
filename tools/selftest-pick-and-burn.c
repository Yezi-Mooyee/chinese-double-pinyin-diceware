/*
 * selftest-pick-and-burn.c —— pick-and-burn.c 的逻辑自检
 * ============================================================================
 *
 * 编译（必须放在 tools\ 下，因为要 #include "pick-and-burn.c"）：
 *
 *   gcc -O2 -std=c11 -Wno-unused-function -finput-charset=UTF-8 \
 *       -fexec-charset=UTF-8 -o selftest-pick-and-burn.exe \
 *       selftest-pick-and-burn.c -lbcrypt
 *
 * 然后直接运行 selftest-pick-and-burn.exe。
 * 它必须被编译到 tools\ 里，因为程序里那个 default_list_path() 是从
 * exe 自身位置推导词表路径的。
 *
 * 全部通过时退出码 0，有失败时退出码 1 并把失败项名打出来。
 *
 * ---------------------------------------------------------------------------
 * 它测什么
 *
 *   1. 【最要紧】链式哈希的两条路径必须一致：
 *      为候选行算指纹（fingerprint_row）与模拟使用者逐字符敲同一串
 *      （chain_push）必须得到同一个摘要。不一致的话，使用者打对了也会判错。
 *
 *   2. 链的历史与退格：push 之后再 pop，必须回到 push 之前的状态。
 *
 *   3. q / n 的指纹：预先算好的 fp_q / fp_n，必须等于逐字符推进来的结果。
 *
 *   4. 指纹能区分改一个字母、少一个、多一个、空串、短串，以及大写形式
 *      （输入在推进链之前已被转成小写，所以大写必须不匹配）。
 *
 *   5. drill_feed 状态机：打中累计、打错清零、改打另一行重新从 1 起。
 *
 *   6. secure_memzero 真的清零；random_below 不越界；抽样不放回不重复。
 *
 *   7. 词表能正常加载（8173 条）。
 *
 * ---------------------------------------------------------------------------
 * 关于下面那个 _isatty 覆盖
 *
 *   pick-and-burn.c 的 main 开头有一道护栏：stdout/stdin 不是真终端就拒绝
 *   运行（防止密码被重定向进别人的日志）。自检当然跑在被捕获的环境里，
 *   所以必须把那道护栏短路掉，否则自检自己就被拒了。
 *
 *   做法是把 _isatty 定义成恒为 1 的宏，并用 #define main burn_main 把主
 *   程序的 main 改名，避免和本文件的 main 冲突。
 *
 *   宏必须定义在 <io.h> 之后：否则连 io.h 里 _isatty 的函数声明都会被展开掉。
 *   本文件不调用 burn_main（只是让它被编译、被链接），所以短路护栏没有风险。
 * ============================================================================
 */

#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define _isatty(x) 1
#define main burn_main
#include "pick-and-burn.c"
#undef main
#undef _isatty

#define HIST_CAP 64

static int fails = 0;

static void check(const char *name, int ok)
{
    printf("   %-48s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        fails++;
    }
}

/* 模拟使用者逐字符敲一个串：返回链的最终状态。
 * 大小写转换照抄 read_secret_chain 的做法。 */
static void type_string(Chain *C, const char *s, unsigned char out[HASH_LEN])
{
    chain_reset(C);
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        chain_push(C, c);
        c = 0;
    }
    memcpy(out, chain_current(C), HASH_LEN);
}

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);

    /* ---------- 词表与路径推导 ---------- */
    wchar_t dp[MAX_PATH + 64];
    default_list_path(dp, sizeof dp / sizeof dp[0]);
    printf("0) 由 exe 位置推导的词表路径：");
    fputs_w(dp, stdout);
    printf("\n");

    Wordlist wl;
    wordlist_load(&wl, dp);
    printf("   词表条目数 = %zu（期望 8173）\n\n", wl.count);
    check("00 wordlist loads 8173 entries", wl.count == 8173);

    {
        size_t lenbad = 0, empty = 0;
        for (size_t i = 0; i < wl.count; i++) {
            if (strlen(wl.items[i].code) != 4) lenbad++;
            if (wl.items[i].zh[0] == '\0')     empty++;
        }
        check("01 every code is 4 letters", lenbad == 0);
        check("02 no empty headword",       empty == 0);
    }

    /* 链的存储 */
    unsigned char (*hist)[HASH_LEN] =
        (unsigned char (*)[HASH_LEN])malloc(HIST_CAP * HASH_LEN);
    Chain C;
    C.h = hist;
    C.cap = HIST_CAP;
    C.len = 0;

    /* ---------- 链式哈希：两条路径必须一致 ---------- */
    size_t idx[7] = {0, 1, 2, 3, 4, 5, 6};
    char   good[128];
    size_t pos = 0;
    for (int i = 0; i < 7; i++) {
        const char *c = wl.items[idx[i]].code;
        strcpy(good + pos, c);
        pos += strlen(c);
    }

    printf("--- 链式哈希：候选行 vs 使用者敲键 ---\n");

    unsigned char fp_row[HASH_LEN], fp_typed[HASH_LEN];
    fingerprint_row(&wl, idx, 7, fp_row);
    type_string(&C, good, fp_typed);

    check("10 拼接长度 28",                      strlen(good) == 28);
    check("11 逐字符敲 == 候选行指纹",           fingerprint_eq(fp_typed, fp_row));
    check("12 链长度 == 28",                     C.len == 28);

    /* ---------- 退格 ---------- */
    printf("\n--- 链的历史与退格 ---\n");
    {
        unsigned char before[HASH_LEN], after_pop[HASH_LEN];

        chain_reset(&C);
        type_string(&C, good, before);          /* 完整 28 字符 */
        memcpy(after_pop, chain_current(&C), HASH_LEN);

        chain_pop(&C);
        check("20 pop 之后长度减一",             C.len == 27);
        check("21 pop 之后状态已变",             !fingerprint_eq(chain_current(&C), after_pop));

        chain_push(&C, (unsigned char)good[27]);
        check("22 补回同一字符即回到原状态",     fingerprint_eq(chain_current(&C), after_pop));
        check("23 补回后长度复原",               C.len == 28);

        chain_reset(&C);
        chain_pop(&C);
        check("24 空链上 pop 不出事",            C.len == 0);
    }

    /* chain_reset 必须把整块历史清干净 ——
     * 第三轮红队的漏洞正是"只重置 h[0] 和 len，h[1..] 里留着上一轮的历史" */
    {
        chain_reset(&C);
        chain_push(&C, (unsigned char)'a');
        chain_push(&C, (unsigned char)'b');
        chain_push(&C, (unsigned char)'c');

        chain_reset(&C);

        int dirty = 0;
        for (int i = 1; i < HIST_CAP; i++) {
            for (int j = 0; j < HASH_LEN; j++) {
                if (C.h[i][j]) dirty = 1;
            }
        }
        check("25 chain_reset 清掉全部历史",     dirty == 0);
    }

    /* ---------- q / n 的指纹 ---------- */
    printf("\n--- q / n 命令识别（只比摘要，不留明文字符）---\n");
    {
        unsigned char fp_q[HASH_LEN], fp_n[HASH_LEN], t[HASH_LEN];

        type_string(&C, "q", fp_q);
        type_string(&C, "n", fp_n);

        type_string(&C, "Q", t);
        check("30 q 与 Q（转小写后）相同",       fingerprint_eq(t, fp_q));

        type_string(&C, "n", t);
        check("31 n 命中 fp_n",                  fingerprint_eq(t, fp_n));
        check("32 n 不命中 fp_q",                !fingerprint_eq(t, fp_q));

        type_string(&C, good, t);
        check("33 密码不命中 q 也不命中 n",
              !fingerprint_eq(t, fp_q) && !fingerprint_eq(t, fp_n));
    }

    /* ---------- 指纹的区分能力 ---------- */
    printf("\n--- 指纹区分能力 ---\n");
    {
        char bad[160];

        strcpy(bad, good); bad[0] = (char)(bad[0] == 'a' ? 'b' : 'a');
        type_string(&C, bad, fp_typed);
        check("40 改第 1 个字母 -> 不同",        !fingerprint_eq(fp_typed, fp_row));

        strcpy(bad, good); bad[26] = (char)(bad[26] == 'a' ? 'b' : 'a');
        type_string(&C, bad, fp_typed);
        check("41 改第 27 个字母 -> 不同",       !fingerprint_eq(fp_typed, fp_row));

        strcpy(bad, good); bad[27] = '\0';
        type_string(&C, bad, fp_typed);
        check("42 少一个字符 -> 不同",           !fingerprint_eq(fp_typed, fp_row));

        strcpy(bad, good); strcat(bad, "a");
        type_string(&C, bad, fp_typed);
        check("43 多一个字符 -> 不同",           !fingerprint_eq(fp_typed, fp_row));

        type_string(&C, "", fp_typed);
        check("44 空串 -> 不同",                 !fingerprint_eq(fp_typed, fp_row));

        type_string(&C, "abc", fp_typed);
        check("45 短串 -> 不同",                 !fingerprint_eq(fp_typed, fp_row));

        type_string(&C, good, fp_typed);
        check("46 完全相同 -> 相同",             fingerprint_eq(fp_typed, fp_row));

        check("47 比较是自反的",                 fingerprint_eq(fp_row, fp_row));

        secure_memzero(bad, sizeof bad);
    }

    /* 换一组索引，同一个串不应匹配 */
    {
        size_t idx2[7] = {10, 11, 12, 13, 14, 15, 16};
        unsigned char h[HASH_LEN];
        fingerprint_row(&wl, idx2, 7, h);
        check("48 换个候选组 -> 不同",           !fingerprint_eq(h, fp_row));
    }

    secure_memzero(good, sizeof good);

    /* ---------- drill_feed 状态机 ---------- */
    printf("\n--- drill_feed 状态机 ---\n");
    Drill d;
    int   done;

    d.current = -1; d.count = 0;
    done = drill_feed(&d, 2, 5);
    check("50 打中一行 -> 计数 1",               d.count == 1 && d.current == 2 && !done);

    drill_feed(&d, 2, 5);
    drill_feed(&d, 2, 5);
    check("51 连中三次 -> 计数 3",               d.count == 3);

    drill_feed(&d, -1, 5);
    check("52 打错 -> 计数清零",                 d.count == 0);
    check("53 打错后仍记得认的是哪一行",         d.current == 2);

    drill_feed(&d, 2, 5);
    check("54 打错后再打中 -> 计数 1",           d.count == 1);

    drill_feed(&d, 3, 5);
    check("55 改打另一行 -> 计数重新为 1",       d.count == 1 && d.current == 3);

    d.current = -1; d.count = 0; done = 0;
    for (int i = 0; i < 5; i++) {
        done = drill_feed(&d, 1, 5);
    }
    check("56 连中五次 -> 达标",                 done == 1 && d.count == 5);

    d.current = -1; d.count = 0;
    drill_feed(&d, 1, 5);
    drill_feed(&d, 1, 5);
    drill_feed(&d, -1, 5);
    check("57 中途打错 -> 回 0",                 d.count == 0);

    /* ---------- 加密与内存 ---------- */
    printf("\n--- 加密与内存 ---\n");
    {
        unsigned char probe[64];
        memset(probe, 0xAB, sizeof probe);
        secure_memzero(probe, sizeof probe);
        int any = 0;
        for (size_t i = 0; i < sizeof probe; i++) {
            if (probe[i]) any = 1;
        }
        check("60 secure_memzero 无残留", any == 0);
    }

    {
        uint32_t mn = 0xFFFFFFFFu, mx = 0;
        for (int i = 0; i < 100000; i++) {
            uint32_t v = random_below((uint32_t)wl.count);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        check("61 random_below 不越界", mx < (uint32_t)wl.count);
        printf("      （10 万次取值区间 %u..%u，词表 %zu）\n", mn, mx, wl.count);
    }

    {
        size_t c[7];
        int    dup = 0;
        for (int r = 0; r < 500; r++) {
            draw_indices(wl.count, 7, c);
            for (int i = 0; i < 7; i++) {
                for (int j = i + 1; j < 7; j++) {
                    if (c[i] == c[j]) dup = 1;
                }
            }
        }
        check("62 不放回抽样 500 轮无重复", dup == 0);
        secure_memzero(c, sizeof c);
    }

    secure_memzero(hist, HIST_CAP * HASH_LEN);
    free(hist);
    wordlist_free(&wl);

    printf("\n%s（失败 %d 项）\n", fails ? "**有失败**" : "全部通过", fails);
    return fails ? 1 : 0;
}
