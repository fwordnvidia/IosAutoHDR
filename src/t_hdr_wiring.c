// t_hdr_wiring：GPU 上屏路径的「接线」测试。
//
// 为什么单独一套：`nvn_hdr_hist.inc` 与 `nvn_mtl.m` 里的 ObjC 本机编不了（要 xcodebuild），
// 但那段代码用的判据全在头文件里。这里把 .inc 的平滑循环照抄成 C，用同样的函数跑一遍，
// 就能在没有 Metal 的机器上断言「纸白不动」这条语义确实落到了 GPU 路径上。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("  ✗ %s\n", m); } } while (0)

// ── 照抄 nvn_hdr_hist.inc 的回读处理（只有 white 可用，没有直方图）──
typedef struct { float lo, hi, head; int init; } knee_state;
static void inc_step_hz(knee_state *k, float v_hi, float v_white, float v_scene, float hz);

static void inc_step(knee_state *k, float v_hi, float v_white, float v_scene) {
    inc_step_hz(k, v_hi, v_white, v_scene, S2A_HDR_SMOOTH_HZ);
}
static void inc_step_hz(knee_state *k, float v_hi, float v_white, float v_scene, float hz) {
    float cur_lo = k->lo, cur_hi = k->lo + k->hi;
    float tgt_lo = 0.0f, tgt_hi = 0.0f;
    if (s2a_hdr_l1b_targets_from_white(v_white, &tgt_lo, &tgt_hi)) {
        float lo = k->init ? s2a_hdr_auto_step_l1b_hz(cur_lo, tgt_lo, v_scene, hz) : tgt_lo;
        float hi = k->init ? s2a_hdr_auto_step_l1b_hz(cur_hi, tgt_hi, v_scene, hz) : tgt_hi;
        if (!(hi > lo + 1e-4f)) hi = lo + 1e-4f;
        k->lo = lo; k->hi = hi - lo; k->init = 1;
    } else {
        float nxt = k->init ? s2a_hdr_auto_step_l1(cur_hi, v_hi, v_scene) : v_hi;
        float lo = S2A_HDR_KNEE_RATIO * nxt;
        k->lo = lo; k->hi = nxt - lo; k->init = 1;
    }
}

// 照抄 .inc 新增的两路场景判据：重心（kernel 给的 v[3]）+ 白点变化
static float inc_scene(float v_scene, float prev_white, float v_white, int have_prev) {
    return have_prev ? s2a_hdr_scene_fuse(v_scene, s2a_hdr_scene_weight_white(prev_white, v_white)) : 0.0f;
}

// 用当前 knee 算某个输入码值的增益（与上屏 MSL 的公式一致）
static float gain_at(const knee_state *k, float y_lin) {
    float knee_lo = k->lo, knee_hi = k->lo + k->hi;
    float u = (y_lin - knee_lo) / (knee_hi - knee_lo);
    if (!(u > 0.0f)) return 1.0f;
    if (u > 1.0f) u = 1.0f;
    return 1.0f + (k->head - 1.0f) * (u * u * (3.0f - 2.0f * u));
}

