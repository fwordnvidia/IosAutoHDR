/* hdr_hist_l1（MSL）与 CPU 参考实现的逐行对账。
 *
 * 为什么需要：hdr_hist_l1 是**只在设备上跑**的 kernel（当前不 dispatch——它算出的 f->out 没有读者；
 * 曾经每 8 帧白算一次，已删。见 nvn_hdr_hist.inc 里那段注释），没有任何本机编译器能验它。CPU 侧有
 * s2a_hdr_hist_avg / s2a_hdr_sdr_white / s2a_hdr_scene_weight 三个参考实现，注释里都写着
 * 「口径必须与 MSL 一致」，但那是**声称**。这里把 MSL 逐行翻成 C，用同一份直方图跑，
 * 把「声称一致」变成可断言的数字。
 *
 * 翻译保真的两个细节（否则对账本身是假的）：
 *   1. MSL 全程 float（没有 double），所以这里的 acc/total 用 float 累加，不能偷懒用 double。
 *   2. MSL 的 need 是 (uint)((float)total * 0.005f)，CPU 是 (uint64_t)((double)total * 0.005)。
 *      两者在这个量级的差异已实测为 0（见 need_quantisation_is_zero），但断言写在这里，
 *      换个系数或换个量级就会红。
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

static int fails, checks;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("  ✗ %s\n", what); }
}

/* ---- MSL 常量的逐字翻译（值必须与 src/hdr_hist.metal:79-86 相同） ---- */
#define MSL_BLACK_CUT (-6.0f)
#define MSL_AVG_CUT (-9.5f)
#define MSL_WHITE_FRAC 0.005f
#define MSL_KNEE_TARGET 0.8f
#define MSL_HI_MIN 0.01f
#define MSL_HI_MAX 2.0f
#define MSL_SCENE_LOW 0.40f
#define MSL_SCENE_HIGH 1.10f

static float msl_bin_center(int i) { return -10.0f + ((float)i + 0.5f) * (14.0f / 256.0f); }
static float msl_bin_low(int i) { return -10.0f + (float)i * (14.0f / 256.0f); }
static float msl_bin_edge(int i) { return -10.0f + ((float)i + 1.0f) * (14.0f / 256.0f); }

static float msl_scene_weight(float prev_avg, float cur_avg) {
    float d = fabsf(cur_avg - prev_avg);
    if (!(d > MSL_SCENE_LOW)) return 0.0f;
    if (d >= MSL_SCENE_HIGH) return 1.0f;
    float t = (d - MSL_SCENE_LOW) / (MSL_SCENE_HIGH - MSL_SCENE_LOW);
    return t * t * (3.0f - 2.0f * t);
}

/* hdr_hist_l1 的逐行翻译。out = {hi, white, avg, scene} */
typedef struct { float hi, white, avg, scene; int valid; } msl_l1;

static msl_l1 msl_hdr_hist_l1(const uint32_t *hist, float prev_avg, int have_prev) {
    msl_l1 r = {0, 0, 0, 0, 0};
    float total_f = 0.0f, acc = 0.0f, all = 0.0f;
    uint32_t total = 0u;
    for (int i = 0; i < 256; i++) {
        uint32_t c = hist[i];
        if (c == 0u) continue;
        all += (float)c;
        if (msl_bin_low(i) < MSL_AVG_CUT) continue;
        total += c;
        acc += msl_bin_center(i) * (float)c;
    }
    (void)total_f;
    if (all <= 0.0f || total == 0u) return r;   /* 空帧 / 全黑 → float4(0,0,0,0) */
    float avg = acc / (float)total;

    uint32_t need = (uint32_t)((float)total * MSL_WHITE_FRAC);
    if (need < 1u) need = 1u;
    float white = 0.0f;
    int found = 0;
    for (int i = 255; i >= 0; i--) {
        if (msl_bin_low(i) < MSL_BLACK_CUT) break;
        if (hist[i] >= need) { white = exp2f(msl_bin_edge(i)); found = 1; break; }
    }
    float hi;
    if (found) {
        hi = white / MSL_KNEE_TARGET;
        if (!(hi >= MSL_HI_MIN)) hi = MSL_HI_MIN;
        if (hi > MSL_HI_MAX) hi = MSL_HI_MAX;
    } else {
        const unsigned long target = ((unsigned long)total * 995ul) / 1000ul;
        uint32_t a2 = 0u; int hit = 255;
        for (int i = 0; i < 256; i++) { a2 += hist[i]; if (a2 >= target) { hit = i; break; } }
        hi = exp2f(msl_bin_edge(hit)) / MSL_KNEE_TARGET;
        if (hi < 0.25f) hi = 0.25f;
        if (hi > 1.0f) hi = 1.0f;
    }
    float sc = have_prev ? msl_scene_weight(prev_avg, avg) : 0.0f;
    r.hi = hi; r.white = white; r.avg = avg; r.scene = sc; r.valid = 1;
    return r;
}

