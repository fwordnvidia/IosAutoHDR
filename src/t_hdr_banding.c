// 带阶（banding）：8 位 SDR 输入 → 16F 线性输出，中间乘了一条增益曲线。
// 如果增益的导数在某处偏大，相邻两个 8 位码（间隔约 1/255 sRGB）就会被拉成更大的亮度台阶，
// 在暗部渐变上表现为可见的「一圈一圈」。这里把台阶量出来，再判断要不要抖动（dither）。
//
// 判据用「输出台阶 / 输入台阶」的比值（增益的局部斜率）：
//   比值 = 1 表示原样放大、不额外制造台阶；比值 = 2 表示台阶宽度翻倍。
// 另外按 BT.1886 的暗部感知（Weber 1%）看哪一段真的会被人眼看见。
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"
#include <stdio.h>
#include <math.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static float lum(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

int main(void) {
    puts("t_hdr_banding：8 位码相邻台阶经过 gain 曲线后放大多少（1.0 = 原样）");
    s2a_hdr_itm p;
    p.head = 2.0f; p.knee_lo = 0.35f; p.knee_hi = 1.0f;

    float worst = 0; int worst_code = 0;
    printf("  %-10s %-10s %-10s %-10s %-10s\n", "码值", "输入lin", "输出lin", "台阶", "放大");
    for (int code = 1; code < 255; code++) {
        float a[3], b[3], oa[3], ob[3];
        for (int c = 0; c < 3; c++) {
            a[c] = s2a_hdr_to_linear((float)(code - 1) / 255.0f);
            b[c] = s2a_hdr_to_linear((float)(code) / 255.0f);
        }
        s2a_hdr_itm_rgb_lin(&p, a, oa);
        s2a_hdr_itm_rgb_lin(&p, b, ob);
        float d_in = lum(b) - lum(a), d_out = lum(ob) - lum(oa);
        if (d_in <= 0) continue;
        float ratio = d_out / d_in;
        if (ratio > worst) { worst = ratio; worst_code = code; }
        if (code == 8 || code == 32 || code == 96 || code == 160 || code == 200 || code == 254)
            printf("  %-10d %-10.6f %-10.6f %-10.6f %-10.3f\n", code, lum(a), lum(oa), d_in, ratio);
    }
    printf("  最坏放大 %.3f 倍（码值 %d，输入 lin %.4f）\n", worst, worst_code, lum((float[3]){s2a_hdr_to_linear((float)worst_code / 255.0f),
                                                                                            s2a_hdr_to_linear((float)worst_code / 255.0f),
                                                                                            s2a_hdr_to_linear((float)worst_code / 255.0f)}));

    CHECK(worst < 7.0f, "增益把 8 位台阶放大 %.2f 倍（>7 就必须在上屏前加抖动，否则平滑渐变出圈）", worst);
    CHECK(worst > 0.99f, "最坏放大 %.3f 应当 ≥1（有扩张就一定会放大台阶）", worst);

    // 16F 存得下吗：输出最小值附近的可表示精度 vs 最小的输出台阶
    const float rel16 = 1.0f / 2048.0f;       // 16F 尾数 10 位 ⇒ 相对步长 2^-11
    float min_rel = 1e9f;
    for (int code = 1; code < 40; code++) {
        float a[3] = {0}, b[3] = {0}, oa[3], ob[3];
        for (int c = 0; c < 3; c++) {
            a[c] = s2a_hdr_to_linear((float)(code - 1) / 255.0f);
            b[c] = s2a_hdr_to_linear((float)code / 255.0f);
        }
        s2a_hdr_itm_rgb_lin(&p, a, oa);
        s2a_hdr_itm_rgb_lin(&p, b, ob);
        float d = lum(ob) - lum(oa);
        float rel = d / (lum(oa) > 1e-9f ? lum(oa) : 1e-9f);
        if (d > 0 && rel < min_rel) min_rel = rel;
    }
    printf("  暗部最小相对台阶 %.2e（16F 相对步长 %.2e，比值 %.0f×）\n", min_rel, rel16, min_rel / rel16);
    CHECK(min_rel > rel16 * 4.0f, "暗部相对台阶 %.2e 已接近 16F 量化（%.2e），需要抖动", min_rel, rel16);

    // head 扫描：纸白必须恒为 1.00，峰值必须等于 head
    puts("  head   probe[0.18]  probe[0.832]  probe[1.0]");
    const float heads[5] = {1.2f, 2.0f, 4.0f, 8.0f, 16.0f};
    const float white_lin = s2a_hdr_to_linear(0.832f);   // L1b：knee_lo = 纸白
    for (int i = 0; i < 5; i++) {
        s2a_hdr_itm q = p; q.head = heads[i]; q.knee_lo = white_lin;
        float g[3];
        float vals[3] = {0.18f, 0.832f, 1.0f};
        for (int k = 0; k < 3; k++) {
            float v[3] = {0}, o[3] = {0};
            for (int c = 0; c < 3; c++) v[c] = s2a_hdr_to_linear(vals[k]);
            s2a_hdr_itm_rgb_lin(&q, v, o);
            g[k] = lum(o) / (lum(v) > 1e-9f ? lum(v) : 1.0f);
        }
        printf("  %4.1f   %-13.4f %-14.4f %.4f\n", heads[i], g[0], g[1], g[2]);
        CHECK(fabsf(g[1] - 1.0f) < 1e-4f, "head=%.1f 时纸白增益 %.5f 不是 1（L1b 语义：knee_lo=纸白）", heads[i], g[1]);
        CHECK(fabsf(g[2] - heads[i]) < 1e-4f, "head=%.1f 时纯白增益 %.4f 不等于 head", heads[i], g[2]);
    }

    // 两套入口必须等价（编码入口 = 解码后走线性入口），差 ≤1e-6
    float maxdiff = 0;
    for (int code = 0; code <= 255; code++) {
        float enc[3], lin[3], o1[3], o2[3];
        for (int c = 0; c < 3; c++) { enc[c] = (float)code / 255.0f; lin[c] = s2a_hdr_to_linear(enc[c]); }
        s2a_hdr_itm_rgb(&p, enc, o1);
        s2a_hdr_itm_rgb_lin(&p, lin, o2);
        for (int c = 0; c < 3; c++) { float d = fabsf(o1[c] - o2[c]); if (d > maxdiff) maxdiff = d; }
    }
    CHECK(maxdiff < 1e-6f, "编码入口与线性入口不一致（最大差 %.2e）", maxdiff);

    // 最大斜率 vs 白点：纸白越低（画面越暗）→ knee 区间越宽 → 曲线越缓
    puts("  白点   knee 区间        最坏放大");
    const float whites[5] = {0.74f, 0.55f, 0.35f, 0.20f, 0.12f};   // 由窄到宽
    float prev_worst = 1e9f;
    for (int i = 0; i < 5; i++) {
        s2a_hdr_itm q = p; q.knee_lo = whites[i];
        float w = 0;
        for (int code = 1; code < 255; code++) {
            float a[3], b[3], oa[3], ob[3];
            for (int c = 0; c < 3; c++) {
                a[c] = s2a_hdr_to_linear((float)(code - 1) / 255.0f);
                b[c] = s2a_hdr_to_linear((float)code / 255.0f);
            }
            s2a_hdr_itm_rgb_lin(&q, a, oa); s2a_hdr_itm_rgb_lin(&q, b, ob);
            float di = lum(b) - lum(a); if (di <= 0) continue;
            float r = (lum(ob) - lum(oa)) / di;
            if (r > w) w = r;
        }
        printf("  %.2f   %.2f..%.2f          %.2f\n", whites[i], whites[i], q.knee_hi, w);
        CHECK(w < prev_worst + 1e-3f, "白点 %.2f 的最坏放大 %.2f 反而更大（区间变宽应当更缓）", whites[i], w);
        prev_worst = w;
    }

    if (fails) { printf("t_hdr_banding：%d 项失败\n", fails); return 1; }
    printf("✓ t_hdr_banding：台阶放大 / 16F 余量 / head 扫描 %d 项 → 全过\n", checks);
    return 0;
}
