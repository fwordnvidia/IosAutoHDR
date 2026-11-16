// HUD 剔除常数的敏感度扫描：`S2A_HDR_CLUSTER_MAX_FRAC`（5%）与 `S2A_HDR_CLUSTER_GAP`（3 bin）。
//
// 目的不是再证明一次「能剔除」，而是回答「这两个数为什么是这个值」：
//   如果结论在 3%..8% / 2..5 bin 这一大片区间里都不变，说明常数坐在平台区，不是刀刃上。
//
// 输出 CSV：case,hud_frac,white_plain,white_bulk,kept
//   kept=1 表示剔除了 HUD（white_bulk 落回内容簇），0 表示保留（把 HUD 当真实高亮）。
#include <stdio.h>
#include <string.h>

#include "nvn_hdr_auto_l1.h"

// 造一个直方图：内容簇 [c_lo, c_hi] 均匀，HUD 单 bin 在 hud_bin，总像素 100000，HUD 占比 hud_frac。
static void build(uint32_t *hist, int c_lo, int c_hi, int hud_bin, double hud_frac) {
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
    double total = 100000.0;
    uint32_t hud = (uint32_t)(total * hud_frac);
    uint32_t body = (uint32_t)(total - hud);
    int n = c_hi - c_lo + 1;
    for (int i = c_lo; i <= c_hi; i++) hist[i] = body / (uint32_t)n;
    if (hud_bin >= 0) hist[hud_bin] += hud;
}

// 线性亮度 → 最近的 bin（直方图 bins 覆盖到远超 1.0 的范围，所以必须按亮度反查，
//   不能拿「bin 号」当亮度用：bin 210 的边是 2.9，不是 0.86）
static int bin_for(float lum) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        float d = fabsf(s2a_hdr_hist_edge(i) - lum);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int main(void) {
    printf("case,hud_frac,white_plain,white_bulk,kept\n");
    const char *names[] = {
        "内容 0.10-0.50 + HUD",
        "内容 0.05-0.30 + 孤立高光",
        "内容 0.30-0.75 + HUD",
        "内容 0.02-0.20 + 字幕",
    };
    struct { int lo, hi, hud; } cs[4];
    {   // 内容按线性亮度给：0.10..0.50 / HUD 0.70（sRGB 0.86 ≈ 线性 0.70）
        cs[0].lo = bin_for(0.10f); cs[0].hi = bin_for(0.50f); cs[0].hud = bin_for(0.70f);
        cs[1].lo = bin_for(0.05f); cs[1].hi = bin_for(0.30f); cs[1].hud = bin_for(0.60f);
        cs[2].lo = bin_for(0.30f); cs[2].hi = bin_for(0.75f); cs[2].hud = bin_for(0.95f);
        cs[3].lo = bin_for(0.02f); cs[3].hi = bin_for(0.20f); cs[3].hud = bin_for(0.55f);
    }
    double fracs[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.08, 0.15};
    for (int c = 0; c < 4; c++) {
        for (unsigned f = 0; f < sizeof fracs / sizeof *fracs; f++) {
            uint32_t hist[S2A_HDR_HIST_BINS];
            build(hist, cs[c].lo, cs[c].hi, cs[c].hud, fracs[f]);
            float plain = 0, bulk = 0;
            int okp = s2a_hdr_sdr_white(hist, &plain);
            int okb = s2a_hdr_sdr_white_bulk(hist, &bulk);
            printf("%s,%.3f,%.4f,%.4f,%d\n", names[c], fracs[f],
                   okp ? plain : -1.0f, okb ? bulk : -1.0f, (okb && bulk < plain - 1e-3) ? 1 : 0);
        }
    }
    // 对照：真实高亮内容（簇连续到高位），任何 frac 下都不该被丢
    for (unsigned f = 0; f < sizeof fracs / sizeof *fracs; f++) {
        uint32_t hist[S2A_HDR_HIST_BINS];
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
        for (int i = bin_for(0.10f); i <= bin_for(1.00f); i++) hist[i] = 400;   // 连续，无空档
        hist[bin_for(1.00f)] += (uint32_t)(100000 * fracs[f]);
        float plain = 0, bulk = 0;
        int okp = s2a_hdr_sdr_white(hist, &plain), okb = s2a_hdr_sdr_white_bulk(hist, &bulk);
        printf("真实高亮(连续),%.3f,%.4f,%.4f,%d\n", fracs[f], okp ? plain : -1.0f,
               okb ? bulk : -1.0f, (okb && bulk < plain - 1e-3) ? 1 : 0);
    }
    // ---- 第二张表：把 (max_frac, gap) 拉开，看默认值是否坐在平台区 ----
    // 期望：① 4 个 HUD 用例在 0.005~0.02 覆盖率上必须被剔（hit_hud 满）；② 真实连续高亮一个都不能丢。
    printf("\n#max_frac,gap,hit_hud_of_12,false_drop_of_8\n");
    double mf[] = {0.01, 0.02, 0.03, 0.05, 0.08, 0.12, 0.20};
    int gaps[] = {2, 3, 4, 5, 6};
    for (unsigned m = 0; m < sizeof mf / sizeof *mf; m++)
        for (unsigned g = 0; g < sizeof gaps / sizeof *gaps; g++) {
            int hit = 0, falsedrop = 0;
            for (int c = 0; c < 4; c++) {
                double fr[] = {0.005, 0.01, 0.02};
                for (int k = 0; k < 3; k++) {
                    uint32_t hist[S2A_HDR_HIST_BINS];
                    build(hist, cs[c].lo, cs[c].hi, cs[c].hud, fr[k]);
                    float plain = 0, bulk = 0;
                    if (!s2a_hdr_sdr_white(hist, &plain)) continue;
                    if (!s2a_hdr_sdr_white_bulk_opt(hist, mf[m], gaps[g], &bulk)) continue;
                    if (bulk < plain - 1e-3f) hit++;
                }
            }
            double fr2[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.08, 0.15};
            for (int k = 0; k < 8; k++) {
                uint32_t hist[S2A_HDR_HIST_BINS];
                for (int i = 0; i < S2A_HDR_HIST_BINS; i++) hist[i] = 0;
                for (int i = bin_for(0.10f); i <= bin_for(1.00f); i++) hist[i] = 400;
                hist[bin_for(1.00f)] += (uint32_t)(100000 * fr2[k]);
                float plain = 0, bulk = 0;
                if (!s2a_hdr_sdr_white(hist, &plain)) continue;
                if (!s2a_hdr_sdr_white_bulk_opt(hist, mf[m], gaps[g], &bulk)) continue;
                if (bulk < plain - 1e-3f) falsedrop++;
            }
            printf("%.2f,%d,%d/12,%d/8\n", mf[m], gaps[g], hit, falsedrop);
        }

    return 0;
}