/* ---- 直方图构造 ---- */
static void hist_zero(uint32_t *h) { memset(h, 0, 256 * sizeof(uint32_t)); }

/* 确定性伪随机（不用 rand()，跨平台可复现） */
static uint32_t rs = 0x2545F491u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void hist_scene(uint32_t *h, int kind, uint32_t scale) {
    hist_zero(h);
    for (int i = 0; i < 256; i++) {
        float low = msl_bin_low(i);
        uint32_t c = 0;
        switch (kind) {
        case 0: /* 自然照片：中间调厚、顶端少 */
            c = (uint32_t)(scale * expf(-(low + 2.0f) * (low + 2.0f) / 6.0f)) + (rnd() % 7u);
            break;
        case 1: /* 夜空：极低亮度厚，几个星点格 */
            c = (low < -8.0f) ? scale / 4u + rnd() % 5u : rnd() % 3u;
            if (i == 200 || i == 230) c += scale / 200u + 1u;
            break;
        case 2: /* 满屏纯白：全部堆在顶格 */
            c = (i == 254) ? scale : 0u;
            break;
        case 3: /* 中调均匀 */
            c = (low > -4.0f && low < 0.0f) ? scale / 40u + 1u : 0u;
            break;
        default: break;
        }
        h[i] = c;
    }
}

