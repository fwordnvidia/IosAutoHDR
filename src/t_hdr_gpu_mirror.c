// t_hdr_gpu_mirror：把 MSL kernel `hdr_hist_l1`（src/hdr_hist.metal）逐行翻成 C，
// 和 CPU 参考实现比同一份直方图上的四个输出：hi / white / avg / scene。
//
// 为什么必须有两个实现对比：GPU 路径在本机编不了（要 xcrun metal），CI 也停着。
// 「算法在 CPU 上测过」不等于「GPU 上跑的是同一套算法」——这份测试就是那条缝的补丁。
// 翻译是逐行照抄（连循环顺序、float 精度、黑场跳过都一样），旁边标了 metal 里对应的位置。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"          // s2a_hdr_sat_lab_rgb / s2a_hdr_lab_from_lin：上屏 MSL 的 CPU 对照
#include "nvn_hdr_auto_l1.h"

static int fails = 0, total = 0;
static void ck(int cond, const char *what, double got, double want) {
    total++;
    if (!cond) { fails++; printf("  ✗ %s（实测 %.6f，期望 %.6f）\n", what, got, want); }
}

// ───── MSL 的镜像 ─────  （metal: kHistLo/kHistHi/kHistBins 与 CPU 同一组）
#define K_HIST_BINS 256
#define K_HIST_LO  (-10.0f)
#define K_HIST_HI  4.0f
#define K_BLACK_CUT   (-6.0f)   // metal: constant float kBlackCut（白点用）
#define K_AVG_CUT     (-9.5f)   // metal: constant float kAvgCut（重心用）
#define K_WHITE_FRAC  0.005f
#define K_KNEE_TARGET 0.8f
#define K_L1_HI_MIN   0.01f
#define K_L1_HI_MAX   2.0f
#define K_SCENE_LOW   0.40f
#define K_SCENE_HIGH  1.10f

static float m_bin_center(int i) { return K_HIST_LO + ((float)i + 0.5f) * ((K_HIST_HI - K_HIST_LO) / (float)K_HIST_BINS); }
static float m_bin_low(int i)    { return K_HIST_LO + (float)i * ((K_HIST_HI - K_HIST_LO) / (float)K_HIST_BINS); }
static float m_bin_edge(int i)   { return K_HIST_LO + ((float)i + 1.0f) * ((K_HIST_HI - K_HIST_LO) / (float)K_HIST_BINS); }

typedef struct { float hi, white, avg, scene; int valid; } mirror_out;

// 对应 metal: kernel void hdr_hist_l1(...)
static mirror_out m_hdr_hist_l1(const uint32_t hist[K_HIST_BINS], float prev_avg, int have_prev) {
    mirror_out o; memset(&o, 0, sizeof o);
    unsigned total = 0u;      // metal 用 uint：超过 4.29e9 会溢出，这里一致
    float acc = 0.0f, all = 0.0f;
    for (int i = 0; i < K_HIST_BINS; i++) {
        unsigned c = hist[i];
        if (c == 0u) continue;
        all += (float)c;
        if (m_bin_low(i) < K_AVG_CUT) continue;     // ← 只排真黑（metal: kAvgCut）
        total += c;
        acc += m_bin_center(i) * (float)c;          // ← float 累加，不是 double
    }
    if (!(all > 0.0f) || total == 0u) return o;     // 空帧 / 全黑
    float avg = acc / (float)total;

    unsigned need = (unsigned)((float)total * K_WHITE_FRAC);
    if (need < 1u) need = 1u;
    float white = 0.0f;
    int found = 0;
    for (int i = K_HIST_BINS - 1; i >= 0; i--) {
        if (m_bin_low(i) < K_BLACK_CUT) break;
        if (hist[i] >= need) { white = exp2f(m_bin_edge(i)); found = 1; break; }
    }
    float hi;
    if (found) {
        hi = white / K_KNEE_TARGET;
        if (!(hi >= K_L1_HI_MIN)) hi = K_L1_HI_MIN;
        if (hi > K_L1_HI_MAX) hi = K_L1_HI_MAX;
    } else {
        unsigned target = (unsigned)((float)total * 0.995f);
        unsigned a2 = 0u; int hit = K_HIST_BINS - 1;
        for (int i = 0; i < K_HIST_BINS; i++) { a2 += hist[i]; if (a2 >= target) { hit = i; break; } }
        hi = exp2f(m_bin_edge(hit)) / K_KNEE_TARGET;
        if (hi < 0.25f) hi = 0.25f;
        if (hi > 1.0f) hi = 1.0f;
    }
    float sc = 0.0f;
    if (have_prev) {
        float d = fabsf(avg - prev_avg);
        if (d > K_SCENE_LOW) {
            if (d >= K_SCENE_HIGH) sc = 1.0f;
            else { float t = (d - K_SCENE_LOW) / (K_SCENE_HIGH - K_SCENE_LOW); sc = t * t * (3.0f - 2.0f * t); }
        }
    }
    o.hi = hi; o.white = white; o.avg = avg; o.scene = sc; o.valid = 1;
    return o;
}

