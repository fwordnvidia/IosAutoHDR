// t_hdr_letterbox：信箱/黑边与新出现的「带噪声的黑边」对分析的影响。
//
// 为什么单独一套：2.35:1 的内容放在 16:9 缓冲里，或者影片在两种画幅间切换，是极常见的场景；
// 真实播放链路的「黑」往往是 6~20 号码值（编码噪声、limited-range 误标），不是纯 0。
// 这一套钉住三件事：
//   1) **决定输出的量**（白点 → L1b 拐点）与画幅无关：带不带黑边、黑边脏不脏，结果逐位相同；
//   2) 用白点做的切镜判据在「整幅 ↔ 信箱」之间给 0 权重（不会因为黑边出现就当成切镜）；
//   3) 已知行为：`hist_avg`（重心，-9.5 以上才计入）在黑边带噪声时会被拉低约 2 stop ——
//      这是刻意保留的取舍（把门槛提到 -6 会整片丢掉星空的星点），用断言把它钉住，
//      将来若改口径，这条会红。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto_l1.h"

static int total, fails;

static void ck(int ok, const char *msg, float got, float want) {
    total++;
    if (!ok) {
        fails++;
        printf("  ✗ %s：got %.6f want %.6f\n", msg, got, want);
    }
}

// 同一套内容分布，content_frac 的像素是画面，其余是「黑」：
//   bar_code = -1 表示纯 0，否则按 8 位码值 bar_code 填（模拟编码噪声/抬黑）
static void make(uint32_t h[S2A_HDR_HIST_BINS], float content_frac, int bar_code) {
    memset(h, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    const uint32_t n = 120000;
    const uint32_t lit = (uint32_t)((float)n * content_frac);
    h[s2a_hdr_hist_bin(0.20f)] += lit * 40 / 100;
    h[s2a_hdr_hist_bin(0.35f)] += lit * 30 / 100;
    h[s2a_hdr_hist_bin(0.832f)] += lit * 28 / 100;
    h[s2a_hdr_hist_bin(0.97f)] += lit * 2 / 100;
    if (bar_code < 0) {
        h[0] += n - lit;
    } else {
        float lin = s2a_hdr_to_linear((float)bar_code / 255.0f);
        h[s2a_hdr_hist_bin(lin)] += n - lit;
    }
}

typedef struct { float white, bulk, lo, hi, avg; int have; } res;

static res analyze(uint32_t h[S2A_HDR_HIST_BINS]) {
    res r; memset(&r, 0, sizeof r);
    float w = 0.0f, wb = 0.0f;
    r.have = s2a_hdr_sdr_white(h, &w) ? 1 : 0;
    r.white = w;
    r.bulk = s2a_hdr_sdr_white_bulk(h, &wb) ? wb : w;
    r.avg = s2a_hdr_hist_avg(h, NULL);
    s2a_hdr_auto_l1b b; memset(&b, 0, sizeof b);
    if (s2a_hdr_auto_l1b_step_fused(h, r.bulk, 0.55f, 1.0f, 0.0f, 0.0f, 0, &b)) {
        r.lo = b.knee_lo; r.hi = b.knee_hi;
    }
    return r;
}

int main(void) {
    uint32_t full_h[S2A_HDR_HIST_BINS], lb_h[S2A_HDR_HIST_BINS];
    make(full_h, 1.00f, -1);
    res full = analyze(full_h);

    puts("画幅无关性：白点与 L1b 拐点不能因为黑边而变");
    const float fracs[3] = {0.85f, 0.74f, 0.60f};   // 16:10 / 2.35:1 / 2.76:1 大概
    for (int i = 0; i < 3; i++) {
        make(lb_h, fracs[i], -1);
        res r = analyze(lb_h);
        char msg[96];
        snprintf(msg, sizeof msg, "干净黑边 %.2f：白点与满幅相同", (double)fracs[i]);
        ck(r.white == full.white, msg, r.white, full.white);
        snprintf(msg, sizeof msg, "干净黑边 %.2f：bulk 白点相同", (double)fracs[i]);
        ck(r.bulk == full.bulk, msg, r.bulk, full.bulk);
        snprintf(msg, sizeof msg, "干净黑边 %.2f：knee_lo 相同", (double)fracs[i]);
        ck(fabsf(r.lo - full.lo) < 1e-6f, msg, r.lo, full.lo);
        snprintf(msg, sizeof msg, "干净黑边 %.2f：knee_hi 相同", (double)fracs[i]);
        ck(fabsf(r.hi - full.hi) < 1e-6f, msg, r.hi, full.hi);
    }

    puts("脏黑边（编码噪声 / limited-range 误标）也不能改白点");
    {
        const int codes[3] = {6, 14, 20};
        for (int i = 0; i < 3; i++) {
            make(lb_h, 0.74f, codes[i]);
            res r = analyze(lb_h);
            char msg[96];
            snprintf(msg, sizeof msg, "黑边码值 %d：白点仍与满幅相同", codes[i]);
            ck(r.white == full.white, msg, r.white, full.white);
            snprintf(msg, sizeof msg, "黑边码值 %d：knee_lo 仍相同", codes[i]);
            ck(fabsf(r.lo - full.lo) < 1e-6f, msg, r.lo, full.lo);
        }
    }

    puts("已知行为：重心会被脏黑边拉低（刻意保留，改成 -6 会丢星空）");
    {
        make(lb_h, 0.74f, 6);
        res r = analyze(lb_h);
        ck(fabsf(r.avg - full.avg) > 1.0f, "码值 6 的黑边把重心拉低 >1 stop（钉住现状）",
           r.avg, full.avg);
        make(lb_h, 0.74f, -1);
        res r2 = analyze(lb_h);
        ck(fabsf(r2.avg - full.avg) < 1e-6f, "干净黑边不改重心（纯黑不进重心）", r2.avg, full.avg);
    }

    puts("画幅切换不该被当成切镜（用白点一路判）");
    {
        // 整幅 ↔ 信箱：白点不动 → 白点路的权重必须是 0
        float w_full = 0.0f, w_lb = 0.0f;
        s2a_hdr_sdr_white_bulk(full_h, &w_full);
        make(lb_h, 0.74f, 14);
        s2a_hdr_sdr_white_bulk(lb_h, &w_lb);
        float w_lb2 = 0.0f;
        s2a_hdr_sdr_white_bulk(lb_h, &w_lb2);
        float ww = s2a_hdr_scene_weight_white(w_full, w_lb2);
        ck(ww == 0.0f, "整幅 → 脏黑边信箱：白点路权重为 0", ww, 0.0f);
        (void)w_lb;
        // 重心一路确实会误判成「切镜」—— 这正是必须两路取大的理由
        res r_lb = analyze(lb_h);
        float wa = s2a_hdr_scene_weight(full.avg, r_lb.avg);
        ck(wa > 0.5f, "对照：重心一路把它当切镜（所以需要白点路兜住）", wa, 1.0f);
    }

    printf("%s t_hdr_letterbox：画幅与黑边 %d 项（失败 %d）\n", fails ? "✗" : "✓", total, fails);
    return fails ? 1 : 0;
}
