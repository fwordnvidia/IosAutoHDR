// 16F 画布取不到 drawable 时的回退守卫（s2a_hdr_fmt_guard_step）的单元测试。
// 目的：证明「静默永久黑屏」被换成「约一秒后自动降级到 8 位」，且不会过早误降、不会反复抖动。
#include <stdio.h>

#include "nvn_hdr.h"

static int fails, checks;

static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("  ✗ %s\n", what); }
}

// 连续喂 n 帧「取不到」，返回这 n 帧里触发降级的次数
static int feed(s2a_hdr_fmt_guard *g, int n, int got, int want16) {
    int hits = 0;
    for (int i = 0; i < n; i++) hits += s2a_hdr_fmt_guard_step(g, got, want16);
    return hits;
}

int main(void) {
    // ---- 用例 1：16F 一直拿不到 drawable ⇒ 恰好第 60 帧降级一次 ----
    {
        s2a_hdr_fmt_guard g = {0, 0, 0, 0};
        ok(feed(&g, S2A_HDR_NIL_LIMIT - 1, 0, 1) == 0, "前 59 帧不降级（起局阶段不误判）");
        ok(g.nil_run == S2A_HDR_NIL_LIMIT - 1, "记数到 59");
        ok(s2a_hdr_fmt_guard_step(&g, 0, 1) == 1, "第 60 帧触发降级");
        ok(g.latched == 1, "降级后置位");
        ok(feed(&g, 600, 0, 1) == 0, "之后不再重复触发（不抖动）");
    }

    // ---- 用例 2：中途拿到过 drawable ⇒ 计数归零，永远不降级 ----
    {
        s2a_hdr_fmt_guard g = {0, 0, 0, 0};
        int hits = 0;
        for (int f = 0; f < 600; f++) hits += s2a_hdr_fmt_guard_step(&g, (f % 30) == 0, 1);
        ok(hits == 0, "只要偶尔出帧就不降级");
        ok(g.latched == 0, "未置位");
        ok(g.nil_run < S2A_HDR_NIL_LIMIT, "计数被反复清零");
    }

    // ---- 用例 3：本来就在 8 位（want16=0）⇒ 守卫完全不动作 ----
    {
        s2a_hdr_fmt_guard g = {0, 0, 0, 0};
        ok(feed(&g, 1000, 0, 0) == 0, "8 位画布不需要守卫");
        ok(g.latched == 0, "8 位不置位");
    }

    // ---- 用例 4：已经降级后即使再想要 16F 也不回跳 ----
    {
        s2a_hdr_fmt_guard g = {0, 0, 0, 0};
        ok(feed(&g, S2A_HDR_NIL_LIMIT, 0, 1) == 1, "先降一级");
        ok(feed(&g, 120, 1, 1) == 0, "出帧了也不清 latched");
        ok(feed(&g, 120, 0, 1) == 0, "再拿不到也不二次降级");
        ok(g.latched == 1, "锁死");
    }

    // ---- 用例 5：空指针不崩 ----
    ok(s2a_hdr_fmt_guard_step(NULL, 0, 1) == 0, "NULL 安全");


    // ---- 降级后的回头路（rearm + 指数退避）--------------------------------------------
    // 动机：外接 SDR 屏 → 内建 XDR 屏、系统 HDR 开关被重新打开、分屏拖回全屏，都会让 16F
    // 重新可用。只降不升 = 「一次意外换来永久 HDR 关」。
    {
        s2a_hdr_fmt_guard g2 = {0, 0, 0, 0};
        for (int i = 0; i < S2A_HDR_NIL_LIMIT - 1; i++) s2a_hdr_fmt_guard_step(&g2, 0, 1);
        ok(s2a_hdr_fmt_guard_step(&g2, 0, 1) == 1, "第 60 帧降级一次");
        ok(g2.latched == 1, "降级后锁存");
        int fired = 0, at = -1;
        for (int i = 0; i < S2A_HDR_REARM_FRAMES + 5; i++) {
            if (s2a_hdr_fmt_guard_rearm(&g2, 1) == 1) { fired++; if (at < 0) at = i; }
        }
        ok(fired == 1, "降级后只试探一次（窗口内不重复）");
        ok(at == S2A_HDR_REARM_FRAMES, "第一次试探正好在第 600 帧（10 秒 @60Hz）");
        ok(g2.latched == 0, "试探后解除锁存，守卫重新累计 nil 帧");

        for (int i = 0; i < S2A_HDR_NIL_LIMIT - 1; i++) s2a_hdr_fmt_guard_step(&g2, 0, 1);
        ok(s2a_hdr_fmt_guard_step(&g2, 0, 1) == 1, "试探失败再次降级");
        ok(g2.step == S2A_HDR_REARM_FRAMES * 2, "失败后退避翻倍（1200 帧 = 20 秒）");
        int at2 = -1;
        for (int i = 0; i < S2A_HDR_REARM_FRAMES * 2 + 5; i++)
            if (s2a_hdr_fmt_guard_rearm(&g2, 1) == 1 && at2 < 0) at2 = i;
        ok(at2 == S2A_HDR_REARM_FRAMES * 2, "第二次试探等 1200 帧");

        for (int round = 0; round < 8; round++) {
            for (int i = 0; i < S2A_HDR_NIL_LIMIT - 1; i++) s2a_hdr_fmt_guard_step(&g2, 0, 1);
            s2a_hdr_fmt_guard_step(&g2, 0, 1);                 // 这一帧降级
            for (int i = 0; i < g2.step + 1; i++) s2a_hdr_fmt_guard_rearm(&g2, 1);   // 等满后试探（失败）
        }
        ok(g2.step == S2A_HDR_REARM_FRAMES * S2A_HDR_REARM_MAX, "退避封顶在 600×8 = 4800 帧（80 秒）");

        g2.latched = 0;                                        // 模拟 16F 已经拿到 drawable
        s2a_hdr_fmt_guard_ok(&g2);
        ok(g2.step == S2A_HDR_REARM_FRAMES && g2.wait == 0, "16F 稳定后退避复位到 10 秒");
        s2a_hdr_fmt_guard g3 = {0, 1, 300, 300};
        s2a_hdr_fmt_guard_ok(&g3);
        ok(g3.step == 300, "锁存期间 ok() 不动状态");

        s2a_hdr_fmt_guard g4 = {0, 1, 0, S2A_HDR_REARM_FRAMES};
        int any = 0;
        for (int i = 0; i < 2000; i++) if (s2a_hdr_fmt_guard_rearm(&g4, 0) == 1) any = 1;
        ok(!any, "不想要 16F 时永不试探");
        ok(s2a_hdr_fmt_guard_rearm(NULL, 1) == 0, "rearm(NULL) 安全");
    }
    printf("%s t_hdr_fmtguard：16F 画布回退守卫 %d 项 → 全过\n", fails ? "✗" : "✓", checks);
    return fails ? 1 : 0;
}
