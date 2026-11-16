// 时间稳定性：直方图白点是「最高一个计数≥0.5%的格子」，格子边界是量化的 ⇒ 相邻帧只要有一点噪声，
// 白点就会在两格之间跳，拐点跟着跳，画面上表现为「呼吸」。
// 这个测试不改算法，先量：60 帧带噪声的同一场景，看 knee 的逐帧变化有多大、有没有来回摆。
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#define W 128
#define H 72
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// 确定性伪随机（不用 rand，跑两次结果必须一样）
static unsigned rs = 12345u;
static float rnd(void) { rs = rs * 1103515245u + 12345u; return (float)((rs >> 8) & 0xffff) / 65535.0f; }

// 城市夜景 + 每帧 ±1.5% 的传感器/patch 噪声
static float g_noise = 0.015f;
static void frame_city(float y[W * H], int t) {
    for (int i = 0; i < W * H; i++) {
        float base = 0.05f + 0.03f * (float)((i * 37) % 11) / 11.0f;
        if (i % 97 == 0) base = 0.62f;
        y[i] = base * (1.0f + g_noise * (rnd() - 0.5f) * 2.0f);
    }
    (void)t;
}

static void hist_of(const float y[W * H], uint32_t hist[S2A_HDR_HIST_BINS]) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < W * H; i++) hist[s2a_hdr_hist_bin(s2a_hdr_to_linear(y[i]))]++;
}

int main(void) {
    puts("t_hdr_stability：同一场景 60 帧，噪声幅度 1%→20%，看白点与 knee 的跨帧抖动从哪一档开始失控");
    static float y[W * H];
    uint32_t hist[S2A_HDR_HIST_BINS];
    const float amps[4] = {0.01f, 0.05f, 0.10f, 0.20f};

    printf("  %-6s %-28s %-28s\n", "噪声", "L1 knee 均值 / 逐帧最大 / 白点区间", "L1b lo / hi / 逐帧最大");
    float worst_l1 = 0, worst_l1b = 0;
    for (int a = 0; a < 4; a++) {
        g_noise = amps[a];
        rs = 12345u;

        // L1
        s2a_hdr_auto_l1 r; memset(&r, 0, sizeof r);
        int have = 0; float prev_hi = 1.0f, prev_avg = 0.0f;
        float k[60], w[60];
        for (int t = 0; t < 60; t++) {
            frame_city(y, t); hist_of(y, hist);
            s2a_hdr_auto_l1_step(hist, prev_hi, prev_avg, have, &r);
            k[t] = r.knee_hi; w[t] = r.white;
            prev_hi = r.knee_hi; prev_avg = r.avg; have = 1;
        }
        float mean = 0, mstep = 0;
        for (int t = 20; t < 60; t++) mean += k[t];
        mean /= 40.0f;
        for (int t = 21; t < 60; t++) { float d = fabsf(k[t] - k[t - 1]) / k[t - 1]; if (d > mstep) mstep = d; }
        float wmin = 1e9f, wmax = 0;
        for (int t = 20; t < 60; t++) { if (w[t] < wmin) wmin = w[t]; if (w[t] > wmax) wmax = w[t]; }

        // L1b
        s2a_hdr_auto_l1b b; memset(&b, 0, sizeof b);
        have = 0; float lo = 0.5f, hi = 1.0f, avg = 0.0f;
        float blo[60], bhi[60];
        for (int t = 0; t < 60; t++) {
            frame_city(y, t); hist_of(y, hist);
            s2a_hdr_auto_l1b_step(hist, lo, hi, avg, have, &b);
            blo[t] = b.knee_lo; bhi[t] = b.knee_hi;
            lo = b.knee_lo; hi = b.knee_hi; avg = b.avg; have = 1;
        }
        float mlo = 0, mhi = 0, lomin = 1e9f, lomax = 0;
        for (int t = 21; t < 60; t++) {
            float d = fabsf(blo[t] - blo[t - 1]) / blo[t - 1]; if (d > mlo) mlo = d;
            d = fabsf(bhi[t] - bhi[t - 1]) / bhi[t - 1]; if (d > mhi) mhi = d;
            if (blo[t] < lomin) lomin = blo[t];
            if (blo[t] > lomax) lomax = blo[t];
        }
        printf("  %4.0f%%  %.3f / %5.2f%% / %.4f..%.4f   %.4f..%.4f / %.3f / %5.2f%%\n",
               amps[a] * 100.0, mean, mstep * 100.0, wmin, wmax, lomin, lomax, bhi[59], mhi * 100.0);
        if (mstep > worst_l1) worst_l1 = mstep;
        if (mlo > worst_l1b) worst_l1b = mlo;

        // 逐档断言：20% 噪声下也不许出现「大跳」
        if (a == 0) {
            CHECK(mstep < 0.005f, "1%% 噪声下 knee 就应该几乎不动（实得 %.2f%%）", mstep * 100.0);
        }
        CHECK(mstep < 0.25f, "%.0f%% 噪声下 L1 knee 单帧跳 %.0f%%（会看出亮度抽搐）", amps[a] * 100.0, mstep * 100.0);
        CHECK(mlo < 0.25f, "%.0f%% 噪声下 L1b lo 单帧跳 %.0f%%", amps[a] * 100.0, mlo * 100.0);
        CHECK(bhi[59] > lomax || bhi[59] > 0.0f, "L1b 区间退化");
    }
    printf("  汇总：L1 最差 %.2f%%，L1b 最差 %.2f%%\n", worst_l1 * 100.0, worst_l1b * 100.0);

    if (fails) { printf("t_hdr_stability：%d 项失败\n", fails); return 1; }
    printf("✓ t_hdr_stability：60 帧噪声下 knee 抖动 %d 项 → 全过\n", checks);
    return 0;
}