// ───── 比对 ─────
static void compare(const char *name, const uint32_t h[K_HIST_BINS], int expect_equal) {
    mirror_out m = m_hdr_hist_l1(h, -3.0f, 1);

    // CPU 侧：avg / white / scene 各自算
    int ok = 0;
    float c_avg = s2a_hdr_hist_avg(h, &ok);
    float c_white = 0.0f;
    int c_has_white = s2a_hdr_sdr_white(h, &c_white);
    float c_scene = s2a_hdr_scene_weight(-3.0f, c_avg);

    if (!m.valid) {
        ck(ok == 0, name, 1, 0);   // GPU 说空帧，CPU 的判断也必须是空
        return;
    }
    printf("  %-22s GPU avg %8.4f | CPU avg %8.4f | Δ %+.5f\n", name, (double)m.avg, (double)c_avg,
           (double)(m.avg - c_avg));
    if (expect_equal) {
        ck(fabsf(m.avg - c_avg) < 2e-3f, "avg 两侧应一致", m.avg - c_avg, 0);
        ck(fabsf(m.scene - c_scene) < 2e-3f, "scene 两侧应一致", m.scene - c_scene, 0);
        if (c_has_white) ck(fabsf(m.white - c_white) < 5e-3f, "white 两侧应一致", m.white - c_white, 0);
    }
}


