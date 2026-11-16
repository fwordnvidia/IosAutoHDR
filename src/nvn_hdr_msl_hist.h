// 设备侧直方图链路的逐行 C 转写：分箱（hdr_hist_add）+ 分位→knee（hdr_hist_knee）。
//
// 为什么单独成头文件：单测（t_hdr_bin_mirror / t_hdr_knee_mirror）分别守住这两步，
// 而设备上它们是**串起来**的（kernel 铺直方图 → 回调里算 knee → 写 g_hdr_knee）。
// tools/hdr_device_image.c 用同一份转写跑整幅图，验证「设备算出的 knee」与「CPU 算出的 knee」
// 在真实内容上一致，因此转写必须只有一份。
//
// 两处已修的真分歧都体现在这里：
//   1) 黑像素进 0 号格（旧版整帧跳过 ⇒ lit_frac 恒 1.0 ⇒ 暗场分支在设备上从不执行）；
//   2) 分位阈值用整数千分 (total*995)/1000（旧版 float 乘法在 1,515,877 个总数上少 1 ⇒ hi 差一格）。
#ifndef S2A_NVN_HDR_MSL_HIST_H
#define S2A_NVN_HDR_MSL_HIST_H
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "nvn_hdr_auto.h"

#define S2A_MSL_HIST_LO (-10.0f)
#define S2A_MSL_HIST_HI (4.0f)

// 逐行翻译自 src/hdr_hist.metal 的 hdr_hist_add：y=0 与负数进 0 号格，非有限值夹最高格。
static inline int s2a_msl_hist_bin(float y) {
    if (!(y > 0.0f)) return 0;
    if (!isfinite(y)) return S2A_HDR_HIST_BINS - 1;
    float l = (log2f(y) - S2A_MSL_HIST_LO) / (S2A_MSL_HIST_HI - S2A_MSL_HIST_LO);
    l = l < 0.0f ? 0.0f : (l > 0.9999999f ? 0.9999999f : l);
    return (int)(unsigned)(l * 256.0f);
}

// 逐行翻译自 src/hdr_hist.metal 的 hdr_hist_knee（整数千分阈值版）。
static inline int s2a_msl_hist_knee(const uint32_t h[S2A_HDR_HIST_BINS], float *hi, float *lo, int *hit_out) {
    unsigned long total = 0ul;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) total += (unsigned long)h[i];
    if (total == 0ul) return 0;
    unsigned long target = (total * 995ul) / 1000ul;
    if (target < 1ul) target = 1ul;
    unsigned long acc = 0ul, hit = (unsigned long)(S2A_HDR_HIST_BINS - 1);
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        acc += (unsigned long)h[i];
        if (acc >= target) { hit = (unsigned long)i; break; }
    }
    float edge = exp2f(S2A_MSL_HIST_LO + (S2A_MSL_HIST_HI - S2A_MSL_HIST_LO) * ((float)hit + 1.0f) / (float)S2A_HDR_HIST_BINS);
    float hh = edge / 0.8f;
    hh = hh < 0.25f ? 0.25f : (hh > 1.0f ? 1.0f : hh);
    *hi = hh;
    *lo = 0.55f * hh;
    if (hit_out) *hit_out = (int)hit;
    return 1;
}

// —— 逐行翻译自 src/hdr_hist.metal 的 hdr_hist_l1（白点 / 重心 / 切镜权重）——
// 用途：这条 kernel 在设备上只做两件事——① 与 CPU 直方图结论对账（差 >0.02/0.05 就 MLOG），
// ② blit 读不到直方图时作为 fallback。两者的正确性都依赖它和 CPU 参考实现同口径。
#define S2A_MSL_KBLACKCUT (-6.0f)
#define S2A_MSL_KAVGCUT (-9.5f)
#define S2A_MSL_KWHITEFRAC 0.005f
#define S2A_MSL_KKNEETARGET 0.8f
#define S2A_MSL_KL1HIMIN 0.01f
#define S2A_MSL_KL1HIMAX 2.0f
#define S2A_MSL_KSCENELOW 0.40f
#define S2A_MSL_KSCENEHIGH 1.10f

