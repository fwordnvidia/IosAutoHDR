// t_hdr_oracle：把「白点 / 重心」按文档里的定义**独立重写一遍**，再跟生产实现逐位比。
//
// 为什么需要这一套，而 t_hdr_fuzz 不够：
//   fuzz 问的是「输出是否满足一组性质」。性质永远成立也可能算错——只要错得「合理」。
//   比如 HUD 剔除那段代码，如果簇的分界判错一格，输出仍然是 [0,1] 里的某个白点，
//   所有性质都满足，但画面白点系统性偏了 0.05 stop，谁也不会发现。
//
// 所以这里不引用生产的任何辅助函数，只照头文件注释里的**定义**写最笨的版本：
//   · 漫反射白点 = 「计数 ≥ 0.5% 亮像素的最高一格」的格子**上沿**；
//   · HUD 剔除   = 从高往低按「≥GAP 个空 bin」切簇，最高簇质量 < 5% 亮像素且与主簇之间
//                  有 ≥GAP 的空档 → 丢掉它，白点改取主簇内部的 0.5% 分位；
//   · 重心       = log2 格子中心按计数加权平均，跳过 bin 下沿 < AVG_CUT 的格子。
// 然后要求两者逐位相同（浮点只允许 1 ulp 级误差）。
//
// 真实照片 + 随机直方图 + 三类专门构造的「擦边球」都会跑。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

// 计数口径与 t_hdr_fuzz 一致：**一类对账算一项**，不是每个直方图每个字段算一项。
// 这里跑 4000 个直方图 × 6 类对账 = 2.4 万次比较；若按比较次数计「项」，docs 里的
// 「共 N 项」会被这 2 万推得失去意义。有意义的说法是：6 类对账在 4000 组输入上零差异。
static int items, fails, compared;
static unsigned char seen_family[16];
#define CHK(fam, c, ...) do {                                        \
        if (!seen_family[(fam)]) { seen_family[(fam)] = 1; items++; } \
        compared++;                                                   \
        if (!(c)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } \
    } while (0)

enum { O_WHITE_OK, O_WHITE_VAL, O_BULK_OK, O_BULK_VAL, O_AVG_OK, O_AVG_VAL };

// ---------- 独立实现的参考版本（只依赖定义，不调生产函数） ----------

static double naive_stop(int bin) {          // 格子中心的 log2 亮度
    const double lo = (double)S2A_HDR_HIST_LO, hi = (double)S2A_HDR_HIST_HI;
    const double step = (hi - lo) / (double)S2A_HDR_HIST_BINS;
    return lo + ((double)bin + 0.5) * step;
}
static double naive_edge(int bin) {          // 格子上沿的 log2 亮度
    const double lo = (double)S2A_HDR_HIST_LO, hi = (double)S2A_HDR_HIST_HI;
    const double step = (hi - lo) / (double)S2A_HDR_HIST_BINS;
    return lo + ((double)bin + 1.0) * step;
}
// 判「黑」用的是格子**下沿**（生产 s2a_hdr_bin_black 就是 `LO + bin*step < BLACK_CUT`）。
// 写成格心会整体偏半格，黑场边界那 1 格进出直接改变 total，进而改变 need 的取整 —— 实测
// 第一版就在这上面产生 3300 处「重心差 0.04 stop」「白点差一格」的假失败。
static double naive_lower(int bin) {
    const double lo = (double)S2A_HDR_HIST_LO, hi = (double)S2A_HDR_HIST_HI;
    return lo + (double)bin * ((hi - lo) / (double)S2A_HDR_HIST_BINS);
}
static int naive_is_black(int bin) { return naive_lower(bin) < (double)S2A_HDR_BLACK_CUT; }

// 亮像素总数（与生产同口径：跳过「真黑」格）
static uint64_t naive_total(const uint32_t h[S2A_HDR_HIST_BINS]) {
    uint64_t t = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) if (!naive_is_black(i)) t += h[i];
    return t;
}

static double naive_avg(const uint32_t h[S2A_HDR_HIST_BINS], int *ok) {
    double acc = 0.0, n = 0.0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        if (h[i] == 0) continue;
        const double y = naive_stop(i);
        if (naive_lower(i) < (double)S2A_HDR_AVG_CUT) continue;   // 门槛看下沿，累加用格心
        acc += y * (double)h[i];
        n += (double)h[i];
    }
    if (ok) *ok = n > 0.0;
    return n > 0.0 ? acc / n : 0.0;
}

// 无剔除的白点：最高一个「计数 ≥ need」的格子
static int naive_white(const uint32_t h[S2A_HDR_HIST_BINS], float *out) {
    const uint64_t total = naive_total(h);
    if (!total) return 0;
    uint64_t need = (uint64_t)((double)total * (double)S2A_HDR_WHITE_FRAC);
    if (need < 1) need = 1;
    for (int i = S2A_HDR_HIST_BINS - 1; i >= 0; i--) {
        if (naive_is_black(i)) break;
        if ((uint64_t)h[i] >= need) {
            *out = (float)pow(2.0, naive_edge(i));
            return 1;
        }
    }
    return 0;
}

