// hdr_fidelity —— 「这个功能在真实照片上到底改了多少画面」。
//
// 自动 HDR 最容易被用户骂的两种错，都不是数值精度问题：
//   ① 该不动的地方动了 —— UI/文字/暗部/纸白以下的内容被抬亮，「像蒙了层灰」
//   ② 该动的地方没动 —— 拐点定得太高，整幅图几乎全是恒等，开了跟没开一样
// 所以这里逐像素给三个数：纸白以下有没有动（必须逐位 0）、有多少像素真的吃到增益、
// 增益分布（均值/中位/最大）。
//
// 读 8 位 PPM(P6)，走与上屏完全相同的链路（分箱 → L1b 定拐点 → s2a_hdr_grade_rgb_map）。
//   映射档由 S2A_HDR_MAP 决定（0 = knee 缺省，1 = BT.2408 内容映射），与设备同一个开关，
//   所以同一份表能同时回答「默认保守档改多少」和「内容档改多少」。
// 用法：hdr_fidelity [--head N] [--sat S] [--p3 P] [--stride N] a.ppm [b.ppm ...]
// 输出 CSV：file,knee_lo,knee_hi,pct_below,max_dlo_below,pct_ident,pct_gain5,
//           gain_mean,gain_p50,gain_max,code_changed,above_p05,above_p20,above_p50
//
// 后三列是**敏感度**，不是预设：如果把纸白从「0.5 分位」下移到 5/20/50 分位，会有多少比例
// 的像素落到拐点之上（也就是开始吃增益）。它们纯粹是直方图算术，不走判定逻辑——
// 用来回答「默认档是不是太保守」这个问题，而不是偷偷改默认值。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/nvn_hdr.h"
#include "../src/nvn_hdr_auto.h"
#include "../src/nvn_hdr_auto_l1.h"

static float lum(const float v[3]) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; }

