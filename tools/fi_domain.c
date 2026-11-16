// fi_domain.c —— 补帧的运动估计在两个「域」上是否等价？
//
// 背景：上游 fi_mv_setup 的 MSL 里有两个写死的常数，都是在 **8 位 sRGB 编码域** 上成立的：
//   · match 里 `if (zsad <= best * 1.02 + 0.05) bd = int2(0);`   —— SAD 的绝对偏移 0.05
//   · spread 里 `if (all(abs(cur.read(gp).rgb - prev.read(gp).rgb) < 0.004)) v = float2(0);`
//     —— 逐通道差 0.004，约等于「差不到 1 个 8 位码值」（1/255 = 0.00392）
// 我们把 HDR 的补帧输入换成了 RGBA16Float（线性、可 > 1）。texture read 不解码，
// 所以 luma 提取读到的值从「sRGB 编码」变成「线性」——两者的数值尺度完全不同：
//   线性 0.18 == 编码 0.46；线性 0.004 == 编码 0.06（约 15 个码值）。
//
// 这个工具不跑 Metal，只把两条路径在同一张真实照片上各算一遍，量出：
//   1) 同内容、±1 码值噪声下的 SAD 中位数（决定 `best*1.02+0.05` 还是不是一个合理门槛）
//   2) 「前后两帧这个像素一样」的命中率（决定 `0.004` 还挡不挡得住静止物体）
//   3) 把 16F 的线性值**先转回编码域**再算 luma，两个数是否回到 8 位路径的量级
//
// 用法：cc -O2 -w -I src tools/fi_domain.c -lm -o build/fi_domain && ./build/fi_domain build/real/p_*.ppm
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int w, h; unsigned char *p; } Img;

static int read_ppm(const char *path, Img *o) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8] = {0};
    if (fscanf(f, "%7s", magic) != 1 || strcmp(magic, "P6")) { fclose(f); return 0; }
    int w = 0, h = 0, mx = 0;
    if (fscanf(f, "%d %d %d", &w, &h, &mx) != 3 || w <= 0 || h <= 0) { fclose(f); return 0; }
    fgetc(f);
    size_t n = (size_t)w * h * 3;
    unsigned char *b = malloc(n);
    if (!b || fread(b, 1, n, f) != n) { free(b); fclose(f); return 0; }
    fclose(f);
    o->w = w; o->h = h; o->p = b;
    return 1;
}

static inline float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static inline float lin_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

// 域 0：8 位路径，read 得到 sRGB 编码值，luma 直接在编码域加权
// 域 1：16F 路径不改，read 得到线性值，luma 在线性域加权（当前实现的真实行为）
// 域 2：16F 路径 + 转回编码域再算 luma（本工具要验证的修法）
static float luma_at(const Img *im, int x, int y, int domain) {
    const unsigned char *q = im->p + ((size_t)y * im->w + x) * 3;
    float r = q[0] / 255.0f, g = q[1] / 255.0f, b = q[2] / 255.0f;
    if (domain == 0) return 0.299f * r + 0.587f * g + 0.114f * b;
    float lr = srgb_to_lin(r), lg = srgb_to_lin(g), lb = srgb_to_lin(b);
    float l = 0.299f * lr + 0.587f * lg + 0.114f * lb;
    return domain == 1 ? l : lin_to_srgb(l);
}

// down：每 4x4 平均成一个 luma 像素（与 MSL 的 down kernel 同构）
static void quarter_luma(const Img *im, int domain, float *out, int *qw, int *qh) {
    int w = im->w / 4, h = im->h / 4;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float s = 0;
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) s += luma_at(im, x * 4 + i, y * 4 + j, domain);
        out[y * w + x] = s / 16.0f;
    }
    *qw = w; *qh = h;
}

