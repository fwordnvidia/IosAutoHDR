// hdr_ref —— 脱离 S2Arm/模拟器也能跑的参考实现命令行。
// 读 8 位 PPM(P6) 帧，按与补丁完全相同的参数层（nvn_hdr.h / nvn_hdr_auto_l1.h）算出
// 每帧的拐点与增益，输出：① 8 位预览 PPM；② 一行 stats（含 4 个固定探针的增益）。
// 用途：把「算法」和「Metal 宿主」解耦，谁都能在自己机器上复现同一组数字。
//
// 用法：
//   hdr_ref --synth city --frames 8 --stats                 # 自带四种合成场景
//   hdr_ref --curve --head 2 --knee-lo 0.35                 # 增益曲线 CSV（给画图脚本用）
//   hdr_ref -i in.ppm -o out.ppm --head 2 --algo l1b --stats
//   参数：--head N --white-frac F --algo l1|l1b --sat S --p3 P --stride N --frames N
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/nvn_hdr.h"
#include "../src/nvn_hdr_auto.h"
#include "../src/nvn_hdr_auto_l1.h"

typedef struct { int w, h; unsigned char *px; } img;

static unsigned rs = 12345u;
static float rnd(void) { rs = rs * 1103515245u + 12345u; return (float)((rs >> 8) & 0xffff) / 65535.0f; }

// ---------- PPM ----------
static img *ppm_read(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); return NULL; }
    char m[3] = {0}; int maxv = 0;
    if (fscanf(f, "%2s", m) != 1 || strcmp(m, "P6") != 0) { fclose(f); fprintf(stderr, "%s: 只支持 P6\n", p); return NULL; }
    int c; while ((c = fgetc(f)) != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#')) {
        if (c == '#') while ((c = fgetc(f)) != EOF && c != '\n') {}
    }
    ungetc(c, f);
    img *im = calloc(1, sizeof *im);
    if (fscanf(f, "%d %d %d", &im->w, &im->h, &maxv) != 3 || maxv != 255 || im->w <= 0 || im->h <= 0) {
        fclose(f); free(im); fprintf(stderr, "%s: 头不对\n", p); return NULL;
    }
    fgetc(f);
    size_t n = (size_t)im->w * im->h * 3;
    im->px = malloc(n);
    if (fread(im->px, 1, n, f) != n) { fclose(f); free(im->px); free(im); return NULL; }
    fclose(f);
    return im;
}
// 16 位预览：把仍未量化的 sRGB 编码值按 65535 输出，用来量「上屏 16F 到底还有没有量化台阶」
static int ppm_write16(const char *p, int w, int h, const float *enc) {
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "%s: 打不开\n", p); return 0; }
    fprintf(f, "P6\n%d %d\n65535\n", w, h);
    for (size_t i = 0; i < (size_t)w * h * 3; i++) {
        float v = enc[i]; if (!(v > 0.0f)) v = 0.0f; if (v > 1.0f) v = 1.0f;
        unsigned short q = (unsigned short)lroundf(v * 65535.0f);
        unsigned char b[2] = { (unsigned char)(q >> 8), (unsigned char)(q & 0xff) };
        fwrite(b, 1, 2, f);
    }
    fclose(f);
    return 1;
}

static int ppm_write(const char *p, const img *im) {
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    fwrite(im->px, 1, (size_t)im->w * im->h * 3, f);
    fclose(f);
    return 0;
}

