// HDR 质量的量化报告（不只是断言）：四种典型画面走完整条链路，输出「多少 nit、9.9 分位在哪、多久收敛」。
// 目的有两个：
//   1) 给「扩张是否真的看得见」一个数（SDR 参考白按 BT.2408 的 203 nit 记，head=2 ⇒ 峰值 406 nit）；
//   2) 给「自适应会不会抖」一个数（硬切镜头后多少帧贴近目标 knee）。
// 断言只锁住「绝不能坏」的性质（不发散、不越 head、灰不偏色、扩张真的发生），具体数值打印出来供人看。
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#define W 160
#define H 90
#define PW_NITS 203.0f   // BT.2408：SDR 图形白 = 203 nit

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { float y[W * H]; } frame;   // sRGB 编码的亮度（三通道同值，用来算统计）
typedef struct { float mean, p999, max, over; } stats;

static void fill_dark(frame *f) {           // 太空/深夜：96% 极暗，少量星点与霓虹
    for (int i = 0; i < W * H; i++) f->y[i] = (i % 97 == 0) ? 0.02f : 0.0035f;
    for (int i = 0; i < W * H; i += 613) f->y[i] = 0.85f;      // ~0.16% 的星点
}
static void fill_city(frame *f) {           // 城市夜景：整体偏暗，招牌是亮点
    for (int i = 0; i < W * H; i++) f->y[i] = 0.05f + 0.03f * ((i * 37) % 11) / 11.0f;
    for (int i = 0; i < W * H; i += 97) f->y[i] = 0.62f;       // ~1% 的招牌
}
static void fill_ui(frame *f) {             // 满屏 UI 白：这就是把 L1 钳位顶爆的那种画面
    for (int i = 0; i < W * H; i++) f->y[i] = 0.92f;
    for (int i = 0; i < W * H; i += 41) f->y[i] = 0.06f;       // 文字
}
static void fill_mixed(frame *f) {          // 混合：横向灰阶 + 5% 高光
    for (int i = 0; i < W * H; i++) f->y[i] = (float)(i % W) / (float)(W - 1);
    for (int i = 0; i < W * H; i += 19) f->y[i] = 1.0f;
}

static void hist_of(const frame *f, uint32_t hist[S2A_HDR_HIST_BINS]) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < W * H; i++) hist[s2a_hdr_hist_bin(s2a_hdr_to_linear(f->y[i]))]++;
}

// 跑到稳态：40 帧足够（coef 最小 0.1，10 帧就到 65%）
static s2a_hdr_auto_l1 converge(const frame *f) {
    uint32_t hist[S2A_HDR_HIST_BINS];
    s2a_hdr_auto_l1 r; memset(&r, 0, sizeof r);
    hist_of(f, hist);
    int have = 0; float prev_hi = 1.0f, prev_avg = 0.0f;
    for (int i = 0; i < 40; i++) {
        s2a_hdr_auto_l1_step(hist, prev_hi, prev_avg, have, &r);
        prev_hi = r.knee_hi; prev_avg = r.avg; have = 1;
    }
    return r;
}