static int read_ppm(const char *p, int *W, int *H, unsigned char **px) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); return 0; }
    char magic[3] = {0};
    int maxv = 0;
    // 必须先吃 magic：否则 P6 会被当成宽度读，w/h/maxval 全错位
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) { fclose(f); fprintf(stderr, "%s: 只支持 P6\n", p); return 0; }
    int c;
    while ((c = fgetc(f)) != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#'))
        if (c == '#') while ((c = fgetc(f)) != EOF && c != '\n') {}
    if (c == EOF) { fclose(f); return 0; }
    ungetc(c, f);
    if (fscanf(f, "%d %d %d", W, H, &maxv) != 3 || maxv != 255 || *W <= 0 || *H <= 0) {
        fclose(f); fprintf(stderr, "%s: 头不对（要 P6 / maxval 255）\n", p); return 0;
    }
    fgetc(f);                                  // 单个空白
    size_t n = (size_t)(*W) * (*H) * 3;
    *px = malloc(n);
    if (!*px || fread(*px, 1, n, f) != n) { fclose(f); free(*px); fprintf(stderr, "%s: 数据不足\n", p); return 0; }
    fclose(f);
    return 1;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv) {
    float head = 2.0f, sat = 1.0f, p3 = 0.0f;
    int map = s2a_hdr_map();          // 0 = knee（缺省）, 1 = BT.2408 内容映射
    int stride = 1, files = 0;
    const char *paths[64];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--head") && i + 1 < argc) head = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat") && i + 1 < argc) sat = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--p3") && i + 1 < argc) p3 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) map = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else if (argv[i][0] == '-') { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
        else if (files < 64) paths[files++] = argv[i];
    }
    if (!files) { fprintf(stderr, "用法：hdr_fidelity [--head N] [--sat S] [--p3 P] [--stride N] a.ppm ...\n"); return 2; }
    if (stride < 1) stride = 1;

    printf("file,knee_lo,knee_hi,pct_below,max_dlo_below,pct_ident,pct_gain5,gain_mean,gain_p50,gain_max,"
           "code_changed,above_p05,above_p20,above_p50\n");
    for (int k = 0; k < files; k++) {
        int W = 0, H = 0;
        unsigned char *px = NULL;
        if (!read_ppm(paths[k], &W, &H, &px)) return 1;

        static uint32_t hist[S2A_HDR_HIST_BINS];
        memset(hist, 0, sizeof hist);
        for (int y = 0; y < H; y += stride)
            for (int x = 0; x < W; x += stride) {
                size_t i = ((size_t)y * W + x) * 3;
                float lin[3];
                for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)px[i + c] / 255.0f);
                hist[s2a_hdr_hist_bin(lum(lin))]++;
            }

        s2a_hdr_auto_l1b b;
        memset(&b, 0, sizeof b);
        s2a_hdr_auto_l1b_step_fused(hist, 0.0f, 0.5f, 1.0f, 0.0f, 0.0f, 0, &b);

        s2a_hdr_itm p;
        p.head = head; p.knee_lo = b.knee_lo; p.knee_hi = b.knee_hi;

        long total = 0, below = 0, ident = 0, gain5 = 0, changed = 0;
        double max_dlo_below = 0.0, sum_gain = 0.0, max_gain = 0.0;
        float *gains = malloc(sizeof(float) * (size_t)W * H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                size_t i = ((size_t)y * W + x) * 3;
                float lin[3], out[3];
                int code_in[3], code_out[3];
                for (int c = 0; c < 3; c++) {
                    code_in[c] = px[i + c];
                    lin[c] = s2a_hdr_to_linear((float)px[i + c] / 255.0f);
                }
                s2a_hdr_grade_rgb_map(&p, sat, p3, map, lin, out);
                float li = lum(lin), lo = lum(out);
                float g = li > 1e-9f ? lo / li : 1.0f;
                total++;
                if (li <= b.knee_lo) {
                    below++;
                    double d = fabs((double)lo - (double)li);
                    if (d > max_dlo_below) max_dlo_below = d;
                }
                int same = 1;
                for (int c = 0; c < 3; c++) {
                    // 按 8 位码值比较：把线性值编码回 sRGB 再取整，才是用户能看到的差别
                    float e = s2a_hdr_to_srgb(out[c] < 0.0f ? 0.0f : out[c]);
                    code_out[c] = (int)lrintf(e * 255.0f);
                    if (code_out[c] < 0) code_out[c] = 0;
                    if (code_out[c] > 255) code_out[c] = 255;
                    if (code_out[c] != code_in[c]) same = 0;
                }
                if (same) ident++; else changed++;
                if (g > 1.05f) gain5++;
                sum_gain += g;
                if (g > max_gain) max_gain = g;
                gains[y * W + x] = g;
            }
        // 敏感度：从最高格往下累加，找到累计占比达到 5/20/50% 的那一格，数它之上还有多少像素
        long all = 0;
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) all += hist[i];
        double above[3] = {0, 0, 0};
        const double q[3] = {0.05, 0.20, 0.50};
        for (int qi = 0; qi < 3; qi++) {
            long acc = 0;
            for (int i = S2A_HDR_HIST_BINS - 1; i >= 0; i--) {
                acc += hist[i];
                if (all && (double)acc / (double)all >= q[qi]) { above[qi] = all ? (double)acc / all : 0; break; }
            }
        }
        qsort(gains, (size_t)W * H, sizeof(float), cmp_float);
        double p50 = gains[((size_t)W * H) / 2];
        free(gains);
        free(px);
        printf("%s,%.4f,%.4f,%.4f,%.9f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               paths[k], b.knee_lo, b.knee_hi,
               total ? (double)below / total : 0.0, max_dlo_below,
               total ? (double)ident / total : 0.0, total ? (double)gain5 / total : 0.0,
               total ? sum_gain / total : 1.0, p50, max_gain,
               total ? (double)changed / total : 0.0, above[0], above[1], above[2]);
    }
    return 0;
}
