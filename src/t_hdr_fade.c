// t_hdr_fade：淡出 / 黑场 / 淡入 的时间行为。
//
// 为什么单独一套：270 帧真实序列（硬切 + 交叉淡入 + 淡到黑 + 黑场 + 从黑淡入）量出两个缺陷——
//   1) 淡到黑的过程中画面整体等比变暗，白点跟着掉，knee_lo 被拖低，中调增益反而从 ×1.00 涨到 ×1.46
//      （越黑越亮，抽动感）；
//   2) 黑场之后回到画面，knee 从暗场落地值 0.02 慢慢爬 30 帧，先闪一下亮。
// 修法是「均匀变亮/变暗时整帧冻结，暗区里只要有动就冻结，跨出暗区当切镜一帧到位」。
// 这里把这条判据钉住：冻结必须真的不动、硬切必须不被误判成淡出、稳态星空必须仍然扩张。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("  ✗ %s\n", m); } } while (0)

// 一帧"正常内容"：一片中间调 + 一小片纸白 + 少量高光。scale 用作整体等比缩放（= 淡入淡出）。
static void make_frame(uint32_t hist[S2A_HDR_HIST_BINS], float scale) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    const float base[5] = {0.12f, 0.20f, 0.30f, 0.45f, 0.62f};
    for (int i = 0; i < 5; i++) hist[s2a_hdr_hist_bin(base[i] * scale)] = 15000;
    hist[s2a_hdr_hist_bin(0.832f * scale)] = 20000;   // 纸白（计数 ≥ 0.5%）
    hist[s2a_hdr_hist_bin(0.96f * scale)] = 300;      // 少量高光（< 0.5%，不参与白点判定）
}

typedef struct { float lo, hi, white, avg; int have; } st;

static void step(st *s, const uint32_t hist[S2A_HDR_HIST_BINS], float hz) {
    s2a_hdr_auto_l1b b;
    memset(&b, 0, sizeof b);
    if (!s2a_hdr_auto_l1b_step_fused_hz(hist, 0.0f, s->lo, s->hi, s->avg, s->white, s->have, hz, &b)) return;
    if (b.frozen) { s->avg = b.avg; return; }          // 冻结：只推进 avg（照抄 .inc 的语义）
    s->lo = b.knee_lo; s->hi = b.knee_hi; s->white = b.white; s->avg = b.avg; s->have = 1;
}

