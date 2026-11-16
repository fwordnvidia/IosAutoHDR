// t_hdr_auto_l1：自动 HDR 的 L1 参数层（白点 / 黑场截断 / 场景感知平滑）。
// 纯 C，不依赖 Metal；与 graphics/nvn/gfx/nvn/nvn_hdr_auto_l1.h 一一对应。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr_auto_l1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  ✗ %s\n", msg); fails++; } } while (0)

static void add(float y, uint32_t n) { (void)y; (void)n; }

static void put(uint32_t hist[S2A_HDR_HIST_BINS], float y, uint32_t n) {
    hist[s2a_hdr_hist_bin(y)] += n;
}

int main(void) {
    uint32_t h[S2A_HDR_HIST_BINS];
    s2a_hdr_auto_l1 r;

    // 1 空直方图：不产出参数
    memset(h, 0, sizeof h);
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 1, &r) == 0, "空直方图应返回 0");
    { int ok = 1; float a = s2a_hdr_hist_avg(h, &ok); CHECK(!ok && a == 0.0f, "空直方图重心 ok=0"); }

    // 2 均匀 18% 灰：重心 ≈ log2(0.18) = -2.47，白点落在该格上沿
    memset(h, 0, sizeof h); put(h, 0.18f, 10000);
    int ok = 0; float avg18 = s2a_hdr_hist_avg(h, &ok);
    CHECK(ok, "18% 灰重心 ok");
    CHECK(fabsf(avg18 - (-2.4739f)) < 0.06f, "18% 灰重心 ≈ -2.47");
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, avg18, 0, &r) == 1, "首帧可算");
    CHECK(r.first == 1, "首帧标记 first");
    CHECK(r.knee_hi <= S2A_HDR_KNEE_L1_MAX && r.knee_hi >= S2A_HDR_KNEE_L1_MIN, "knee 在 L1 区间内");
    CHECK(fabsf(r.knee_lo - S2A_HDR_KNEE_RATIO * r.knee_hi) < 1e-5f, "knee_lo = 0.55·knee_hi");

    // 3 一颗太阳不改白点：10000 个 0.18 灰 + 8 个 8.0（0.08% < 0.5%）
    memset(h, 0, sizeof h); put(h, 0.18f, 10000); put(h, 8.0f, 8);
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &r) == 1, "太阳场景可算");
    CHECK(r.white > 0.15f && r.white < 0.30f, "白点仍是 18% 灰区（不被单像素高光顶走）");

    // 4 大面积白（≥0.5%）：白点抬到 1.0 附近
    memset(h, 0, sizeof h); put(h, 0.18f, 10000); put(h, 1.0f, 100);
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &r) == 1, "大块白可算");
    CHECK(r.white >= 1.0f && r.white < 1.5f, "白点抬到 1.0 附近");

    // 5 黑场截断：黑边（-8 stops）不计入白点统计
    memset(h, 0, sizeof h);
    for (int i = 0; i < 20; i++) put(h, 0.00390625f, 500);   // -8 stops，低于截断
    put(h, 0.18f, 2000);
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &r) == 1, "带黑边可算");
    CHECK(r.white > 0.15f && r.white < 0.30f, "黑边不参与白点");
    { float w = -1.0f; memset(h, 0, sizeof h); put(h, 0.001f, 1000);
      CHECK(s2a_hdr_sdr_white(h, &w) == 0, "全在黑场截断以下 → 无白点"); }

    // 6 场景切换：差 5 stops ⇒ 权重 1 ⇒ 直接跳到目标，不平滑
    float w_hard = s2a_hdr_scene_weight(-6.0f, -1.0f);
    CHECK(w_hard > 0.999f, "5 stops 硬切 ⇒ 权重 1");
    float w_none = s2a_hdr_scene_weight(-3.0f, -2.8f);
    CHECK(w_none == 0.0f, "0.2 stops ⇒ 权重 0（走平滑）");
    float w_mid = s2a_hdr_scene_weight(-3.0f, -2.0f);
    CHECK(w_mid > 0.0f && w_mid < 1.0f, "1 stop ⇒ 0<权重<1");
    CHECK(fabsf(s2a_hdr_auto_step_l1(0.5f, 1.0f, 1.0f) - 1.0f) < 1e-6f, "权重 1 时立刻到目标");
    CHECK(fabsf(s2a_hdr_auto_step_l1(0.5f, 1.0f, 0.0f) - 0.55f) < 1e-6f, "权重 0 时按 0.1 平滑");

    // 7 死区只在静止画面生效：0.5→0.505（差 1%）不动；但硬切时同样的小差也照走
    CHECK(s2a_hdr_auto_step_l1(0.500f, 0.505f, 0.0f) == 0.500f, "死区：静止下 1% 不动");
    CHECK(s2a_hdr_auto_step_l1(0.500f, 0.505f, 1.0f) == 0.505f, "硬切时死区不挡");

    // 8 单调：亮场景的 knee 不低于暗场景
    uint32_t hd[S2A_HDR_HIST_BINS], hb[S2A_HDR_HIST_BINS];
    memset(hd, 0, sizeof hd); put(hd, 0.02f, 8000); put(hd, 0.09f, 2000);
    memset(hb, 0, sizeof hb); put(hb, 0.60f, 8000); put(hb, 1.00f, 2000);
    s2a_hdr_auto_l1 rd, rb;
    CHECK(s2a_hdr_auto_l1_step(hd, 1.0f, 0.0f, 0, &rd) == 1, "暗场景可算");
    CHECK(s2a_hdr_auto_l1_step(hb, 1.0f, 0.0f, 0, &rb) == 1, "亮场景可算");
    CHECK(rb.knee_hi >= rd.knee_hi - 1e-6f, "亮场景 knee ≥ 暗场景 knee");

    // 9 场景权重有界
    for (float d = -8.0f; d <= 8.0f; d += 0.25f) {
        float w = s2a_hdr_scene_weight(0.0f, d);
        CHECK(w >= 0.0f && w <= 1.0f, "场景权重恒在 [0,1]");
    }

    // 10 全黑画面（只有 bin0，低于 AVG_CUT）→ 无可测内容，返回 0，调用方保持上一帧。
    //    口径与 GPU 的 hdr_hist_l1 一致（那边同样返回 float4(0,0,0,0)），t_hdr_gpu_mirror 守着。
    memset(h, 0, sizeof h); h[0] = 100000;
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 1, &r) == 0, "全黑不可测（重心门限 = 真黑，与 GPU 同）");
    // 但只要有一格在 AVG_CUT 之上就必须能算（星空的暗星就在这个区间）
    memset(h, 0, sizeof h); h[0] = 100000; h[s2a_hdr_hist_bin(0.003f)] = 50;
    CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 1, &r) == 1, "纯黑+少量暗星仍可算（0.003 在门限之上）");
    CHECK(r.knee_hi >= S2A_HDR_KNEE_L1_MIN, "暗星 knee 不越过下界");

    (void)add;
    printf("%s t_hdr_auto_l1：白点 / 黑场截断 / 场景平滑 %d 项 → %s（失败 %d）\n",
           fails ? "✗" : "✓", checks, fails ? "有失败" : "全过", fails);
    return fails != 0;
}