// ---------- 合成场景（与 t_hdr_quality 同一套语义，方便交叉核对） ----------
static void synth(img *im, const char *kind, int t) {
    int W = im->w, H = im->h;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float v;
        float u = (float)x / (float)(W - 1), w = (float)y / (float)(H - 1);
        if (!strcmp(kind, "city")) {
            v = 0.05f + 0.03f * (float)((x * 37 + y * 11) % 11) / 11.0f;
            if (((x * 7 + y * 13) % 97) == 0) v = 0.62f;
        } else if (!strcmp(kind, "uiwhite")) {
            v = 0.86f;
            if (u > 0.6f && w > 0.6f) v = 0.52f;      // 一块灰面板
        } else if (!strcmp(kind, "dark")) {
            v = 0.004f;
            if (((x * 31 + y * 17) % 131) == 0) v = 0.72f;  // 星点
        } else if (!strcmp(kind, "hud") || !strcmp(kind, "hudnb")) {
            // 游戏画面里最常见的两种干扰：① 顶部/底部黑边；② 静态 HUD 白字
            // 内容：中间调为主导，白点 0.50；HUD 覆盖约 1.2% 像素、亮度 0.86；
            // hudnb = 同图但无黑边无 HUD，作为对照组。
            int bars = strcmp(kind, "hudnb") != 0;
            int y0 = bars ? H / 12 : 0, y1 = bars ? H - H / 12 : H;
            if (y < y0 || y >= y1) { v = 0.0f; }
            else {
                v = 0.10f + 0.40f * u;               // 内容 0.10..0.50
                if (!strcmp(kind, "hud") && w > 0.90f) v = 0.86f;   // 底部 HUD 带
            }
        } else if (!strcmp(kind, "cut")) {
            // 同 APL 切镜：两段画面的重心几乎一样，只有白点跳。
            //   前 32 帧：中间调 0.28 + 2% 的 0.55 面板；后 32 帧：同样的底色，面板换成 0.95。
            //   平均亮度两段几乎相同（面板只占 2%），但白点差约 0.79 stop ⇒ 只有白点判据能一帧到位。
            int phase = (t >= 32);
            v = 0.28f;
            if (x > W * 9 / 10 && y > H * 9 / 10) v = phase ? 0.95f : 0.55f;
        } else if (!strcmp(kind, "star")) {
            // 星空：99.9% 纯黑 + 0.1% 的亮星。float4 回读里看不出「大半是黑的」，
            // 这正是必须把直方图 blit 回 CPU 的理由（t_hdr_wiring 第 15 组守着）。
            v = 0.0f;
            if (((x * 41 + y * 23) % 997) == 0) v = 0.72f;
            size_t j = ((size_t)y * W + x) * 3;
            unsigned char bb = (unsigned char)lroundf(v * 255.0f);
            im->px[j] = im->px[j + 1] = im->px[j + 2] = bb;
            continue;                                  // 星星不加噪声（0.0 要真的是 0.0）
        } else if (!strcmp(kind, "fade")) {
            // 淡入：整帧增益 0.18 → 1.0，逐帧变化；APL 单调上升，考「平滑会不会抽动/过冲」
            float g = 0.18f + 0.82f * (float)t / 63.0f;
            v = g * (0.25f + 0.75f * u);
            if (w > 0.9f) v = g;                       // 顶部一条均匀带
        } else { // ramp：全灰阶 + 一小块纯白
            v = u;
            if (w > 0.85f) v = 1.0f;
        }
        v *= (1.0f + 0.01f * (rnd() - 0.5f) * 2.0f);   // 1% 噪声
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        unsigned char b = (unsigned char)lroundf(v * 255.0f);
        size_t i = ((size_t)y * W + x) * 3;
        im->px[i] = im->px[i + 1] = im->px[i + 2] = b;
    }
    (void)t;
}

// ---------- 参数层 ----------
static int g_hudguard;   // --hudguard：白点走「孤立簇剔除」版本
static int g_scene_fuse; // --scene-fuse：场景判据加「白点变化」（与 nvn_hdr_hist.inc 的上屏路径一致）
typedef struct { float knee_lo, knee_hi, white, avg, lit, scene; int frozen; } st;