int main(void) {
    const float hz = 30.0f;
    uint32_t h[S2A_HDR_HIST_BINS];
    st s;

    // ── 1. 稳态：30 帧不动 ──
    make_frame(h, 1.0f); memset(&s, 0, sizeof s);
    step(&s, h, hz);
    float lo0 = s.lo;
    for (int i = 0; i < 30; i++) step(&s, h, hz);
    CHECK(fabsf(s.lo - lo0) < 1e-6f, "稳态 30 帧 knee_lo 不动");
    printf("  · 稳态 white=%.4f lo=%.4f（纸白输入 0.832）\n", s.white, s.lo);
    CHECK(fabsf(s.white - 0.832f) < S2A_HDR_HIST_BIN_STOP, "稳态白点 = 纸白（一个格宽内）");
    CHECK(fabsf(s.lo - s.white) < 1e-4f, "稳态 knee_lo = 纸白（纸白以下不动）");

    // ── 2. 淡出 40 帧：1.0 → 0.02 等比变暗 ──
    memset(&s, 0, sizeof s);
    make_frame(h, 1.0f); step(&s, h, hz);
    for (int i = 0; i < 5; i++) step(&s, h, hz);       // 先到稳态
    float fade_lo = s.lo;
    float max_dev = 0.0f; int frozen_n = 0;
    for (int i = 1; i <= 40; i++) {
        float k = powf(0.02f / 1.0f, (float)i / 40.0f);
        make_frame(h, k);
        float before = s.lo;
        s2a_hdr_auto_l1b b; memset(&b, 0, sizeof b);
        int ok = s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &b);
        if (ok && b.frozen) { frozen_n++; s.avg = b.avg; }
        else if (ok) { s.lo = b.knee_lo; s.hi = b.knee_hi; s.white = b.white; s.avg = b.avg; }
        float dev = fabsf(s.lo - fade_lo);
        if (dev > max_dev) max_dev = dev;
        (void)before;
    }
    CHECK(frozen_n >= 36, "淡出 40 帧里至少 36 帧被冻结");
    CHECK(max_dev < 1e-5f, "淡出全程 knee_lo 位移 < 1e-5（不随亮度掉）");

    // ── 3. 硬切到暗场景：不许被当成淡出 ──
    memset(&s, 0, sizeof s);
    make_frame(h, 1.0f); step(&s, h, hz);
    for (int i = 0; i < 5; i++) step(&s, h, hz);
    uint32_t hd[S2A_HDR_HIST_BINS];
    memset(hd, 0, sizeof hd);
    for (int i = 0; i < 5; i++) hd[s2a_hdr_hist_bin(0.05f + 0.02f * (float)i)] = 15000;
    hd[s2a_hdr_hist_bin(0.21f)] = 20000;
    s2a_hdr_auto_l1b b; memset(&b, 0, sizeof b);
    int ok = s2a_hdr_auto_l1b_step_fused_hz(hd, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &b);
    CHECK(ok == 1, "硬切帧可用");
    CHECK(b.frozen == 0, "硬切到暗场景不被判成淡出（不冻结）");
    CHECK(b.knee_lo < s.lo - 0.05f, "硬切后 knee_lo 往新白点走（不是原地不动）");

    // ── 4. 空直方图（全黑）：返回 0，调用方保持 ──
    memset(h, 0, sizeof h);
    float keep_lo = s.lo, keep_hi = s.hi, keep_w = s.white;
    memset(&b, 0, sizeof b);
    ok = s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &b);
    CHECK(ok == 0, "全黑帧返回 0（不可测）");
    CHECK(s.lo == keep_lo && s.hi == keep_hi && s.white == keep_w, "调用方保持上一帧（未被改动）");

    // ── 5. 从黑回到画面：跨出暗区一帧到位 ──
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 20; i++) { memset(h, 0, sizeof h); s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &b); if (b.frozen) s.avg = b.avg; }
    make_frame(h, 0.05f);   // 很暗但可测
    step(&s, h, hz);
    step(&s, h, hz);
    float dark_lo = s.lo;
    make_frame(h, 1.0f);    // 回到原画面
    memset(&b, 0, sizeof b);
    ok = s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &b);
    CHECK(ok == 1, "回到画面可用");
    CHECK(b.knee_lo > dark_lo + 0.1f, "跨出暗区 knee_lo 一帧抬高（当切镜）");
    CHECK(b.knee_lo > 0.5f, "一帧就接近纸白（不是从 0.02 爬）");

    // ── 6. 稳态星空：仍然扩张到暗场落地 0.02（冻结不能把它也冻住）──
    memset(&s, 0, sizeof s);
    memset(h, 0, sizeof h);
    h[s2a_hdr_hist_bin(0.0f)] = 99900;
    h[s2a_hdr_hist_bin(0.72f)] = 100;
    for (int i = 0; i < 80; i++) {
        s2a_hdr_auto_l1b x; memset(&x, 0, sizeof x);
        if (!s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &x)) continue;
        if (x.frozen) { s.avg = x.avg; continue; }
        s.lo = x.knee_lo; s.hi = x.knee_hi; s.white = x.white; s.avg = x.avg; s.have = 1;
    }
    CHECK(fabsf(s.lo - S2A_HDR_DARK_FLOOR) < 2e-3f, "稳态星空仍然落到暗场落地 0.02");

    // ── 7. 淡入（升）：等高比升亮也冻结，避免渐变期间重定目标 ──
    memset(&s, 0, sizeof s);
    make_frame(h, 1.0f); step(&s, h, hz);
    for (int i = 0; i < 5; i++) step(&s, h, hz);
    float in_lo = s.lo; int rise_frozen = 0;
    for (int i = 1; i <= 30; i++) {
        float k = powf(1.0f / 0.05f, (float)i / 30.0f) * 0.05f;
        make_frame(h, k);
        s2a_hdr_auto_l1b x; memset(&x, 0, sizeof x);
        if (!s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hz, &x)) continue;
        if (x.frozen) { rise_frozen++; s.avg = x.avg; continue; }
        s.lo = x.knee_lo; s.hi = x.knee_hi; s.white = x.white; s.avg = x.avg;
    }
    CHECK(rise_frozen >= 20, "淡入期间至少 20 帧冻结（不追着变亮重定目标）");
    printf("  · 淡入 rise_frozen=%d lo %.4f → %.4f\n", rise_frozen, in_lo, s.lo);
    CHECK(fabsf(s.lo - in_lo) < 0.08f, "淡入期间 knee_lo 基本不动");

    // ── 8. 帧率无关：同一段「每秒 4 stop」的淡出，在 30/60/120 Hz 下都必须冻结 ──
    // 旧判据写的是 stop/帧（0.05..0.40），120 Hz 时 4 stop/s 只有 0.033/帧，落在窗口之下
    // ⇒ 完全不冻结，淡出时中调照样越黑越亮；反过来 30 Hz 的 1 秒淡 5 stop = 0.167/帧 反而更宽松。
    // S2Arm 有 30/60/120 三档，所以判据换成 stop/秒（S2A_HDR_FADE_MIN_S/MAX_S）。
    {
        const float rates_hz[3] = {30.0f, 60.0f, 120.0f};
        int frozen_by_hz[3] = {0, 0, 0};
        float drift_by_hz[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            const float hzk = rates_hz[k];
            const int frames = (int)hzk;                  // 淡出持续 1 秒
            memset(&s, 0, sizeof s);
            make_frame(h, 1.0f); step(&s, h, hzk);
            for (int i = 0; i < 5; i++) step(&s, h, hzk);
            float lo_start = s.lo;
            for (int i = 1; i <= frames; i++) {
                // 每秒 4 stop ⇒ 总落差 4 stop，均匀分到 frames 帧
                float k_scale = powf(2.0f, -4.0f * (float)i / (float)frames);
                make_frame(h, k_scale);
                s2a_hdr_auto_l1b x; memset(&x, 0, sizeof x);
                if (!s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, s.lo, s.hi, s.avg, s.white, s.have, hzk, &x)) continue;
                if (x.frozen) { frozen_by_hz[k]++; s.avg = x.avg; continue; }
                s.lo = x.knee_lo; s.hi = x.knee_hi; s.white = x.white; s.avg = x.avg;
            }
            drift_by_hz[k] = fabsf(s.lo - lo_start);
        }
        printf("  · 帧率无关：30Hz 冻 %d/30（漂 %.4f） 60Hz 冻 %d/60（漂 %.4f） 120Hz 冻 %d/120（漂 %.4f）\n",
               frozen_by_hz[0], drift_by_hz[0], frozen_by_hz[1], drift_by_hz[1], frozen_by_hz[2], drift_by_hz[2]);
        // 允许末帧被夹（跨出时按切镜处理），所以判定是「帧数 − 2」而不是「全部帧」
        for (int k = 0; k < 3; k++) {
            char msg[96];
            snprintf(msg, sizeof msg, "%.0f Hz：4 stop/s 淡出冻结 %d/%d 帧", (double)rates_hz[k],
                     frozen_by_hz[k], (int)rates_hz[k]);
            CHECK(frozen_by_hz[k] >= (int)rates_hz[k] - 2, msg);
        }
        CHECK(drift_by_hz[0] < 1e-4f && drift_by_hz[1] < 1e-4f && drift_by_hz[2] < 1e-4f,
              "冻结期间 knee_lo 三档都不动");
    }

    printf("%s t_hdr_fade：淡出/黑场/淡入/硬切/星空 %d 项 → %s\n",
           fails ? "✗" : "✓", checks, fails ? "有失败" : "全过");
    return fails ? 1 : 0;
}
