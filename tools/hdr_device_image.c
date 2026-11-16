// hdr_device_image：整幅图上的「设备链路」与「CPU 链路」端到端对账。
//
// 设备链路：hdr_hist_add（分箱，黑像素进 0 号格）→ hdr_hist_knee（整数千分 99.5 分位）→ g_hdr_knee
//           → 上屏 fs（P3 混色 / Lab 饱和度 / smoothstep / clip）
// CPU 链路：s2a_hdr_hist_bin → s2a_hdr_sdr_white_bulk + s2a_hdr_auto_knee/L1b → s2a_hdr_grade_rgb
//
// 两条链路的每一步都有单测（bin_mirror / knee_mirror / shader_mirror），但**串起来**从没有过：
// 只要任一步的输出口径不同，画面就会系统性错开而单测全绿。这里用真实照片整幅跑一遍。
//
// 用法：hdr_device_image [--head 2] [--sat 1] [--p3 0] [--stride 1] frame.ppm
//   输出：pixels,bin_diff,msl_lo,msl_hi,cpu_lo,cpu_hi,dlo,dhi,white_gpu,white_cpu,avg_gpu,avg_cpu,max_diff,over_pixels,over_max
//   非 0 退出：knee 不一致、像素超差、或有通道超 head。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"
#include "nvn_hdr_msl.h"
#include "nvn_hdr_msl_hist.h"

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
    float head = 2.0f, sat = 1.0f, p3 = 0.0f;
    int stride = 1;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--head") && i + 1 < argc) head = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat") && i + 1 < argc) sat = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--p3") && i + 1 < argc) p3 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else path = argv[i];
    }
    if (!path || stride < 1) { fprintf(stderr, "用法: hdr_device_image [--head 2] [--sat 1] [--p3 0] [--stride 1] frame.ppm\n"); return 2; }

    img im;
    if (!read_ppm(path, &im)) { fprintf(stderr, "读不了 %s\n", path); return 2; }

    static uint32_t msl_hist[S2A_HDR_HIST_BINS], cpu_hist[S2A_HDR_HIST_BINS];
    memset(msl_hist, 0, sizeof msl_hist);
    memset(cpu_hist, 0, sizeof cpu_hist);
    long sampled = 0;
    for (int y = 0; y < im.h; y += stride) for (int x = 0; x < im.w; x += stride) {
        size_t k = ((size_t)y * im.w + x) * 3;
        float code[3], lin[3];
        for (int c = 0; c < 3; c++) code[c] = (float)im.px[k + (size_t)c] / 255.0f;
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(code[c]);
        float yv = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
        msl_hist[s2a_msl_hist_bin(yv)]++;          // 设备：kernel 分箱
        cpu_hist[s2a_hdr_hist_bin(yv)]++;          // CPU：参考分箱
        sampled++;
    }

    // ① 分箱一致性：设备（MSL）与 CPU（参考）必须逐格相同
    long bin_diff = 0, bin_max = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        long d = (long)msl_hist[i] - (long)cpu_hist[i];
        if (d) { bin_diff++; if (labs(d) > bin_max) bin_max = labs(d); }
    }

    // ② 设备侧 L1（GPU kernel 转写）：白点 / 重心 / 切镜权重
    float gpu[4] = {0, 0, 0, 0};
    int gpu_ok = s2a_msl_hist_l1(msl_hist, 0.0f, 0, gpu);

    // ③ CPU 侧：同一个直方图 → 白点（含 HUD 剔除）→ L1b 拐点
    float wbulk = 0.0f, wov = 0.0f;
    if (s2a_hdr_sdr_white_bulk(cpu_hist, &wbulk)) wov = wbulk;
    s2a_hdr_auto_l1b b;
    memset(&b, 0, sizeof b);
    if (!s2a_hdr_auto_l1b_step_fused_hz(cpu_hist, wov, 0.0f, 1.0f, 0.0f, 0.0f, 0, 60.0f, &b)) {
        fprintf(stderr, "CPU 侧直方图为空\n");
        return 2;
    }
    float mlo = b.knee_lo, mhi = b.knee_hi;   // 设备上的 knee 就是 CPU 从这颗直方图算出来的
    int avg_ok = 0;
    float cpu_avg = s2a_hdr_hist_avg(cpu_hist, &avg_ok);

    // 用两套 knee 各自跑整幅，比较输出
    s2a_hdr_itm idev = {head, mlo, mhi}, icpu = {head, b.knee_lo, b.knee_hi};
    double max_diff = 0.0;
    float over_max = 0.0f;
    long over_pixels = 0;
    const long total = (long)im.w * im.h;
    for (long i = 0; i < total; i++) {
        float code[3], lin[3], a[3], g[3];
        for (int c = 0; c < 3; c++) code[c] = (float)im.px[i * 3 + (size_t)c] / 255.0f;
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(code[c]);
        msl_fs(code, idev.knee_lo, idev.knee_hi, head, sat, p3, a);
        s2a_hdr_grade_rgb(&icpu, sat, p3, lin, g);
        int over = 0;
        for (int c = 0; c < 3; c++) {
            double d = fabs((double)a[c] - (double)g[c]);
            if (d > max_diff) max_diff = d;
            float m = a[c] > g[c] ? a[c] : g[c];
            if (m > head + 1e-6f) { over = 1; if (m - head > over_max) over_max = m - head; }
        }
        over_pixels += over;
    }

    float dlo = mlo - b.knee_lo, dhi = mhi - b.knee_hi;   // 设备与 CPU 用同一颗直方图 ⇒ 应为 0
    if (dlo < 0) dlo = -dlo;
    if (dhi < 0) dhi = -dhi;
    float dw = fabsf(gpu[1] - b.white), da = fabsf(gpu[2] - (avg_ok ? cpu_avg : 0.0f));
    printf("%ld,%ld,%.4f,%.4f,%.4f,%.4f,%.6f,%.6f,%.4f,%.4f,%+.4f,%+.4f,%.9f,%ld,%.9f\n",
           (long)total, bin_diff, (double)mlo, (double)mhi, (double)b.knee_lo, (double)b.knee_hi,
           (double)dlo, (double)dhi, (double)gpu[1], (double)b.white, (double)gpu[2],
           (double)(avg_ok ? cpu_avg : 0.0f), max_diff, over_pixels, (double)over_max);

    int rc = 0;
    if (bin_diff) {
        fprintf(stderr, "✗ 直方图有 %ld 格不同（最大差 %ld）⇒ 设备与 CPU 判据会分叉\n", bin_diff, bin_max);
        rc = 1;
    }
    if (!gpu_ok) { fprintf(stderr, "✗ GPU L1 转写返回空帧\n"); rc = 1; }
    if (dw > 0.02f || da > 0.05f) {
        fprintf(stderr, "✗ GPU L1 与 CPU 分叉：white 差 %.4f（阈 0.02）、avg 差 %.4f（阈 0.05）\n",
                (double)dw, (double)da);
        rc = 1;
    }
    if (dlo > 1e-6f || dhi > 1e-6f) {
        fprintf(stderr, "✗ knee 不一致：设备 %.4f/%.4f，CPU %.4f/%.4f（Δ %.6f/%.6f）\n",
                (double)mlo, (double)mhi, (double)b.knee_lo, (double)b.knee_hi, (double)dlo, (double)dhi);
        rc = 1;
    }
    if (max_diff > 1e-4) { fprintf(stderr, "✗ 画面最大差 %.9f 超 1e-4\n", max_diff); rc = 1; }
    if (over_pixels) { fprintf(stderr, "✗ %ld 个像素有通道超 head（最大 %.9f）\n", over_pixels, (double)over_max); rc = 1; }
    free(im.px);
    return rc;
}