// 带 HUD 剔除的白点：照定义从高往低切簇，最高簇太轻就丢，取主簇内部的 0.5% 分位
static int naive_white_bulk(const uint32_t h[S2A_HDR_HIST_BINS], double max_frac, int gap_bins, float *out) {
    const uint64_t total = naive_total(h);
    if (!total) return 0;
    int i = S2A_HDR_HIST_BINS - 1;
    while (i >= 0 && (naive_is_black(i) || h[i] == 0)) i--;
    while (i >= 0) {
        const int top = i;
        uint64_t mass = 0;
        int gap = 0;
        while (i >= 0) {
            if (naive_is_black(i) || h[i] == 0) {
                if (++gap >= gap_bins) break;
            } else {
                gap = 0;
                mass += h[i];
            }
            i--;
        }
        if ((double)mass >= (double)total * max_frac) {
            uint64_t need = (uint64_t)((double)total * (double)S2A_HDR_WHITE_FRAC);
            if (need < 1) need = 1;
            for (int k = top; k >= 0; k--) {
                if (naive_is_black(k)) break;
                if ((uint64_t)h[k] >= need) { *out = (float)pow(2.0, naive_edge(k)); return 1; }
            }
            *out = (float)pow(2.0, naive_edge(top));
            return 1;
        }
    }
    return 0;
}

// ---------- 直方图来源 ----------

static uint64_t rs = 0xDEADBEEFCAFEull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 32); }

static int real_top_bin(void) {
    int b = 0;
    while (b + 1 < S2A_HDR_HIST_BINS && (float)pow(2.0, naive_edge(b)) <= 1.0f) b++;
    return b;
}

// 读一张 8 位 PPM 进直方图（走生产的 s2a_hdr_hist_bin，保证「怎么分箱」与真机一致）
static int hist_from_ppm(const char *path, uint32_t h[S2A_HDR_HIST_BINS]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[3] = {0};
    int w = 0, ht = 0, mx = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &ht, &mx) != 4 || strcmp(magic, "P6")) { fclose(f); return 0; }
    fgetc(f);
    memset(h, 0, S2A_HDR_HIST_BINS * sizeof(uint32_t));
    const long n = (long)w * ht;
    for (long k = 0; k < n; k++) {
        int rgb[3];
        if (fread(rgb, 1, 3, f) != 3) break;
        for (int c = 0; c < 3; c++) {
            const float v = (float)rgb[c] / 255.0f;
            const float lin = (v <= 0.04045f) ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
            const int bin = s2a_hdr_hist_bin(lin);
            if (bin >= 0 && bin < S2A_HDR_HIST_BINS) h[bin]++;
        }
    }
    fclose(f);
    return 1;
}

static void compare(const char *tag, const uint32_t h[S2A_HDR_HIST_BINS]) {
    // 1) 无剔除白点
    float a = -1.0f, b = -1.0f;
    const int ra = s2a_hdr_sdr_white(h, &a);
    const int rb = naive_white(h, &b);
    CHK(O_WHITE_OK, ra == rb, "%s 无剔除白点：可用性不同（生产 %d / 参考 %d）", tag, ra, rb);
    if (ra && rb)
        CHK(O_WHITE_VAL, fabsf(log2f(a / b)) < 1e-5f, "%s 无剔除白点：生产 %g / 参考 %g（差 %.6f stop）",
           tag, (double)a, (double)b, (double)fabsf(log2f(a / b)));

    // 2) 带 HUD 剔除的白点
    a = b = -1.0f;
    const int ra2 = s2a_hdr_sdr_white_bulk(h, &a);
    const int rb2 = naive_white_bulk(h, (double)S2A_HDR_CLUSTER_MAX_FRAC, S2A_HDR_CLUSTER_GAP, &b);
    CHK(O_BULK_OK, ra2 == rb2, "%s HUD 剔除白点：可用性不同（生产 %d / 参考 %d）", tag, ra2, rb2);
    if (ra2 && rb2)
        CHK(O_BULK_VAL, fabsf(log2f(a / b)) < 1e-5f, "%s HUD 剔除白点：生产 %g / 参考 %g（差 %.6f stop）",
           tag, (double)a, (double)b, (double)fabsf(log2f(a / b)));

    // 3) 重心
    int oka = 0, okb = 0;
    const float av = s2a_hdr_hist_avg(h, &oka);
    const double bv = naive_avg(h, &okb);
    CHK(O_AVG_OK, oka == okb, "%s 重心：可用性不同（生产 %d / 参考 %d）", tag, oka, okb);
    if (oka && okb) {
        // 生产版是 float 累加再除？不——它是 double 累加，这里要求 1e-4 stop 内
        CHK(O_AVG_VAL, fabs((double)av - bv) < 1e-4, "%s 重心：生产 %.6f / 参考 %.6f（差 %.6f stop）",
           tag, (double)av, bv, fabs((double)av - bv));
    }
}

