// sweep_scene：场景切换阈值（SCENE_LOW / SCENE_HIGH）的敏感度扫描。
//
// 为什么要扫：这两个常数决定「什么时候放弃平滑、直接跳到新目标」。
//   太低 → 普通镜头运动/淡入被当成切镜，画面每帧都在跳（闪烁）；
//   太高 → 真切镜要平滑过去，切镜后 1 秒内亮度缓慢爬升（观感像曝光漂移）。
// 判据不是「某个值看起来对」，而是找一片**两端都安全**的平台区，默认值落在平台中间。
//
// 输出 CSV：low,high,fade_jumps,cut_latency_frames,flicker_jumps
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr_auto_l1.h"

// 生成一帧的直方图：亮度按给定分布铺开，用来算 log2 重心
static void frame_hist(uint32_t h[S2A_HDR_HIST_BINS], float level, float spread) {
    memset(h, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < 4096; i++) {
        float t = (float)i / 4095.0f - 0.5f;            // -0.5..0.5
        float y = level * (1.0f + spread * t);          // 围绕 level 展开
        if (y < 0.002f) y = 0.002f;
        h[s2a_hdr_hist_bin(y)]++;
    }
}

static float avg_of(uint32_t h[S2A_HDR_HIST_BINS]) {
    int ok = 0;
    return s2a_hdr_hist_avg(h, &ok);
}

// 淡入：level 从 0.05 线性到 0.60（64 帧）。任何一帧被判定为「切镜」都算误报。
static int fade_jumps(float low, float high) {
    uint32_t h[S2A_HDR_HIST_BINS];
    float prev = 0.0f;
    int jumps = 0;
    for (int f = 0; f < 64; f++) {
        float level = 0.05f + (0.55f * (float)f) / 63.0f;
        frame_hist(h, level, 0.8f);
        float a = avg_of(h);
        if (f > 0 && s2a_hdr_scene_weight_opt(prev, a, low, high) > 0.5f) jumps++;
        prev = a;
    }
    return jumps;
}

// 抖动：同一场景叠加 ±1.5% 亮度噪声 60 帧，不该被判成切镜
static int flicker_jumps(float low, float high) {
    uint32_t h[S2A_HDR_HIST_BINS];
    float prev = 0.0f;
    int jumps = 0;
    for (int f = 0; f < 60; f++) {
        float jitter = 1.0f + 0.015f * sinf((float)f * 1.7f);
        frame_hist(h, 0.35f * jitter, 0.8f);
        float a = avg_of(h);
        if (f > 0 && s2a_hdr_scene_weight_opt(prev, a, low, high) > 0.5f) jumps++;
        prev = a;
    }
    return jumps;
}

// 真切镜：暗场景 → 亮场景，返回「切镜后还需要几帧才把 knee_lo 拉到新目标的 95%」
static int cut_latency(float low, float high) {
    uint32_t hd[S2A_HDR_HIST_BINS], hb[S2A_HDR_HIST_BINS];
    frame_hist(hd, 0.05f, 0.8f);
    frame_hist(hb, 0.55f, 0.8f);

    // 用 l1b 的完整通路，把 low/high 换成被扫的值：这里直接内联同样的平滑逻辑
    s2a_hdr_auto_l1b st;
    memset(&st, 0, sizeof st);
    float lo = 0.0f, avg = 0.0f;
    // 先在暗场景稳定 30 帧
    for (int f = 0; f < 30; f++) {
        uint32_t *h = hd;
        int ok = 0;
        float a = s2a_hdr_hist_avg(h, &ok);
        float white = 0.0f;
        s2a_hdr_sdr_white(h, &white);
        float target_lo = white > 0.0f ? white : 0.1f;
        if (target_lo > 0.95f) target_lo = 0.95f;
        float sw = (f == 0) ? 0.0f : s2a_hdr_scene_weight_opt(avg, a, low, high);
        lo = (f == 0) ? target_lo : s2a_hdr_auto_step_l1b(lo, target_lo, sw);
        avg = a;
    }
    // 切到亮场景，量需要几帧
    int ok = 0;
    float a_new = s2a_hdr_hist_avg(hb, &ok);
    float white_new = 0.0f;
    s2a_hdr_sdr_white(hb, &white_new);
    float target_new = white_new > 0.0f ? white_new : 0.5f;
    if (target_new > 0.95f) target_new = 0.95f;
    float start = lo;
    float span = target_new - start;
    if (fabsf(span) < 1e-4f) return 0;
    for (int f = 0; f < 120; f++) {
        float sw = s2a_hdr_scene_weight_opt(avg, a_new, low, high);
        lo = s2a_hdr_auto_step_l1b(lo, target_new, sw);
        if (fabsf(lo - start) / fabsf(span) >= 0.95f) return f + 1;
        avg = a_new;   // 之后场景权重回到 0，靠常规平滑继续走
    }
    return 999;
}

