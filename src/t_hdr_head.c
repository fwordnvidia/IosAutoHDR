// 运行期 headroom：Apple EDR 的可用上界「随时」会变，宿主每帧读一次并用死区过滤
//
// 依据（developer.apple.com/documentation/metal/performing-your-own-tone-mapping）：
//   1.0 对应 SDR 最大亮度；面板更亮时值可以到 1.0 以上（100 nit UI + 400 nit 面板 ⇒ 到 4.0）；
//   上界随时变化，渲染时应把值映射进 [0, 当前上界]；系统对超出的值做钳位。
// 本套守两件事：死区/钳位的判定，以及「换 head 时纸白不跳」（纸白以下逐位不动是整条链的性质）。
#include <stdio.h>
#include <math.h>
#include "nvn_hdr.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(...) do { checks++; printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-6f)

int main(void) {
    printf("运行期 headroom：死区 / 钳位 / 换 head 时纸白不动\n");

    // 1) 死区：1% 以内不动（读数抖一下不该让画面闪）
    CHECK(NEAR(s2a_hdr_head_track(2.0f, 2.01f), 2.0f), "2.00 -> 2.01 应保持 2.00，实得 %.4f", (double)s2a_hdr_head_track(2.0f, 2.01f));
    CHECK(NEAR(s2a_hdr_head_track(2.0f, 1.99f), 2.0f), "2.00 -> 1.99 应保持 2.00，实得 %.4f", (double)s2a_hdr_head_track(2.0f, 1.99f));
    CHECK(NEAR(s2a_hdr_head_track(2.0f, 2.03f), 2.03f), "2.00 -> 2.03 应采纳 2.03，实得 %.4f", (double)s2a_hdr_head_track(2.0f, 2.03f));
    OK("死区 1%%：2.00→2.01/1.99 不动、2.00→2.03 采纳");

    // 2) 非法与越界：读数 < 1 保持原值；读数 > 4 钳到 4
    CHECK(NEAR(s2a_hdr_head_track(2.0f, 0.4f), 2.0f), "读数 0.4 应被忽略，实得 %.4f", (double)s2a_hdr_head_track(2.0f, 0.4f));
    CHECK(NEAR(s2a_hdr_head_track(2.0f, 9.0f), 4.0f), "读数 9.0 应钳到 4.0，实得 %.4f", (double)s2a_hdr_head_track(2.0f, 9.0f));
    OK("非法读数（<1）忽略、超界（9.0）钳到 4.0");

    // 3) 覆盖值与 s2a_hdr_itm_params 的联动（缺省 2.0 → 覆盖 3.0 → 清回 2.0）
    s2a_hdr_head_clear();
    s2a_hdr_itm p0 = s2a_hdr_itm_params();
    CHECK(NEAR(p0.head, 2.0f), "缺省 head 应为 2.0，实得 %.4f", (double)p0.head);
    s2a_hdr_head_set(3.0f);
    s2a_hdr_itm p1 = s2a_hdr_itm_params();
    CHECK(NEAR(p1.head, 3.0f), "覆盖 3.0 后应读到 3.0，实得 %.4f", (double)p1.head);
    s2a_hdr_head_set(0.5f);   // 低于 1 的覆盖被钳到 1
    s2a_hdr_itm p2 = s2a_hdr_itm_params();
    CHECK(NEAR(p2.head, 1.0f), "覆盖 0.5 应钳到 1.0，实得 %.4f", (double)p2.head);
    s2a_hdr_head_clear();
    s2a_hdr_itm p3 = s2a_hdr_itm_params();
    CHECK(NEAR(p3.head, p0.head), "清掉覆盖应回落缺省 %.2f，实得 %.4f", (double)p0.head, (double)p3.head);
    OK("覆盖值联动：2.0 → 3.0 → 1.0（钳位）→ 回落 2.0");

    // 4) 换 head 时纸白不跳：y == knee_lo 恒为 ×1.0000，y == knee_hi 恒为 head
    const float heads[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    int paper_ok = 1, cap_ok = 1;
    for (int i = 0; i < 4; i++) {
        s2a_hdr_itm p;
        p.head = heads[i]; p.knee_lo = 0.55f; p.knee_hi = 1.0f;
        if (!NEAR(s2a_hdr_itm_gain(&p, p.knee_lo), 1.0f)) paper_ok = 0;
        if (!NEAR(s2a_hdr_itm_gain(&p, p.knee_hi), heads[i])) cap_ok = 0;
    }
    CHECK(paper_ok, "head 从 1.0 到 4.0，y == knee_lo 的增益都应恰为 1.000000");
    CHECK(cap_ok, "y == knee_hi 的增益应恰等于 head（1/2/3/4）");
    OK("换 head：纸白（y=0.55）恒 ×1.000000，顶端（y=1.0）恰为 head");

    // 5) 单调：head 变大时同一亮度的增益不降（否则拖亮滑块会让高光反而暗下去）
    {
        int mono = 1;
        const float ys[6] = {0.3f, 0.55f, 0.7f, 0.85f, 1.0f, 1.2f};
        for (int k = 0; k < 6; k++) {
            float prev = -1.0f;
            for (int i = 0; i < 4; i++) {
                s2a_hdr_itm p;
                p.head = heads[i]; p.knee_lo = 0.55f; p.knee_hi = 1.0f;
                float g = s2a_hdr_itm_gain(&p, ys[k]);
                if (g < prev - 1e-6f) mono = 0;
                prev = g;
            }
        }
        CHECK(mono, "head 1→4 时同一亮度的增益应单调不降");
        OK("单调性：6 组亮度上 head 1→2→3→4 增益都不降");
    }

    // 6) S2A_HDR_HEADROOM 是否「显式指定」：判据必须与 s2a_hdr_itm_params 的解析一致，
    //    否则宿主那条「每帧跟随屏幕 EDR 上界」的路会让 A/B 开关看起来没用（见 nvn_mtl.m hdr_head_tick）
    {
        int pin_ok = 1;
        unsetenv("S2A_HDR_HEADROOM");
        if (s2a_hdr_env_pinned() != 0) pin_ok = 0;                     // 没设 → 不钉
        setenv("S2A_HDR_HEADROOM", "", 1);
        if (s2a_hdr_env_pinned() != 0) pin_ok = 0;                     // 空串 → 不钉（按缺省 2.0 走）
        setenv("S2A_HDR_HEADROOM", "1.0", 1);
        if (s2a_hdr_env_pinned() != 1) pin_ok = 0;                     // 下界内 → 钉
        setenv("S2A_HDR_HEADROOM", "4", 1);
        if (s2a_hdr_env_pinned() != 1) pin_ok = 0;                     // 上界 → 钉
        setenv("S2A_HDR_HEADROOM", "5", 1);
        if (s2a_hdr_env_pinned() != 0) pin_ok = 0;                     // 超界 → 不钉（与 itm_params 一样忽略）
        setenv("S2A_HDR_HEADROOM", "0.5", 1);
        if (s2a_hdr_env_pinned() != 0) pin_ok = 0;                     // 低于 1 → 不钉
        setenv("S2A_HDR_HEADROOM", "abc", 1);
        if (s2a_hdr_env_pinned() != 0) pin_ok = 0;                     // 非数字 → 不钉
        unsetenv("S2A_HDR_HEADROOM");
        CHECK(pin_ok, "S2A_HDR_HEADROOM 的钉住判据与 itm_params 的解析不一致（unset/空/1.0/4/5/0.5/abc）");
        OK("钉住判据：unset、空串、5、0.5、abc 不钉；1.0 与 4 钉");
    }

    printf("%s t_hdr_head：运行期 headroom %d 项（失败 %d）\n", fails ? "✗" : "✓", checks, fails);
    return fails ? 1 : 0;
}
