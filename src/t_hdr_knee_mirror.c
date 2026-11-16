// L0 分位 → knee：MSL 的 hdr_hist_knee 与 CPU 的 s2a_hdr_auto_knee 对账
//
// 为什么需要：这两个函数算的是同一件事（99.5 分位落在哪一格、该格的 hi/lo 取多少），
// 一个跑在 GPU、一个跑在全部单测里。阈值口径只要差 1，同一帧就会落在相邻 bin，
// hi 差一格 = 0.0547 stop —— 肉眼看不出，但它让「测试结论」与「设备行为」系统性错开。
//
// 抓到的真分歧：阈值原先是 (uint)((float)total * 0.995f) vs (uint64)((double)total * 0.995)。
//   扫描 1..8,300,000 个像素总数，有 1,515,878 个两边不同（float 版少 1）；本例第 3 组
//   就构造了「累计刚好等于 CPU 阈值」的情形，旧 MSL 会落到前一格。
// 修法：两侧都改成整数千分 (total * 995) / 1000，逐位一致。
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_msl_hist.h"

static int fails, total_checks;
#define CHECK(cond, ...) do { total_checks++; if (!(cond)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(...) do { total_checks++; printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

// 逐行翻译自 src/hdr_hist.metal 的 hdr_hist_knee —— 现在只有一份，见 nvn_hdr_msl_hist.h
static int msl_knee(const uint32_t h[S2A_HDR_HIST_BINS], float *hi, float *lo, int *hit_out) {
    return s2a_msl_hist_knee(h, hi, lo, hit_out);
}

// 旧版 MSL（float 阈值），只用来证明这条分歧真实存在过
static int msl_knee_old(const uint32_t h[S2A_HDR_HIST_BINS], float *hi, float *lo) {
    unsigned total = 0u;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) total += h[i];
    if (total == 0u) return 0;
    const unsigned target = (unsigned)((float)total * 0.995f);
    unsigned acc = 0u, hit = (unsigned)(S2A_HDR_HIST_BINS - 1);
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        acc += h[i];
        if (acc >= target) { hit = (unsigned)i; break; }
    }
    float edge = exp2f(-10.0f + (4.0f - (-10.0f)) * ((float)hit + 1.0f) / 256.0f);
    float hh = edge / 0.8f;
    hh = hh < 0.25f ? 0.25f : (hh > 1.0f ? 1.0f : hh);
    *hi = hh; *lo = 0.55f * hh;
    return 1;
}

static void fill(uint32_t h[S2A_HDR_HIST_BINS], uint32_t per_bin, int nbin) {
    memset(h, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < nbin; i++) h[i] = per_bin;
}

