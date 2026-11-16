// 图像级用例：夜景 / HUD 白条 / 字幕条 / 淡出到底 —— 用真实像素验证「用户看得见」的性质
//
// 直方图级用例（t_hdr_hud 等）只证明判据算得对；这里把判据接到像素上，断言最终输出：
//   · 夜景：纯黑仍输出 0（不许把黑洗成灰），星点有增益但不超过 head
//   · HUD 白条 / 字幕条：纸白恒 ×1.0000，且**拐点与「没有白条的那一帧」逐位相同**
//     （对照组用不做 HUD 剔除的白点 ⇒ 拐点被钉死，证明这条断言有判别力）
//   · 淡出到底：knee_lo 保持不动（冻结），只有 avg 在下掉
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

static int fails, total;
static void ok(int cond, const char *what) {
    total++;
    if (!cond) { fails++; printf("  ✗ %s\n", what); }
}

#define W 320
#define H 180

typedef struct { int w, h; unsigned char px[W * H * 3]; } frame;
static frame fr_a, fr_b;

static void clear_frame(frame *f, int code) {
    f->w = W; f->h = H;
    memset(f->px, code, sizeof f->px);
}
static void put(const frame *f, int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return;
    size_t k = ((size_t)y * f->w + x) * 3;
    unsigned char *p = (unsigned char *)f->px;
    p[k] = (unsigned char)r; p[k + 1] = (unsigned char)g; p[k + 2] = (unsigned char)b;
}

// 用一幅 8 位帧铺直方图（与设备同口径：Rec.709 亮度，黑进 0 号格）
static void hist_of(const frame *f, uint32_t hist[S2A_HDR_HIST_BINS]) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    const unsigned char *p = f->px;
    for (int i = 0; i < f->w * f->h; i++) {
        float l[3];
        for (int c = 0; c < 3; c++) l[c] = s2a_hdr_to_linear((float)p[i * 3 + c] / 255.0f);
        float y = 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
        hist[s2a_hdr_hist_bin(y)]++;
    }
}

// 一帧 → 拐点（走与设备同一条 CPU 判定：HUD 剔除后的白点 → L1b 目标）
static int knee_of(const frame *f, int reject_hud, s2a_hdr_itm *itm, float *paper) {
    static uint32_t hist[S2A_HDR_HIST_BINS];
    hist_of(f, hist);
    float w = 0.0f;
    if (reject_hud) {
        float wb = 0.0f;
        if (!s2a_hdr_sdr_white_bulk(hist, &wb)) return 0;
        w = wb;
    } else {
        if (!s2a_hdr_sdr_white(hist, &w)) return 0;
    }
    float lo = 0.0f, hi = 0.0f;
    if (!s2a_hdr_l1b_targets_from_white(w, &lo, &hi)) return 0;
    itm->head = 2.0f; itm->knee_lo = lo; itm->knee_hi = hi;
    if (paper) *paper = s2a_hdr_itm_gain(itm, lo);
    return 1;
}

