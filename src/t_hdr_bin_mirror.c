// GPU 直方图分箱 / 黑像素口径 与 CPU 的对账
//
// 为什么单开一套：src/hdr_hist.metal 的 hdr_hist_add 决定「设备上真正进直方图的是哪些像素」，
// 而全部单测用的是 CPU 的 s2a_hdr_hist_bin()。两边只要有一处分歧，测试通过 ≠ 设备正确
// （t_hdr_gpu_mirror 就是这样抓到 avg 差 3.5 stop 的）。这里把 MSL 的算式逐行翻成 C 再对账。
//
// 已抓到的真分歧：MSL 原来 `if (!(y > 0.0f)) return;` 把黑像素整个丢掉，CPU 却把它记进 0 号格。
// 后果不是白点（s2a_hdr_sdr_white 本来就把黑格排除在分母外），而是 lit_frac：
// 设备上分母不含黑 ⇒ 夜景/星空（96% 以上纯黑）lit 恒为 1.0 ⇒ deep_dark 永不成立 ⇒
// dark_passage / DARK_FLOOR 这条暗场分支在设备上从不执行。现在 MSL 改为记进 0 号格。
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_msl_hist.h"
#include "nvn_hdr_auto_l1.h"

static int fails, total;
#define CHECK(cond, ...) do { total++; if (!(cond)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(...) do { total++; printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

// 逐行翻译自 src/hdr_hist.metal 的 hdr_hist_add —— 现在只有一份，见 nvn_hdr_msl_hist.h。
// count_black = 0 保留旧行为（整帧跳过黑像素），只为证明那条分歧真实存在过。
static inline float msl_lin_h(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static inline int msl_bin(float y, int count_black) {
    if (!count_black && !(y > 0.0f)) return -1;
    return s2a_msl_hist_bin(y);
}

// 用 MSL 的算式铺一幅直方图（stride 采样由调用方给，这里只算亮度）
static void msl_hist(const float *lin, int n, uint32_t hist[S2A_HDR_HIST_BINS], int count_black) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < n; i++) {
        float y = lin[i];
        int b = msl_bin(y, count_black);
        if (b >= 0) hist[b]++;
    }
}

static void cpu_hist(const float *lin, int n, uint32_t hist[S2A_HDR_HIST_BINS]) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < n; i++) hist[s2a_hdr_hist_bin(lin[i])]++;
}