static void analyze(const img *im, int stride, const char *algo, st *prev, int have_prev, st *out) {
    static uint32_t hist[S2A_HDR_HIST_BINS];
    memset(hist, 0, sizeof hist);
    int W = im->w, H = im->h;
    for (int y = 0; y < H; y += stride) for (int x = 0; x < W; x += stride) {
        size_t i = ((size_t)y * W + x) * 3;
        float lin[3];
        for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear((float)im->px[i + (size_t)c] / 255.0f);
        hist[s2a_hdr_hist_bin(0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2])]++;
    }
    if (!strcmp(algo, "l1b")) {
        s2a_hdr_auto_l1b b; memset(&b, 0, sizeof b);
        float wov = 0.0f;
        if (g_hudguard) { float w2 = 0.0f; if (s2a_hdr_sdr_white_bulk(hist, &w2)) wov = w2; }
        // 逐帧判定只走一张入口：与上屏路径（nvn_hdr_hist.inc）和单元测试同一份代码。
        // --scene-fuse 打开「白点跳跃也算切镜」，由 s2a_hdr_auto_l1b_step_fused_hz 内部完成；
        // 关掉时用只按重心的 step_white（对照用，证明同 APL 切镜要 29 帧才到位）。
        int r = g_scene_fuse
            ? s2a_hdr_auto_l1b_step_fused(hist, wov, prev ? prev->knee_lo : 0.5f,
                                          prev ? prev->knee_hi : 1.0f, prev ? prev->avg : 0.0f,
                                          prev ? prev->white : 0.0f, have_prev, &b)
            : s2a_hdr_auto_l1b_step_white(hist, wov, prev ? prev->knee_lo : 0.5f,
                                          prev ? prev->knee_hi : 1.0f, prev ? prev->avg : 0.0f,
                                          have_prev, &b);
        (void)r;
        out->knee_lo = b.knee_lo; out->knee_hi = b.knee_hi; out->white = b.white;
        out->avg = b.avg; out->lit = s2a_hdr_lit_frac(hist);
        out->scene = b.scene; out->frozen = b.frozen;
    } else {
        s2a_hdr_auto_l1 r; memset(&r, 0, sizeof r);
        s2a_hdr_auto_l1_step(hist, prev ? prev->knee_hi : 1.0f, prev ? prev->avg : 0.0f, have_prev, &r);
        out->knee_lo = r.knee_lo; out->knee_hi = r.knee_hi; out->white = r.white;
        out->avg = r.avg; out->lit = s2a_hdr_lit_frac(hist);
        out->scene = r.scene; out->frozen = 0;
    }
}

static float gain_at(float srgb_in, const s2a_hdr_itm *p, float sat, float p3) {
    float v[3];
    for (int c = 0; c < 3; c++) v[c] = s2a_hdr_to_linear(srgb_in);
    float o[3];
    s2a_hdr_grade_rgb(p, sat, p3, v, o);
    float li = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
    float lo = 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
    return li > 1e-9f ? lo / li : 1.0f;
}

