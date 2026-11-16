// HUD / 孤立高光剔除（s2a_hdr_sdr_white_bulk）的单元测试。
// 目的：证明「1% 的 HUD 白字不会吃掉整帧的扩张」，同时「真实高亮内容不会被误丢」。
#include <stdio.h>

#include "nvn_hdr_auto_l1.h"

static int fails, checks;

static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("  ✗ %s\n", what); }
}

static void near(float a, float b, float eps, const char *what) {
    checks++;
    if (!(fabsf(a - b) <= eps)) { fails++; printf("  ✗ %s：%.5f 期望 %.5f（±%.5f）\n", what, a, b, eps); }
}

int main(void) {
    uint32_t hist[S2A_HDR_HIST_BINS];

    // ---- 用例 1：主簇（暗内容）+ 高位孤立 HUD 簇 ----
    // 主簇：bin 40..120 各 1000 个；HUD：**同一个 bin** 里 600 个（≈0.7% 总量）。
    // 为什么要放在单个 bin：HUD 是纯色带（同一种白），真机直方图里就落在一格里；
    // 若摊到多格，每格都不够 0.5% 分位，旧算法本来就不会被顶上去，测不出缺陷。
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    for (int i = 40; i <= 120; i++) hist[i] = 1000;
    hist[211] = 600;
    hist[0] = 50000;   // 大片纯黑（黑边），不算亮像素

    float plain = 0.0f, bulk = 0.0f;
    ok(s2a_hdr_sdr_white(hist, &plain), "旧白点算法应能出值");
    ok(s2a_hdr_sdr_white_bulk(hist, &bulk), "剔除版应能出值");
    ok(plain > 0.5f, "旧算法被 HUD 顶到高位（这是要修的缺陷）");
    ok(bulk <= 0.35f, "剔除版回到主簇顶边（不被 HUD 带走）");
    ok(bulk < plain * 0.6f, "剔除版必须显著低于旧算法");

    // ---- 用例 2：真实亮内容（簇连续到高位）不能被丢 ----
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    for (int i = 40; i <= 210; i++) hist[i] = 500;   // 连续、无空档
    float p2 = 0.0f, b2 = 0.0f;
    ok(s2a_hdr_sdr_white(hist, &p2) && s2a_hdr_sdr_white_bulk(hist, &b2), "两算法都出值");
    near(b2, p2, 1e-5f, "连续亮内容两种算法一致（不误丢）");

    // ---- 用例 3：高位簇够重（≥5%）时不该丢 ----
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    for (int i = 40; i <= 120; i++) hist[i] = 100;   // 主簇 8100
    for (int i = 200; i <= 202; i++) hist[i] = 200;  // 高位 600 ≈ 6.9% 亮像素
    float b3 = 0.0f;
    ok(s2a_hdr_sdr_white_bulk(hist, &b3), "出值");
    ok(b3 > 0.5f, "≥5% 的高位簇按真实高亮处理，保留");

    // ---- 用例 4：空档不足 3 个 bin 时算同一簇 ----
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    for (int i = 40; i <= 120; i++) hist[i] = 1000;
    hist[121] = 0; hist[122] = 0;                    // 只有 2 个空 bin
    for (int i = 123; i <= 125; i++) hist[i] = 100;  // 紧邻的小团（同簇）
    float b4 = 0.0f;
    ok(s2a_hdr_sdr_white_bulk(hist, &b4), "出值");
    // 语义：bulk 返回的是「主簇内部的 0.5% 分位」，不是簇顶。
    // 这里空档只有 2 个 bin（< gap 3）⇒ 两团算同一簇 ⇒ 不该发生剔除，
    // 因此 bulk 必须与普通白点（同样的 0.5% 分位）逐位一致。
    float plain4 = 0.0f;
    ok(s2a_hdr_sdr_white(hist, &plain4), "普通白点可测");
    near(b4, plain4, 1e-6f, "空档 <3 不剔除：bulk 与普通白点一致");

    // ---- 用例 4b：自然照片的形状（主簇占 8% 的高光，连续无空档）----
    // 真实照片（云、雪山、峡谷）里顶部是「大块连续高光」，不是 HUD 那种孤立一小撮。
    // 实测三张系统壁纸：bulk 与普通白点都必须相同（0.5278 / 0.3898 / 0.6626），
    // 否则纸白被抬高 0.3–0.8 stop，扩张被无谓削弱。
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    for (int i = 30; i <= 200; i++) hist[i] = 100;     // 平滑渐变主体
    for (int i = 201; i <= 214; i++) hist[i] = 60;     // 顶部 8% 的大块高光
    float b5 = 0.0f, p5 = 0.0f;
    ok(s2a_hdr_sdr_white_bulk(hist, &b5), "照片形状出值");
    ok(s2a_hdr_sdr_white(hist, &p5), "照片形状普通白点");
    near(b5, p5, 1e-6f, "照片形状：bulk 不改变白点（不误伤大块高光）");

    // ---- 用例 5：全黑 ----
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    float junk = -1.0f;
    ok(!s2a_hdr_sdr_white_bulk(hist, &junk), "全黑时返回 0");

    // ---- 用例 6：白点覆盖入口真的改写 knee ----
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    for (int i = 40; i <= 120; i++) hist[i] = 1000;
    s2a_hdr_auto_l1b a, b;
    ok(s2a_hdr_auto_l1b_step_white(hist, 0.0f, 0.5f, 1.0f, 0.0f, 0, &a), "自算白点");
    ok(s2a_hdr_auto_l1b_step_white(hist, 0.30f, 0.5f, 1.0f, 0.0f, 0, &b), "覆盖白点");
    near(b.white, 0.30f, 1e-6f, "覆盖值原样生效");
    ok(a.white != b.white, "两种入口得到不同白点");

    // ---- 用例 7：字幕开 / 关不能让整帧亮度抽一下 ----
    // 真实使用里最容易看见的缺陷形状：字幕条出现时白点被顶上去，消失又掉回来，
    // 整幅画面跟着一亮一暗（用户会说「闪」）。字幕只占 ~0.8% 像素、落在单一格上，
    // 正是 s2a_hdr_sdr_white_bulk 该丢掉的那种孤立高位簇。
    // 为了证明这个用例真能发现问题，同一段序列再用「不剔除的旧白点」跑一遍做对照。
    {
        float worst_bulk = 0.0f, worst_plain = 0.0f;
        int ok_all = 1;
        for (int pass = 0; pass < 2; pass++) {
            int use_bulk = (pass == 0);
            s2a_hdr_auto_l1b cur = {0, 0, 0, 0, 0, 0, 0};
            float prev_lo = 0.0f, prev_hi = 0.0f, prev_avg = 0.0f, prev_white = 0.0f;
            int have = 0;
            float worst = 0.0f;
            for (int f = 0; f < 90; f++) {
                for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
                for (int i = 40; i <= 120; i++) hist[i] = 1000;   // 场景主体（暗调）
                hist[0] = 40000;                                  // 黑边
                if (f >= 20 && f < 50) hist[214] = 900;           // 20..49 帧：0.8% 纯白字幕
                float wov = 0.0f;
                if (use_bulk) s2a_hdr_sdr_white_bulk(hist, &wov);
                else          s2a_hdr_sdr_white(hist, &wov);
                if (!s2a_hdr_auto_l1b_step_fused_hz(hist, wov, prev_lo, prev_hi, prev_avg, prev_white,
                                                   have, 60.0f, &cur)) { ok_all = 0; break; }
                if (have) {
                    float step = fabsf(cur.knee_lo - prev_lo);
                    if (step > worst) worst = step;
                }
                prev_lo = cur.knee_lo; prev_hi = cur.knee_hi;
                prev_avg = cur.avg; prev_white = cur.white; have = 1;
            }
            if (use_bulk) worst_bulk = worst; else worst_plain = worst;
        }
        ok(ok_all, "字幕序列每帧都应可用");
        ok(worst_bulk < 0.02f, "剔除版：字幕开/关时 knee_lo 不得整段抽动");
        ok(worst_plain > worst_bulk * 4.0f,
           "对照：不剔除的旧白点在这一段明显抽动（否则本用例没有判别力）");
        printf("  · 字幕开/关：剔除版最大逐帧跳变 %.6f，旧白点 %.6f\n", worst_bulk, worst_plain);
    }

    printf("%s t_hdr_hud：HUD/孤立高光剔除 %d 项 → 全过\n", fails ? "✗" : "✓", checks);
    return fails ? 1 : 0;
}