int main(void) {
    printf("== 图像级用例（夜景 / HUD / 字幕 / 淡出）==\n");

    // —— 1) 夜景：全黑底 + 星点，断言「黑不变灰、星点不超 head」——
    {
        clear_frame(&fr_a, 0);
        unsigned int st = 12345u;
        int stars = 0;
        for (int i = 0; i < 400; i++) {
            st = st * 1103515245u + 12345u;
            int x = (int)((st >> 16) % W), y = (int)((st >> 8) % H);
            put(&fr_a, x, y, 190, 205, 255);          // 偏蓝的星点
            stars++;
        }
        s2a_hdr_itm itm; float paper = 0;
        int got = knee_of(&fr_a, 1, &itm, &paper);
        ok(got, "夜景：能算出拐点");
        if (got) {
            // 逐像素跑一遍：纯黑必须仍为 0，任何通道不得超 head
            const unsigned char *p = fr_a.px;
            int black_ok = 1, over = 0, below_ok = 1;
            double sum_in = 0.0, sum_out = 0.0;
            for (int i = 0; i < W * H; i++) {
                float code[3], lin[3], out[3];
                for (int c = 0; c < 3; c++) code[c] = (float)p[i * 3 + c] / 255.0f;
                for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(code[c]);
                s2a_hdr_grade_rgb(&itm, 1.0f, 0.0f, lin, out);
                for (int c = 0; c < 3; c++) {
                    if (p[i * 3 + c] == 0 && out[c] != 0.0f) black_ok = 0;
                    if (out[c] > itm.head + 1e-6f) over = 1;
                }
                float ylin = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
                float yo = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
                sum_in += (double)ylin; sum_out += (double)yo;
                // 中低亮度（低于纸白）必须逐位不变：这就是「UI / 字幕不变亮」的来源。
                // 注意夜空里最亮的星簇**就是**白点（L1b 的纸白 = 0.5% 分位白点），
                // 所以星点本身不该被放大——放大只发生在白点以上的高光。
                if (ylin < itm.knee_lo * 0.999f) {
                    for (int c = 0; c < 3; c++)
                        if (fabsf(out[c] - lin[c]) > 1e-6f) below_ok = 0;
                }
            }
            ok(black_ok, "夜景：纯黑像素输出严格为 0（不洗灰）");
            ok(!over, "夜景：没有通道超过 head");
            ok(below_ok, "夜景：低于纸白的像素逐位不变（中低亮度不放大）");
            // 整幅平均只能微涨：星点占比 0.7%，不该把画面整体拉亮
            double ratio = sum_out / (sum_in > 1e-9 ? sum_in : 1.0);
            ok(ratio < 1.05, "夜景：整幅平均亮度涨幅 < 5%");
            printf("    夜景：paper=%.4f lo=%.4f hi=%.4f 平均比 %.4f\n",
                   (double)paper, (double)itm.knee_lo, (double)itm.knee_hi, ratio);
        }
    }

    // —— 2) HUD 白条：占 1.2% 像素的纯白条，纸白恒 1、拐点与无白条那帧相同 ——
    {
        // 底图：一块中等亮度的渐变（模拟游戏画面）
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int v = 40 + (x * 120) / (W - 1);
            put(&fr_a, x, y, v, v - 6 > 0 ? v - 6 : 0, v - 12 > 0 ? v - 12 : 0);
        }
        memcpy(&fr_b, &fr_a, sizeof fr_a);
        // 白条：底边一条 4 行高（4*320 = 1280 px = 2.2% … 取 2 行 = 1.1%）
        for (int y = H - 2; y < H; y++) for (int x = 0; x < W; x++) put(&fr_b, x, y, 255, 255, 255);

        s2a_hdr_itm clean, hud, naive;
        float p1 = 0, p2 = 0, p3 = 0;
        ok(knee_of(&fr_a, 1, &clean, &p1), "底图：算出拐点");
        ok(knee_of(&fr_b, 1, &hud, &p2), "底图+白条：算出拐点");
        ok(knee_of(&fr_b, 0, &naive, &p3), "底图+白条（不做剔除）：算出拐点");
        ok(fabsf(p2 - 1.0f) < 1e-6f, "HUD：纸白增益恒 ×1.0000");
        ok(fabsf(hud.knee_lo - clean.knee_lo) < 1e-6f && fabsf(hud.knee_hi - clean.knee_hi) < 1e-6f,
           "HUD：拐点与「没有白条的那一帧」逐位相同");
        ok(fabsf(naive.knee_lo - clean.knee_lo) > 1e-3f,
           "HUD：不做剔除的对照组拐点明显不同（判别力）");
        printf("    底图 lo=%.4f hi=%.4f  |  有白条(剔除) lo=%.4f hi=%.4f  |  不剔除 lo=%.4f\n",
               (double)clean.knee_lo, (double)clean.knee_hi, (double)hud.knee_lo, (double)hud.knee_hi,
               (double)naive.knee_lo);
    }

    // —— 3) 字幕条：1 行白字（0.4% 像素），性质同 HUD ——
    {
        s2a_hdr_itm clean, sub, naive;
        float p = 0;
        ok(knee_of(&fr_a, 1, &clean, &p), "字幕：底图拐点");
        memcpy(&fr_b, &fr_a, sizeof fr_a);
        for (int x = 0; x < W; x++) put(&fr_b, x, H - 20, 255, 255, 255);   // 320 px = 0.55%（>0.5% 白点阈值）
        ok(knee_of(&fr_b, 1, &sub, &p), "字幕：加字幕条后拐点");
        ok(knee_of(&fr_b, 0, &naive, &p), "字幕：不做剔除的拐点");
        ok(fabsf(sub.knee_lo - clean.knee_lo) < 1e-6f, "字幕：拐点不受字幕条影响（逐位相同）");
        ok(fabsf(naive.knee_lo - clean.knee_lo) > 1e-4f, "字幕：不剔除时拐点确实被拉走（判别力）");
        printf("    字幕：clean lo=%.4f sub lo=%.4f naive lo=%.4f\n",
               (double)clean.knee_lo, (double)sub.knee_lo, (double)naive.knee_lo);
    }

    // —— 4) 淡出到底：连续变暗 30 帧，knee_lo 保持不动，avg 一路下掉 ——
    {
        static uint32_t hist[S2A_HDR_HIST_BINS];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int v = 150 + (x * 60) / (W - 1);
            put(&fr_a, x, y, v, v, v);
        }
        s2a_hdr_itm itm; float p = 0;
        ok(knee_of(&fr_a, 1, &itm, &p), "淡出：起帧拐点");
        float lo0 = itm.knee_lo, hi0 = itm.knee_hi;
        float prev_avg = 0.0f, prev_white = 0.0f, cur_lo = lo0, cur_hi = hi0;
        int have = 0, lo_moved = 0, avg_dropped = 0;
        float last_avg = 0.0f, last_lo = lo0;
        // 目标 2.4 stop/s @60Hz（实测片源的淡出落在 1.6–4.1）。
        // 注意 sRGB 编码：码值乘 k 对应线性乘 k^2.2，所以码值因子要按 1/2.2 取幂。
        const double lin_per_frame = pow(2.0, -2.4 / 60.0);
        const double code_per_frame = pow(lin_per_frame, 1.0 / 2.2);
        for (int f = 0; f < 60; f++) {
            double k = pow(code_per_frame, (double)f);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int v = 150 + (x * 60) / (W - 1);
                int vv = (int)lround((double)v * k);
                put(&fr_b, x, y, vv, vv, vv);
            }
            hist_of(&fr_b, hist);
            float wb = 0.0f;
            if (!s2a_hdr_sdr_white_bulk(hist, &wb)) break;
            s2a_hdr_auto_l1b b;
            memset(&b, 0, sizeof b);
            if (!s2a_hdr_auto_l1b_step_fused_hz(hist, wb, cur_lo, cur_hi, prev_avg, prev_white, have, 60.0f, &b))
                break;
            if (have && fabsf(b.avg - last_avg) > 1e-6f) avg_dropped = 1;
            if (have && fabsf(b.knee_lo - last_lo) > 1e-3f) lo_moved = 1;
            last_avg = b.avg; last_lo = b.knee_lo;
            prev_avg = b.avg; prev_white = b.white; cur_lo = b.knee_lo; cur_hi = b.knee_hi; have = 1;
        }
        ok(avg_dropped, "淡出：avg 逐帧下掉（画面确实在变暗）");
        ok(!lo_moved, "淡出：knee_lo 全程保持不动（冻结生效，中调不被越黑越抬）");
        ok(fabsf(last_lo - lo0) < 1e-3f, "淡出：末帧 knee_lo 与起始相同");
        printf("    淡出 60 帧 @2.4 stop/s：lo %.4f → %.4f（阈值 1e-3）avg 末帧 %.4f\n",
               (double)lo0, (double)last_lo, (double)last_avg);
    }

    printf(fails ? "✗ t_hdr_frames：%d 项（失败 %d）\n" : "✓ t_hdr_frames：图像级用例 %d 项（失败 0）\n", total, fails);
    return fails ? 1 : 0;
}
