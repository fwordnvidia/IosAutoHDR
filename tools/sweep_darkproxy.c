// sweep_darkproxy：GPU 侧只有 float4(hi, white, avg, scene) 可读，没有直方图，
// 所以「这是不是黑底画面」只能用代理判据。当前代理是「白点 < 2% 白」（S2A_HDR_DARK_WHITE），
// 它只在整幅几乎全黑时成立；一旦画面里有一小撮亮像素（星空、暗场里的灯、字幕），
// 白点就被那群像素顶高，代理判「不是暗场」→ 纸白取到 0.5 以上 → 该扩张的暗部全冻住。
//
// 这个工具把所有候选代理判据放在同一张分布网格上，与 CPU 的真判据（lit_frac < 5%）对比，
// 数「判错」的格数。判错分两种：漏判（该当暗场却按亮场处理）/ 误判（亮场被当暗场）。
//
// 用法：sweep_darkproxy [csv 路径]      → 打印平台区与推荐参数，缺省写 build/sweep_darkproxy.csv
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

// 两个两值分布：底（大部分像素）在 base，亮簇（fraction）在 peak。
static void build_hist(uint32_t *h, float base, float peak, float frac) {
    memset(h, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    const int N = 100000;
    int nb = (int)((1.0f - frac) * (float)N), np = N - nb;
    h[s2a_hdr_hist_bin(base)] += (uint32_t)nb;
    h[s2a_hdr_hist_bin(peak)] += (uint32_t)np;
}

typedef struct { float lo, hi; } tgt;

// 候选 1：现行（只看白点）
static int proxy_white(float avg, float white, float hi, float thr) {
    (void)avg; (void)hi;
    return white < thr;
}
// 候选 2：白点低，或「重心极低但白点不低」（= 少量亮点 + 大片黑）
static int proxy_avg(float avg, float white, float hi, float thr) {
    (void)hi;
    return white < thr || (avg < S2A_HDR_AVG_CUT + 1.0f && white > thr);
}
// 候选 3（白点与重心差 n stop 以上判暗场）没有独立函数：它的公式在 main 里直接算进
// gap 变量（第 80 行附近 `(gap > gaps[g]) != cpu`），因为要同时扫 8 个阈值。
// 原来这里有个 proxy_gap()，从没人调 —— `-Wall -Wextra` 报 unused-function 才发现，已删。

int main(int argc, char **argv) {
    const char *csv = argc > 1 ? argv[1] : "build/sweep_darkproxy.csv";
    FILE *f = fopen(csv, "w");
    if (!f) { perror(csv); return 2; }
    fprintf(f, "base,peak,frac,lit,cpu_dark,avg,white,hi,p1,p2,gap\n");

    const float bases[] = {0.0f, 0.004f, 0.02f, 0.06f, 0.15f};
    const float peaks[] = {0.2f, 0.35f, 0.55f, 0.72f, 0.86f, 1.0f};
    const float fracs[] = {0.001f, 0.003f, 0.005f, 0.01f, 0.02f, 0.05f, 0.10f, 0.30f};
    int n = 0, wrong_white = 0, wrong_avg = 0, wrong_gap[8] = {0};
    static const float gaps[] = {2.0f, 3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 5.5f, 6.0f};
    uint32_t hist[S2A_HDR_HIST_BINS];

    for (size_t bi = 0; bi < sizeof bases / sizeof *bases; bi++)
        for (size_t pi = 0; pi < sizeof peaks / sizeof *peaks; pi++)
            for (size_t fi = 0; fi < sizeof fracs / sizeof *fracs; fi++) {
                float base = s2a_hdr_to_linear(bases[bi]);
                float peak = s2a_hdr_to_linear(peaks[pi]);
                build_hist(hist, base, peak, fracs[fi]);
                int ok = 0;
                float avg = s2a_hdr_hist_avg(hist, &ok);
                if (!ok) continue;
                float white = 0.0f;
                (void)s2a_hdr_sdr_white(hist, &white);
                float hi = 0.0f, lo_u;
                if (!s2a_hdr_auto_knee(hist, &hi, &lo_u)) hi = 0.0f;
                float lit = s2a_hdr_lit_frac(hist);
                int cpu = lit < S2A_HDR_DARK_FRAC;
                int p1 = proxy_white(avg, white, hi, S2A_HDR_DARK_WHITE);
                int p2 = proxy_avg(avg, white, hi, S2A_HDR_DARK_WHITE);
                float gap = (white > 0.0f) ? (log2f(white) - avg) : 99.0f;
                if (p1 != cpu) wrong_white++;
                if (p2 != cpu) wrong_avg++;
                for (int g = 0; g < 8; g++) if ((gap > gaps[g]) != cpu) wrong_gap[g]++;
                fprintf(f, "%.4f,%.4f,%.3f,%.4f,%d,%.4f,%.4f,%.4f,%d,%d,%.3f\n",
                        (double)bases[bi], (double)peaks[pi], (double)fracs[fi], (double)lit, cpu,
                        (double)avg, (double)white, (double)hi, p1, p2, (double)gap);
                n++;
            }

    printf("网格 %d 格（CPU 判据：lit_frac < %.3f）\n", n, (double)S2A_HDR_DARK_FRAC);
    printf("  现行（白点 < %.2f）        判错 %d 格\n", (double)S2A_HDR_DARK_WHITE, wrong_white);
    printf("  重心补充（avg 极低 + 白点高）判错 %d 格\n", wrong_avg);
    printf("  白点-重心 gap 判据：\n");
    for (int g = 0; g < 8; g++)
        printf("    gap > %.1f stop → 判错 %d 格\n", (double)gaps[g], wrong_gap[g]);
    fclose(f);
    printf("  CSV：%s\n", csv);
    return 0;
}