int main(int argc, char **argv) {
    const char *in = NULL, *outp = NULL, *syn = NULL, *algo = "l1b";
    float head = 2.0f, wfrac = S2A_HDR_WHITE_FRAC, sat = 1.0f, p3 = 0.0f;
    int frames = 1, stride = 1, stats = 0, curve = 0, dither = 0, columns = 0, depth16 = 0, seq = 0, statline = 0;

    float curve_lo = 0.35f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "--synth") && i + 1 < argc) syn = argv[++i];
        else if (!strcmp(argv[i], "--head") && i + 1 < argc) head = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--white-frac") && i + 1 < argc) wfrac = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat") && i + 1 < argc) sat = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--p3") && i + 1 < argc) p3 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--algo") && i + 1 < argc) algo = argv[++i];
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--stat-line")) statline = 1;   // 与设备日志同格式（S2A_HDR_LOG 那一行）
        else if (!strcmp(argv[i], "--curve")) curve = 1;
        else if (!strcmp(argv[i], "--dither")) dither = 1;
        else if (!strcmp(argv[i], "--columns")) columns = 1;
        else if (!strcmp(argv[i], "--out16")) depth16 = 1;
        else if (!strcmp(argv[i], "--seq")) seq = 1;
        else if (!strcmp(argv[i], "--hudguard")) g_hudguard = 1;
        else if (!strcmp(argv[i], "--scene-fuse")) g_scene_fuse = 1;
        else if (!strcmp(argv[i], "--knee-lo") && i + 1 < argc) curve_lo = (float)atof(argv[++i]);
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }
    if (frames < 1) frames = 1;

    // --curve：把「增益曲线」本身当成输出（CSV），供 scripts/plot_curves.py 画图。
    // 这样可以保证图里的数字与实际代码同源，而不是手抄。
    if (curve) {
        s2a_hdr_itm p; p.head = head; p.knee_lo = curve_lo; p.knee_hi = 1.0f;
        printf("srgb,gain_lin,out_lin,out_nit\n");
        float prev_g = -1.0f;
        for (int code = 0; code <= 255; code++) {
            float e = (float)code / 255.0f;
            float lin[3], o[3];
            for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(e);
            s2a_hdr_itm_rgb_lin(&p, lin, o);
            float li = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
            float lo = 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
            float g = li > 1e-9f ? lo / li : 1.0f;
            printf("%.6f,%.6f,%.6f,%.3f\n", e, g, lo, lo * 203.0f);
            if (g < prev_g - 1e-6f) { fprintf(stderr, "曲线不单调：code %d\n", code); return 1; }
            prev_g = g;
        }
        return 0;
    }

    img *im = NULL;
    if (syn) { im = calloc(1, sizeof *im); im->w = 256; im->h = 144; im->px = malloc((size_t)im->w * im->h * 3); synth(im, syn, 0); }
    else if (in) { im = ppm_read(in); if (!im) return 1; }
    else { fprintf(stderr, "要么 --synth <city|uiwhite|dark|ramp|hud|hudnb|cut|fade|star>，要么 -i in.ppm\n"); return 2; }

    st prev = {0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0}; int have = 0;
    img *dst = calloc(1, sizeof *dst); dst->w = im->w; dst->h = im->h; dst->px = malloc((size_t)im->w * im->h * 3);
    float *dst16 = depth16 ? malloc(sizeof(float) * (size_t)im->w * im->h * 3) : NULL;

    for (int t = 0; t < frames; t++) {
        if (syn) synth(im, syn, t);
        st cur; analyze(im, stride, algo, have ? &prev : NULL, have, &cur);

        s2a_hdr_itm p;
        p.head = head; p.knee_lo = cur.knee_lo; p.knee_hi = cur.knee_hi;
        (void)wfrac;

        // 预览：按 EDR 语义把线性 >1.0 的内容压回 8 位可显示范围（仅用于肉眼/回归比对）
        double sum = 0; int n = 0;
        for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) {
            size_t i = ((size_t)y * im->w + x) * 3;
            float lin[3], o[3];
            for (int c = 0; c < 3; c++) {
                float enc = (float)im->px[i + (size_t)c] / 255.0f;
                if (dither) {
                    // 确定性哈希噪声 ∈ [-0.5, 0.5] LSB：按像素坐标打散量化台阶，
                    // 幅度只有半个码值，平均亮度不变（三角分布更接近无偏，这里用均匀分布已够）。
                    unsigned hh = (unsigned)(x * 1973 + y * 9277 + c * 26699);
                    hh ^= hh >> 13; hh *= 1274126177u; hh ^= hh >> 16;
                    enc += ((float)(hh & 0xffff) / 65535.0f - 0.5f) / 255.0f;
                }
                lin[c] = s2a_hdr_to_linear(enc);
            }
            s2a_hdr_grade_rgb(&p, sat, p3, lin, o);
            float lum = 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
            sum += lum; n++;
            for (int c = 0; c < 3; c++) {
                float v = o[c] / head;              // 线性 → 显示参考白
                if (v > 1.0f) v = 1.0f;
                float e = s2a_hdr_to_srgb(v);
                if (dst16) dst16[i + (size_t)c] = e;
                dst->px[i + (size_t)c] = (unsigned char)lroundf((e > 1.0f ? 1.0f : e) * 255.0f);
            }
        }
        if (columns) {
            printf("col,in_lin,out_lin\n");
            for (int x = 0; x < im->w; x++) {
                double sl = 0, so = 0; int cnt = 0;
                for (int y = 0; y < im->h; y++) {
                    size_t i = ((size_t)y * im->w + x) * 3;
                    float lin[3], o[3];
                    for (int c = 0; c < 3; c++) {
                        float enc = (float)im->px[i + (size_t)c] / 255.0f;
                        if (dither) {
                            unsigned hh = (unsigned)(x * 1973 + y * 9277 + c * 26699);
                            hh ^= hh >> 13; hh *= 1274126177u; hh ^= hh >> 16;
                            enc += ((float)(hh & 0xffff) / 65535.0f - 0.5f) / 255.0f;
                        }
                        lin[c] = s2a_hdr_to_linear(enc);
                    }
                    s2a_hdr_grade_rgb(&p, sat, p3, lin, o);
                    sl += 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
                    so += 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
                    cnt++;
                }
                printf("%d,%.8f,%.8f\n", x, sl / cnt, so / cnt);
            }
        }
        if (seq && (t == 0)) printf("frame,white,lo,hi,lit,gain_050,gain_100\n");
        if (seq) {
            float lin[3], o[3];
            for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(0.5f);
            s2a_hdr_grade_rgb(&p, sat, p3, lin, o);
            float g5 = 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
            for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(1.0f);
            s2a_hdr_grade_rgb(&p, sat, p3, lin, o);
            float g10 = 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
            printf("%d,%.5f,%.5f,%.5f,%.4f,%.5f,%.5f\n", t, prev.white, prev.knee_lo, prev.knee_hi,
                   prev.lit, g5 / 0.214041f, g10);
        }
        if (statline) {
            // 与设备上 S2A_HDR_LOG 打的那行**同一份格式化代码**：把「本机参考实现算出来的数」
            // 与「设备日志里的数」直接对比，不用手工换算（真机判读全靠这个）。
            s2a_hdr_stat stt;
            memset(&stt, 0, sizeof stt);
            stt.fmt16 = 1; stt.itm = 1;      // 参考实现模拟的就是 16F + ITM 这条链路
            stt.frozen = cur.frozen;
            stt.head = head; stt.knee_lo = cur.knee_lo; stt.knee_hi = cur.knee_hi;
            stt.white = cur.white; stt.avg = cur.avg; stt.scene = cur.scene;
            stt.map = s2a_hdr_map();      // 与设备同一个环境变量开关，行才能逐字符 diff
            char line[192];
            s2a_hdr_status_line(line, sizeof line, &stt);
            printf("%s\n", line);
        }
        if (stats) {
            printf("frame %d algo=%s head=%.2f white=%.4f avg=%.4f lit=%.4f knee=%.4f..%.4f "
                   "mean_nit=%.1f probe[0.18]=%.4f probe[0.50]=%.4f probe[0.832]=%.4f probe[1.0]=%.4f\n",
                   t, algo, head, cur.white, cur.avg, cur.lit, cur.knee_lo, cur.knee_hi,
                   (sum / (n ? n : 1)) * 203.0,
                   gain_at(0.18f, &p, sat, p3), gain_at(0.50f, &p, sat, p3),
                   gain_at(0.832f, &p, sat, p3), gain_at(1.0f, &p, sat, p3));
        }
        prev = cur; have = 1;
    }
    if (outp && depth16) return ppm_write16(outp, im->w, im->h, dst16) ? 0 : 1;   // 成功=0（原来反了：写成功也返回 1）
    if (outp) return ppm_write(outp, dst) ? 1 : 0;
    return 0;
}
