// hdr_shader_image：在**真实照片的每个像素**上对账「上屏 shader 转写」与「CPU 参考实现」。
//
// 与 t_hdr_shader_mirror 的分工：那道闸在色彩空间网格（9³ 编码值 × 拐点 × head × sat × p3）上
// 逐组比对；这里拿一整幅图，用该帧自己的 L1b 拐点，逐像素跑两条路径，累计最大差与超 head 量。
// 网格能保证「算式一致」，整幅能保证「在真实内容上一致」（含 P3 混色、Lab 二分、clip 的交互）。
//
// 用法：hdr_shader_image [--head 2] [--sat 1] [--p3 0] [--dump 前缀] frame.ppm
//   输出：pixels,lo,hi,head,sat,p3,max_diff,over_pixels,over_max
//   退出码非 0 表示超差（>1e-4）或有通道超 head。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"
#include "nvn_hdr_msl.h"

typedef struct { int w, h; unsigned char *px; } img;

static int read_ppm(const char *p, img *im) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char m[3] = {0};
    int maxv = 0;
    if (fscanf(f, "%2s %d %d %d", m, &im->w, &im->h, &maxv) != 4 || strcmp(m, "P6") || maxv != 255) {
        fclose(f); return 0;
    }
    fgetc(f);
    size_t n = (size_t)im->w * im->h * 3;
    im->px = malloc(n);
    if (!im->px || fread(im->px, 1, n, f) != n) { fclose(f); free(im->px); return 0; }
    fclose(f);
    return 1;
}

static int write_ppm(const char *p, int w, int h, const float *rgb) {
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        for (int c = 0; c < 3; c++) {
            float v = rgb[i * 3 + c] / 2.0f;         // head=2 时把 HDR 压到可看的 8 位
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            fputc((int)(v * 255.0f + 0.5f), f);
        }
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    float head = 2.0f, sat = 1.0f, p3 = 0.0f;
    const char *dump = NULL, *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--head") && i + 1 < argc) head = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat") && i + 1 < argc) sat = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--p3") && i + 1 < argc) p3 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "用法: hdr_shader_image [--head 2] [--sat 1] [--p3 0] [--dump 前缀] frame.ppm\n"); return 2; }

    img im;
    if (!read_ppm(path, &im)) { fprintf(stderr, "读不了 %s\n", path); return 2; }

    // 该帧自己的拐点：与 hdr_seq 同一条判定链
    static unsigned hist[S2A_HDR_HIST_BINS];
    memset(hist, 0, sizeof hist);
    const int stride = 2;
    for (int y = 0; y < im.h; y += stride) for (int x = 0; x < im.w; x += stride) {
        size_t k = ((size_t)y * im.w + x) * 3;
        float lin[3];
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)im.px[k + (size_t)c] / 255.0f);
        hist[s2a_hdr_hist_bin(0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2])]++;
    }
    float wbulk = 0.0f, wov = 0.0f;
    if (s2a_hdr_sdr_white_bulk(hist, &wbulk)) wov = wbulk;
    s2a_hdr_auto_l1b b;
    memset(&b, 0, sizeof b);
    if (!s2a_hdr_auto_l1b_step_fused_hz(hist, wov, 0.0f, 1.0f, 0.0f, 0.0f, 0, 60.0f, &b)) {
        fprintf(stderr, "该帧没有可用直方图\n");
        return 2;
    }
    s2a_hdr_itm itm;
    itm.head = head;
    itm.knee_lo = b.knee_lo;
    itm.knee_hi = b.knee_hi;

    float *dev = dump ? malloc((size_t)im.w * im.h * 3 * sizeof(float)) : NULL;
    float *cpu = dump ? malloc((size_t)im.w * im.h * 3 * sizeof(float)) : NULL;
    double max_diff = 0.0;
    float over_max = 0.0f;
    long over_pixels = 0;
    const size_t n = (size_t)im.w * im.h;
    for (size_t i = 0; i < n; i++) {
        float code[3];
        for (int c = 0; c < 3; c++) code[c] = im.px[i * 3 + (size_t)c] / 255.0f;
        float a[3], g[3], lin[3];
        msl_fs(code, itm.knee_lo, itm.knee_hi, head, sat, p3, a);
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(code[c]);
        s2a_hdr_grade_rgb(&itm, sat, p3, lin, g);

        for (int c = 0; c < 3; c++) {
            double d = fabs((double)a[c] - (double)g[c]);
            if (d > max_diff) max_diff = d;
            if (a[c] > head + 1e-6f || g[c] > head + 1e-6f) {
                float ov = (a[c] > g[c] ? a[c] : g[c]) - head;
                if (ov > over_max) over_max = ov;
            }
        }
        int over = 0;
        for (int c = 0; c < 3; c++) if (a[c] > head + 1e-6f || g[c] > head + 1e-6f) over = 1;
        over_pixels += over;
        if (dump) for (int c = 0; c < 3; c++) { dev[i * 3 + (size_t)c] = a[c]; cpu[i * 3 + (size_t)c] = g[c]; }
    }
    printf("%ld,%.4f,%.4f,%.2f,%.2f,%.2f,%.9f,%ld,%.9f\n", (long)n, (double)itm.knee_lo, (double)itm.knee_hi,
           (double)head, (double)sat, (double)p3, max_diff, over_pixels, (double)over_max);

    if (dump) {
        char p1[512], p2[512];
        snprintf(p1, sizeof p1, "%s-dev.ppm", dump);
        snprintf(p2, sizeof p2, "%s-cpu.ppm", dump);
        write_ppm(p1, im.w, im.h, dev);
        write_ppm(p2, im.w, im.h, cpu);
    }
    free(im.px); free(dev); free(cpu);
    if (max_diff > 1e-4) { fprintf(stderr, "✗ 上屏与 CPU 最大差 %.9f 超 1e-4\n", max_diff); return 1; }
    if (over_pixels) { fprintf(stderr, "✗ %ld 个像素有通道超 head（最大 %.9f）\n", over_pixels, (double)over_max); return 1; }
    return 0;
}
