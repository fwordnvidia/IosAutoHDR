// 真实照片上的颜色保真度：逐像素 CIELAB 的色相/彩度/明度变化
//
// 网格用例（t_hdr_color / t_hdr_shader_mirror）只证明「给定 RGB 三元组，两侧一致」；
// 这里回答的是用户能看见的问题：**这张照片被推 HDR 之后，颜色有没有偏**。
// 两条配置各跑一遍：
//   default   sat=1.00 p3=0.00   —— 本补丁缺省，只做亮度扩张、不动颜色
//   tuned     sat=1.15 p3=0.50   —— README 里建议的上限档（可选）
// 输出每个配置的：色相偏移（度）的 中位/p99/最大、>5° 像素占比、近中性像素（C<2）的最大色相偏移、
// 近中性像素（C<2）的 a*/b* 绝对位移、以及亮度增益的中位（只看纸白以上的像素，`S2A_HDR_SDR_CLIP` 以下的像素按设计是 ×1）。
//
// 用法：hdr_color_image [--head 2.0] photo1.ppm [photo2.ppm ...]
// 退出码非 0 = 缺省档出现 > 6° 的色相偏移，或近中性像素被染色（>1°）。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

// —— 色彩空间：sRGB 8 位 → 线性 → XYZ(D65) → Lab → 色相角 ——
static void rgb2xyz(const float lin[3], double xyz[3]) {
    xyz[0] = 0.4123908 * lin[0] + 0.3575843 * lin[1] + 0.1804808 * lin[2];
    xyz[1] = 0.2126390 * lin[0] + 0.7151687 * lin[1] + 0.0721923 * lin[2];
    xyz[2] = 0.0193308 * lin[0] + 0.1191948 * lin[1] + 0.9505322 * lin[2];
}
static double lab_f(double t) {
    return t > 216.0 / 24389.0 ? cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
}
static void xyz2lab(const double xyz[3], double lab[3]) {
    const double wx = 0.9504559, wy = 1.0, wz = 1.0890578;   // D65
    double fx = lab_f(xyz[0] / wx), fy = lab_f(xyz[1] / wy), fz = lab_f(xyz[2] / wz);
    lab[0] = 116.0 * fy - 16.0;
    lab[1] = 500.0 * (fx - fy);
    lab[2] = 200.0 * (fy - fz);
}
static void rgb2lab(const float lin[3], double lab[3]) {
    double xyz[3];
    rgb2xyz(lin, xyz);
    xyz2lab(xyz, lab);
}
static double hue_deg(const double lab[3]) { return atan2(lab[2], lab[1]) * 180.0 / M_PI; }
static double chroma(const double lab[3]) { return sqrt(lab[1] * lab[1] + lab[2] * lab[2]); }
static double hue_delta(double a, double b) {
    double d = fabs(a - b);
    while (d > 360.0) d -= 360.0;
    return d > 180.0 ? 360.0 - d : d;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static double pct(const double *v, int n, double p) {   // p ∈ [0,1]
    if (n <= 0) return 0.0;
    int i = (int)floor(p * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}

// 极简 PPM（P6 / 8 位，允许 # 注释）
static unsigned char *read_ppm(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char tok[64];
    if (fscanf(f, "%63s", tok) != 1 || strcmp(tok, "P6") != 0) { fclose(f); return NULL; }   // 只认 P6
    int vals[3] = {0, 0, 0}, nv = 0;
    while (nv < 3 && fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '#') { int c; while ((c = fgetc(f)) != '\n' && c != EOF) {} continue; }
        vals[nv++] = atoi(tok);
    }
    if (nv < 3 || vals[0] <= 0 || vals[1] <= 0) { fclose(f); return NULL; }
    if (vals[2] != 255) { fclose(f); fprintf(stderr, "只支持 8 位 PPM（maxval=255），%s 是 %d\n", path, vals[2]); return NULL; }
    fgetc(f);                                            // 单个空白
    size_t n = (size_t)vals[0] * vals[1] * 3;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, f) != n) { free(px); fclose(f); return NULL; }
    fclose(f);
    *w = vals[0]; *h = vals[1];
    return px;
}

typedef struct { double hue_med, hue_p99, hue_max, neutral_max, frac5, gain_med; int n, n_over; } stats;