// 稳态跟踪误差：以 rate stop/帧 匀速爬升 300 帧后，knee 落后目标多少（stop）
static float ramp_lag(float low, float high, float rate) {
    // 量的是**爬升途中**的最大落后（stop）；跑满会让两端都被 0.95 钳住，看起来恒为 0（第一版就这么错）
    float target = 0.20f, knee = 0.20f;
    float worst = 0.0f;
    for (int f = 0; f < 200 && target < 0.90f; f++) {
        target *= powf(2.0f, rate);
        float sw = s2a_hdr_scene_weight_opt(0.0f, rate, low, high);   // 每帧重心变化 = rate（log2 域）
        knee = s2a_hdr_auto_step_l1b(knee, target, sw);
        float lag = log2f(target / knee);
        if (lag > worst) worst = lag;
    }
    return worst;
}

// 单帧阶跃：sw 与「几帧到 95%」
static void step_response(float low, float high, float delta, float *sw_out, int *frames_out) {
    float sw = s2a_hdr_scene_weight_opt(0.0f, delta, low, high);
    float knee = 0.20f, target = 0.20f * powf(2.0f, delta);
    int hit = 0;
    for (int f = 0; f < 120 && !hit; f++) {
        knee = s2a_hdr_auto_step_l1b(knee, target, sw);
        if (fabsf(knee - 0.20f) / fabsf(target - 0.20f) >= 0.95f) hit = f + 1;
    }
    *sw_out = sw;
    *frames_out = hit;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--response")) {
        printf("delta_stop,sw_default,frames_to_95\n");
        for (float d = 0.1f; d <= 3.01f; d += 0.1f) {
            float sw = 0; int n = 0;
            step_response(S2A_HDR_SCENE_LOW, S2A_HDR_SCENE_HIGH, d, &sw, &n);
            printf("%.2f,%.3f,%d\n", (double)d, (double)sw, n);
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--at")) {
        // 单个 delta 的权重与 95% 到位帧数（ref_check 用它钉住「中等切镜」这一档，
        // 0.1 步长的表插不出 0.66 这种真实出现的值）
        float d = (float)atof(argv[2]);
        float sw = 0; int n = 0;
        step_response(S2A_HDR_SCENE_LOW, S2A_HDR_SCENE_HIGH, d, &sw, &n);
        printf("%.4f,%d\n", (double)sw, n);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--lag")) {
        printf("ramp_stop_per_frame,lag_stop_default,lag_lo0.25,lag_lo1.0\n");
        const float rates[] = {0.02f, 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.8f, 1.0f};
        for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
            printf("%.2f,%.3f,%.3f,%.3f\n", (double)rates[i],
                   (double)ramp_lag(S2A_HDR_SCENE_LOW, S2A_HDR_SCENE_HIGH, rates[i]),
                   (double)ramp_lag(0.25f, 1.6f, rates[i]),
                   (double)ramp_lag(1.0f, 1.6f, rates[i]));
        }
        return 0;
    }
    printf("low,high,fade_jumps,cut_latency_frames,flicker_jumps\n");
    const float lows[]  = {0.25f, 0.35f, 0.5f, 0.65f, 0.8f, 1.0f};
    const float highs[] = {1.0f, 1.3f, 1.6f, 2.0f, 2.5f};
    for (unsigned i = 0; i < sizeof lows / sizeof lows[0]; i++) {
        for (unsigned j = 0; j < sizeof highs / sizeof highs[0]; j++) {
            if (highs[j] <= lows[i]) continue;
            printf("%.2f,%.2f,%d,%d,%d\n", (double)lows[i], (double)highs[j],
                   fade_jumps(lows[i], highs[j]), cut_latency(lows[i], highs[j]),
                   flicker_jumps(lows[i], highs[j]));
        }
    }
    return 0;
}