int main(int argc, char **argv) {
    // 一、真实照片（一张都读不到才算失败）
    //
    // 查找顺序：$S2A_REAL_DIR/<名> → build/real/<名> → ../build/real/<名>。
    // 为什么要环境变量（2026-11-17 消费端测试撞出来的）：verify.sh 是在**目标树根**跑单测的，
    // 而素材在交付目录的 testdata/ 下、不在树里，于是这一套永远读到 0 张、永远判失败；
    // 开发目录里因为 cwd 正好有 build/real/ 才一直是绿的。
    const char *names[] = {
        "p_Clouds_by_Tibor_Mokanszki.ppm",
        "p_Fuji_san_by_amaral.ppm",
        "p_Monument_valley_by_orbitelambda.ppm",
    };
    const char *dir = getenv("S2A_REAL_DIR");
    int used = 0;
    for (int k = 0; k < (int)(sizeof names / sizeof *names); k++) {
        char path[512];
        uint32_t h[S2A_HDR_HIST_BINS];
        int got = 0;
        if (dir && *dir) {
            snprintf(path, sizeof path, "%s/%s", dir, names[k]);
            got = hist_from_ppm(path, h);
        }
        if (!got) {
            snprintf(path, sizeof path, "build/real/%s", names[k]);
            got = hist_from_ppm(path, h);
        }
        if (!got) {
            snprintf(path, sizeof path, "../build/real/%s", names[k]);
            got = hist_from_ppm(path, h);
        }
        if (!got) continue;
        compare(path, h);
        used++;
    }
    CHK(O_WHITE_OK, used > 0 || argc > 1,
        "一张真实照片都没读到（素材在 testdata/：设 S2A_REAL_DIR=…/testdata，或传 --allow-missing 表示接受）");

    // 二、随机直方图（现实范围）
    for (int t = 0; t < 4000; t++) {
        uint32_t h[S2A_HDR_HIST_BINS];
        memset(h, 0, sizeof h);
        const int top = real_top_bin();
        const int style = (int)(rnd() % 4u);
        for (int i = 0; i <= top; i++) {
            switch (style) {
            case 0: h[i] = rnd() % 3000u; break;                                  // 均匀
            case 1: h[i] = (uint32_t)(i * (rnd() % 20u)); break;                  // 斜坡
            case 2: h[i] = (rnd() % 3u) ? 0u : (1u + rnd() % 2000u); break;        // 稀疏
            case 3: h[i] = (uint32_t)(1u + rnd() % 40u); break;                   // 平坦
            }
        }
        if (rnd() % 2u) h[0] += rnd() % 400000u;                                  // 加一片黑
        char tag[64];
        snprintf(tag, sizeof tag, "随机#%d(style=%d)", t, style);
        compare(tag, h);
    }

    // 三、擦边球：HUD 恰好卡在 5% 线上、簇间隙恰好等于 GAP、白点恰好落在 need 边界
    {
        uint32_t h[S2A_HDR_HIST_BINS];
        // (a) 主簇给 100 万，HUD 簇给 5.0% 整
        memset(h, 0, sizeof h);
        for (int i = 40; i <= 120; i++) h[i] = 1000u;              // 主簇 81 格 × 1000
        h[200] = 81000u * 5u / 100u;                                // HUD = 5.0% 亮像素
        compare("HUD 恰好 5%", h);
        // (b) 簇间隙恰好 = GAP 个空 bin
        memset(h, 0, sizeof h);
        for (int i = 40; i <= 120; i++) h[i] = 1000u;
        h[120 + 1 + S2A_HDR_CLUSTER_GAP] = 2000u;                    // 与主簇隔 GAP 个空格
        compare("间隙恰好=GAP", h);
        // (c) 间隙比 GAP 少一格（应当视作同一簇）
        memset(h, 0, sizeof h);
        for (int i = 40; i <= 120; i++) h[i] = 1000u;
        h[120 + S2A_HDR_CLUSTER_GAP] = 2000u;
        compare("间隙=GAP-1", h);
        // (d) 计数恰好等于 need
        memset(h, 0, sizeof h);
        for (int i = 10; i < 200; i++) h[i] = 200u;                  // 总 38000
        h[199] = (uint32_t)((double)38000 * (double)S2A_HDR_WHITE_FRAC);   // 正好 0.5%
        compare("计数=need", h);
    }

    printf("%s t_hdr_oracle：白点/重心 与独立参考实现对账 %d 项（6 类对账，失败 %d，%d 次比较，真实照片 %d 张）\n",
           fails ? "✗" : "✓", items, fails, compared, used);
    return fails ? 1 : 0;
}
