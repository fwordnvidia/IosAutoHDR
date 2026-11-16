// hdr_seq：按帧序列跑「与上屏同一条」判定链，用来量时间行为。
//
// 与单帧批测（hdr_batch）的区别：这里保留跨帧状态（prev_lo/hi/avg/white/have_prev + 帧率），
// 走的入口就是 nvn_hdr_hist.inc 用的 s2a_hdr_auto_l1b_step_fused，因此量到的
// 「过渡快慢、有没有来回抖、慢速运动会不会误判成切镜」就是上屏真实会发生的。
//
// 用法：hdr_seq [--hz 60] [--head 2] frame000.ppm frame001.ppm ...
//   列：frame,white,avg,lo,hi,scene,gwhite,g050,g083,dlo,gpaper,over
//   gpaper/over 是端到端不变量：纸白（= lo）增益必须恰 1.0000；极端饱和度探针
//   (1,0.24,1)@sat=3 调色后不得有通道超 head（超了系统会硬钳）。
//   判据交给 scripts/report_video.py，这里只出数。
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
    if (!im->px || fread(im->px, 1, n, f) != n) { fclose(f); free(im->px); return 0; }
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    float hz = 60.0f;
    int head_arg = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--hz") && i + 1 < argc) { hz = (float)atof(argv[++i]); continue; }
        if (!strcmp(argv[i], "--head") && i + 1 < argc) { head_arg = atoi(argv[++i]); continue; }
        break;
    }
    if (i >= argc) { fprintf(stderr, "用法: hdr_seq [--hz 60] [--head 2] *.ppm\n"); return 2; }
    const float head = (head_arg >= 1 && head_arg <= 4) ? (float)head_arg : 2.0f;

    static uint32_t hist[S2A_HDR_HIST_BINS];
    float lo = 0.0f, hi = 1.0f, avg = 0.0f, white = 0.0f;
    int have = 0;
    printf("frame,white,avg,lo,hi,scene,gwhite,g050,g083,dlo,gpaper,over\n");

    for (int a = i, n = 0; a < argc; a++, n++) {
        img im;
        if (!read_ppm(argv[a], &im)) { fprintf(stderr, "跳过 %s\n", argv[a]); continue; }
        memset(hist, 0, sizeof hist);
        const int stride = 2;
        for (int y = 0; y < im.h; y += stride) for (int x = 0; x < im.w; x += stride) {
            size_t k = ((size_t)y * im.w + x) * 3;
            float lin[3];
            for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)im.px[k + (size_t)c] / 255.0f);
            hist[s2a_hdr_hist_bin(0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2])]++;
        }
        float wov = 0.0f, wbulk = 0.0f;
        if (s2a_hdr_sdr_white_bulk(hist, &wbulk)) wov = wbulk;

        s2a_hdr_auto_l1b b;
        memset(&b, 0, sizeof b);
        if (!s2a_hdr_auto_l1b_step_fused_hz(hist, wov, lo, hi, avg, white, have, hz, &b)) {
            printf("%d,nan,nan,nan,nan,nan,nan,nan,nan,nan,nan,nan\n", n);   // 全黑帧：保持上一帧
            free(im.px);
            continue;
        }
        float dlo = have ? b.knee_lo - lo : 0.0f;
        // 增益用与上屏 MSL 相同的 smoothstep 公式
        s2a_hdr_itm itm; itm.head = head; itm.knee_lo = b.knee_lo; itm.knee_hi = b.knee_hi;
        // 端到端不变量：拿与上屏相同的调色链算两个必须成立的性质
        float gpaper = s2a_hdr_itm_gain(&itm, b.knee_lo);
        float probe[3] = {1.0f, s2a_hdr_to_linear(0.24f), 1.0f}, graded[3];
        s2a_hdr_grade_rgb(&itm, 3.0f, 0.0f, probe, graded);
        float mx = graded[0];
        if (graded[1] > mx) mx = graded[1];
        if (graded[2] > mx) mx = graded[2];
        float over = mx > itm.head ? mx - itm.head : 0.0f;
        printf("%d,%.4f,%+.4f,%.4f,%.4f,%.3f,%.4f,%.4f,%.4f,%+.5f,%.6f,%.6f\n", n, (double)b.white, (double)b.avg,
               (double)b.knee_lo, (double)b.knee_hi, (double)b.scene,
               (double)s2a_hdr_itm_gain(&itm, b.white > 0.0f ? b.white : 0.18f),
               (double)s2a_hdr_itm_gain(&itm, 0.50f),
               (double)s2a_hdr_itm_gain(&itm, 0.832f), (double)dlo, (double)gpaper, (double)over);
        lo = b.knee_lo; hi = b.knee_hi; avg = b.avg; white = b.white; have = 1;
        free(im.px);
    }
    return 0;
}