static inline float s2a_msl_bin_low(int i) {
    return S2A_MSL_HIST_LO + (float)i * ((S2A_MSL_HIST_HI - S2A_MSL_HIST_LO) / (float)S2A_HDR_HIST_BINS);
}
static inline float s2a_msl_bin_center(int i) {
    return S2A_MSL_HIST_LO + ((float)i + 0.5f) * ((S2A_MSL_HIST_HI - S2A_MSL_HIST_LO) / (float)S2A_HDR_HIST_BINS);
}
static inline float s2a_msl_bin_edge(int i) {
    return S2A_MSL_HIST_LO + ((float)i + 1.0f) * ((S2A_MSL_HIST_HI - S2A_MSL_HIST_LO) / (float)S2A_HDR_HIST_BINS);
}
static inline float s2a_msl_scene_weight(float prev_avg, float cur_avg) {
    float d = fabsf(cur_avg - prev_avg);
    if (!(d > S2A_MSL_KSCENELOW)) return 0.0f;
    if (d >= S2A_MSL_KSCENEHIGH) return 1.0f;
    float t = (d - S2A_MSL_KSCENELOW) / (S2A_MSL_KSCENEHIGH - S2A_MSL_KSCENELOW);
    return t * t * (3.0f - 2.0f * t);
}

// out = (hi, white, avg, scene)；返回 0 表示空帧/全黑（调用方保持上一帧）
static inline int s2a_msl_hist_l1(const uint32_t h[S2A_HDR_HIST_BINS], float prev_avg, int have_prev, float out[4]) {
    unsigned long total = 0ul;
    float acc = 0.0f, all = 0.0f;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        uint32_t c = h[i];
        if (c == 0u) continue;
        all += (float)c;
        if (s2a_msl_bin_low(i) < S2A_MSL_KAVGCUT) continue;
        total += (unsigned long)c;
        acc += s2a_msl_bin_center(i) * (float)c;
    }
    if (!(all > 0.0f) || total == 0ul) { out[0] = out[1] = out[2] = out[3] = 0.0f; return 0; }
    float avg = acc / (float)total;

    unsigned long need = (unsigned long)((float)total * S2A_MSL_KWHITEFRAC);
    if (need < 1ul) need = 1ul;
    float white = 0.0f;
    int found = 0;
    for (int i = S2A_HDR_HIST_BINS - 1; i >= 0; i--) {
        if (s2a_msl_bin_low(i) < S2A_MSL_KBLACKCUT) break;
        if ((unsigned long)h[i] >= need) { white = exp2f(s2a_msl_bin_edge(i)); found = 1; break; }
    }
    float hi;
    if (found) {
        hi = white / S2A_MSL_KKNEETARGET;
        if (!(hi >= S2A_MSL_KL1HIMIN)) hi = S2A_MSL_KL1HIMIN;
        if (hi > S2A_MSL_KL1HIMAX) hi = S2A_MSL_KL1HIMAX;
    } else {
        // 与 L0 同口径的整数千分（原 kernel 这里写 float，和 hdr_hist_knee 修前同病）
        unsigned long target = (total * 995ul) / 1000ul;
        if (target < 1ul) target = 1ul;
        unsigned long a2 = 0ul;
        int hit = S2A_HDR_HIST_BINS - 1;
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
            a2 += (unsigned long)h[i];
            if (a2 >= target) { hit = i; break; }
        }
        hi = exp2f(s2a_msl_bin_edge(hit)) / S2A_MSL_KKNEETARGET;
        if (hi < 0.25f) hi = 0.25f;
        if (hi > 1.0f) hi = 1.0f;
    }
    float sc = have_prev ? s2a_msl_scene_weight(prev_avg, avg) : 0.0f;
    out[0] = hi; out[1] = white; out[2] = avg; out[3] = sc;
    return 1;
}

#endif