int main(void) {
    printf("L0 分位 → knee：GPU（MSL）与 CPU 对账\n");

    // 场景一：七种典型直方图（含真实像素量级）
    struct { const char *n; uint32_t per; int nbin; } cases[] = {
        {"单像素", 1, 1},
        {"1000 像素 / 8 格", 125, 8},
        {"1080p 均匀铺满", 2073600 / 256, 256},
        {"1080p 单色", 2073600, 1},
        {"4K 单色", 8294400, 1},
        {"4K 两格", 4147200, 2},
        {"1080p 126 格", 16457, 126},
    };
    int bad = 0;
    uint32_t h[S2A_HDR_HIST_BINS];
    for (int k = 0; k < 7; k++) {
        fill(h, cases[k].per, cases[k].nbin);
        float a1 = 0, a2 = 0, b1 = 0, b2 = 0;
        int hb = -1;
        int oc = s2a_hdr_auto_knee(h, &a1, &a2);
        int om = msl_knee(h, &b1, &b2, &hb);
        if (oc != om || a1 != b1 || a2 != b2) { bad++; printf("      %s：cpu(%.6f,%.6f) msl(%.6f,%.6f)\n",
            cases[k].n, (double)a1, (double)a2, (double)b1, (double)b2); }
    }
    CHECK(bad == 0, "%d / 7 组典型直方图 hi/lo 不同", bad);
    if (bad == 0) OK("7 组典型直方图（1 像素 … 4K 单色）hi/lo 逐位相同");

    // 场景二：阈值口径本身 —— 1..20000 个像素总数逐个比
    int want_bad = 0, first = 0;
    for (uint64_t n = 1; n <= 20000; n++) {
        uint64_t c = (n * S2A_HDR_QUANTILE_PERMILLE) / 1000u; if (c < 1) c = 1;
        unsigned long m = (unsigned long)((n * 995ul) / 1000ul); if (m < 1ul) m = 1ul;
        if ((uint64_t)m != c) { want_bad++; if (!first) first = (int)n; }
    }
    CHECK(want_bad == 0, "像素总数 1..20000 中阈值口径不一致 %d 个（首个 %d）", want_bad, first);
    if (want_bad == 0) OK("阈值口径：1..20000 个像素总数 100%% 一致（整数千分）");

    float oc1 = 0, oc2 = 0, om1 = 0, om2 = 0, oo1 = 0, oo2 = 0;
    float c1 = 0, c2 = 0, m1 = 0, m2 = 0, x1 = 0, x2 = 0;
    (void)oo1; (void)oo2;

    // 场景三：构造一个「旧口径必然翻一格」的直方图，并且让那一格不在 clamp 区间里
    //   找 N 使 float 阈值比整数阈值少 1；把累计到 wantOld 的像素全部塞进 0..149 格，
    //   剩下的塞进 150 格。于是：
    //     旧 MSL（target=wantOld）在第 149 格就满足 → 落在 149；
    //     CPU / 修后 MSL（target=wantC=wantOld+1）要走到第 150 格。
    //   150 格的上沿 ≈ 0.2999 ⇒ hi ≈ 0.3749，远离 0.25 下夹，差别看得见。
    //   方向是单向的：整数阈值是向下取整，float 阈值按最近舍入且几乎总是**多 1**
    //   （4000..8,300,000 里 1,515,877 例多 1、0 例少 1）。所以旧 MSL 只会「更晚」满足条件，
    //   也就是同一帧可能落到**更高**一格。构造：累计恰好等于整数阈值，剩下全塞下一格。
    uint64_t n_show = 0;
    for (uint64_t n = 4000; n <= 8300000; n++) {
        uint64_t c = (n * 995u) / 1000u; if (!c) c = 1;
        unsigned m = (unsigned)((float)n * 0.995f); if (!m) m = 1;
        if ((uint64_t)m == c + 1u) { n_show = n; break; }
    }
    CHECK(n_show != 0, "找不到 float 阈值多 1 的像素总数（构造失败）");
    uint64_t wantC = (n_show * 995u) / 1000u; if (!wantC) wantC = 1;
    uint64_t wantOld = (uint64_t)((float)n_show * 0.995f); if (!wantOld) wantOld = 1;
    memset(h, 0, sizeof h);
    {
        uint32_t per = (uint32_t)(wantC / 150u);
        uint64_t left = wantC;
        for (int i = 0; i < 150; i++) { h[i] = per; left -= per; }
        h[149] += (uint32_t)left;                      // 把余数补在最后一格，保证累计刚好 = wantC
        h[150] = (uint32_t)(n_show - wantC);           // 剩下的一格：让总数回到 n_show
    }
    int hit_c = -1, hit_o = -1, hit_m = -1;
    s2a_hdr_auto_knee(h, &c1, &c2);
    // 复算命中格（用同一口径再扫一遍，只为打印）
    {
        uint64_t acc = 0;
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) { acc += h[i]; if (acc >= wantC) { hit_c = i; break; } }
        acc = 0;
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) { acc += h[i]; if (acc >= wantOld) { hit_o = i; break; } }
    }
    msl_knee(h, &m1, &m2, &hit_m);
    msl_knee_old(h, &x1, &x2);
    CHECK(hit_c == 149, "构造：整数阈值应落在第 149 格，实得 %d", hit_c);
    CHECK(hit_o == 150, "构造：旧 float 阈值应落在第 150 格，实得 %d", hit_o);
    CHECK(x1 != c1, "旧口径与 CPU 应给出不同的 hi（旧 %.6f / cpu %.6f），否则这组构造没意义",
          (double)x1, (double)c1);
    CHECK(m1 == c1 && m2 == c2, "修后 MSL 应与 CPU 逐位相同（msl %.6f / cpu %.6f）", (double)m1, (double)c1);
    OK("翻格用例：N=%llu，旧 MSL hi=%.6f（第 %d 格）→ 修后 hi=%.6f（第 %d 格）= CPU",
       (unsigned long long)n_show, (double)x1, hit_o, (double)m1, hit_m);

    // 场景五：空帧
    memset(h, 0, sizeof h);
    CHECK(s2a_hdr_auto_knee(h, &oc1, &oc2) == 0, "空直方图 CPU 应返回 0（保持上一帧）");
    CHECK(msl_knee(h, &om1, &om2, NULL) == 0, "空直方图 MSL 应返回 0（输出缺省 0.55/2.0）");
    OK("空帧：两侧都返回 0，调用方各自保持上一帧 / 缺省值");

    printf("%s t_hdr_knee_mirror：L0 分位对账 %d 项（失败 %d）\n", fails ? "✗" : "✓", total_checks, fails);
    return fails ? 1 : 0;
}
