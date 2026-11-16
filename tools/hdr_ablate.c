/*
 * 消融实验：把「自动判白点」拆成四档策略，看每一步各自解决什么。
 *
 * 为什么做这个：只给最终曲线，读者无法判断「不这么复杂会怎样」。四档策略是
 *   1. fixed —— 老式固定拐点（0.55 / 1.0），不看画面；
 *   2. apl   —— 只看平均亮度（naive：lo = 平均亮度，hi = 1.0）；
 *   3. white —— 只看白点（lo = 白点，hi = 1.0），不做 HUD 剔除、不做暗场兜底；
 *   4. full  —— 本补丁的 L1b：bulk 白点 + 暗场 lit 判据 + 死区 + 场景融合 + 帧率归一。
 *
 * 输出 CSV：policy,scene,knee_lo,knee_hi,gain_white,gain_050,gain_018,clip_frac
 * 以及切镜序列：policy,scene,frames85
 *
 * 全 C、无依赖；`cc -O2 -w -I src tools/hdr_ablate.c -lm -o hdr_ablate`
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

#define W 96
#define H 54
static unsigned char img[W * H * 3];

static const char *POLICIES[] = {"fixed", "apl", "white", "full"};

/* ---------- 场景 ---------- */
static void fill_uniform(float v) {
    for (int i = 0; i < W * H; i++) {
        float s = s2a_hdr_to_srgb(s2a_hdr_to_linear(v));  /* 保持 v 为编码值语义 */
        unsigned q = (unsigned)(v * 255.0f + 0.5f);
        (void)s;
        img[i * 3 + 0] = img[i * 3 + 1] = img[i * 3 + 2] = (unsigned char)q;
    }
}

static void scene_uiwhite(void) { fill_uniform(0.86f); }

/* 中间调画面 + 底部一行 HUD 白条（1.9% 像素，正是真实 UI 的量级） */
static void scene_hud(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned q = (y == H - 1) ? 242 : 120;   /* 1 行 ≈ 1.9%，低于 5% 剔除门限 */
            unsigned char *p = img + (y * W + x) * 3;
            p[0] = p[1] = p[2] = (unsigned char)q;
        }
}

/* 99.9% 纯黑 + 0.1% 亮点 */
static void scene_star(void) {
    memset(img, 0, sizeof(img));
    for (int i = 0; i < W * H / 1000 * 1 + 5; i++) {
        unsigned char *p = img + ((i * 977) % (W * H)) * 3;
        p[0] = p[1] = p[2] = 184;
    }
}

/* 全屏 18% 灰：用来验「中间调不被抬起」 */
static void scene_mid(void) { fill_uniform(0.46f); }

static void hist_of(const unsigned char *rgb, int n, uint32_t hist[S2A_HDR_HIST_BINS]) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < n; i += 2) {                   /* stride 2，与上屏一致 */
        float lin[3];
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(rgb[i * 3 + c] / 255.0f);
        float y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
        int b = s2a_hdr_hist_bin(y);
        hist[b]++;
    }
}

/* ---------- 四档策略 ---------- */
typedef struct { float lo, hi; } knee;

typedef struct {
    float prev_lo, prev_hi, prev_avg, prev_white;
    int have;
} state;

/* 三档「无自适应平滑」的策略直接给目标；full 走 L1b 的平滑与死区。 */
static knee target_of(int policy, const uint32_t hist[S2A_HDR_HIST_BINS], float hz) {
    knee k = {0.55f, 1.0f};
    (void)hz;
    if (policy == 0) return k;                                   /* fixed */
    if (policy == 1) {                                           /* apl */
        int ok = 0;
        float avg = s2a_hdr_hist_avg(hist, &ok);
        if (!ok) avg = -6.0f;
        float lin = powf(2.0f, avg);
        if (!(lin > 0.0f)) lin = 0.02f;
        k.lo = lin < 0.02f ? 0.02f : (lin > 0.95f ? 0.95f : lin);
        k.hi = 1.0f;
        return k;
    }
    /* white：只看普通白点，不做 bulk 剔除/暗场兜底 */
    float w = 0.0f;
    s2a_hdr_sdr_white(hist, &w);
    if (w <= 0.0f) w = 0.55f;
    k.lo = w < 0.01f ? 0.01f : (w > 0.95f ? 0.95f : w);
    k.hi = 1.0f;
    return k;
}