// L1b 的稳态：lo / hi 两端各自平滑
static s2a_hdr_auto_l1b converge_b(const frame *f) {
    uint32_t hist[S2A_HDR_HIST_BINS];
    s2a_hdr_auto_l1b r; memset(&r, 0, sizeof r);
    hist_of(f, hist);
    int have = 0; float lo = 0.5f, hi = 1.0f, avg = 0.0f;
    for (int i = 0; i < 40; i++) {
        s2a_hdr_auto_l1b_step(hist, lo, hi, avg, have, &r);
        lo = r.knee_lo; hi = r.knee_hi; avg = r.avg; have = 1;
    }
    return r;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// 上屏：sRGB 值 → 线性 → grade（缺省 sat=1/p3=0）→ 乘 PW 换成 nit
static stats apply(const frame *f, const s2a_hdr_itm *p, float head) {
    static float out[W * H];
    for (int i = 0; i < W * H; i++) {
        float lin[3] = {s2a_hdr_to_linear(f->y[i]), s2a_hdr_to_linear(f->y[i]), s2a_hdr_to_linear(f->y[i])};
        float o[3];
        s2a_hdr_grade_rgb(p, 1.0f, 0.0f, lin, o);
        out[i] = o[0] * PW_NITS;
    }
    qsort(out, W * H, sizeof(float), cmp_float);
    stats s = {0, out[(int)((W * H) * 0.999f)], out[W * H - 1], 0};
    double acc = 0;
    for (int i = 0; i < W * H; i++) { acc += out[i]; if (out[i] > head * PW_NITS * 0.999f) s.over += 1.0f; }
    s.mean = (float)(acc / (W * H));
    s.over /= (W * H);
    return s;
}

static void report(const char *name, const frame *f, const s2a_hdr_auto_l1 *r, const stats *s) {
    double apl = 0;
    for (int i = 0; i < W * H; i++) apl += s2a_hdr_to_linear(f->y[i]);
    printf("  %-6s APL %5.3f  white %5.3f  knee %4.2f..%4.2f  →  均值 %6.1f nit  9.9%% %6.1f nit  峰 %6.1f nit  越界 %.2f%%\n",
           name, apl / (W * H), r->white, r->knee_lo, r->knee_hi, s->mean, s->p999, s->max, s->over * 100.0);
}

int main(void) {
    puts("t_hdr_quality：四种画面的量化结果（SDR 白 = 203 nit，head = 2.0 ⇒ 峰值 406 nit）");
    s2a_hdr_itm base = s2a_hdr_itm_params();
    float head = base.head;
    printf("  head = %.2f（S2A_HDR_HEADROOM），纸白 %.0f nit ⇒ 峰值上限 %.0f nit\n", head, PW_NITS, head * PW_NITS);

    frame dark, city, ui, mixed;
    fill_dark(&dark); fill_city(&city); fill_ui(&ui); fill_mixed(&mixed);

    s2a_hdr_auto_l1 rd = converge(&dark), rc = converge(&city), ru = converge(&ui), rm = converge(&mixed);
    s2a_hdr_itm pd = base, pc = base, pu = base, pm = base;
    pd.knee_lo = rd.knee_lo; pd.knee_hi = rd.knee_hi;
    pc.knee_lo = rc.knee_lo; pc.knee_hi = rc.knee_hi;
    pu.knee_lo = ru.knee_lo; pu.knee_hi = ru.knee_hi;
    pm.knee_lo = rm.knee_lo; pm.knee_hi = rm.knee_hi;

    stats sd = apply(&dark, &pd, head), sc = apply(&city, &pc, head);
    stats su = apply(&ui, &pu, head), sm = apply(&mixed, &pm, head);
    report("暗场", &dark, &rd, &sd);
    report("夜城", &city, &rc, &sc);
    report("UI白", &ui, &ru, &su);
    report("混合", &mixed, &rm, &sm);

    // ---- 1) 暗场景：亮部必须真的被抬高，同时均值不许被抬成灰 ----
    //    注意口子：黑场截断（-6 stops）会把 96% 的近黑像素排除在统计外，
    //    于是「白点」落在星点那一格（0.715）——这是面积语义的正常结果，不是 bug；
    //    真正要守住的是「星点确实被抬上去」与「均值仍然是暗的」。
    {
        float star_in = s2a_hdr_to_linear(0.85f) * PW_NITS;         // 输入的星点亮度（线性 → nit）
        float ratio = sd.max / star_in;
        printf("  暗场星点：输入 %.1f nit → 输出 %.1f nit（×%.2f）\n", star_in, sd.max, ratio);
        CHECK(ratio >= 1.35f, "暗场亮部没被抬高：只有 ×%.2f", ratio);
        CHECK(ratio <= head + 1e-3f, "暗场亮部超过 head：×%.2f", ratio);
        CHECK(sd.mean < 2.0f, "暗场均值被抬亮了（%.2f nit）——扩张不该动大面积的暗部", sd.mean);
        CHECK(sd.max <= head * PW_NITS + 0.5f, "暗场峰值越界 %.1f nit", sd.max);
    }

    // ---- 2) UI 白：不许整体过曝，但要有轻微抬升 ----
    CHECK(ru.knee_hi >= 0.9f, "UI 白 knee_hi 应贴近 1.0（实际 %.3f）", ru.knee_hi);
    CHECK(su.max <= head * PW_NITS + 0.5f, "UI 白峰值越界 %.1f nit", su.max);
    CHECK(su.mean > 0.95f * PW_NITS, "UI 白均值被压暗了（%.1f nit）", su.mean);
    CHECK(su.mean < 1.35f * PW_NITS, "UI 白均值过曝（%.1f nit）", su.mean);

    // ---- 3) 混合场景：灰阶上单调、峰值不越界 ----
    {
        float prev = -1; int mono = 1;
        for (int i = 0; i < W; i++) {
            float lin[3] = {s2a_hdr_to_linear((float)i / (W - 1)), s2a_hdr_to_linear((float)i / (W - 1)), s2a_hdr_to_linear((float)i / (W - 1))};
            float o[3]; s2a_hdr_grade_rgb(&pm, 1.0f, 0.0f, lin, o);
            if (i && o[0] < prev - 1e-6f) mono = 0;
            prev = o[0];
        }
        CHECK(mono, "混合场景灰阶经扩张后必须单调");
        CHECK(sm.max <= head * PW_NITS + 0.5f, "混合场景峰值越界 %.1f nit", sm.max);
    }

    // ---- 4) 任何场景都不许把中性灰染成彩色 ----
    {
        int neutral = 1, checked = 0;
        for (int i = 0; i < W * H; i += 7) {
            float lin[3] = {s2a_hdr_to_linear(mixed.y[i]), s2a_hdr_to_linear(mixed.y[i]), s2a_hdr_to_linear(mixed.y[i])};
            float o[3]; s2a_hdr_grade_rgb(&pm, 1.0f, 0.0f, lin, o);
            if (!(o[0] == o[1] && o[1] == o[2])) neutral = 0;
            checked++;
        }
        CHECK(neutral, "中性灰被染色（检查了 %d 点）", checked);
    }

    // ---- 5) 硬切镜头：多少帧贴近目标（scene 权重应让它快）----
    {
        uint32_t hd[S2A_HDR_HIST_BINS], hu[S2A_HDR_HIST_BINS];
        hist_of(&dark, hd); hist_of(&ui, hu);
        s2a_hdr_auto_l1 a; memset(&a, 0, sizeof a);
        float prev_hi = rd.knee_hi, prev_avg = rd.avg;
        int frames = -1; float scene0 = 0.0f;
        for (int i = 0; i < 30; i++) {
            s2a_hdr_auto_l1_step(hu, prev_hi, prev_avg, 1, &a);
            if (i == 0) scene0 = a.scene;
            prev_hi = a.knee_hi; prev_avg = a.avg;
            if (frames < 0 && fabsf(a.knee_hi - ru.knee_hi) <= 0.05f * ru.knee_hi) frames = i + 1;
        }
        printf("  暗场→UI白 硬切：%d 帧内 knee 进入目标的 ±5%%（首帧场景权重 %.2f）\n", frames, scene0);
        CHECK(frames > 0 && frames <= 8, "硬切后收敛过慢：%d 帧", frames);
        CHECK(a.knee_hi > 0.5f, "硬切后 knee 没跟上（%.3f）", a.knee_hi);
    }

    // ---- 6) 同上反方向（UI 白 → 暗场）：不许被慢启动拖住 ----
    {
        uint32_t hd[S2A_HDR_HIST_BINS];
        hist_of(&dark, hd);
        s2a_hdr_auto_l1 a; memset(&a, 0, sizeof a);
        float prev_hi = ru.knee_hi, prev_avg = ru.avg;
        int frames = -1; float scene0 = 0.0f;
        for (int i = 0; i < 30; i++) {
            s2a_hdr_auto_l1_step(hd, prev_hi, prev_avg, 1, &a);
            if (i == 0) scene0 = a.scene;
            prev_hi = a.knee_hi; prev_avg = a.avg;
            if (frames < 0 && fabsf(a.knee_hi - rd.knee_hi) <= 0.05f * rd.knee_hi) frames = i + 1;
        }
        printf("  UI白→暗场 硬切：%d 帧内 knee 进入目标的 ±5%%（首帧场景权重 %.2f）\n", frames, scene0);
        CHECK(frames > 0 && frames <= 8, "反向硬切收敛过慢：%d 帧", frames);
    }

    // ---- 7) 没有非有限值、没有把亮部压暗（gain 恒 ≥ 1）----
    {
        int bad = 0;
        for (int i = 0; i < W * H; i += 3) {
            float y = mixed.y[i], lin = s2a_hdr_to_linear(y);
            float k = s2a_hdr_itm_gain(&pm, lin);
            if (!isfinite(k) || k < 1.0f) bad++;
        }
        CHECK(bad == 0, "gain 出现 <1 或非有限值 %d 次（会把暗部压得更暗）", bad);
    }

    // ---- 8) L1 vs L1b：纸白还动不动？高光还有没有？----
    {
        s2a_hdr_auto_l1b bd = converge_b(&dark), bu = converge_b(&ui), bm = converge_b(&mixed), bc = converge_b(&city);
        printf("  L1b（纸白不动）：\n");
        printf("  %-6s knee %4.2f..%4.2f\n", "暗场", bd.knee_lo, bd.knee_hi);
        printf("  %-6s knee %4.2f..%4.2f\n", "夜城", bc.knee_lo, bc.knee_hi);
        printf("  %-6s knee %4.2f..%4.2f\n", "UI白", bu.knee_lo, bu.knee_hi);
        printf("  %-6s knee %4.2f..%4.2f\n", "混合", bm.knee_lo, bm.knee_hi);

        s2a_hdr_itm pb = base; pb.knee_lo = bu.knee_lo; pb.knee_hi = bu.knee_hi;
        float y_ui = s2a_hdr_to_linear(0.92f), y_max = s2a_hdr_to_linear(1.0f);
        float g_ui_b = s2a_hdr_itm_gain(&pb, y_ui), g_max_b = s2a_hdr_itm_gain(&pb, y_max);
        float g_ui_a = s2a_hdr_itm_gain(&pu, y_ui);
        printf("  UI白：纸白增益 L1 ×%.2f → L1b ×%.2f；纯白增益 ×%.2f（head %.1f）\n", g_ui_a, g_ui_b, g_max_b, head);
        CHECK(g_ui_b <= 1.06f, "L1b 下满屏 UI 白仍被整体抬高 ×%.2f（目标：纸白不动）", g_ui_b);
        CHECK(g_ui_a > 1.10f, "L1 本应把纸白抬高（记录用，实际 ×%.2f）——若这里红了说明基线变了", g_ui_a);
        CHECK(g_max_b >= 0.8f * head, "L1b 下纯白只拿到 ×%.2f（高光没了）", g_max_b);

        // 黑底画面（星空）：L1b 必须仍把星点抬上去
        s2a_hdr_itm pdb = base; pdb.knee_lo = bd.knee_lo; pdb.knee_hi = bd.knee_hi;
        float g_star = s2a_hdr_itm_gain(&pdb, s2a_hdr_to_linear(0.85f));
        printf("  暗场星点增益 L1b ×%.2f（L1 ×%.2f）\n", g_star, s2a_hdr_itm_gain(&pd, s2a_hdr_to_linear(0.85f)));
        CHECK(g_star >= 1.4f, "黑底画面星点没被抬高：×%.2f", g_star);
        CHECK(g_star <= head + 1e-3f, "黑底画面星点超过 head：×%.2f", g_star);

        // 灰阶单调 + 中性不偏色
        float prev = -1; int mono = 1;
        for (int i = 0; i < W; i++) {
            float lin[3] = {s2a_hdr_to_linear((float)i / (W - 1)), s2a_hdr_to_linear((float)i / (W - 1)), s2a_hdr_to_linear((float)i / (W - 1))};
            float o[3]; s2a_hdr_grade_rgb(&pb, 1.0f, 0.0f, lin, o);
            if (i && o[0] < prev - 1e-6f) mono = 0;
            if (!(o[0] == o[1] && o[1] == o[2])) mono = 0;
            prev = o[0];
        }
        CHECK(mono, "L1b 灰阶必须单调且中性不偏色");

        // 硬切收敛（L1b 两端都平滑，仍要求 ≤8 帧）
        uint32_t hu[S2A_HDR_HIST_BINS], hd[S2A_HDR_HIST_BINS];
        hist_of(&ui, hu); hist_of(&dark, hd);
        s2a_hdr_auto_l1b t; memset(&t, 0, sizeof t);
        float lo = bd.knee_lo, hi = bd.knee_hi, avg = bd.avg; int frames = -1;
        for (int i = 0; i < 30; i++) {
            s2a_hdr_auto_l1b_step(hu, lo, hi, avg, 1, &t);
            lo = t.knee_lo; hi = t.knee_hi; avg = t.avg;
            if (frames < 0 && fabsf(t.knee_lo - bu.knee_lo) <= 0.05f * bu.knee_lo) frames = i + 1;
        }
        printf("  L1b 暗场→UI白：%d 帧内 lo 进入 ±5%%\n", frames);
        CHECK(frames > 0 && frames <= 8, "L1b 收敛过慢：%d 帧", frames);
        CHECK(lo <= bu.knee_lo * 1.10f + 0.01f, "L1b 的 lo 没跟上（%.3f vs %.3f）", lo, bu.knee_lo);
    }

    if (fails) { printf("t_hdr_quality：%d 项失败\n", fails); return 1; }
    printf("✓ t_hdr_quality：四场景量化 + L1/L1b 纸白对照 + 硬切收敛 %d 项 → 全过\n", checks);
    return 0;
}