// head 是「只读一次并缓存」的（运行期不变），同一进程里 setenv 再读拿不到新值。
// 所以 head 的取值测试另起进程跑：t_hdr_wiring head <值>  → 打印解析结果。
static float head_in_subprocess(const char *me, const char *val) {
    char cmd[512];
    if (val) snprintf(cmd, sizeof cmd, "%s head %s 2>/dev/null", me, val);
    else snprintf(cmd, sizeof cmd, "%s head - 2>/dev/null", me);
    FILE *p = popen(cmd, "r");
    if (!p) return -1.0f;
    float v = -1.0f;
    if (fscanf(p, "%f", &v) != 1) v = -1.0f;
    pclose(p);
    return v;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "head") == 0) {
        if (strcmp(argv[2], "-") != 0) setenv("S2A_HDR_HEADROOM", argv[2], 1);
        else unsetenv("S2A_HDR_HEADROOM");
        printf("%.4f\n", (double)s2a_hdr_itm_params().head);
        return 0;
    }
    const char *me = argv[0];
    // 1 目标端：普通白点
    float lo = 0, hi = 0;
    CHECK(s2a_hdr_l1b_targets_from_white(0.18f, &lo, &hi) == 1, "白点 0.18 可推目标");
    CHECK(fabsf(lo - 0.18f) < 1e-6f, "lo = 纸白（不是 0.55×hi）");
    CHECK(fabsf(hi - S2A_HDR_SDR_CLIP) < 1e-6f, "hi = SDR 裁切点");

    // 2 满屏 UI 白：纸白贴顶时要留出可扩张区间
    CHECK(s2a_hdr_l1b_targets_from_white(0.99f, &lo, &hi) == 1, "白点贴顶可用");
    CHECK(fabsf(lo - 0.95f) < 1e-6f, "lo 夹到 0.95");

    // 3 黑底画面（代理判据）：白点极低时从近黑开始扩张
    CHECK(s2a_hdr_l1b_targets_from_white(0.005f, &lo, &hi) == 1, "极低白点可用");
    CHECK(fabsf(lo - S2A_HDR_DARK_FLOOR) < 1e-6f, "暗场 lo = DARK_FLOOR");

    // 4 不可用：0 / NaN 都退回 L1
    CHECK(s2a_hdr_l1b_targets_from_white(0.0f, &lo, &hi) == 0, "白点 0 → 退回 L1");
    CHECK(s2a_hdr_l1b_targets_from_white(NAN, &lo, &hi) == 0, "白点 NaN → 退回 L1");

    // 5 退化保护：区间不许塌（lo 与 hi 太近时撑开 0.02）
    s2a_hdr_l1b_targets_from_white(0.999f, &lo, &hi);
    CHECK(hi > lo + 0.019f, "区间至少 0.02");

    // 6 连续 60 帧满屏 UI 白：纸白增益必须停在 1.00（这是 L1 的老毛病：1.57 倍）
    knee_state k = {0.55f, 0.45f, 2.0f, 0};
    for (int i = 0; i < 60; i++) inc_step(&k, 1.04f, 0.832f, 0.0f);
    float g_paper = gain_at(&k, 0.832f);
    CHECK(fabsf(g_paper - 1.0f) < 0.02f, "60 帧后纸白增益 ≈1.00（纸白不动）");
    printf("  · UI 白 60 帧：knee_lo %.4f knee_hi %.4f 纸白增益 %.4f 纯白增益 %.4f\n",
           (double)k.lo, (double)(k.lo + k.hi), (double)g_paper, (double)gain_at(&k, 1.0f));

    // 7 同一场景下纯白必须吃满 head（扩张真的发生了，不是整体压平）
    CHECK(fabsf(gain_at(&k, 1.0f) - 2.0f) < 0.05f, "纯白增益 ≈ head=2.0");

    // 8 硬切到暗场（white 骤降）：一帧到位（首帧后 scene=1 的路径由 l1b 的 coef 保证）
    knee_state k2 = {0.55f, 0.45f, 2.0f, 0};
    inc_step(&k2, 0.35f, 0.35f, 0.0f);
    inc_step(&k2, 0.35f, 0.35f, 1.0f);
    CHECK(fabsf(k2.lo - 0.35f) < 1e-3f, "硬切后 knee_lo 跟到新纸白");

    // 9 白点消失（空帧/全黑）→ 退回 L1 路径且不崩：hi 仍有限、lo < hi
    knee_state k3 = {0.55f, 0.45f, 2.0f, 1};
    for (int i = 0; i < 10; i++) inc_step(&k3, 0.9f, 0.0f, 0.0f);
    CHECK(isfinite(k3.hi) && k3.hi > 0.0f, "退回 L1 后 knee 仍有限");
    CHECK(k3.lo + k3.hi > k3.lo, "knee_hi > knee_lo");

    // 10 head 通路：S2A_HDR_HEADROOM 真能改到 itm_params（以前 g_hdr_knee[2] 永远是 2.0）。
    //    越界值的策略是「忽略、保持缺省」，不是夹断——别把 0.5 当成 1.0。
    CHECK(fabsf(head_in_subprocess(me, NULL) - 2.0f) < 1e-6f, "未设 → 2.0");
    CHECK(fabsf(head_in_subprocess(me, "4") - 4.0f) < 1e-6f, "headroom=4 生效");
    CHECK(fabsf(head_in_subprocess(me, "1.5") - 1.5f) < 1e-6f, "headroom=1.5 生效");
    CHECK(fabsf(head_in_subprocess(me, "0.5") - 2.0f) < 1e-6f, "headroom<1 被忽略（保持 2.0）");
    CHECK(fabsf(head_in_subprocess(me, "99") - 2.0f) < 1e-6f, "headroom>4 被忽略（保持 2.0）");
    CHECK(fabsf(head_in_subprocess(me, "abc") - 2.0f) < 1e-6f, "非数字被忽略（保持 2.0）");

    // 11 head=4 时纸白仍然不动、纯白吃到 4
    knee_state k4 = {0.55f, 0.45f, 4.0f, 0};
    for (int i = 0; i < 60; i++) inc_step(&k4, 1.04f, 0.832f, 0.0f);
    CHECK(fabsf(gain_at(&k4, 0.832f) - 1.0f) < 0.02f, "head=4 纸白仍不动");
    CHECK(fabsf(gain_at(&k4, 1.0f) - 4.0f) < 0.1f, "head=4 纯白吃到 4");


    // 12 同 APL 切镜：重心几乎不动（v_scene≈0），只有白点从 0.30 跳到 0.85。
    //    只看重心的旧路径会走满 29 帧斜坡；两路取大之后必须 1~2 帧到位。
    {
        knee_state w = {0.30f, 0.70f, 2.0f, 1};
        float pw = 0.30f;                     // 上一帧白点
        int frames_old = 0, frames_new = 0;
        knee_state a = w, b = w;
        for (int i = 0; i < 40; i++) {
            // 旧路径只看重心：同 APL 切换时两帧重心只差 0.05 stop → 权重 0，斜坡照走
            inc_step(&a, 1.04f, 0.85f, s2a_hdr_scene_weight(-2.00f, -1.95f));
            if (fabsf(a.lo - 0.85f) < 0.05f * (0.85f - 0.30f)) { frames_old = i + 1; break; }
        }
        for (int i = 0; i < 40; i++) {
            inc_step(&b, 1.04f, 0.85f, inc_scene(s2a_hdr_scene_weight(-2.00f, -1.95f), pw, 0.85f, 1));
            if (fabsf(b.lo - 0.85f) < 0.05f * (0.85f - 0.30f)) { frames_new = i + 1; break; }
        }
        printf("  · 同 APL 切镜：旧 %d 帧 / 新 %d 帧（85%% 到位）\n", frames_old, frames_new);
        CHECK(frames_old > 10, "旧路径确实慢（>10 帧）");
        CHECK(frames_new <= 2, "白点判据让它 1~2 帧到位");
        CHECK(frames_new < frames_old, "融合判据严格快于只看重心");
    }

    // 13 白点慢变不许误判成切镜：每帧 0.5% 的白点漂移（≈0.007 stop）
    {
        knee_state k5 = {0.30f, 0.70f, 2.0f, 1};
        float pw = 0.30f, wv = 0.30f, max_w = 0.0f;
        for (int i = 0; i < 120; i++) {
            wv *= 1.005f;
            float sw = inc_scene(0.0f, pw, wv, 1);
            if (sw > max_w) max_w = sw;
            inc_step(&k5, 1.04f, wv, sw);
            pw = wv;
        }
        printf("  · 慢漂移 120 帧：最大 scene 权重 %.4f\n", (double)max_w);
        CHECK(max_w < 0.05f, "慢漂移不触发切镜（权重 <0.05）");
    }

    // 14 一侧测不出（白点 0 / NaN）时白点判据必须让路，不能把 scene 拉成 1
    CHECK(s2a_hdr_scene_weight_white(0.0f, 0.5f) == 0.0f, "prev_white=0 → 权重 0");
    CHECK(s2a_hdr_scene_weight_white(0.5f, 0.0f) == 0.0f, "white=0 → 权重 0");
    CHECK(s2a_hdr_scene_weight_white(NAN, 0.5f) == 0.0f, "prev_white=NaN → 权重 0");
    CHECK(s2a_hdr_scene_fuse(0.3f, 0.7f) == 0.7f, "融合取大");
    CHECK(s2a_hdr_scene_fuse(0.3f, NAN) == 0.0f, "一路 NaN 不许把 scene 变 NaN/1（NaN 比较恒假 → 取 0）");


    // 15 真直方图路径（blit 回读）与旧的 float4 代理路径：星空类画面是分水岭
    //    星空 = 99.9% 纯黑 + 0.1% 的 0.72 亮点：lit=0.001 < 5% ⇒ 暗场，纸白从 0.02 起。
    //    float4 里只有 white≈0.49 / avg≈-1.06，看不出「大部分是黑的」⇒ 代理必然把它当亮场，
    //    纸白取到 0.49，整幅星空冻住。这就是为什么要 blit 1 KB 直方图回 CPU。
    {
        static uint32_t h[S2A_HDR_HIST_BINS];
        memset(h, 0, sizeof h);
        h[s2a_hdr_hist_bin(0.0f)] = 99900;
        h[s2a_hdr_hist_bin(s2a_hdr_to_linear(0.72f))] = 100;

        float w = 0.0f, avg = 0.0f, lo = 0.0f, hi = 0.0f;
        CHECK(s2a_hdr_auto_l1b_targets(h, 0.0f, &w, &avg, &lo, &hi) == 1, "星空直方图可测");
        CHECK(fabsf(lo - S2A_HDR_DARK_FLOOR) < 1e-6f, "星空：目标 lo = DARK_FLOOR（按 lit 判暗场）");

        float plo = 0.0f, phi = 0.0f;
        CHECK(s2a_hdr_l1b_targets_from_white(w, &plo, &phi) == 1, "代理路径可用");
        CHECK(plo > 0.4f, "代理路径把星空当亮场（这就是旧 GPU 行为）");

        // 从亮场纸白 0.5 起步，连续 60 帧同一星空：必须一步步落到 DARK_FLOOR。
        // 平滑是 10%/帧（scene=0 时），所以不是一帧到位 —— 这里断言的是「会到」，不是「立刻到」。
        float klo = 0.5f, khi = 1.0f, pw = 0.0f, pa = -1.0f;
        s2a_hdr_auto_l1b b;
        int moved = 0;
        for (int i = 0; i < 60; i++) {
            if (!s2a_hdr_auto_l1b_step_fused(h, 0.0f, klo, khi, pa, pw, 1, &b)) break;
            if (b.knee_lo < klo - 1e-6f) moved++;
            klo = b.knee_lo; khi = b.knee_hi; pw = b.white; pa = b.avg;
        }
        CHECK(moved >= 58, "星空连续帧里 knee_lo 单调下降");
        CHECK(fabsf(klo - S2A_HDR_DARK_FLOOR) < 2e-3f, "60 帧后 knee_lo 收到 DARK_FLOOR");
        printf("  · 星空：lit %.4f 白点 %.3f 目标 lo %.4f → 60 帧后 knee_lo %.4f（float4 代理取 %.3f，永不下来）\n",
               (double)s2a_hdr_lit_frac(h), (double)w, (double)lo, (double)klo, (double)plo);
    }

    // 16 白点没变时 fused 与 step_white 必须逐位相同（融合只在白点跳变时生效）
    {
        static uint32_t h[S2A_HDR_HIST_BINS];
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) h[i] = 0;
        h[s2a_hdr_hist_bin(0.30f)] = 60000;
        h[s2a_hdr_hist_bin(0.80f)] = 40000;
        s2a_hdr_auto_l1b a1, a2;
        memset(&a1, 0, sizeof a1); memset(&a2, 0, sizeof a2);
        int r1 = s2a_hdr_auto_l1b_step_white(h, 0.0f, 0.5f, 1.0f, -1.0f, 1, &a1);
        // prev_avg 用本帧实测重心：否则会被「均匀变亮/变暗冻结」（L1c）拦下，
        // 那种情况本来就不该和 step_white 相同。这里只想验「白点没变时融合不引入差」。
        int r2 = s2a_hdr_auto_l1b_step_fused(h, 0.0f, 0.5f, 1.0f, a1.avg, a1.white, 1, &a2);
        CHECK(r1 == 1 && r2 == 1, "两入口同帧可用");
        CHECK(memcmp(&a1.knee_lo, &a2.knee_lo, sizeof(float)) == 0, "白点不变 → knee_lo 逐位相同");
        CHECK(memcmp(&a1.knee_hi, &a2.knee_hi, sizeof(float)) == 0, "白点不变 → knee_hi 逐位相同");
    }

    // 17 白点死区：直方图按 0.0547 stop 分格，内容微动会让白点在相邻两格之间跳，
    // 每跳一次 knee_lo 就被拽一下（真实照片慢推序列实测 11 次方向翻转）。死区取 1.5 格。
    {
        static uint32_t h1[S2A_HDR_HIST_BINS], h2[S2A_HDR_HIST_BINS];
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) { h1[i] = 0; h2[i] = 0; }
        // 同一内容，只是顶部的 0.5% 尖峰落在**相邻两格**（相差恰好 1 格 = 0.0547 stop）。
        // 这必须用格编号而不是「随手挑两个亮度」：0.65 与 0.70 相差 2 格，超出死区是应该动的。
        int kb = s2a_hdr_hist_bin(0.60f);
        float e0 = s2a_hdr_hist_edge(kb), e1 = s2a_hdr_hist_edge(kb + 1);
        h1[kb] = 90000; h1[s2a_hdr_hist_bin(e0 + 0.001f)] = 600;
        h2[kb] = 90000; h2[s2a_hdr_hist_bin(e1 + 0.001f)] = 600;
        s2a_hdr_auto_l1b a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        CHECK(s2a_hdr_auto_l1b_step_fused(h1, 0.0f, 0.60f, 1.0f, -3.0f, 0.0f, 0, &a) == 1, "死区基准帧可用");
        float jitter_max = 0.0f;
        float klo = a.knee_lo, khi = a.knee_hi, pw = a.white, pa = a.avg;
        for (int i = 0; i < 40; i++) {
            const uint32_t *h = (i & 1) ? h2 : h1;
            if (!s2a_hdr_auto_l1b_step_fused(h, 0.0f, klo, khi, pa, pw, 1, &b)) break;
            float d = fabsf(b.knee_lo - klo);
            if (d > jitter_max) jitter_max = d;
            klo = b.knee_lo; khi = b.knee_hi; pw = b.white; pa = b.avg;
        }
        CHECK(jitter_max < 1e-6f, "一格白点抖动不移动 knee_lo（死区内冻结）");
        printf("  · 死区：%.1f 格阈值，40 帧交替抖动后 knee_lo 最大位移 %.2e\n",
               (double)(S2A_HDR_WHITE_DEADBAND / S2A_HDR_HIST_BIN_STOP), (double)jitter_max);

        // 真实切镜（白点跨 ~1.7 stop）必须照常响应：死区不许把切换也冻住
        static uint32_t h3[S2A_HDR_HIST_BINS];
        for (int i = 0; i < S2A_HDR_HIST_BINS; i++) h3[i] = 0;
        h3[s2a_hdr_hist_bin(0.60f)] = 40000; h3[s2a_hdr_hist_bin(1.90f)] = 400;
        s2a_hdr_auto_l1b c;
        memset(&c, 0, sizeof c);
        CHECK(s2a_hdr_auto_l1b_step_fused(h3, 0.0f, klo, khi, pa, pw, 1, &c) == 1, "切镜帧可用");
        CHECK(c.knee_lo > klo + 1e-3f, "白点跳 1.7 stop：死区不拦，knee_lo 立刻动");
        printf("  · 切镜：knee_lo %.4f → %.4f（一帧）\n", (double)klo, (double)c.knee_lo);
    }

    // ── 上屏路径的帧率自适应：.inc 用时间戳估 Hz，再喂给 coef ──
    // 判据：同样的物理时间（秒）内，30/60/120 Hz 的收敛进度必须一致；
    // 若 .inc 固定按 60 Hz 算，30 Hz 掉帧时会慢一半、120 Hz 切档会快一倍。
    {
        float hz30 = 0.0f;
        for (int i = 0; i < 100; i++) hz30 = s2a_hdr_hz_from_dt(1.0 / 30.0, hz30);
        knee_state a = {0}, b = {0};
        a.head = b.head = 2.0f;
        a.lo = b.lo = 0.20f; a.hi = b.hi = 1.0f - 0.20f; a.init = b.init = 1;
        // 同一段物理时间 0.2 s：30 Hz 走 6 帧、60 Hz 走 12 帧
        for (int i = 0; i < 6; i++) inc_step_hz(&a, 1.0f, 0.42f, 0.0f, hz30);
        for (int i = 0; i < 12; i++) inc_step_hz(&b, 1.0f, 0.42f, 0.0f, S2A_HDR_SMOOTH_HZ);
        CHECK(fabsf(a.lo - b.lo) < 0.005f, "0.2 秒后 30 Hz 与 60 Hz 的 knee_lo 应一致（±0.005）");
        CHECK(a.lo > 0.20f && a.lo < 0.41f, "0.2 秒时还没到目标（否则这条判据测不出东西）");
        printf("  · 0.2 秒后 knee_lo：30 Hz %.5f / 60 Hz %.5f\n", (double)a.lo, (double)b.lo);
        // 掉帧：前 6 帧按 60 Hz、后 6 帧按 30 Hz（= 0.1 s + 0.2 s），对比纯 60 Hz 走 18 帧（0.3 s）
        knee_state c = {0}, e = {0};
        c.head = e.head = 2.0f;
        c.lo = e.lo = 0.20f; c.hi = e.hi = 1.0f - 0.20f; c.init = e.init = 1;
        for (int i = 0; i < 6; i++) inc_step_hz(&c, 1.0f, 0.42f, 0.0f, S2A_HDR_SMOOTH_HZ);
        for (int i = 0; i < 6; i++) inc_step_hz(&c, 1.0f, 0.42f, 0.0f, hz30);
        for (int i = 0; i < 18; i++) inc_step_hz(&e, 1.0f, 0.42f, 0.0f, S2A_HDR_SMOOTH_HZ);
        CHECK(fabsf(c.lo - e.lo) < 0.01f, "掉帧（60→30）0.3 秒后与稳态 60 Hz 差 <0.01");
        printf("  · 掉帧 0.3 秒后 knee_lo：混 %.5f / 纯 60 Hz %.5f\n", (double)c.lo, (double)e.lo);
        // 非法 hz 不许让 knee 停住或跳变
        knee_state d = {0};
        d.head = 2.0f;
        for (int i = 0; i < 40; i++) inc_step_hz(&d, 1.0f, 0.42f, 0.0f, NAN);
        CHECK(d.lo > 0.20f && d.lo <= 0.42f + 1e-6f, "hz=NaN 时 knee 仍在推进且不越过目标");
    }

    printf("%s t_hdr_wiring：上屏接线 / 纸白不动 / head 通路 %d 项 → %s（失败 %d）\n",
           fails ? "✗" : "✓", checks, fails ? "有失败" : "全过", fails);
    return fails != 0;
}