int main(void) {
    printf("GPU（MSL）与 CPU 的分箱口径对账\n");

    // 场景一：256 个 8 位码值（游戏源实际会遇到的全部亮度）
    int bad = 0, first = -1;
    for (int c = 0; c < 256; c++) {
        float y = msl_lin_h((float)c / 255.0f);
        if (s2a_hdr_hist_bin(y) != msl_bin(y, 1)) { bad++; if (first < 0) first = c; }
    }
    CHECK(bad == 0, "8 位码值分箱分歧 %d / 256（第一个 code %d）", bad, first);
    if (bad == 0) OK("8 位码值 256/256 同格（含 code 0 的黑）");

    // 场景二：全部 256 个精确 bin 边界（浮点截断最容易翻车的位置）
    bad = 0;
    for (int k = 0; k < 256; k++) {
        float y = powf(2.0f, -10.0f + (float)k * (14.0f / 256.0f));
        if (s2a_hdr_hist_bin(y) != msl_bin(y, 1)) bad++;
    }
    CHECK(bad == 0, "bin 边界分歧 %d / 256", bad);
    if (bad == 0) OK("256 个 bin 边界 2^(LO+k·step) 全部同格");

    // 场景三：20 万随机样本
    unsigned st = 1234u;
    bad = 0;
    for (int i = 0; i < 200000; i++) {
        st = st * 1664525u + 1013904223u;
        double t = (double)(st >> 8) / 16777216.0 * 15.0 - 11.0;
        float y = (float)pow(2.0, t);
        if (s2a_hdr_hist_bin(y) != msl_bin(y, 1)) bad++;
    }
    CHECK(bad == 0, "随机样本分歧 %d / 200000", bad);
    if (bad == 0) OK("20 万个 log2 均匀样本全部同格");

    // 场景四：特殊值
    float sp[] = {0.0f, -0.0f, -1.0f, NAN, INFINITY, 16.0f, 32.0f, 1e9f};
    const char *nm[] = {"0", "-0", "-1", "NaN", "inf", "16", "32", "1e9"};
    bad = 0;
    for (int i = 0; i < 8; i++) {
        int a = s2a_hdr_hist_bin(sp[i]), b = msl_bin(sp[i], 1);
        if (a != b) { bad++; printf("      %s: cpu=%d msl=%d\n", nm[i], a, b); }
    }
    CHECK(bad == 0, "特殊值分歧 %d / 8", bad);
    if (bad == 0) OK("0 / -0 / 负数 / NaN / inf / 超大 全部同格");

    // 场景五：黑像素口径 —— 这就是修掉的那条真分歧
    //   夜空：96% 纯黑 + 4% 亮度 0.02 的地面
    enum { N = 40000 };
    static float lin[N];
    for (int i = 0; i < N; i++) lin[i] = (i < (int)(N * 0.96f)) ? 0.0f : 0.02f;
    uint32_t h_skip[S2A_HDR_HIST_BINS], h_count[S2A_HDR_HIST_BINS], h_cpu[S2A_HDR_HIST_BINS];
    msl_hist(lin, N, h_skip, 0);
    msl_hist(lin, N, h_count, 1);
    cpu_hist(lin, N, h_cpu);

    float lit_skip = s2a_hdr_lit_frac(h_skip);
    float lit_count = s2a_hdr_lit_frac(h_count);
    float lit_cpu = s2a_hdr_lit_frac(h_cpu);
    CHECK(fabsf(lit_skip - 1.0f) < 1e-6f, "修之前（丢黑）lit_frac 应为 1.0，实得 %.4f", (double)lit_skip);
    CHECK(fabsf(lit_count - 0.04f) < 0.01f, "修之后 lit_frac 应 ≈0.0400，实得 %.4f", (double)lit_count);
    CHECK(lit_count == lit_cpu, "修之后与 CPU 逐位相同（msl %.6f cpu %.6f）", (double)lit_count, (double)lit_cpu);
    if (fabsf(lit_skip - 1.0f) < 1e-6f && fabsf(lit_count - 0.04f) < 0.01f)
        OK("黑像素口径：丢黑时 lit=1.0000（暗场判据永不成立），记黑后 lit=0.0400 = CPU");

    // 场景六：同一帧的整条分析链 —— 白点两口径相同，但暗场判定完全不同
    float w_skip = 0.0f, w_count = 0.0f;
    int ok_skip = s2a_hdr_sdr_white(h_skip, &w_skip);
    int ok_count = s2a_hdr_sdr_white(h_count, &w_count);
    CHECK(ok_skip == ok_count && w_skip == w_count,
          "白点不该受口径影响（skip %d/%.6f count %d/%.6f）", ok_skip, (double)w_skip, ok_count, (double)w_count);
    if (ok_skip == ok_count && w_skip == w_count)
        OK("白点与黑像素口径无关（%.4f），所以这条分歧只在暗场分支上表现出真差异", (double)w_count);

    int deep_skip = (s2a_hdr_hist_avg(h_skip, NULL) < S2A_HDR_DIM_APL) && (lit_skip < S2A_HDR_DARK_FRAC);
    int deep_count = (s2a_hdr_hist_avg(h_count, NULL) < S2A_HDR_DIM_APL) && (lit_count < S2A_HDR_DARK_FRAC);
    CHECK(deep_skip == 0, "丢黑时 deep_dark 应为 0（设备原来就走这条，暗场分支从不执行）");
    CHECK(deep_count == 1, "记黑后 deep_dark 应为 1（与 CPU/单测一致）");
    if (deep_skip == 0 && deep_count == 1)
        OK("deep_dark：丢黑 0 → 记黑 1，设备与单测从此走同一分支");

    // 场景七：0 号格计数对总像素的守恒（GPU 不能多算也不能少算）
    uint64_t t_skip = 0, t_count = 0, t_cpu = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) { t_skip += h_skip[i]; t_count += h_count[i]; t_cpu += h_cpu[i]; }
    CHECK(t_count == (uint64_t)N, "记黑后总数应为 %d，实得 %llu", N, (unsigned long long)t_count);
    CHECK(t_skip == (uint64_t)(N - (int)(N * 0.96f)), "丢黑时总数应为亮像素数，实得 %llu", (unsigned long long)t_skip);
    CHECK(t_count == t_cpu, "记黑后总数与 CPU 相同（%llu）", (unsigned long long)t_cpu);
    if (t_count == (uint64_t)N && t_count == t_cpu)
        OK("像素守恒：MSL 记黑 %llu = CPU %llu = 全部 %d", (unsigned long long)t_count,
           (unsigned long long)t_cpu, N);

    printf("%s t_hdr_bin_mirror：GPU/CPU 分箱对账 %d 项（失败 %d）\n", fails ? "✗" : "✓", total, fails);
    return fails ? 1 : 0;
}