static stats run(const unsigned char *px, int w, int h, float sat, float p3, float head) {
    static uint32_t hist[S2A_HDR_HIST_BINS];
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < w * h; i++) {
        float l[3];
        for (int c = 0; c < 3; c++) l[c] = s2a_hdr_to_linear((float)px[i * 3 + c] / 255.0f);
        hist[s2a_hdr_hist_bin(0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2])]++;
    }
    float white = 0.0f, lo = 0.0f, hi = 0.0f;
    s2a_hdr_itm itm = { head, 0.55f, 1.0f };
    if (s2a_hdr_sdr_white_bulk(hist, &white) && s2a_hdr_l1b_targets_from_white(white, &lo, &hi)) {
        itm.knee_lo = lo;
        itm.knee_hi = hi;
    }
    stats st;
    memset(&st, 0, sizeof st);
    st.n = w * h;
    double *hues = malloc(sizeof(double) * (size_t)st.n);
    double *gains = malloc(sizeof(double) * (size_t)st.n);
    double neutral_max = 0.0, gain_sum = 0.0;
    int n_over5 = 0, n_gain = 0;
    for (int i = 0; i < st.n; i++) {
        float lin[3], out[3];
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)px[i * 3 + c] / 255.0f);
        s2a_hdr_grade_rgb(&itm, sat, p3, lin, out);
        double li[3], lo_[3];
        rgb2lab(lin, li);
        rgb2lab(out, lo_);
        double ci = chroma(li), co = chroma(lo_);
        double dh = (ci > 1.0 && co > 0.5) ? hue_delta(hue_deg(li), hue_deg(lo_)) : 0.0;
        hues[i] = dh;
        if (ci < 2.0) {                                  // 近中性：不许被染色
            // 用 a*/b* 的绝对位移（Δ√(a²+b²) 会掩盖方向翻转；色相角在 C≈1 时分母太小）
            double dab = sqrt((lo_[1] - li[1]) * (lo_[1] - li[1]) + (lo_[2] - li[2]) * (lo_[2] - li[2]));
            if (dab > neutral_max) neutral_max = dab;
        }
        if (dh > 5.0) n_over5++;
        float yi = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
        float yo = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
        gains[i] = (yi > 1e-6f) ? (double)(yo / yi) : 1.0;
        if (yi > itm.knee_lo) { gain_sum += gains[i]; n_gain++; }
    }
    qsort(hues, st.n, sizeof(double), cmp_double);
    st.hue_med = pct(hues, st.n, 0.5);
    st.hue_p99 = pct(hues, st.n, 0.99);
    st.hue_max = pct(hues, st.n, 1.0);
    st.neutral_max = neutral_max;
    st.frac5 = 100.0 * (double)n_over5 / (double)st.n;
    st.n_over = n_over5;
    st.gain_med = n_gain ? gain_sum / n_gain : 1.0;
    free(hues); free(gains);
    return st;
}

int main(int argc, char **argv) {
    float head = 2.0f;
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "--head")) { head = (float)atof(argv[2]); first = 3; }
    if (argc <= first) { fprintf(stderr, "用法：hdr_color_image [--head 2.0] photo.ppm ...\n"); return 2; }
    int bad = 0;
    printf("photo,cfg,sat,p3,pixels,hue_med,hue_p99,hue_max,neutral_dab,over5_pct,gain_med_above_lo\n");
    for (int a = first; a < argc; a++) {
        int w = 0, h = 0;
        unsigned char *px = read_ppm(argv[a], &w, &h);
        if (!px) { fprintf(stderr, "读不出 %s\n", argv[a]); return 2; }
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        const char *dot = strrchr(base, '.');
        char name[256];
        size_t len = dot ? (size_t)(dot - base) : strlen(base);
        if (len >= sizeof name) len = sizeof name - 1;
        memcpy(name, base, len); name[len] = 0;
        const float cfgs[2][2] = {{1.00f, 0.00f}, {1.15f, 0.50f}};
        for (int k = 0; k < 2; k++) {
            stats s = run(px, w, h, cfgs[k][0], cfgs[k][1], head);
            printf("%s,%s,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f\n", name,
                   k ? "tuned" : "default", (double)cfgs[k][0], (double)cfgs[k][1],
                   s.n,
                   s.hue_med, s.hue_p99, s.hue_max, s.neutral_max, s.frac5, s.gain_med);
            if (!k) {
                if (s.hue_max > 6.0) { fprintf(stderr, "✗ %s 缺省档最大色相偏移 %.2f° > 6°\n", name, s.hue_max); bad = 1; }
                if (s.neutral_max > 0.5) { fprintf(stderr, "✗ %s 近中性像素 a*/b* 位移 %.2f > 0.5\n", name, s.neutral_max); bad = 1; }
            }
        }
        free(px);
    }
    return bad;
}