// 把一帧换成「同内容 + ±1 码值确定性噪声」，代表压缩/时域抖动下最温和的帧间差异
static void jitter(Img *im, unsigned seed) {
    unsigned s = seed;
    for (size_t i = 0; i < (size_t)im->w * im->h * 3; i++) {
        s = s * 1103515245u + 12345u;
        int d = (int)((s >> 16) % 3) - 1;
        int v = (int)im->p[i] + d;
        im->p[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法：fi_domain <照片.ppm ...>\n"); return 2; }
    static const char *DN[3] = {"8 位 sRGB 编码域", "16F 线性域（当前实现）", "16F 转回编码域（本工具验证的修法）"};
    int bad = 0;
    printf("%-34s %-26s %8s %8s %8s %8s\n", "照片", "域", "SAD中位", "SAD零比", "静止命中", "偏零可用");
    for (int a = 1; a < argc; a++) {
        Img im; if (!read_ppm(argv[a], &im)) { fprintf(stderr, "读不了 %s\n", argv[a]); return 2; }
        Img im2 = im;
        im2.p = malloc((size_t)im.w * im.h * 3);
        memcpy(im2.p, im.p, (size_t)im.w * im.h * 3);
        jitter(&im2, 12345u + (unsigned)a);

        for (int d = 0; d < 3; d++) {
            int qw = 0, qh = 0, qw2 = 0, qh2 = 0;
            float *la = malloc(sizeof(float) * (size_t)(im.w / 4) * (size_t)(im.h / 4));
            float *lb = malloc(sizeof(float) * (size_t)(im2.w / 4) * (size_t)(im2.h / 4));
            quarter_luma(&im, d, la, &qw, &qh);
            quarter_luma(&im2, d, lb, &qw2, &qh2);

            // 逐 8x8 块（半尺寸 luma 上即为原图 32x32 块）算同位置 SAD —— 就是 zsad
            int nb = 0, zeros = 0;
            float *sads = malloc(sizeof(float) * 4096);
            for (int by = 0; by + 8 <= qh && nb < 4096; by += 8)
                for (int bx = 0; bx + 8 <= qw && nb < 4096; bx += 8) {
                    float sad = 0;
                    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                        sad += fabsf(la[(by + y) * qw + bx + x] - lb[(by + y) * qw + bx + x]);
                    if (sad == 0) zeros++;
                    sads[nb++] = sad;
                }
            qsort(sads, nb, sizeof(float), cmp_float);
            float med = nb ? sads[nb / 2] : 0;

            // `abs(cur-prev) < 0.004` 的命中率：逐像素（原图尺度，三通道全满足）
            long hit = 0, tot = (long)im.w * im.h;
            for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) {
                const unsigned char *p1 = im.p + ((size_t)y * im.w + x) * 3, *p2 = im2.p + ((size_t)y * im.w + x) * 3;
                int ok = 1;
                for (int c = 0; c < 3; c++) {
                    float v1 = p1[c] / 255.0f, v2 = p2[c] / 255.0f;
                    if (d == 1) { v1 = srgb_to_lin(v1); v2 = srgb_to_lin(v2); }
                    else if (d == 2) { v1 = srgb_to_lin(v1); v2 = srgb_to_lin(v2); v1 = lin_to_srgb(v1); v2 = lin_to_srgb(v2); }
                    if (fabsf(v1 - v2) >= 0.004f) { ok = 0; break; }
                }
                hit += ok;
            }
            // `zsad <= best*1.02 + 0.05` 在「噪声块」上还有没有余量：best 取同位置 SAD 本身，
            // 条件退化为 0 <= 0.02*sad + 0.05，恒真；真正的门槛是「邻域更优时要赢过 0.05 的绝对偏移」。
            // 这里报的是「0.05 相当于多少个噪声块 SAD」，即 SAD 噪声底与常数之比。
            printf("%-34s %-26s %8.5f %7.1f%% %7.1f%% %7.2f×\n",
                   a == 1 ? argv[a] : "", DN[d], med, nb ? 100.0 * zeros / nb : 0,
                   tot ? 100.0 * hit / tot : 0, med > 0 ? 0.05f / med : 0);
            free(la); free(lb); free(sads);
        }
        free(im2.p); free(im.p);
    }
    return bad;
}
