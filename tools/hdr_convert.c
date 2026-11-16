// hdr_convert：把一张 SDR 图（P6 PPM）转成 16 位线性 HDR 预览。
//
// 这是给使用者用的那一个命令：读图 → 直方图分析（HUD 剔除 → L1b）→ 色域/饱和度 →
// 逆色调映射 → 写 16 位 PPM（值域 [0, head]，65535 = head）。
// 用的就是补丁里同一份 nvn_hdr*.h，所以「预览什么样」= 上屏会算出什么样（差一个显示映射）。
//
// 用法：hdr_convert in.ppm out.ppm [--head 2] [--p3 1] [--sat 1.2] [--algo l1b|l1] [--map knee|bt2408] [--dither]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

typedef struct { int w, h; unsigned *px; } img;

static int read_ppm(const char *p, img *im) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", p); return 0; }
    char m[3] = {0}; int maxv = 0;
    if (fscanf(f, "%2s %d %d %d", m, &im->w, &im->h, &maxv) != 4 || strcmp(m, "P6") || maxv != 255) {
        fprintf(stderr, "%s 不是 8 位 P6 PPM\n", p); fclose(f); return 0;
    }
    fgetc(f);
    size_t n = (size_t)im->w * im->h * 3;
    unsigned char *b = malloc(n);
    if (!b || fread(b, 1, n, f) != n) { fclose(f); free(b); return 0; }
    fclose(f);
    im->px = malloc(n * sizeof(unsigned));
    for (size_t i = 0; i < n; i++) im->px[i] = b[i];
    free(b);
    return 1;
}

// 16 位 PPM：值域 [0, head]，65535 = head。写成「显示无关的线性能量」，不是 sRGB 编码。
static int write_ppm16(const char *p, int w, int h, const float *lin, float head) {
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "写不了 %s\n", p); return 0; }
    fprintf(f, "P6\n%d %d\n65535\n", w, h);
    unsigned char *row = malloc((size_t)w * 3 * 2);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                float v = lin[((size_t)y * w + x) * 3 + c] / head;
                if (!(v > 0.0f)) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                unsigned q = (unsigned)lrintf(v * 65535.0f);
                row[(x * 3 + c) * 2 + 0] = (unsigned char)(q >> 8);
                row[(x * 3 + c) * 2 + 1] = (unsigned char)(q & 0xff);
            }
        }
        fwrite(row, 1, (size_t)w * 3 * 2, f);
    }
    free(row);
    fclose(f);
    return 1;
}

// 确定性抖动（同一像素每次一样），只影响最低几位，用来把「台阶」换成「颗粒」
static float dither_of(int x, int y, int c) {
    unsigned h = (unsigned)(x * 1973 + y * 9277 + c * 26699);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return ((float)(h & 0xffffu) / 65535.0f) - 0.5f;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    float head = 2.0f, p3 = 0.0f, sat = 1.0f;
    int map_bt2408 = 0;
    int algo_l1b = 1, dither = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--head") && i + 1 < argc) head = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--p3") && i + 1 < argc) p3 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat") && i + 1 < argc) sat = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "bt2408")) map_bt2408 = 1;
            else if (!strcmp(m, "knee")) map_bt2408 = 0;
            else { fprintf(stderr, "--map 只能是 knee 或 bt2408\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--algo") && i + 1 < argc) algo_l1b = strcmp(argv[++i], "l1") != 0;
        else if (!strcmp(argv[i], "--dither")) dither = 1;
        else if (!in) in = argv[i];
        else out = argv[i];
    }
    if (!in || !out) {
        fprintf(stderr, "用法: hdr_convert in.ppm out.ppm [--head 2] [--p3 1] [--sat 1.2] [--algo l1b|l1] [--map knee|bt2408] [--dither]\n");
        return 2;
    }
    if (!(head >= 1.0f && head <= 4.0f)) { fprintf(stderr, "--head 必须在 [1,4]\n"); return 2; }

    img im;
    if (!read_ppm(in, &im)) return 1;

    // 1) 直方图（与上屏完全相同的采样：Rec.709 亮度、stride 2、sRGB 解码）
    static uint32_t hist[S2A_HDR_HIST_BINS];
    memset(hist, 0, sizeof hist);
    size_t npx = (size_t)im.w * im.h;
    float *lin = malloc(npx * 3 * sizeof(float));
    for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) {
        size_t k = ((size_t)y * im.w + x) * 3;
        for (int c = 0; c < 3; c++) lin[k + c] = s2a_hdr_to_linear((float)im.px[k + c] / 255.0f);
        if (y % 2 == 0 && x % 2 == 0)
            hist[s2a_hdr_hist_bin(0.2126f * lin[k] + 0.7152f * lin[k + 1] + 0.0722f * lin[k + 2])]++;
    }

    // 2) 分析：HUD/孤立高光剔除后的白点 → L1b 目标（纸白不动、裁切点 = SDR 白）
    float wbulk = 0.0f, wov = 0.0f, lo = 0.55f, hi = 1.0f;
    if (s2a_hdr_sdr_white_bulk(hist, &wbulk)) wov = wbulk;
    s2a_hdr_auto_l1b b;
    memset(&b, 0, sizeof b);
    int ok = s2a_hdr_auto_l1b_step_fused(hist, wov, 0.55f, 1.0f, 0.0f, 0.0f, 0, &b);
    if (ok) { lo = b.knee_lo; hi = b.knee_hi; }
    else {   // 全黑帧：没有可测内容，按 L1 兜底
        float unused;
        if (s2a_hdr_auto_knee(hist, &hi, &unused)) lo = hi * S2A_HDR_KNEE_RATIO;
    }
    s2a_hdr_itm itm; itm.head = head; itm.knee_lo = lo; itm.knee_hi = hi;

    // 3) 逐像素：解码 → 色域/饱和度 → 映射 → 写 16 位线性
    //    --map bt2408：BT.2408-8 §5.1 的 display-referred 放置（×head 的幂函数，黑仍为 0），
    //    与缺省的 knee 方案并列，两条都保留 —— 观感不同，用途不同（见 docs/DESIGN.md）。
    for (size_t k = 0; k + 2 < npx * 3; k += 3) {
        float out_rgb[3];
        if (map_bt2408) {
            float g[3];
            if (sat == 1.0f && p3 <= 0.0f) {
                memcpy(g, lin + k, sizeof g);
            } else {
                s2a_hdr_p3_rgb(lin + k, p3, g);
                s2a_hdr_sat_rgb(g, sat, g);
            }
            s2a_hdr_bt2408_rgb_lin(g, head, out_rgb);
        } else {
            s2a_hdr_grade_rgb(&itm, sat, p3, lin + k, out_rgb);
        }
        if (dither) {
            int x = (int)((k / 3) % (size_t)im.w), y = (int)((k / 3) / (size_t)im.w);
            for (int c = 0; c < 3; c++) out_rgb[c] += dither_of(x, y, c) / 65535.0f * head;
        }
        lin[k] = out_rgb[0]; lin[k + 1] = out_rgb[1]; lin[k + 2] = out_rgb[2];
    }
    if (!write_ppm16(out, im.w, im.h, lin, head)) return 1;

    printf("%s → %s  %dx%d  head %.1f  map %s  knee_lo %.4f  knee_hi %.4f  white %.4f  p3 %.1f  sat %.2f  algo %s\n",
           in, out, im.w, im.h, (double)head, map_bt2408 ? "bt2408" : "knee",
           (double)itm.knee_lo, (double)itm.knee_hi,
           (double)b.white, (double)p3, (double)sat, algo_l1b ? "l1b" : "l1");
    return 0;
}
