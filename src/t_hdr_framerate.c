// t_hdr_framerate：平滑的时间常数必须与帧率无关。
//
// 为什么单独测：S2A_HDR_SMOOTH 是「每帧走多少」，直接用会让 120 Hz 收敛比 30 Hz 快 4 倍。
// S2Arm 有 30/60/120 三档，切换档位时「平滑速度变了」在画面上就是抽动/掉帧感。
// 判据不是「系数相等」（当然不等），而是**同样的物理时间收敛到同样的进度**。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr_auto_l1.h"

static int fails = 0, total = 0;
static void ck(int cond, const char *what, double got, double want) {
    total++;
    if (!cond) { fails++; printf("  ✗ %s（实测 %.6f，期望 %.6f）\n", what, got, want); }
}

// 同一份直方图的「未平滑目标」：have_prev=0 时 step 直接采用目标，不做平滑
static float target_lo_of(const uint32_t hist[S2A_HDR_HIST_BINS], float *t_hi) {
    s2a_hdr_auto_l1b t;
    memset(&t, 0, sizeof t);
    if (!s2a_hdr_auto_l1b_step_white_hz(hist, 0.0f, 0.0f, 0.0f, 0.0f, 0, 60.0f, &t)) return -1.0f;
    if (t_hi) *t_hi = t.knee_hi;
    return t.knee_lo;
}

// 从「亮场景」的历史出发跑到给定帧数，返回最后 knee_lo；frames_to_95 = 收敛到 95% 用了多少帧
static float run_l1b(const uint32_t hist[S2A_HDR_HIST_BINS], float target, int frames, float hz,
                     float *out_hi, int *frames_to_95) {
    int okc = 0;
    // prev_avg 必须等于本帧重心，scene_w 才会是 0；否则阶跃被判成「场景切换」直接全速一帧到位，
    // 时间常数根本测不出来（第一版就是这么写错的）。
    float avg = s2a_hdr_hist_avg(hist, &okc);
    float lo = 0.80f, hi = 1.00f;
    int have = 1, hit = -1;
    s2a_hdr_auto_l1b st;
    memset(&st, 0, sizeof st);
    float span = 0.80f - target;
    for (int f = 0; f < frames; f++) {
        if (!s2a_hdr_auto_l1b_step_white_hz(hist, 0.0f, lo, hi, avg, have, hz, &st)) return -1.0f;
        lo = st.knee_lo; hi = st.knee_hi; have = 1;
        if (hit < 0 && span > 1e-6f && (0.80f - lo) / span >= 0.95f) hit = f + 1;
    }
    if (out_hi) *out_hi = hi;
    if (frames_to_95) *frames_to_95 = hit;
    return lo;
}

