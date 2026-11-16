// hdr_cost：每帧「控制器」开销的实测（不是 GPU 着色器的开销）。
//
// 为什么只量这一块：这套补丁在设备上的每帧 CPU 工作只有两件——
//   1) 把 256×uint32（1 KB）的直方图从 shared buffer blit 回 CPU（driver 的事，量不到）；
//   2) 调一次 s2a_hdr_auto_l1b_step_fused_hz（扫 256 格 + 几次 log2f/powf），算出 knee 与 head。
// 而 s2a_hdr_grade_rgb / s2a_hdr_itm_rgb 这类逐像素函数跑在 GPU 上（C 侧只被 hdr_convert 这类
// 离线工具用），把它们和帧预算比没有意义。所以这里量的判据是：
//   「控制器一次调用 < 一帧预算的 2%」，30/60/120 Hz 三档各自算。
//
// 输出 CSV：case,us_per_call,budget_pct_30hz,budget_pct_60hz,budget_pct_120hz
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto_l1.h"

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

// 一帧典型内容：中间调 + 纸白 + 少量高光（和 t_hdr_fade 的合成帧同分布）
static void make_hist(uint32_t h[S2A_HDR_HIST_BINS], float scale) {
    memset(h, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    const float base[5] = {0.12f, 0.20f, 0.30f, 0.45f, 0.62f};
    for (int i = 0; i < 5; i++) h[s2a_hdr_hist_bin(base[i] * scale)] = 15000;
    h[s2a_hdr_hist_bin(0.832f * scale)] = 20000;
    h[s2a_hdr_hist_bin(0.96f * scale)] = 300;
}

// 取 N 次一批的中位数，避开调度抖动
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static double time_step_per_call(float hz, int iters) {
    uint32_t h[S2A_HDR_HIST_BINS];
    make_hist(h, 1.0f);
    s2a_hdr_auto_l1b b;
    memset(&b, 0, sizeof b);
    float lo = 0.5278f, hi = 1.0f, avg = -3.0933f, white = 0.5278f;
    // 预热：把首次的页错误与分支预测从计时里去掉
    for (int i = 0; i < 2000; i++) s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, lo, hi, avg, white, 1, hz, &b);
    double t0 = now_us();
    for (int i = 0; i < iters; i++) s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, lo, hi, avg, white, 1, hz, &b);
    double t1 = now_us();
    return (t1 - t0) / (double)iters;
}

static double time_bulk_white_per_call(int iters) {
    uint32_t h[S2A_HDR_HIST_BINS];
    make_hist(h, 1.0f);
    float w = 0.0f;
    for (int i = 0; i < 2000; i++) s2a_hdr_sdr_white_bulk(h, &w);
    double t0 = now_us();
    for (int i = 0; i < iters; i++) s2a_hdr_sdr_white_bulk(h, &w);
    double t1 = now_us();
    return (t1 - t0) / (double)iters;
}

int main(void) {
    const int iters = 200000, batches = 7;
    double samples[7];
    const float hzs[3] = {30.0f, 60.0f, 120.0f};

    printf("case,us_per_call,budget_pct_30hz,budget_pct_60hz,budget_pct_120hz\n");
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < batches; i++) samples[i] = time_step_per_call(hzs[k], iters);
        qsort(samples, batches, sizeof(double), cmp_double);
        double us = samples[batches / 2];
        char name[32];
        snprintf(name, sizeof name, "step_fused@%.0fhz", (double)hzs[k]);
        printf("%s,%.4f,%.3f,%.3f,%.3f\n", name, us,
               100.0 * us / (1e6 / 30.0), 100.0 * us / (1e6 / 60.0), 100.0 * us / (1e6 / 120.0));
    }
    for (int i = 0; i < batches; i++) samples[i] = time_bulk_white_per_call(iters);
    qsort(samples, batches, sizeof(double), cmp_double);
    printf("white_bulk,%.4f,%.3f,%.3f,%.3f\n", samples[batches / 2],
           100.0 * samples[batches / 2] / (1e6 / 30.0),
           100.0 * samples[batches / 2] / (1e6 / 60.0),
           100.0 * samples[batches / 2] / (1e6 / 120.0));
    return 0;
}
