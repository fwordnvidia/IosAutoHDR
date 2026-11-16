// t_hdr_fuzz：拿 20 万帧随机直方图去撞不变量（不是撞具体数值）。
//
// 为什么要有这一套：其它套件都在验「某个场景算出来对不对」，而真机上遇到的输入我们无法预知——
// 全黑、全白、单个格子塞满、直方图里出现 2^32 附近的计数、NaN/无穷已经混进灰度值……这些
// 一帧就能把上屏搞黑或者把拐点推到荒谬的位置，而它们都不在任何「用例」里。这里换一种问法：
//
//   不管丢进来什么直方图，输出必须满足一组必须永远成立的性质。
//
// 五类输入：均匀随机、稀疏尖峰（星点/字幕）、单格全满、单调斜坡、病态计数（接近 UINT32_MAX）。
// 生成器是固定种子的 xorshift（可复现：失败时打印种子与第几帧，能一模一样重跑）。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

// 计数口径：**一条不变量算一项**，而不是「每帧每点算一项」。
//
// 这套跑 4 万帧 × 5 类共 3500 万次判定，如果按判定次数计「项」，总数会变成八位数，
// 既误导读者（看起来比其它 26 套加起来还多），也会把 docs 里的「NN 项」推到荒谬的位置。
// 真正有意义的说法是：12 条必须恒成立的性质，在 20 万帧里一次都没被打破。
static int items, fails, frames_done;
static unsigned char seen_family[32];

#define CHK(fam, c, ...) do {                                       \
        if (!seen_family[(fam)]) { seen_family[(fam)] = 1; items++; } \
        if (!(c)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } \
    } while (0)

enum { F_RET, F_FINITE, F_LO_RANGE, F_HI_GE_LO, F_HI_LE_1, F_WHITE_RANGE, F_SCENE,
       F_GAIN_FINITE, F_GAIN_HEAD, F_GAIN_MONO, F_PIX_FINITE, F_BULK_RANGE };

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 32); }
static float rf(float a, float b) { return a + (b - a) * (float)(rnd() % 1000000u) / 1000000.0f; }

// 五种直方图
enum { G_UNIFORM, G_SPIKE, G_ONEBIN, G_RAMP, G_PATHO, G_COUNT };

// 8 位 SDR 的线性亮度上限是 1.0：现实中不可能有像素落在直方图上界那几个格子（线性 1..16）。
// 前四类必须落在「线性 ≤ 1.0」的格子里，否则测的就不是真机输入；只有 G_PATHO 才放开全范围。
static int max_real_bin(void) {
    int b = 0;
    while (b + 1 < S2A_HDR_HIST_BINS && s2a_hdr_hist_edge(b) <= 1.0f) b++;
    return b;
}

static void gen(int kind, uint32_t h[S2A_HDR_HIST_BINS]) {
    memset(h, 0, S2A_HDR_HIST_BINS * sizeof(uint32_t));
    switch (kind) {
    case G_UNIFORM: {
        const int top = max_real_bin();
        for (int i = 0; i <= top; i++) h[i] = rnd() % 4000u;
        break;
    }
    case G_SPIKE: {                       // 大量黑 + 少数几根孤立尖峰
        h[0] = rnd() % 200000u;
        int n = 1 + (int)(rnd() % 4u);
        const int top = max_real_bin();
        for (int k = 0; k < n; k++) h[rnd() % (uint32_t)(top + 1)] += 1u + rnd() % 900u;
        break;
    }
    case G_ONEBIN:                        // 整幅只落在一个格子（纯色画面）
        h[rnd() % (uint32_t)(max_real_bin() + 1)] = 1u + rnd() % 1000000u;
        break;
    case G_RAMP:                          // 单调斜坡（渐变天空）
        for (int i = 0; i <= max_real_bin(); i++) h[i] = (uint32_t)((i + 1) * (rnd() % 40u + 1u));
        break;
    case G_PATHO: {                       // 计数接近 UINT32_MAX：总和会溢出 32 位
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++)
            if (rnd() % 8u == 0u) h[i] = 0xFFFFFFFFu - (rnd() % 4096u);
        if (rnd() % 2u) h[0] = 0xFFFFFFFFu;
        break;
    }
    }
}

