// hdr_batch：对一批真实照片跑一遍自动判定，输出每张图的决策与风险指标。
// 合成场景（city/uiwhite/dark/ramp/star）是可控的，但真实照片会带来它们没有的东西：
// 大块天空、雾、高光反光、JPEG 噪声。这里量的就是「在这类内容上判据会不会乱动」。
//
// 用法：hdr_batch *.ppm > build/real.csv
//   列：file,w,h,apl,black_frac,white,white_bulk,avg,knee_lo,knee_hi,clipped_frac,hue_rms
//   白点 white 与剔除 HUD 后的 white_bulk 必须相同 —— 真实照片里没有「孤立亮簇」，
//   一旦不同就说明剔除判据在自然内容上误伤（把太阳/反光当成 HUD 剔掉）。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

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
    if (!im->px || fread(im->px, 1, n, f) != n) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: hdr_batch *.ppm\n"); return 2; }
    printf("file,w,h,apl,black_frac,white,white_bulk,avg,knee_lo,knee_hi,clipped_frac,hue_rms\n");
    const float head = 2.0f;
    s2a_hdr_itm itm; itm.head = head; itm.knee_lo = 0.55f; itm.knee_hi = 1.0f;

    for (int a = 1; a < argc; a++) {
        img im;
        if (!read_ppm(argv[a], &im)) { fprintf(stderr, "跳过 %s\n", argv[a]); continue; }
        static uint32_t hist[S2A_HDR_HIST_BINS];
        memset(hist, 0, sizeof hist);
        const int stride = 2;
        double sum = 0.0;
        long cnt = 0, black = 0;
        for (int y = 0; y < im.h; y += stride) for (int x = 0; x < im.w; x += stride) {
            size_t i = ((size_t)y * im.w + x) * 3;
            float lin[3];
            for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)im.px[i + (size_t)c] / 255.0f);
            float Y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
            hist[s2a_hdr_hist_bin(Y)]++;
            sum += (double)Y; cnt++;
            if (Y <= 0.0f) black++;
        }
        float apl = cnt ? (float)(sum / (double)cnt) : 0.0f;
        float black_frac = cnt ? (float)black / (float)cnt : 0.0f;

        float white = 0.0f, bulk = 0.0f, avg = 0.0f, lo = 0.0f, hi = 0.0f;
        (void)s2a_hdr_auto_l1b_targets(hist, 0.0f, &white, &avg, &lo, &hi);
        if (!s2a_hdr_sdr_white_bulk(hist, &bulk)) bulk = white;

        // 输出越界：逐像素按最终 knee 算 gain，看有没有通道超过 head（PSNR/显示白上限）
        long over = 0, tot = 0;
        double hue_acc = 0.0; long hue_n = 0;
        for (int y = 0; y < im.h; y += stride) for (int x = 0; x < im.w; x += stride) {
            size_t i = ((size_t)y * im.w + x) * 3;
            float lin[3];
            for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)im.px[i + (size_t)c] / 255.0f);
            float out[3];
            s2a_hdr_itm_rgb_lin(&itm, lin, out);
            float mx = fmaxf(out[0], fmaxf(out[1], out[2]));
            if (mx > head + 1e-4f) over++;
            tot++;
            // 色相保持：ITM 按亮度统一缩放，通道比值必须不变（这里量最大相对偏差）
            float mxin = fmaxf(lin[0], fmaxf(lin[1], lin[2]));
            if (mxin > 1e-3f) {
                for (int c = 0; c < 3; c++) {
                    float r0 = lin[c] / mxin, r1 = (mx > 1e-9f) ? out[c] / mx : r0;
                    double d = (double)r0 - (double)r1;
                    hue_acc += d * d; hue_n++;
                }
            }
        }
        float clipped = tot ? (float)over / (float)tot : 0.0f;
        float hue_rms = hue_n ? (float)sqrt(hue_acc / (double)hue_n) : 0.0f;
        printf("%s,%d,%d,%.5f,%.4f,%.4f,%.4f,%+.4f,%.4f,%.4f,%.5f,%.2e\n",
               argv[a], im.w, im.h, (double)apl, (double)black_frac, (double)white, (double)bulk,
               (double)avg, (double)lo, (double)hi, (double)clipped, (double)hue_rms);
        free(im.px);
    }
    return 0;
}
