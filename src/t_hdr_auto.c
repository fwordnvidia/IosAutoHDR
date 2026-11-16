// t_hdr_auto：HDR 自适应 knee 的参数算法（nvn_hdr_auto.h）。
// 只测纯 C 的量化/分位/平滑，不碰 GPU。期望值都能手算出来（见每条 CHECK 的注释）。
#include "nvn_hdr_auto.h"
#include <stdio.h>
#include <string.h>

static int g_n, g_bad;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_bad++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t hist[S2A_HDR_HIST_BINS];

// 往某个线性亮度值上放 n 个样本
static void put(float y, uint32_t n) { int b = s2a_hdr_hist_bin(y); hist[b] += n; }

int main(void) {
    // ---- 量化 ----
    // log2(1.0)=0，(-10..4) 均分 256 格 → 0 落在第 (0+10)*256/14 = 182.86 格 → 182
    CHECK(s2a_hdr_hist_bin(1.0f) == 182, "线性 1.0 应落 bin 182，实得 %d", s2a_hdr_hist_bin(1.0f));
    CHECK(s2a_hdr_hist_bin(0.001f) == 0, "线性 0.001 应落 bin 0，实得 %d", s2a_hdr_hist_bin(0.001f));
    CHECK(s2a_hdr_hist_bin(16.0f) == 255, "线性 16 应落最后一格 255，实得 %d", s2a_hdr_hist_bin(16.0f));
    CHECK(s2a_hdr_hist_bin(0.0f) == 0, "0 应落 bin 0，实得 %d", s2a_hdr_hist_bin(0.0f));
    CHECK(s2a_hdr_hist_bin(-1.0f) == 0, "负数应落 bin 0，实得 %d", s2a_hdr_hist_bin(-1.0f));
    // 单调：bin 随亮度不减
    int mono = 1;
    for (int i = 0; i < 1000; i++) {
        float a = powf(2.0f, -10.0f + 14.0f * (float)i / 1000.0f);
        float b = powf(2.0f, -10.0f + 14.0f * (float)(i + 1) / 1000.0f);
        if (s2a_hdr_hist_bin(a) > s2a_hdr_hist_bin(b)) mono = 0;
    }
    CHECK(mono, "bin 必须随亮度单调不减");

    // ---- 边界 ----
    // bin 182 的上界 = 2^(-10 + 183*14/256) = 2^(0.0078125) = 1.00543
    {
        float e = s2a_hdr_hist_edge(182);
        CHECK(e > 1.0054f && e < 1.0055f, "bin 182 上界应约 1.00543，实得 %.5f", e);
    }
    CHECK(s2a_hdr_hist_edge(-5) == s2a_hdr_hist_edge(0), "越界 bin 应钳到 0");
    CHECK(s2a_hdr_hist_edge(999) == s2a_hdr_hist_edge(255), "越界 bin 应钳到 255");

    // ---- 空直方图 ----
    {
        float hi = -1, lo = -1;
        memset(hist, 0, sizeof hist);
        CHECK(s2a_hdr_auto_knee(hist, &hi, &lo) == 0, "空直方图应返回 0");
        CHECK(hi == -1 && lo == -1, "空直方图不应写输出");
    }

    // ---- 全亮：99.5 分位在最后 → knee_hi 钳到 1.0 ----
    {
        float hi = 0, lo = 0;
        memset(hist, 0, sizeof hist);
        put(1.0f, 1000);
        CHECK(s2a_hdr_auto_knee(hist, &hi, &lo) == 1, "非空直方图应返回 1");
        // y995 = 1.00272 → 1.00272/0.8 = 1.2534 → 钳到 1.0
        CHECK(hi == 1.0f, "全 1.0 的画面 knee_hi 应钳到 1.0，实得 %.4f", hi);
        CHECK(fabsf(lo - 0.55f) < 1e-6f, "knee_lo 应等于 0.55*hi，实得 %.4f", lo);
    }

    // ---- 全暗：0.02 线性 → log2=-5.64 → 1.00272 之类算出来的值 < 0.25 → 钳到下限 ----
    {
        float hi = 0, lo = 0;
        memset(hist, 0, sizeof hist);
        put(0.02f, 1000);
        CHECK(s2a_hdr_auto_knee(hist, &hi, &lo) == 1, "暗画面也应算得出来");
        // log2(0.02) = -5.6439 → bin = (-5.6439+10)*18.2857 = 79.66 → 79
        // edge(79) = 2^(-10 + 80*14/256) = 2^(-5.625) = 0.02027 → /0.8 = 0.02534 → 钳到 0.25
        CHECK(hi == S2A_HDR_KNEE_MIN, "暗画面 knee_hi 应钳到下限 0.25，实得 %.4f", hi);
    }

    // ---- 中间：让 99.5 分位落在 0.64 → knee_hi = 0.8 ----
    {
        float hi = 0, lo = 0;
        memset(hist, 0, sizeof hist);
        // 找 edge 最接近 0.64 的 bin：0.64 = 2^-0.6439 → bin = (-0.6439+10)*18.2857 = 171.1
        // log2(0.66265)=-0.5937 → bin 172；edge(172) = 2^(-10+173*14/256) = 2^(-0.5390625) = 0.68825
        put(0.66265f, 1000);
        CHECK(s2a_hdr_auto_knee(hist, &hi, &lo) == 1, "应算得出");
        // 0.68825 / 0.8 = 0.8603
        CHECK(hi > 0.860f && hi < 0.861f, "knee_hi 应约 0.8603，实得 %.4f", hi);
    }

    // ---- 分位行为：99.5% 的体量决定位置，最亮 0.5% 不参与 ----
    {
        float hi = 0, lo = 0;
        memset(hist, 0, sizeof hist);
        put(0.2f, 9950);      // 99.5%
        put(8.0f, 50);        // 最亮 0.5%：一堆纯白/超白
        CHECK(s2a_hdr_auto_knee(hist, &hi, &lo) == 1, "应算得出");
        // 累计到 0.2 那格正好 9950 = want(1e4*0.995) → 停在那格，y995≈0.2075
        // 0.2075/0.8 = 0.2594（> 0.25，不钳）
        CHECK(hi < 0.30f, "最亮 0.5%% 不该把 knee 推高，实得 %.4f", hi);
    }

    // ---- 平滑 ----
    CHECK(fabsf(s2a_hdr_auto_step(1.0f, 0.0f) - 0.9f) < 1e-6f, "一阶平滑：1.0→0.0 第一步应 0.9，实得 %.4f", s2a_hdr_auto_step(1.0f, 0.0f));
    CHECK(fabsf(s2a_hdr_auto_step(0.9f, 1.0f) - 0.91f) < 1e-6f, "反向同理，实得 %.4f", s2a_hdr_auto_step(0.9f, 1.0f));
    // 死区：变化 0.01 < 0.02 → 完全不动
    CHECK(s2a_hdr_auto_step(0.5f, 0.51f) == 0.5f, "死区内应保持不动，实得 %.4f", s2a_hdr_auto_step(0.5f, 0.51f));
    CHECK(s2a_hdr_auto_step(0.5f, 0.485f) == 0.5f, "死区内（向下）也应保持不动，实得 %.4f", s2a_hdr_auto_step(0.5f, 0.485f));
    CHECK(s2a_hdr_auto_step(0.5f, 0.47f) != 0.5f, "死区外（向下）应开始动");
    // 死区边界外：0.03 会动
    CHECK(s2a_hdr_auto_step(0.5f, 0.53f) != 0.5f, "死区外应开始动");
    // 收敛性：连续 60 次 1.0→0.25，应逼近 0.25 且单调下降
    {
        float v = 1.0f; int down = 1;
        for (int i = 0; i < 60; i++) { float n = s2a_hdr_auto_step(v, 0.25f); if (n > v) down = 0; v = n; }
        CHECK(down, "平滑必须单调逼近目标");
        CHECK(v < 0.27f && v > 0.25f, "60 帧后应停在死区边缘（0.25~0.27），实得 %.4f", v);   // 死区 0.02：差小于它就停
    }

    // ---- knee 比例恒为 0.55 ----
    {
        float hi = 0, lo = 0;
        memset(hist, 0, sizeof hist);
        put(0.5f, 777);
        s2a_hdr_auto_knee(hist, &hi, &lo);
        CHECK(fabsf(lo - S2A_HDR_KNEE_RATIO * hi) < 1e-6f, "knee_lo 恒等于 0.55*hi");
    }

    if (g_bad) return 1;

    /* ---- 收敛速度：GPU 每 30 帧调一次，从 1.0 掉到 0.25 要多久 ----
       帧数 = 次数 * 30，60fps 下换算成秒；超过 3 秒就算看得见，列入文档 */
    {
        float hi = 1.0f; int n = 0, frames;
        do { hi = s2a_hdr_auto_step(hi, 0.25f); n++; } while (n < 400 && s2a_hdr_auto_step(hi, 0.25f) != hi);
        frames = n * S2A_HDR_HIST_EVERY;
        printf("    · 1.00 → 0.25 用了 %d 次（%d 帧，60fps 下 %.1f 秒）\n", n, frames, frames / 60.0f);
        CHECK(n >= 5 && n <= 40, "收敛步数应在 5..40 次之间，实得 %d", n);
        CHECK(hi < 0.25f + S2A_HDR_DEADBAND, "终值应落在死区内，实得 %.4f", hi);
    }

    /* ---- 反方向：0.25 涨回 1.0 同样是渐进的 ---- */
    {
        float hi = 0.25f; int n = 0;
        do { hi = s2a_hdr_auto_step(hi, 1.0f); n++; } while (n < 400 && s2a_hdr_auto_step(hi, 1.0f) != hi);
        printf("    · 0.25 → 1.00 用了 %d 次（%d 帧，60fps 下 %.1f 秒）\n", n, n * S2A_HDR_HIST_EVERY, n * S2A_HDR_HIST_EVERY / 60.0f);
        CHECK(n >= 5 && n <= 40, "反向收敛步数应在 5..40 次之间，实得 %d", n);
    }

    printf("✓ t_hdr_auto：自适应 knee（量化 / 分位 / 边界 / 死区 / 平滑）%d 项 → 全过\n", g_n);
    return 0;
}