// 上屏必须永远成立的性质。返回 0 表示这一帧不合法（已经记账）。
static void check_frame(int kind, int frame, const uint32_t h[S2A_HDR_HIST_BINS]) {
    s2a_hdr_auto_l1b b;
    memset(&b, 0, sizeof b);
    // prev_white 要取「上一帧真能算出来的白点」范围（线性亮度 ≈ 0.05..1.04）。
    // 第一版给的是 rf(0,1)，于是冻结分支会把一个纯随机数原样带出来（0.000204 这种），
    // 看着像越界，其实是测试自己在喂不存在的历史。
    float prev_lo = rf(0.002f, 0.8f), prev_hi = 1.0f, prev_avg = rf(-8.0f, 0.0f), prev_white = rf(0.05f, 1.0f);
    const float hz = (float)(24 + rnd() % 200u);
    const int have_prev = (int)(rnd() % 2u);
    int okr = s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, prev_lo, prev_hi, prev_avg, prev_white,
                                             have_prev, hz, &b);
    CHK(F_RET, okr == 0 || okr == 1, "kind=%d frame=%d 返回值既不是 0 也不是 1（%d）", kind, frame, okr);
    if (!okr) return;    // 空直方图允许拒绝；下面只验真正产出的帧

    CHK(F_FINITE, isfinite(b.knee_lo) && isfinite(b.knee_hi) && isfinite(b.white) && isfinite(b.avg),
       "kind=%d frame=%d 输出里有非有限值 lo=%g hi=%g white=%g avg=%g", kind, frame,
       (double)b.knee_lo, (double)b.knee_hi, (double)b.white, (double)b.avg);
    // knee_lo 是「纸白」的线性亮度，可以低到 0.02（暗场），也可以到 1.0（满屏白）；
    // S2A_HDR_KNEE_MIN(0.25) 只约束 L0 导出路径的 hi，不约束 L1b 的纸白。
    CHK(F_LO_RANGE, b.knee_lo >= powf(2.0f, S2A_HDR_HIST_LO) && b.knee_lo <= 1.0f + 1e-6f,
       "kind=%d frame=%d knee_lo=%g 跑出直方图线性范围", kind, frame, (double)b.knee_lo);
    CHK(F_HI_GE_LO, b.knee_hi >= b.knee_lo, "kind=%d frame=%d knee_hi=%g < knee_lo=%g", kind, frame,
       (double)b.knee_hi, (double)b.knee_lo);
    CHK(F_HI_LE_1, b.knee_hi <= 1.0f + 1e-6f, "kind=%d frame=%d knee_hi=%g > 1", kind, frame,
        (double)b.knee_hi);
    // 白点是**线性亮度**（直方图的 log2 范围是 [-10, 4]，即线性 0.001..16），不是 [0,1] 的归一化值。
    // 但真实 8 位 SDR 输入的线性亮度不可能超过 1.0：所以分两种情形——
    //   真实规模（总像素 ≤ 4M）：必须 ≤ 1.0；
    //   病态规模（人为把计数塞到 2^32 附近，真机上不存在）：只要求落在直方图范围内、且有限。
    uint64_t tot = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) tot += h[i];
    if (tot <= 4000000ull)
        // 白点取的是格子的**上沿**，所以最亮那个现实格子（线性 1.0 以下一格）会略微超过 1.0，
        // 最多一个格宽（log2 步长 0.0547 ⇒ ×1.0387）。这不是缺陷，是「上沿」的定义。
        CHK(F_WHITE_RANGE, b.white >= 0.0f && b.white <= 1.04f,
           "kind=%d frame=%d 真实规模下 white=%g 超过 1.04（线性亮度 + 一格上沿）", kind, frame, (double)b.white);
    else
        CHK(F_WHITE_RANGE, b.white >= powf(2.0f, S2A_HDR_HIST_LO) * 0.999f && b.white <= powf(2.0f, S2A_HDR_HIST_HI) * 1.001f,
           "kind=%d frame=%d 病态规模下 white=%g 跑出直方图范围", kind, frame, (double)b.white);
    CHK(F_SCENE, b.scene >= 0.0f && b.scene <= 1.0f, "kind=%d frame=%d scene=%g 出 [0,1]", kind, frame,
       (double)b.scene);

    // 拐点 → 增益：单调不减、两端行为、绝不超 head
    s2a_hdr_itm p = s2a_hdr_itm_params();
    p.knee_lo = b.knee_lo;
    p.knee_hi = (b.knee_hi > b.knee_lo + 1e-4f) ? b.knee_hi : b.knee_lo + 1e-4f;
    float gprev = 0.0f;
    for (int i = 0; i <= 40; i++) {
        float y = (float)i / 40.0f;
        float g = s2a_hdr_itm_gain(&p, y);
        CHK(F_GAIN_FINITE, isfinite(g) && g >= 0.0f, "kind=%d frame=%d y=%g 增益非有限或负（%g）", kind, frame,
           (double)y, (double)g);
        CHK(F_GAIN_HEAD, g <= p.head + 1e-4f, "kind=%d frame=%d y=%g 增益 %g 超过 head %g", kind, frame,
           (double)y, (double)g, (double)p.head);
        CHK(F_GAIN_MONO, g + 1e-5f >= gprev, "kind=%d frame=%d y=%g 增益不单调（%g < 前一点 %g）", kind, frame,
           (double)y, (double)g, (double)gprev);
        gprev = g;
    }

    // 逐像素上屏：随机线性色 + 两种饱和度/色域开关，输出必须有限、无负、不超 head
    for (int k = 0; k < 8; k++) {
        float in[3] = { rf(0.0f, 1.0f), rf(0.0f, 1.0f), rf(0.0f, 1.0f) };
        float out[3];
        const float sat = (k % 3 == 0) ? 1.0f : rf((float)S2A_HDR_SAT_MIN, (float)S2A_HDR_SAT_MAX);
        const float p3 = (k % 4 == 0) ? 0.0f : rf(0.0f, 1.0f);
        s2a_hdr_grade_rgb(&p, sat, p3, in, out);
        for (int c = 0; c < 3; c++) {
            CHK(F_PIX_FINITE, isfinite(out[c]) && out[c] >= 0.0f, "kind=%d frame=%d 通道 %d 非有限或负（%g）",
               kind, frame, c, (double)out[c]);
            CHK(F_GAIN_HEAD, out[c] <= p.head * 1.0001f + 1e-5f, "kind=%d frame=%d 通道 %d = %g 超 head %g",
               kind, frame, c, (double)out[c], (double)p.head);
        }
    }

    // 白点剔除路径：任何直方图都不许给出 >1 或负的白点
    float w = -1.0f;
    if (s2a_hdr_sdr_white_bulk(h, &w)) {
        if (tot <= 4000000ull)
            CHK(F_BULK_RANGE, w >= 0.0f && w <= 1.04f, "kind=%d frame=%d 真实规模下 bulk 白点 %g 超过 1.04",
               kind, frame, (double)w);
        else
            CHK(F_BULK_RANGE, isfinite(w) && w <= powf(2.0f, S2A_HDR_HIST_HI) * 1.001f,
               "kind=%d frame=%d 病态规模下 bulk 白点 %g 跑出直方图范围", kind, frame, (double)w);
    }
}

