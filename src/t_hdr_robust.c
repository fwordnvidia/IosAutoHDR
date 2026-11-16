// 鲁棒性：参数层吃进垃圾输入时不许产出 NaN/Inf，也不许把下标算出界。
// 触发场景在真机上并不稀奇：解码失败的黑帧、DMA 里的垃圾像素、驱动给出的 inf、
// 以及「上一帧状态还没初始化就拿来用」。一条 NaN 进了 knee 就会污染整帧上屏。
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static int finite3(float a, float b, float c) { return isfinite(a) && isfinite(b) && isfinite(c); }

int main(void) {
    puts("t_hdr_robust：NaN / Inf / 溢出 / 未初始化状态下的行为");

    // ---- 1. 直方图下标 ----
    const float bad[] = {NAN, INFINITY, -INFINITY, 0.0f, -1.0f, 1e-30f, 1e-45f, 1e30f, -1e30f, 65504.0f};
    int lo_bad = 0, hi_bad = 0;
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        int b = s2a_hdr_hist_bin(bad[i]);
        if (b < 0) lo_bad++;
        if (b >= S2A_HDR_HIST_BINS) hi_bad++;
    }
    CHECK(lo_bad == 0 && hi_bad == 0, "s2a_hdr_hist_bin 出界：%d 个 <0，%d 个 >=%d", lo_bad, hi_bad, S2A_HDR_HIST_BINS);

    // ---- 2. 全零直方图（纯黑帧）----
    uint32_t hist[S2A_HDR_HIST_BINS];
    memset(hist, 0, sizeof hist);
    s2a_hdr_auto_l1 r; memset(&r, 0, sizeof r);
    int ok1 = s2a_hdr_auto_l1_step(hist, 1.0f, 0.0f, 0, &r);
    CHECK(ok1 == 0, "纯黑帧应当返回 0（调用方保持上一帧），实得 %d", ok1);
    s2a_hdr_auto_l1b b; memset(&b, 0, sizeof b);
    int ok2 = s2a_hdr_auto_l1b_step(hist, 0.5f, 1.0f, 0.0f, 0, &b);
    CHECK(ok2 == 0, "纯黑帧 L1b 应当返回 0，实得 %d", ok2);
    CHECK(finite3(r.knee_hi, r.white, r.avg), "纯黑帧产出了 NaN/Inf：hi=%g white=%g avg=%g", r.knee_hi, r.white, r.avg);
    CHECK(finite3(b.knee_lo, b.knee_hi, b.avg), "纯黑帧 L1b 产出了 NaN/Inf：lo=%g hi=%g avg=%g", b.knee_lo, b.knee_hi, b.avg);

    // ---- 3. 计数全满（uint32 溢出压力）----
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0xffffffffu;
    memset(&r, 0, sizeof r);
    int ok3 = s2a_hdr_auto_l1_step(hist, 1.0f, 0.0f, 0, &r);
    memset(&b, 0, sizeof b);
    int ok4 = s2a_hdr_auto_l1b_step(hist, 0.5f, 1.0f, 0.0f, 0, &b);
    CHECK(ok3 == 1 && ok4 == 1, "计数全满时应当正常出结果（%d/%d）", ok3, ok4);
    CHECK(finite3(r.knee_hi, r.white, r.avg), "计数全满产出了 NaN/Inf：hi=%g white=%g avg=%g", r.knee_hi, r.white, r.avg);
    CHECK(r.knee_hi >= 0.0f && r.knee_hi <= 4.0f, "计数全满时 knee_hi=%g 越界", r.knee_hi);
    CHECK(b.knee_lo <= b.knee_hi && b.knee_lo >= 0.0f, "计数全满时 L1b 区间反了：lo=%g hi=%g", b.knee_lo, b.knee_hi);

    // ---- 4. 未初始化 / 垃圾的上一帧状态 ----
    memset(hist, 0, sizeof hist);
    for (int i = 100; i < 140; i++) hist[i] = 100;        // 一段正常内容
    const float bad_prev[] = {NAN, INFINITY, -INFINITY, -5.0f, 1e9f, 0.0f};
    int bad_out = 0;
    for (unsigned i = 0; i < sizeof bad_prev / sizeof *bad_prev; i++) {
        memset(&r, 0, sizeof r);
        s2a_hdr_auto_l1_step(hist, bad_prev[i], bad_prev[i], 1, &r);
        if (!finite3(r.knee_hi, r.white, r.avg)) bad_out++;
        memset(&b, 0, sizeof b);
        s2a_hdr_auto_l1b_step(hist, bad_prev[i], bad_prev[i], bad_prev[i], 1, &b);
        if (!finite3(b.knee_lo, b.knee_hi, b.avg)) bad_out++;
    }
    CHECK(bad_out == 0, "上一帧状态是 NaN/Inf/负数时产出了 %d 次 NaN/Inf", bad_out);

    // ---- 5. 随机 fuzz：计数按对数均匀分布，含极端值 ----
    unsigned rs = 2024u;
    int nan_out = 0, range_out = 0;
    for (int it = 0; it < 200000; it++) {
        rs = rs * 1103515245u + 12345u;
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
            unsigned r2 = rs = rs * 1103515245u + 12345u;
            if ((r2 & 7) == 0) hist[i] = 0;
            else if ((r2 & 63) == 0) hist[i] = 0xffffffffu;
            else hist[i] = (r2 >> 8) & 0xffff;
        }
        memset(&r, 0, sizeof r);
        s2a_hdr_auto_l1_step(hist, 1.0f, 0.0f, 0, &r);
        memset(&b, 0, sizeof b);
        s2a_hdr_auto_l1b_step(hist, 0.5f, 1.0f, 0.0f, 0, &b);
        if (!finite3(r.knee_hi, r.white, r.avg) || !finite3(b.knee_lo, b.knee_hi, b.avg)) nan_out++;
        if (r.knee_hi < 0.0f || r.knee_hi > 4.0f || b.knee_lo < 0.0f || b.knee_hi < b.knee_lo) range_out++;
    }
    CHECK(nan_out == 0, "fuzz 20 万次出现 %d 次 NaN/Inf", nan_out);
    CHECK(range_out == 0, "fuzz 20 万次出现 %d 次区间越界/反转", range_out);

    // ---- 6. 像素侧：垃圾输入进到线性化 ----
    const float px[] = {NAN, INFINITY, -INFINITY, -1.0f, 0.0f, 1.0f, 1e30f};
    int px_bad = 0;
    for (unsigned i = 0; i < sizeof px / sizeof *px; i++) {
        float lin = s2a_hdr_to_linear(px[i]);
        if (!isfinite(lin) || lin < 0.0f) px_bad++;
    }
    CHECK(px_bad == 0, "s2a_hdr_to_linear 对垃圾输入产出了 %d 次非有限/负值", px_bad);

    // ---- 7. 上屏编码：>head 的值也被压回 [0,1] ----
    int enc_bad = 0;
    for (float v = 0.0f; v <= 1.0f; v += 0.005f) {
        float e = s2a_hdr_to_srgb(v);
        if (!(e >= 0.0f && e <= 1.0f)) enc_bad++;
    }
    CHECK(enc_bad == 0, "s2a_hdr_to_srgb 在 0..1 范围上产出 %d 次越界", enc_bad);
    // >1 的线性值编码后本来就 >1（显示参考白之上），上屏由调用方按 head 归一，这里只要求单调
    int mono_bad = 0; float prev_e = -1.0f;
    for (float v = 0.0f; v <= 8.0f; v += 0.01f) { float e = s2a_hdr_to_srgb(v); if (e < prev_e) mono_bad++; prev_e = e; }
    CHECK(mono_bad == 0, "s2a_hdr_to_srgb 在 0..8 上不单调（%d 处）", mono_bad);

    if (fails) { printf("t_hdr_robust：%d 项失败\n", fails); return 1; }
    printf("✓ t_hdr_robust：下标 / 空直方图 / 溢出 / 垃圾状态 / 20 万次 fuzz %d 项 → 全过\n", checks);
    return 0;
}