// ── 上屏 MSL 的饱和度（s2a_sat）：逐行照抄 src_msl_hdr 里的 CIELAB 版本 ──
// 为什么要测：上屏着色器是 C 字符串，编译器看不到；唯一能在 Linux 上守的就是
// 「把 MSL 翻成 C，和 CPU 参考实现对同一批颜色比结果」。这条曾经抓到过 avg 分叉
// （CPU 把纯黑算进重心，差 3.5 stop），现在也用来看住色相。
static float m_lab_f(float t) { return t > 0.008856f ? powf(t, 1.0f / 3.0f) : (7.787f * t + 16.0f / 116.0f); }
static float m_lab_finv(float t) { float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.0f / 116.0f) / 7.787f; }
static void m_lin_lab(const float rgb[3], float lab[3]) {
    float x = 0.4124564f * rgb[0] + 0.3575761f * rgb[1] + 0.1804375f * rgb[2];
    float y = 0.2126729f * rgb[0] + 0.7151522f * rgb[1] + 0.0721750f * rgb[2];
    float z = 0.0193339f * rgb[0] + 0.1191920f * rgb[1] + 0.9503041f * rgb[2];
    float fx = m_lab_f(x / 0.95047f), fy = m_lab_f(y), fz = m_lab_f(z / 1.08883f);
    lab[0] = 116.0f * fy - 16.0f; lab[1] = 500.0f * (fx - fy); lab[2] = 200.0f * (fy - fz);
}
static void m_lab_lin(const float lab[3], float rgb[3]) {
    float fy = (lab[0] + 16.0f) / 116.0f, fx = fy + lab[1] / 500.0f, fz = fy - lab[2] / 200.0f;
    float x = 0.95047f * m_lab_finv(fx), y = m_lab_finv(fy), z = 1.08883f * m_lab_finv(fz);
    rgb[0] = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    rgb[1] = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    rgb[2] = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;
}
static float m_sat_min(const float lab[3], float s) {
    float t[3] = {lab[0], lab[1] * s, lab[2] * s}, rgb[3];
    m_lab_lin(t, rgb);
    return rgb[0] < rgb[1] ? (rgb[0] < rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] < rgb[2] ? rgb[1] : rgb[2]);
}
static void m_sat(const float in[3], float sat, float out[3]) {
    if (sat == 1.0f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    float lab[3];
    m_lin_lab(in, lab);
    if (fabsf(lab[1]) < 1e-3f && fabsf(lab[2]) < 1e-3f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    float s = sat;
    if (m_sat_min(lab, s) < 0.0f) {
        float lo = 1.0f, hi = s;
        for (int i = 0; i < 14; i++) { float mid = 0.5f * (lo + hi); if (m_sat_min(lab, mid) >= 0.0f) lo = mid; else hi = mid; }
        s = lo;
    }
    float t[3] = {lab[0], lab[1] * s, lab[2] * s};
    m_lab_lin(t, out);
    for (int i = 0; i < 3; i++) if (out[i] < 0.0f) out[i] = 0.0f;
}

// MSL 的 s2a_sat 与 CPU 的 s2a_hdr_sat_lab_rgb 比：同一批颜色 × 同一批饱和度
static void sat_parity(void) {
    static const float cols[][3] = {
        {0.20f, 0.20f, 0.20f},   // 中性灰：必须逐位不变
        {0.95f, 0.95f, 0.95f},   // 近白
        {0.80f, 0.05f, 0.02f},   // 高饱和红：Lab 与线性锚定差得最明显
        {0.02f, 0.60f, 0.90f},   // 青蓝：容易穿出负区，触发二分
        {0.03f, 0.02f, 0.35f},   // 深蓝：s>2 时必穿负区
        {1.20f, 0.90f, 0.70f},   // 超白（HDR 高光）：>1 不能被夹掉
    };
    static const float sats[] = {1.0f, 1.2f, 1.6f, 2.0f, 2.5f};
    float worst = 0.0f;
    int neutral_ok = 1, clip_ok = 1;
    for (unsigned i = 0; i < sizeof cols / sizeof cols[0]; i++) {
        for (unsigned j = 0; j < sizeof sats / sizeof sats[0]; j++) {
            float a[3], b[3];
            s2a_hdr_sat_lab_rgb(cols[i], sats[j], a);
            m_sat(cols[i], sats[j], b);
            for (int k = 0; k < 3; k++) {
                float d = fabsf(a[k] - b[k]);
                if (d > worst) worst = d;
            }
            if (i < 2 && sats[j] != 1.0f) {   // 灰/近白：两个实现都必须逐位返回输入
                if (memcmp(a, b, sizeof a) != 0 || memcmp(a, cols[i], sizeof a) != 0) neutral_ok = 0;
            }
            if (i == 5) {   // 超白：不能被夹到 1.0
                if (b[0] < 1.0f || a[0] < 1.0f) clip_ok = 0;
            }
        }
    }
    total++;
    if (worst > 2e-3f) { fails++; printf("  ✗ MSL/CPU 饱和度最大通道差 %.5f（>2e-3）\n", (double)worst); }
    else printf("  · MSL vs CPU 饱和度：30 组最大通道差 %.6f\n", (double)worst);
    total++;
    if (!neutral_ok) { fails++; printf("  ✗ 中性色没有逐位返回输入\n"); }
    total++;
    if (!clip_ok) { fails++; printf("  ✗ 超白被夹到 1.0（高光被裁）\n"); }
}

int main(void) {
    uint32_t h[K_HIST_BINS];
    unsigned seed = 12345u;
    #define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)

    // 1) 普通内容：亮像素铺在 0.05..1.0（无视黑场差异）
    memset(h, 0, sizeof h);
    for (int i = 0; i < 20000; i++) {
        float y = 0.05f + 0.95f * (float)(RND() % 1000) / 1000.0f;
        h[s2a_hdr_hist_bin(y)]++;
    }
    compare("普通内容", h, 1);

    // 2) 带黑边（电影黑边 40% 纯黑）：这正是两个实现会分叉的地方
    memset(h, 0, sizeof h);
    for (int i = 0; i < 12000; i++) { float y = 0.10f + 0.8f * (float)(RND() % 1000) / 1000.0f; h[s2a_hdr_hist_bin(y)]++; }
    for (int i = 0; i < 8000; i++) h[s2a_hdr_hist_bin(0.0f)]++;   // 40% 纯黑
    compare("带黑边", h, 1);

    // 3) 全黑
    memset(h, 0, sizeof h);
    compare("全黑", h, 1);

    // 4) 单个 bin 有值
    memset(h, 0, sizeof h);
    h[s2a_hdr_hist_bin(0.18f)] = 1;
    compare("单 bin", h, 1);

    // 5) 随机 200 组
    int mism = 0;
    for (int t = 0; t < 200; t++) {
        memset(h, 0, sizeof h);
        int n = (int)(RND() % 5000) + 1;
        for (int i = 0; i < n; i++) {
            float y = 0.001f + 3.0f * (float)(RND() % 1000) / 1000.0f;
            h[s2a_hdr_hist_bin(y)]++;
        }
        mirror_out m = m_hdr_hist_l1(h, -2.0f, 1);
        int ok = 0;
        float c_avg = s2a_hdr_hist_avg(h, &ok);
        if (m.valid && fabsf(m.avg - c_avg) > 2e-3f && fabsf(m.avg - c_avg) / (fabsf(c_avg) + 1e-6f) > 1e-3f) mism++;
    }
    total++;
    if (mism) { fails++; printf("  ✗ 随机 200 组里有 %d 组 avg 不一致\n", mism); }

    // 6) 上屏 MSL 的饱和度：与 CPU 参考实现比（色相/中性/不夹高光）
    sat_parity();

    printf("%s t_hdr_gpu_mirror：GPU/CPU 同算法 %d 项（失败 %d）\n", fails ? "✗" : "✓", total, fails);
    return fails ? 1 : 0;
}