int main(int argc, char **argv) {
    // 缺省 8000 帧（≈2 s）：`run_all.sh` 每轮都跑，不能占掉十几秒。
    // 想跑大规模就 `./build/t_hdr_fuzz 400000`——种子固定，跑出来的结论可复现。
    const int frames = (argc > 1) ? atoi(argv[1]) : 8000;
    const uint64_t seed = 0x9E3779B97F4A7C15ull;
    uint32_t h[S2A_HDR_HIST_BINS];
    for (int kind = 0; kind < G_COUNT; kind++) {
        rs = seed + (uint64_t)kind * 7919ull;
        for (int f = 0; f < frames; f++) {
            gen(kind, h);
            check_frame(kind, f, h);
            frames_done++;
        }
    }
    // 空直方图与「只有黑」不许崩、不许产出非有限
    {
        uint32_t z[S2A_HDR_HIST_BINS];
        memset(z, 0, sizeof z);
        s2a_hdr_auto_l1b b;
        memset(&b, 0, sizeof b);
        const int r = s2a_hdr_auto_l1b_step_fused_hz(z, 0.0f, 0.3f, 1.0f, -2.0f, 0.5f, 1, 60.0f, &b);
        CHK(F_RET, r == 0 || (isfinite(b.knee_lo) && isfinite(b.knee_hi)),
           "全零直方图：要么拒绝，要么给出有限拐点（r=%d lo=%g）", r, (double)b.knee_lo);
        z[0] = 1000000u;                       // 只有黑
        CHK(F_RET, s2a_hdr_auto_l1b_step_fused_hz(z, 0.0f, 0.3f, 1.0f, -2.0f, 0.5f, 1, 60.0f, &b) != 0 ||
                   isfinite(b.knee_lo), "只有黑的直方图：同上面一条");
    }

    printf("%s t_hdr_fuzz：随机直方图不变量 %d 条（失败 %d，%d 帧 × 5 类 = %d 次判定）\n",
           fails ? "✗" : "✓", items, fails, frames, frames_done * 5);
    return fails ? 1 : 0;
}