int main(void) {
    // 1) 系数本身：60 Hz 原值、单调、边界
    float c60 = s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 60.0f);
    printf("  S2A_HDR_SMOOTH=%.4f，60 Hz 系数=%.6f\n", (double)S2A_HDR_SMOOTH, (double)c60);
    ck(fabsf(c60 - S2A_HDR_SMOOTH) < 1e-6f, "60 Hz 系数应等于常数本身", c60, S2A_HDR_SMOOTH);
    // 方向：帧率越低，每帧要走得越多（每秒帧数少），所以 30 Hz 系数更大、120 Hz 更小
    ck(s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 30.0f) > c60, "30 Hz 每帧系数应更大", s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 30.0f), c60);
    ck(s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 120.0f) < c60, "120 Hz 每帧系数应更小", s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 120.0f), c60);
    float prev = 2.0f;
    int mono = 1;
    for (float hz = 1.0f; hz <= 240.0f; hz += 1.0f) {
        float c = s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, hz);
        if (!(c < prev)) mono = 0;
        prev = c;
    }
    ck(mono, "系数应随帧率严格递减（1..240 Hz 全程）", mono, 1);

    // 2) 非法帧率退回参考帧率，不许出 NaN
    ck(fabsf(s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 0.0f) - c60) < 1e-6f, "hz=0 应退回 60 Hz", s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 0.0f), c60);
    ck(isfinite(s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, NAN)), "hz=NaN 结果必须有限", s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, NAN), 0);
    ck(s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, -5.0f) == c60, "hz<0 应退回 60 Hz", s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, -5.0f), c60);
    ck(s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 1e9f) <= 1.0f, "hz 超大也不许超过 1", s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 1e9f), 1.0f);

    // 3) 关键判据：同样的秒数内收敛进度相同
    //    30 Hz 跑 1.0 s = 30 帧；60 Hz = 60 帧；120 Hz = 120 帧。三者 knee_lo 差异应 < 0.2%
    uint32_t hist[S2A_HDR_HIST_BINS];
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < 4000; i++) hist[s2a_hdr_hist_bin(0.05f + 0.02f * (float)(i % 4))]++;
    float tgt = target_lo_of(hist, NULL);
    printf("  该帧的未平滑目标 knee_lo=%.6f（从 0.80 收敛过来）\n", (double)tgt);
    float hi30 = 0, hi60 = 0, hi120 = 0;
    float lo30 = run_l1b(hist, tgt, 30, 30.0f, &hi30, NULL);
    float lo60 = run_l1b(hist, tgt, 60, 60.0f, &hi60, NULL);
    float lo120 = run_l1b(hist, tgt, 120, 120.0f, &hi120, NULL);
    printf("  1.0 s 后 knee_lo：30 Hz %.6f / 60 Hz %.6f / 120 Hz %.6f\n", (double)lo30, (double)lo60, (double)lo120);
    float d1 = fabsf(lo30 - lo60), d2 = fabsf(lo60 - lo120);
    ck(d1 < 0.002f * (lo30 > 1e-3f ? lo30 : 1.0f), "30 vs 60 Hz 同秒数结果应一致（<0.2%）", d1, 0);
    ck(d2 < 0.002f * (lo60 > 1e-3f ? lo60 : 1.0f), "60 vs 120 Hz 同秒数结果应一致（<0.2%）", d2, 0);

    // 4) 收敛所需的**帧数**按帧率成比例（秒数相同）
    int n30 = 0, n60 = 0, n120 = 0;
    run_l1b(hist, tgt, 240, 30.0f, NULL, &n30);
    run_l1b(hist, tgt, 480, 60.0f, NULL, &n60);
    run_l1b(hist, tgt, 960, 120.0f, NULL, &n120);
    printf("  收敛到 95%% 的帧数：30 Hz %d / 60 Hz %d / 120 Hz %d\n", n30, n60, n120);
    ck(n30 > 0 && n60 > 0 && n120 > 0, "三档都应能收敛", n30 + n60 + n120, 0);
    if (n30 > 0 && n60 > 0 && n120 > 0) {
        float r1 = (float)n60 / (float)n30, r2 = (float)n120 / (float)n60;
        printf("  帧数比：60/30=%.3f（期望≈2）、120/60=%.3f（期望≈2）\n", (double)r1, (double)r2);
        ck(fabsf(r1 - 2.0f) < 0.25f, "帧数比 60/30 应≈2", r1, 2.0f);
        ck(fabsf(r2 - 2.0f) < 0.25f, "帧数比 120/60 应≈2", r2, 2.0f);
    }

    // 5) 旧调用点（不传 hz）必须与 60 Hz 路径逐位相同
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < 3000; i++) hist[s2a_hdr_hist_bin(0.30f + 0.01f * (float)(i % 5))]++;
    s2a_hdr_auto_l1b a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    s2a_hdr_auto_l1b_step_white(hist, 0.0f, 0.6f, 0.9f, -1.0f, 1, &a);
    s2a_hdr_auto_l1b_step_white_hz(hist, 0.0f, 0.6f, 0.9f, -1.0f, 1, 60.0f, &b);
    ck(memcmp(&a, &b, sizeof a) == 0, "旧签名必须与 60 Hz 路径逐位相同", 0, 0);

    // 6) 单帧跳变不许因为高帧率变大：120 Hz 一帧的进度 < 30 Hz 一帧
    float s30 = s2a_hdr_auto_step_l1b_hz(0.10f, 0.90f, 0.0f, 30.0f) - 0.10f;
    float s120 = s2a_hdr_auto_step_l1b_hz(0.10f, 0.90f, 0.0f, 120.0f) - 0.10f;
    printf("  单帧进度：30 Hz %.6f / 120 Hz %.6f\n", (double)s30, (double)s120);
    ck(s30 > s120, "30 Hz 单帧进度应大于 120 Hz（同样的时间常数）", s30, s120);

    // ---- 帧率估计（s2a_hdr_hz_from_dt）：从两次回调的时间差反推 Hz ----
    // 上屏路径没有「层刷新率」可读，只能这样估；估错就等价于用错 coef。
    {
        float hz = 0.0f;
        for (int i = 0; i < 200; i++) hz = s2a_hdr_hz_from_dt(1.0 / 60.0, hz);
        ck(hz > 59.0f && hz < 61.0f, "稳态 60 Hz 应估回 60", hz, 60.0f);

        hz = 0.0f;
        for (int i = 0; i < 200; i++) hz = s2a_hdr_hz_from_dt(1.0 / 120.0, hz);
        ck(hz > 119.0f && hz < 121.0f, "稳态 120 Hz 应估回 120", hz, 120.0f);

        hz = 0.0f;
        for (int i = 0; i < 200; i++) hz = s2a_hdr_hz_from_dt(1.0 / 30.0, hz);
        ck(hz > 29.0f && hz < 31.0f, "稳态 30 Hz 应估回 30", hz, 30.0f);

        // 60 → 30 掉帧：应该在十几帧内跟过去（时间常数 8 帧），而不是一直按 60 算
        hz = 0.0f;
        for (int i = 0; i < 200; i++) hz = s2a_hdr_hz_from_dt(1.0 / 60.0, hz);
        int frames = 0;
        while (hz > 40.0f && frames < 200) { hz = s2a_hdr_hz_from_dt(1.0 / 30.0, hz); frames++; }
        ck(frames > 0 && frames < 30, "60→30 掉帧应在 30 帧内跟上", (float)frames, 30.0f);

        // 异常 dt：0 / 负 / 超大 / 无穷不产生非法 Hz（非法值会让 coef 变成 NaN，画面整帧不变）
        ck(s2a_hdr_hz_from_dt(0.0, 0.0f) == S2A_HDR_SMOOTH_HZ, "dt=0 退回参考帧率", s2a_hdr_hz_from_dt(0.0, 0.0f), S2A_HDR_SMOOTH_HZ);
        ck(s2a_hdr_hz_from_dt(-0.01, 0.0f) == S2A_HDR_SMOOTH_HZ, "负 dt 退回参考帧率", s2a_hdr_hz_from_dt(-0.01, 0.0f), S2A_HDR_SMOOTH_HZ);
        ck(s2a_hdr_hz_from_dt(5.0, 0.0f) == S2A_HDR_SMOOTH_HZ, "5 秒停帧退回参考帧率", s2a_hdr_hz_from_dt(5.0, 0.0f), S2A_HDR_SMOOTH_HZ);
        ck(s2a_hdr_hz_from_dt(1.0 / 1000.0, 0.0f) == S2A_HDR_HZ_MAX, "1000 Hz 夹到上限", s2a_hdr_hz_from_dt(1.0 / 1000.0, 0.0f), S2A_HDR_HZ_MAX);
        ck(s2a_hdr_hz_from_dt(1.0 / 5.0, 0.0f) == S2A_HDR_HZ_MIN, "5 Hz 夹到下限", s2a_hdr_hz_from_dt(1.0 / 5.0, 0.0f), S2A_HDR_HZ_MIN);

        // 估出来的 Hz 直接进 coef：30 Hz 估值的系数必须等于直接用 30 算的系数
        float hz30 = 0.0f;
        for (int i = 0; i < 200; i++) hz30 = s2a_hdr_hz_from_dt(1.0 / 30.0, hz30);
        {
            double a = (double)s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, hz30);
            double b = (double)s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, 30.0f);
            ck(fabs(a - b) < 2e-3, "估计值的系数与直接给 30 Hz 一致", a, b);
        }
    }

    // 白点死区与帧率无关：内容微动（±0.05 stop，小于 S2A_HDR_WHITE_DEADBAND = 1.5 格 ≈ 0.082 stop）
    //   不应该让白点/拐点动。死区判的是「两帧白点的 stop 差」，与帧率无关，但平滑系数是按帧率
    //   归一化的，所以要证明三档下都真的不动（否则高刷档会变成慢速漂移）。
    {
        const float hzs[3] = {30.0f, 60.0f, 120.0f};
        for (int k = 0; k < 3; k++) {
            uint32_t h[S2A_HDR_HIST_BINS];
            s2a_hdr_auto_l1b b;
            float lo = 0.0f, hi = 0.0f, white = 0.0f, avg = 0.0f;
            int have = 0, moved = 0, frozen = 0;
            for (int i = 0; i < 60; i++) {
                memset(h, 0, sizeof h);
                // 内容微动：纸白在 0.800 与 0.800*2^0.05 之间抖，中间调同步微动
                float jitter = (i & 1) ? 0.05f : 0.0f;
                h[s2a_hdr_hist_bin(0.20f)] = 15000;
                h[s2a_hdr_hist_bin(0.32f)] = 15000;
                h[s2a_hdr_hist_bin(0.800f * powf(2.0f, jitter))] = 20000;
                memset(&b, 0, sizeof b);
                if (!s2a_hdr_auto_l1b_step_fused_hz(h, 0.0f, lo, hi, avg, white, have, hzs[k], &b)) continue;
                if (b.frozen) frozen++;
                if (have && fabsf(b.knee_lo - lo) > 1e-6f) moved++;
                if (have && fabsf(b.white - white) > 1e-6f) moved++;
                if (!b.frozen) { lo = b.knee_lo; hi = b.knee_hi; white = b.white; }
                avg = b.avg;
                have = 1;
            }
            char msg[96];
            snprintf(msg, sizeof msg, "%.0f Hz：死区内的白点抖动不让 knee/白点移动", (double)hzs[k]);
            ck(moved == 0, msg, (float)moved, 0.0f);
        }
    }

    printf("%s t_hdr_framerate：帧率归一化 %d 项（失败 %d）\n", fails ? "✗" : "✓", total, fails);
    return fails ? 1 : 0;
}