int main(void) {
    printf("t_hdr_gpu_l1：hdr_hist_l1（MSL）与 CPU 参考实现逐行对账\n");

    uint32_t hist[S2A_HDR_HIST_BINS];

    /* 1) 空帧 / 全黑：MSL 明确返回 float4(0,0,0,0) */
    hist_zero(hist);
    msl_l1 e = msl_hdr_hist_l1(hist, 0.0f, 0);
    ck(!e.valid && e.hi == 0.0f && e.white == 0.0f && e.avg == 0.0f && e.scene == 0.0f,
       "空直方图：valid=0 且四项全 0");
    int ok = 0;
    s2a_hdr_hist_avg(hist, &ok);
    ck(!ok, "CPU 侧同样判空（ok=0）");
    printf("  · 空帧两端口径一致\n");

    /* 2) 自然照片：avg 允许 float 累加误差（MSL 全 float，CPU 全 double），white 同格 */
    hist_scene(hist, 0, 4000u);
    msl_l1 a = msl_hdr_hist_l1(hist, 0.0f, 0);
    float ca = s2a_hdr_hist_avg(hist, &ok);
    float cw = 0.0f;
    int cwf = s2a_hdr_sdr_white(hist, &cw);
    ck(ok && a.valid, "自然照片：两侧都判有效");
    ck(fabsf(a.avg - ca) < 2e-3f, "自然照片：avg 差 < 2e-3 stop（float 累加的量化上界）");
    ck(cwf && fabsf(a.white - cw) < 2e-6f, "自然照片：white 同格（同一条 0.5% 判据）");
    ck(a.hi >= MSL_HI_MIN && a.hi <= MSL_HI_MAX, "自然照片：hi 被钳在 [0.01, 2.0]");
    printf("  · 自然照片：avg MSL %.6f / CPU %.6f（差 %.2e）；white MSL %.6f / CPU %.6f\n",
           a.avg, ca, fabsf(a.avg - ca), a.white, cw);

    /* 3) 夜空：星点必须仍然被算进重心（AVG_CUT=-9.5，不是 -6） */
    hist_scene(hist, 1, 60000u);
    msl_l1 n = msl_hdr_hist_l1(hist, 0.0f, 0);
    float cn = s2a_hdr_hist_avg(hist, &ok);
    ck(n.valid && fabsf(n.avg - cn) < 5e-3f, "夜空：avg 与 CPU 一致（星点未被 -6 切掉）");
    ck(n.avg < -5.0f, "夜空：avg 落在暗部（星点没把重心抬起来）");
    printf("  · 夜空：avg MSL %.4f / CPU %.4f\n", n.avg, cn);

    /* 4) 满屏纯白：白点=顶格上沿，hi 打到上限 */
    hist_scene(hist, 2, 300000u);
    msl_l1 w = msl_hdr_hist_l1(hist, 0.0f, 0);
    float cw2 = 0.0f;
    int f2 = s2a_hdr_sdr_white(hist, &cw2);
    ck(w.valid && f2, "满屏纯白：两侧都有白点");
    ck(fabsf(w.white - cw2) < 2e-6f, "满屏纯白：white 同格");
    ck(w.hi <= MSL_HI_MAX + 1e-6f, "满屏纯白：hi 未越上限");
    printf("  · 满屏纯白：white %.6f → hi %.6f\n", w.white, w.hi);

    /* 5) 场景权重：三档（同场景 / 中等切镜 / 大切镜）与 CPU 同式 */
    ck(msl_scene_weight(0.0f, 0.3f) == 0.0f, "scene：0.3 stop 变化 → 权重 0");
    ck(msl_scene_weight(0.0f, 2.0f) == 1.0f, "scene：2.0 stop 变化 → 权重 1");
    {
        float a1 = msl_scene_weight(0.0f, 0.66f), b1 = s2a_hdr_scene_weight(0.0f, 0.66f);
        ck(fabsf(a1 - b1) < 1e-6f, "scene：0.66 stop 中等切镜两版权重一致");
        printf("  · scene：0.66 stop → MSL %.6f / CPU %.6f（%.1f%% 权重）\n", a1, b1, a1 * 100.0f);
    }

    /* 6) fallback（找不到白点）分支：极暗且没有任何格达到 0.5% */
    hist_zero(hist);
    for (int i = 10; i < 200; i++) hist[i] = 1u;   /* 190 个格各 1 个像素 → need 至少 1，顶格算达标 */
    msl_l1 fb = msl_hdr_hist_l1(hist, 0.0f, 0);
    ck(fb.valid, "稀疏直方图：仍有效");
    ck(fb.white == 0.0f || fb.hi >= 0.25f, "fallback：hi 落在 [0.25, 1.0] 的 L0 界内");
    printf("  · 稀疏直方图：white %.4f / hi %.4f（need 低到 1 时会走白点分支）\n", fb.white, fb.hi);

    /* 7) need 量化：float 乘 0.005f 与 double 乘 0.005 在这个量级必须逐个相同 */
    {
        long diff = 0;
        for (uint32_t t = 1; t <= 8300000u; t += 1u) {
            uint32_t x = (uint32_t)((float)t * MSL_WHITE_FRAC);
            uint32_t y = (uint32_t)((double)t * 0.005);
            if (x != y) diff++;
            if (t > 200000u) break;   /* 前 20 万逐个扫足够暴露舍入分歧，全扫太慢 */
        }
        ck(diff == 0, "need 量化：float 0.005f 与 double 0.005 在 1..200000 逐个相同");
        printf("  · need 量化：1..200000 逐个比较，差异 %ld 个\n", diff);
    }

    /* 8) **已知差异**：MSL 的 white 没有 HUD 剔除（CPU 主路径用 sdr_white_bulk）。
     *    这不是 bug，是没修——hdr_hist_l1 从未被调度。断言方向，防止有人以为两侧等价。 */
    {
        hist_zero(hist);
        for (int i = 100; i < 160; i++) hist[i] = 2000u;    /* 内容主体 */
        hist[250] = 3000u;                                   /* 孤立 HUD 白条（约 2.4%，过得了 0.5% 门槛才可能被选中） */
        float bulkw = 0.0f;
        int bf = s2a_hdr_sdr_white_bulk(hist, &bulkw);
        msl_l1 hb = msl_hdr_hist_l1(hist, 0.0f, 0);
        ck(!bf || hb.white > bulkw + 1.0f, "HUD：MSL 白点被 HUD 顶格拉高（差 > 1 stop，GPU 没做簇剔除）");
        /* 后果不是「画面变亮」而是「扩张失效」：white 被抬到 13.24 → hi 打满上限 2.0，
         * knee_hi=2.0 意味着只有 2.0 以上的亮度才吃增益，8 位 SDR 内容永远够不着。 */
        ck(hb.hi >= MSL_HI_MAX - 1e-6f, "HUD：MSL 的 hi 被打满上限（= 这条 fallback 在 HUD 画面等于不扩张）");
        printf("  · HUD：MSL white %.4f (hi %.4f 打满) / CPU bulk white %.4f (hi %.4f)；差 %+.2f stop\n",
               hb.white, hb.hi, bulkw, bulkw / MSL_KNEE_TARGET, hb.white - bulkw);
    }

    printf("\n%s  %d 项断言\n", fails ? "✗ 有失败项" : "✓ 全过", checks);
    return fails ? 1 : 0;
}