static knee step_policy(int policy, state *st, const uint32_t hist[S2A_HDR_HIST_BINS], float hz,
                        s2a_hdr_auto_l1b *out) {
    if (policy == 3) {
        float wbulk = 0.0f;
        float wov = 0.0f;
        if (s2a_hdr_sdr_white_bulk(hist, &wbulk)) wov = wbulk;
        (void)s2a_hdr_auto_l1b_step_fused_hz(hist, wov, st->prev_lo, st->prev_hi, st->prev_avg,
                                             st->prev_white, st->have, hz, out);
        st->prev_lo = out->knee_lo;
        st->prev_hi = out->knee_hi;
        st->prev_avg = out->avg;
        st->prev_white = out->white;
        st->have = 1;
        knee k = {out->knee_lo, out->knee_hi};
        return k;
    }
    knee t = target_of(policy, hist, hz);
    /* 与 full 用同一个平滑系数（0.10 @60Hz），这样时间行为可比 */
    float coef = s2a_hdr_coef_at_hz(0.10f, hz);
    if (!st->have) { st->prev_lo = t.lo; st->prev_hi = t.hi; st->have = 1; }
    st->prev_lo += (t.lo - st->prev_lo) * coef;
    st->prev_hi += (t.hi - st->prev_hi) * coef;
    t.lo = st->prev_lo;
    t.hi = st->prev_hi;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->knee_lo = t.lo;
        out->knee_hi = t.hi;
        out->white = t.lo;
        int ok2 = 0;
        out->avg = s2a_hdr_hist_avg(hist, &ok2);
    }
    return t;
}

static float gain_at(const knee *k, float y_lin, float head) {
    s2a_hdr_itm itm;
    itm.head = head;
    itm.knee_lo = k->lo;
    itm.knee_hi = k->hi < k->lo ? k->lo : k->hi;
    return s2a_hdr_itm_gain(&itm, y_lin);
}

typedef struct {
    const char *name;
    void (*fill)(void);
    float probe;       /* 这个场景要看的探针（编码值） */
} scene_def;

static const scene_def SCENES[] = {
    {"uiwhite", scene_uiwhite, 0.86f},
    {"hud",     scene_hud,     0.47f},
    {"star",    scene_star,    0.02f},
    {"mid",     scene_mid,     0.46f},
};

int main(int argc, char **argv) {
    int seq = (argc > 1 && !strcmp(argv[1], "--seq"));
    const float head = 2.0f;
    const float hz = 60.0f;

    if (!seq) {
        printf("policy,scene,knee_lo,knee_hi,gain_white,gain_018,gain_050,gain_090,clip_frac\n");
        for (int p = 0; p < 4; p++) {
            for (unsigned s = 0; s < sizeof(SCENES) / sizeof(SCENES[0]); s++) {
                SCENES[s].fill();
                uint32_t hist[S2A_HDR_HIST_BINS];
                hist_of(img, W * H, hist);
                state st = {0};
                s2a_hdr_auto_l1b b;
                /* 跑到稳态：60 帧同一画面 */
                knee k = {0.55f, 1.0f};
                for (int f = 0; f < 60; f++) k = step_policy(p, &st, hist, hz, &b);

                float y_probe = s2a_hdr_to_linear(SCENES[s].probe);
                float y_white = s2a_hdr_to_linear(0.86f);
                /* 越界比例：整帧逐像素算 */
                int over = 0, tot = 0;
                s2a_hdr_itm itm = {head, k.lo, k.hi};
                for (int i = 0; i < W * H; i += 7) {
                    float lin[3], out[3];
                    for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(img[i * 3 + c] / 255.0f);
                    s2a_hdr_itm_rgb_lin(&itm, lin, out);
                    for (int c = 0; c < 3; c++)
                        if (out[c] > head + 1e-4f) over++;
                    tot++;
                }
                (void)y_probe;
                printf("%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", POLICIES[p], SCENES[s].name,
                       k.lo, k.hi, gain_at(&k, y_white, head),
                       gain_at(&k, 0.180f, head), gain_at(&k, 0.500f, head), gain_at(&k, 0.900f, head),
                       tot ? (float)over / (float)(tot * 3) : 0.0f);
            }
        }
        return 0;
    }

    /* 切镜序列：前 32 帧中点画面，后 32 帧 UI 白；量「多少帧到 85%」。
       两遍：第一遍只求稳态（f=63 的 lo），第二遍才算覆盖率。 */
    printf("policy,scene,frames85,knee_first,knee_last\n");
    for (int p = 0; p < 4; p++) {
        float lo[64];
        for (int pass = 0; pass < 2; pass++) {
            state st = {0};
            s2a_hdr_auto_l1b b;
            knee k = {0.55f, 1.0f};
            for (int f = 0; f < 64; f++) {
                if (f < 32) scene_mid(); else scene_uiwhite();
                uint32_t hist[S2A_HDR_HIST_BINS];
                hist_of(img, W * H, hist);
                k = step_policy(p, &st, hist, hz, &b);
                lo[f] = k.lo;
            }
            if (pass == 1) break;
        }
        float first = lo[0], after = lo[32], steady = lo[63];
        int frames85 = -1;
        float span = steady - after;
        if (span > 1e-4f) {
            for (int f = 32; f < 64; f++)
                if ((lo[f] - after) / span >= 0.85f) { frames85 = f - 32; break; }
        } else if (fabsf(after - first) > 1e-3f) {
            frames85 = 0;   /* 切镜当帧就到稳态：只可能是一帧跨过去（场景判据生效） */
        }
        printf("%s,cut,%d,%.4f,%.4f\n", POLICIES[p], frames85, first, lo[63]);
    }
    return 0;
}
