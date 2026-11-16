// 自动 HDR 的 L1：在 L0（99.5 分位 → knee）之上补三件事，让它接近「RTX HDR / Auto HDR」这类
// 逐场景自动 SDR→HDR 的行为，而不是全片一个固定 knee。
//
// L1 补的三件事（每件都对应业界实现里的一个具体旋钮，见 docs/SOURCES.md）：
//   1) **漫反射白点**（diffuse / paper white）：SDR 画面里"白"不是一个像素，而是一片面积。
//      UI、字幕、天空、雪地都是大面积高值；而爆炸、太阳只是极少数像素。
//      L0 只看 99.5 分位，一颗太阳就能把 knee 顶到 1.0，整幅偏暗的夜景因此没 HDR 感。
//      L1 取「计数 ≥ 0.5% 总像素的最高一格」当白点，扩张从白点之上才开始。
//   2) **黑场截断**（black cutoff）：黑边、纯黑背景会污染低端分布，让 percentile 计算偏向暗。
//      低于阈值（缺省 -6 stops ≈ 0.0156 线性）的像素不计入分位统计。
//   3) **场景切换**（scene change）：一阶平滑在硬切镜头时会拖出 4~8 秒的"慢慢变亮"。
//      用直方图的 log2 加权重心判断场景差，差得越大越少平滑，超过上阈值直接跳过去。
//      这与 libplacebo 的 peak_smoothing_period + scene_threshold_low/high 是同一套思路。
//
// 仍然是纯 C、无平台依赖，Linux 上可直接跑 t_hdr_auto_l1。
#ifndef S2A_NVN_HDR_AUTO_L1_H
#define S2A_NVN_HDR_AUTO_L1_H

// SDR 编码域的线性上沿（L1b 用它当 knee_hi：白点之上到裁切点之间拿满 head）
#ifndef S2A_HDR_SDR_CLIP
#define S2A_HDR_SDR_CLIP 1.0f
#endif
// 截断之上的像素少于这个占比就算「黑底画面」：此时白点不可信（星星/孤灯就是唯一亮点）
#ifndef S2A_HDR_DARK_FRAC
#define S2A_HDR_DARK_FRAC 0.05f
#endif
// 黑底画面从这条线开始扩张（线性亮度，≈ -5.6 stops）
#ifndef S2A_HDR_DARK_WHITE
#define S2A_HDR_DARK_WHITE 0.02f    // 白点低于此值当黑底画面（lit_frac 的代理判据）
#endif
#ifndef S2A_HDR_DARK_FLOOR
#define S2A_HDR_DARK_FLOOR 0.02f    // 黑底画面从这条线开始扩张（线性亮度，≈ -5.6 stops）
#endif
// 淡出（fade to black）与暗场的区分。
// 淡到黑时整幅画面等比变暗：白点跟着掉 ⇒ lo 被拖低 ⇒ 中调增益反而越涨
//（实测 270 帧转场序列：淡出 30 帧里 0.50 线性增益从 ×1.000 涨到 ×1.46，
// 即「越淡越亮」，观众看到的是淡出过程被抵消 + 中调抽动）。
// 判据：平均亮度已进暗区（avg < DIM_APL）**且**这一帧还在明显下降（>FADE_SLOPE stop/帧）⇒ 判为淡出，
// 整帧冻结（白点与两端都不动）——淡出期间没有可信的「纸白」可言，保持不动才是对的。
// 反向（离开暗区）不冻结，而是当切镜处理（scene=1）：否则从黑回来要爬 30 帧，
// 期间中调一直被 ×1.4 抬着，表现为「回到画面先闪一下亮」。
#ifndef S2A_HDR_DIM_APL
#define S2A_HDR_DIM_APL (-4.5f)
#endif
// 淡出的判据用「下降速度窗口」而不是「亮度水平」：
//   太慢（< FADE_MIN_S）是同场景微动，正常平滑；
//   太快（> FADE_MAX_S）是硬切到暗场景，必须让拐点跟过去（否则切镜后整段都错）。
// **单位是 stop/秒，不是 stop/帧**：同一段淡出在 30/60/120 Hz 下每帧的落差差 2/4 倍，
//   按帧写常数会让判据在 120 Hz 上整体失效（1 秒淡 4 stop 在 120 Hz 只有 0.033/帧，
//   落在 0.05 之下就完全不冻结，淡出时中调增益照样越黑越涨）。S2Arm 有 30/60/120 三档，
//   所以判据必须与帧率无关：实测 300 帧序列里交叉淡入 1.6–3.1 stop/s、淡入 3.0–4.1 stop/s、
//   单帧硬切 0.66 stop@30fps = 19.8 stop/s（必须判成切镜，不能冻）。
#ifndef S2A_HDR_FADE_MIN_S
#define S2A_HDR_FADE_MIN_S 1.5f
#endif
#ifndef S2A_HDR_FADE_MAX_S
#define S2A_HDR_FADE_MAX_S 16.0f
#endif
#include <math.h>
#include <stdint.h>
#include "nvn_hdr_auto.h"   // L0 的直方图定义与常数

// 漫反射白点：一格至少要占这么多像素才算"一片白"，单像素高光不算。
/* L1 的 knee 上界比 L0 宽：白点可能在 1.0（满屏 UI 白），此时 knee_hi = 1.0/0.8 = 1.25，
   意思是「画面里没有可扩张的高光了」。若沿用 L0 的 1.0 上限，满屏白会被整体乘 2 直接过曝。 */
#define S2A_HDR_KNEE_L1_MAX 2.0f
/* L1 的下界也比 L0 低：白点可能在 0.03（夜景、太空、恐怖游戏），此时 knee 若还钳在 0.25，
   整帧都落在 knee_lo 以下，扩张等于没开。下界只防 knee 塌到 0（0.01 ≈ 1% 线性亮度，
   比任何可见内容都暗），不参与观感取舍。 */
// 白点死区。直方图按 log2 分格（S2A_HDR_HIST_BINS=256 覆盖 -10..+4 ⇒ 每格 0.0547 stop），
// 内容微动会让测出的白点在**相邻两格**之间来回跳，knee_lo 就被拽着来回走
//（实测真实照片慢推序列 11 次方向翻转，幅度 0.7%）。死区取 1.5 格：刚好盖住一个格子的
// 量化跳变，又远小于切镜判据（0.5 stop），真实切换不受影响。
#define S2A_HDR_HIST_BIN_STOP ((S2A_HDR_HIST_HI - S2A_HDR_HIST_LO) / (float)S2A_HDR_HIST_BINS)
#define S2A_HDR_WHITE_DEADBAND (1.5f * S2A_HDR_HIST_BIN_STOP)

#define S2A_HDR_KNEE_L1_MIN 0.01f
#ifndef S2A_HDR_CLUSTER_MAX_FRAC
#define S2A_HDR_CLUSTER_MAX_FRAC 0.05   // 一簇亮像素少于亮像素的 5% 视为 HUD/孤立高光
#endif
#ifndef S2A_HDR_CLUSTER_GAP
#define S2A_HDR_CLUSTER_GAP 3          // 连续 3 个空 bin（约 0.35 stop）才算分开
#endif
// 纸白的定义位置：从最亮格往下累计到总像素的 0.5% 为止，那一格之上就是「高光」。
// 这个数是「激进程度」的总开关——调大它 = 纸白下沉 = 更多像素吃增益（代价见 docs/FIDELITY.md：
// 满屏 UI 白也会被抬起来，那是用户最容易察觉的错）。可用 -D 覆盖，方便对账时换档不必改头文件。
#ifndef S2A_HDR_WHITE_FRAC
#define S2A_HDR_WHITE_FRAC 0.005f
#endif
// 黑场截断：log2 线性亮度低于此值的格子不计入统计（-6 stops = 0.0156）。
#define S2A_HDR_BLACK_CUT (-6.0f)
// 重心的黑场门限：比 BLACK_CUT 更低，只排「真黑」（线性 0.0014 ≈ 8 位码 10/255 之下的死区）。
// 用 -6 会把星空/暗场整片排掉（那些内容常在 0.002..0.01），重心就永远算不出来。
#define S2A_HDR_AVG_CUT   (-9.5f)
// 场景切换：低于 LOW 档完全按平滑走，高于 HIGH 档直接跳（单位 = log2 亮度差）。
// 0.40 / 1.10 是扫出来的：0.66 stop 的「中等切镜」（菜单暗 → 游戏亮这种最常见的量级）
//   在旧 0.5/1.6 下只拿到 5.5% 权重，要 24 帧才走完 95%（300 帧序列里 B 段肉眼可见地爬）；
//   改 0.40/1.10 后权重 31.6%、8 帧到位。下限不能低于 0.35：0.30 stop/帧 的快摇镜必须
//   仍然拿到 0 权重（否则整段画面每帧都在跳），这条有 ref_check 场景十三守着。
#define S2A_HDR_SCENE_LOW 0.40f
#define S2A_HDR_SCENE_HIGH 1.10f

// 直方图的 log2 加权重心（场景亮度）。空直方图返回 0 并写 *ok=0。
static inline int s2a_hdr_bin_black(int bin) {
    float y = S2A_HDR_HIST_LO + (float)bin * ((S2A_HDR_HIST_HI - S2A_HDR_HIST_LO) / (float)S2A_HDR_HIST_BINS);
    return y < S2A_HDR_BLACK_CUT;
}

static inline float s2a_hdr_hist_avg(const uint32_t hist[S2A_HDR_HIST_BINS], int *ok) {
    uint64_t total = 0;
    double acc = 0.0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        uint64_t c = hist[i];
        if (!c) continue;
        // 只排除真正的黑（bin 下沿 < -9.5，即线性 0.0014 以下），不排 -6 以下的暗部。
        // 口径必须与 MSL 的 hdr_hist_l1 完全一致：这条差异是 t_hdr_gpu_mirror 抓出来的
        // （同一份带黑边直方图：CPU 把重心算到 -4.7120，GPU 是 -1.2049，差 3.5 stop）。
        // 而门槛只降到「真黑」而不是 -6：星空的星点常在 0.002..0.01，按 -6 会被整片丢掉。
        if (S2A_HDR_HIST_LO + (float)i * ((S2A_HDR_HIST_HI - S2A_HDR_HIST_LO) / (float)S2A_HDR_HIST_BINS)
            < S2A_HDR_AVG_CUT) continue;
        // 用格子中心当代表值（不是上沿）：重心要的是期望，不是上界
        double y = (double)S2A_HDR_HIST_LO + ((double)i + 0.5) * ((double)(S2A_HDR_HIST_HI - S2A_HDR_HIST_LO) / (double)S2A_HDR_HIST_BINS);
        acc += y * (double)c;
        total += c;
    }
    if (ok) *ok = total != 0;
    return total ? (float)(acc / (double)total) : 0.0f;
}

// 低于黑场截断的格子视作 0 像素（原地不改调用方的数组，用一份阈值判断）。

// 漫反射白点：从亮往暗扫，第一个「计数 ≥ total*WHITE_FRAC」的格子；找不到返回 0。
static inline int s2a_hdr_sdr_white(const uint32_t hist[S2A_HDR_HIST_BINS], float *white) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) if (!s2a_hdr_bin_black(i)) total += hist[i];
    if (!total) return 0;
    uint64_t need = (uint64_t)((double)total * (double)S2A_HDR_WHITE_FRAC);
    if (need < 1) need = 1;
    for (int i = S2A_HDR_HIST_BINS - 1; i >= 0; i--) {
        if (s2a_hdr_bin_black(i)) break;
        if ((uint64_t)hist[i] >= need) { *white = s2a_hdr_hist_edge(i); return 1; }
    }
    return 0;
}

// 白点估计的「HUD / 高光孤立簇」剔除。
//   动机：静态 HUD 白字、准星、字幕通常只占 1% 左右的像素却坐在 0.8 以上的高位。
//   纯白点算法会把整帧的纸白顶到 HUD 亮度上，于是 100% 的内容都不再扩张。
//   做法：从高往低扫「非空 bin 的连续簇」；如果最高那一簇的质量 < S2A_HDR_CLUSTER_MAX_FRAC，
//   且它与下方主簇之间隔着 >= S2A_HDR_CLUSTER_GAP 个空 bin，就判它是 HUD/孤立高光，丢掉它，
//   用下方主簇的顶边当白点。真实的高亮内容（天空、白墙）与主簇相连，不会被误丢。
// 可调参数版本：给敏感度扫描用（结论应证明默认常数坐在平台区，而不是刀刃上）。
static inline int s2a_hdr_sdr_white_bulk_opt(const uint32_t hist[S2A_HDR_HIST_BINS], double max_frac,
                                             int gap_bins, float *white) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) if (!s2a_hdr_bin_black(i)) total += hist[i];
    if (!total) return 0;

    // 从最高非黑 bin 往下走，按「空 bin」切簇
    int i = S2A_HDR_HIST_BINS - 1;
    while (i >= 0 && (s2a_hdr_bin_black(i) || hist[i] == 0)) i--;
    while (i >= 0) {
        int top = i;
        uint64_t mass = 0;
        int gap = 0;
        while (i >= 0) {
            if (s2a_hdr_bin_black(i) || hist[i] == 0) {
                if (++gap >= gap_bins) break;   // 够深的空档 → 簇结束
            } else {
                gap = 0;
                mass += hist[i];
            }
            i--;
        }
        // 这一簇够重（≥ 5% 亮像素）就是主簇：白点仍按「主簇内部的 0.5% 分位」取，
        // 而不是直接取簇顶。取簇顶会在自然照片上把纸白抬得过高（实测大块天空/雪地
        // 抬高 0.3–0.8 stop，扩张被无谓削弱），因为主簇本身能占到 5%–40% 的像素。
        // 分位只算主簇（即 top 以下、被丢掉的那些孤立簇不参与），这样：
        //   · HUD 场景：孤立 HUD 簇被丢，分位落在内容白点上（0.2381）✔
        //   · 自然照片：主簇完整，分位≈普通白点（0.5278 / 0.3898 / 0.6626）✔
        if ((double)mass >= (double)total * max_frac) {
            // 主簇内部仍用与 s2a_hdr_sdr_white 完全相同的判据（「最高一个计数 ≥ 0.5% 的格」），
            // 只是把被丢掉的孤立簇排除在扫描范围外。这样「没有孤立簇」时两者逐位相同，
            // 有 HUD 时白点落在内容白点上，而不是簇顶（簇顶会在自然照片上把纸白抬高 0.3–0.8 stop）。
            uint64_t need = (uint64_t)((double)total * (double)S2A_HDR_WHITE_FRAC);
            if (need < 1) need = 1;
            for (int k = top; k >= 0; k--) {
                if (s2a_hdr_bin_black(k)) break;
                if ((uint64_t)hist[k] >= need) { *white = s2a_hdr_hist_edge(k); return 1; }
            }
            *white = s2a_hdr_hist_edge(top);
            return 1;
        }
    }
    return 0;
}

static inline int s2a_hdr_sdr_white_bulk(const uint32_t hist[S2A_HDR_HIST_BINS], float *white) {
    return s2a_hdr_sdr_white_bulk_opt(hist, (double)S2A_HDR_CLUSTER_MAX_FRAC, S2A_HDR_CLUSTER_GAP, white);
}

// 截断之上的像素占比：黑底画面（星空、纯黑过场）里这个值很小，白点估计会退化成「星星就是白」，
//   这时应该按「暗场景」处理（从近黑开始扩张），而不是把星星当纸白冻住。
static inline float s2a_hdr_lit_frac(const uint32_t hist[S2A_HDR_HIST_BINS]) {
    uint64_t lit = 0, all = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        all += hist[i];
        if (!s2a_hdr_bin_black(i)) lit += hist[i];
    }
    return all ? (float)((double)lit / (double)all) : 0.0f;
}

// 场景差 → 0..1 的「突变程度」。用两帧重心的 log2 差，低阈值以下为 0、高阈值以上为 1。
static inline float s2a_hdr_scene_weight_opt(float prev_avg, float cur_avg, float low, float high) {
    float d = cur_avg - prev_avg;
    if (d < 0) d = -d;
    if (!(high > low)) return (d > low) ? 1.0f : 0.0f;   // 退化成硬阈值，仍可用
    if (!(d > low)) return 0.0f;
    if (d >= high) return 1.0f;
    float t = (d - low) / (high - low);
    return t * t * (3.0f - 2.0f * t);   // smoothstep，避免阈值处跳变
}

static inline float s2a_hdr_scene_weight(float prev_avg, float cur_avg) {
    return s2a_hdr_scene_weight_opt(prev_avg, cur_avg, S2A_HDR_SCENE_LOW, S2A_HDR_SCENE_HIGH);
}

// 白点变化同样是「切镜」的证据，而且比重心更灵：菜单 → 游戏、同亮度过场、UI 层级整体变暗，
//   这些切换里**整帧重心几乎不动**（都是同一批中间调），但白点会跳一两档。只看 avg 的后果是
//   平滑斜坡照走（29 帧），画面在新场景上先暗后亮地爬过去。
//   判据复用心重心的同一组阈值（单位都是 stop，log2 比值），所以不需要新常数。
static inline float s2a_hdr_scene_weight_white(float prev_white, float white) {
    if (!(prev_white > 0.0f) || !(white > 0.0f)) return 0.0f;   // 一侧测不出：不作判据
    return s2a_hdr_scene_weight(log2f(prev_white), log2f(white));
}

// 两路证据取大：任一路判定「切了」，就按切镜处理（coef = 1，一帧到位）。
static inline float s2a_hdr_scene_fuse(float w_avg, float w_white) {
    float w = (w_avg > w_white) ? w_avg : w_white;
    if (!(w > 0.0f)) return 0.0f;
    return (w > 1.0f) ? 1.0f : w;
}

// 帧率归一化：S2A_HDR_SMOOTH 是「60 Hz 下每帧走多少」。同一个时间常数在不同帧率下
//   每帧系数必须换算，否则 120 Hz 收敛比 30 Hz 快 4 倍——S2Arm 有 30/60/120 三档，
//   切换档位时画面会因为「平滑速度变了」而看起来像掉帧或抽动。
//   coef_hz = 1 - (1 - coef60)^(60/hz)：把「每帧保留比例」按时间折算。
#define S2A_HDR_SMOOTH_HZ 60.0f

// 帧率估计：上屏路径拿不到「层的刷新率」，只能从两次回调的时间差自己算。
// 为什么需要：coef 是「每帧走多少」，同一系数在 30/60/120 Hz 下每秒走的总量差 4 倍，
// 切档或掉帧时画面行为会变（60→30 掉帧时收敛突然变慢一半，观感就是「卡一下才跟上」）。
// 这里用 dt 的指数滑动平均（时间常数约 8 帧），再换算成 Hz；异常 dt 一律夹到 [1/240, 1/24]。
#define S2A_HDR_HZ_MIN 24.0f
#define S2A_HDR_HZ_MAX 240.0f
#define S2A_HDR_HZ_TAU 8.0f

static inline float s2a_hdr_hz_from_dt(double dt, float prev_hz) {
    if (!(dt > 1e-4) || !(dt < 1.0)) return (prev_hz > 0.0f) ? prev_hz : S2A_HDR_SMOOTH_HZ;
    float inst = (float)(1.0 / dt);
    if (!(inst >= S2A_HDR_HZ_MIN)) inst = S2A_HDR_HZ_MIN;
    if (inst > S2A_HDR_HZ_MAX) inst = S2A_HDR_HZ_MAX;
    if (!(prev_hz > 0.0f)) return inst;
    float a = 1.0f / (1.0f + S2A_HDR_HZ_TAU);
    float hz = prev_hz + (inst - prev_hz) * a;
    if (!(hz >= S2A_HDR_HZ_MIN)) hz = S2A_HDR_HZ_MIN;
    if (hz > S2A_HDR_HZ_MAX) hz = S2A_HDR_HZ_MAX;
    return hz;
}

static inline float s2a_hdr_coef_at_hz(float coef60, float hz) {
    if (!(hz > 0.0f)) hz = S2A_HDR_SMOOTH_HZ;   // 0 / NaN / 负数都按参考帧率处理
    if (hz < 1.0f) hz = 1.0f;
    if (hz > 240.0f) hz = 240.0f;               // 上限只为防 powf 溢出，与真实档位无关
    if (!(coef60 > 0.0f)) return 0.0f;
    if (coef60 >= 1.0f) return 1.0f;
    return 1.0f - powf(1.0f - coef60, S2A_HDR_SMOOTH_HZ / hz);
}

// 场景感知的平滑：突变越强，越少平滑（coef 从 SMOOTH 升到 1）。
static inline float s2a_hdr_auto_step_l1(float prev_hi, float target_hi, float scene_w) {
    float c = S2A_HDR_SMOOTH + (1.0f - S2A_HDR_SMOOTH) * scene_w;
    float d = target_hi - prev_hi;
    if (d < 0) d = -d;
    if (d < S2A_HDR_DEADBAND && scene_w <= 0.0f) return prev_hi;   // 死区只管静止画面
    return prev_hi + (target_hi - prev_hi) * c;
}

// 同上，但按实际帧率换算系数（hz<=0 或非有限 → 按 60 Hz）。
static inline float s2a_hdr_auto_step_l1_hz(float prev_hi, float target_hi, float scene_w, float hz) {
    float c = s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, hz);
    c = c + (1.0f - c) * scene_w;
    float d = target_hi - prev_hi;
    if (d < 0) d = -d;
    if (d < S2A_HDR_DEADBAND && scene_w <= 0.0f) return prev_hi;
    return prev_hi + (target_hi - prev_hi) * c;
}

// L1 主入口：一次算齐 knee_hi / knee_lo / 白点 / 重心 / 场景权重。
//   prev_hi  上一帧真正用的 knee_hi（首帧传 S2A_HDR_KNEE_MAX 之外的值也行，会被钳）
//   prev_avg 上一帧的重心（首帧传 NaN/任意值，*first=1 时不做场景判断）
typedef struct {
    float knee_hi, knee_lo;
    float white;      // 漫反射白点（线性亮度），0 = 未测出
    float avg;        // 本帧 log2 重心
    float scene;      // 0..1 突变程度
    int   first;      // 1 = 这帧没有历史（调用方应直接采用 knee_hi，不做平滑）
} s2a_hdr_auto_l1;

static inline int s2a_hdr_auto_l1_step(const uint32_t hist[S2A_HDR_HIST_BINS],
                                       float prev_hi, float prev_avg, int have_prev,
                                       s2a_hdr_auto_l1 *out) {
    if (!hist || !out) return 0;
    // 上一帧状态可能是垃圾（首帧未初始化 / 驱动崩过 / 调用方传错）：非有限就当作没有历史。
    if (have_prev && !(isfinite(prev_hi) && isfinite(prev_avg))) have_prev = 0;
    int ok = 0;
    float avg = s2a_hdr_hist_avg(hist, &ok);
    if (!ok) return 0;                                   // 全黑：调用方保持上一帧
    out->avg = avg;
    out->first = !have_prev;
    out->scene = have_prev ? s2a_hdr_scene_weight(prev_avg, avg) : 0.0f;

    float white = 0.0f;
    out->white = s2a_hdr_sdr_white(hist, &white) ? white : 0.0f;

    // 目标 knee：有白点时以白点为基准（白点之上才扩张），否则退回 L0 的分位算法。
    float hi;
    if (out->white > 0.0f) {
        hi = out->white / S2A_HDR_KNEE_TARGET;
    } else {
        float lo_unused;
        if (!s2a_hdr_auto_knee(hist, &hi, &lo_unused)) return 0;
    }
    if (!(hi >= S2A_HDR_KNEE_L1_MIN)) hi = S2A_HDR_KNEE_L1_MIN;   // 兜 NaN
    if (hi > S2A_HDR_KNEE_L1_MAX) hi = S2A_HDR_KNEE_L1_MAX;

    out->knee_hi = out->first ? hi : s2a_hdr_auto_step_l1(prev_hi, hi, out->scene);
    out->knee_lo = S2A_HDR_KNEE_RATIO * out->knee_hi;
    return 1;
}

// ---------- L1b：把「纸白不动」做成硬约束（本轮新增，与上面的 L1 并存）----------
// L1 的问题（量化测试 t_hdr_quality 里看得见）：白点 white 经 knee = white/S2A_HDR_KNEE_TARGET 得拐点，
//   而 knee_lo = 0.55 * knee，于是 0.55/0.8 = 0.69 倍白点就开始吃增益 ⇒ 满屏 UI 白整体被抬到 ×1.27 纸白。
//   RTX HDR 那类实现的语义是「纸白保持，白点之上的高光才扩张」。
// L1b 就按这个语义做：knee_lo = 白点，knee_hi = SDR 裁切点(1.0)，两端各自平滑。
//   暗场（白点 0.03）⇒ 0.03 之上全程缓抬，1.0 处拿满 head；亮场（白点 0.83）⇒ 纸白完全不动，只有近白高光抬。
// 两端都要平滑，所以 lo 也进状态；旧 L1 只平滑 hi（lo 由 ratio 导出），这正是它把纸白抬起来的原因。
static inline float s2a_hdr_auto_step_l1b_hz(float prev, float target, float scene_w, float hz) {
    float coef = s2a_hdr_coef_at_hz(S2A_HDR_SMOOTH, hz);
    coef = coef + (1.0f - coef) * scene_w;
    if (coef > 1.0f) coef = 1.0f;
    float v = prev + (target - prev) * coef;
    return (v > target) == (prev > target) ? v : target;   // 不越过目标（避免两端互推超调）
}

static inline float s2a_hdr_auto_step_l1b(float prev, float target, float scene_w) {
    return s2a_hdr_auto_step_l1b_hz(prev, target, scene_w, S2A_HDR_SMOOTH_HZ);
}

typedef struct {
    float knee_lo, knee_hi;   // 线性亮度：lo 以下完全不动，hi 处拿满 head
    float white, avg, scene;
    int   first;
    int   frozen;             // 1 = 本帧被判为淡出，一切保持上一帧（仅 fused 路径会置位）
} s2a_hdr_auto_l1b;

// 与 s2a_hdr_auto_l1_step 同输入多一个 prev_lo；返回 1 表示这一帧可用。
// white_override > 0 时直接采用（HUD 剔除路径传入的稳健白点），否则自己算。
// 目标区间（lo = 纸白，hi = SDR 裁切点）：把「看直方图决定两端」这段单独抽出来，
// 好让「有直方图的路径」与「只有 float4 的路径」共用同一份判据（以前是两份，暗场判据已经分叉过一次）。
// 返回 0 = 这一帧不可用（空直方图 / 白点与分位都测不出）。
static inline int s2a_hdr_auto_l1b_targets(const uint32_t hist[S2A_HDR_HIST_BINS], float white_override,
                                           float *pwhite, float *pavg, float *plo, float *phi) {
    if (!hist) return 0;
    int ok = 0;
    float avg = s2a_hdr_hist_avg(hist, &ok);
    if (!ok) return 0;                       // 没有高于真黑阈的像素：不可测
    float white = 0.0f;
    float w = s2a_hdr_sdr_white(hist, &white) ? white : 0.0f;
    if (white_override > 0.0f) w = white_override;   // HUD 剔除后的稳健白点

    float lo, hi;
    float lit = s2a_hdr_lit_frac(hist);
    // 暗场判据放在白点之前：星空这类「99.9% 纯黑 + 几颗星」的画面里，白点常常**测不出来**
    //（顶部 0.5% 的边界落在纯黑格里），此时若先看白点就会掉进「白点未知」分支，
    // 用 99.5 分位乘 0.8 得到 0.39 —— 整幅星空被冻住，这正是旧 GPU 代理的老毛病。
    if (lit < S2A_HDR_DARK_FRAC) {
        // 黑底画面（星空/纯黑过场）：白点是「唯一剩下的亮点」，不能当纸白冻住 —— 从近黑开始扩张
        lo = S2A_HDR_DARK_FLOOR;
        hi = S2A_HDR_SDR_CLIP;
    } else if (w > 0.0f) {
        lo = w;                              // 纸白：这是「不动」的那条线
        hi = S2A_HDR_SDR_CLIP;               // 1.0
    } else {
        float lo_unused;
        if (!s2a_hdr_auto_knee(hist, &hi, &lo_unused)) return 0;
        lo = hi * S2A_HDR_KNEE_RATIO;
    }
    if (!(lo >= 0.002f)) lo = 0.002f;         // 兜 NaN / 0
    if (lo > 0.95f) lo = 0.95f;               // 白点贴顶时留出可扩张区间
    if (!(hi > lo + 0.02f)) hi = lo + 0.02f;  // 退化保护：区间不许塌
    if (hi > 2.0f) hi = 2.0f;
    *pwhite = w; *pavg = avg; *plo = lo; *phi = hi;
    return 1;
}

static inline int s2a_hdr_auto_l1b_step_white_hz(const uint32_t hist[S2A_HDR_HIST_BINS], float white_override,
                                        float prev_lo, float prev_hi, float prev_avg, int have_prev,
                                        float hz, s2a_hdr_auto_l1b *out) {
    if (!hist || !out) return 0;
    // 三个上一帧量都要挡非有限值；lo > hi 这种反向状态也只能丢弃（否则插值会穿过目标）。
    if (have_prev && !(isfinite(prev_lo) && isfinite(prev_hi) && isfinite(prev_avg) && prev_lo <= prev_hi))
        have_prev = 0;
    float w = 0.0f, avg = 0.0f, lo = 0.0f, hi = 0.0f;
    if (!s2a_hdr_auto_l1b_targets(hist, white_override, &w, &avg, &lo, &hi)) return 0;
    out->white = w; out->avg = avg;
    out->first = !have_prev;
    out->scene = have_prev ? s2a_hdr_scene_weight(prev_avg, avg) : 0.0f;
    out->knee_lo = out->first ? lo : s2a_hdr_auto_step_l1b_hz(prev_lo, lo, out->scene, hz);
    out->knee_hi = out->first ? hi : s2a_hdr_auto_step_l1b_hz(prev_hi, hi, out->scene, hz);
    if (out->knee_hi <= out->knee_lo + 1e-4f) out->knee_hi = out->knee_lo + 1e-4f;
    return 1;
}

// 上屏路径用的版本（nvn_hdr_hist.inc 通过 blit 把直方图拷回 CPU 后直接调这个）：
// 与 step_white_hz 唯一的差别是场景判据多一路「白点变化」，见 s2a_hdr_scene_fuse。
// 这样 GPU 侧不再有任何「代理判据」——用的就是 t_hdr_* 测的同一份代码。
static inline int s2a_hdr_auto_l1b_step_fused_hz(const uint32_t hist[S2A_HDR_HIST_BINS], float white_override,
                                        float prev_lo, float prev_hi, float prev_avg, float prev_white,
                                        int have_prev, float hz, s2a_hdr_auto_l1b *out) {
    if (!hist || !out) return 0;
    if (have_prev && !(isfinite(prev_lo) && isfinite(prev_hi) && isfinite(prev_avg) && prev_lo <= prev_hi))
        have_prev = 0;
    float w = 0.0f, avg = 0.0f, lo = 0.0f, hi = 0.0f;
    if (!s2a_hdr_auto_l1b_targets(hist, white_override, &w, &avg, &lo, &hi)) return 0;
    if (have_prev && prev_white > 0.0f && w > 0.0f &&
        fabsf(log2f(w / prev_white)) < S2A_HDR_WHITE_DEADBAND) {
        // 落在死区内：白点保持上一帧。目标端要跟着一起冻结，否则 lo 仍是被量化跳变
        // 拽过去的那个新值，knee_lo 照样来回抖（只有非暗场时纸白才等于 lo）。
        w = prev_white;
        if (lo > S2A_HDR_DARK_FLOOR * 1.0001f) {
            lo = w;
            if (!(lo >= 0.002f)) lo = 0.002f;
            if (lo > 0.95f) lo = 0.95f;
            if (!(hi > lo + 0.02f)) hi = lo + 0.02f;
        }
    }
    out->frozen = 0;
    if (have_prev) {
        const int dim = avg < S2A_HDR_DIM_APL;
        const int dim_prev = prev_avg < S2A_HDR_DIM_APL;
        const float drop = prev_avg - avg;
        // 每秒落差（stop/s）：判据的唯一单位，见 S2A_HDR_FADE_MIN_S 的注释
        const float hz_ok = (hz >= 1.0f && hz <= 480.0f) ? hz : S2A_HDR_SMOOTH_HZ;
        const float fade_rate = fabsf(drop) * hz_ok;

        // 淡出/淡入：中速升降 + 白点同向变（整幅等比变暗/变亮）。
        // 另外「白点测不出来（快黑透）且还在变暗」也冻结：这时没有可信纸白，
        // 猜一个暗场落地值只会让回到画面时先闪一下（实测 205 帧落到 0.02、回来爬 30 帧）。
        // 只按「每秒多少 stop」判，不按白点同向性判：冻结期间 prev_white 是陈旧的，
        // 拿它和新测的白点比，淡入的第一帧必然看起来像「白点在反方向猛跳」而漏判。
        const int uniform_fade = drop > 0.0f && fade_rate > S2A_HDR_FADE_MIN_S && fade_rate < S2A_HDR_FADE_MAX_S;
        // 淡入（升）同样冻结：升亮过程中测出来的白点只是"当前有多亮"，不是这段内容的纸白；
        // 跟着它走会把 knee 先拖低再抬回（270 帧序列实测 0.6579 → 0.0306 → 爬回）。
        // 升亮同样冻结到「动作为止」：淡入结束时第一帧静止，之后的平滑会把 knee 收到新纸白
        // （单调、不闪）；跟着变亮过程追白点会把 knee 先拖到 0.02 再爬回来，那正是要避免的。
        // 变亮同样冻结（从黑淡入时追白点会把 knee 先拖到 0.02 再爬回，见 SEQUENCE.md）；
        // 代价是交叉淡入结束时那一帧会按切镜判据一步到位（300 帧序列实测 0.1178 –
        // 与内容真正切换同帧，属于设计内行为，不是抖动）。
        const int uniform_rise = drop < 0.0f && fade_rate > S2A_HDR_FADE_MIN_S && fade_rate < S2A_HDR_FADE_MAX_S;
        const int goes_black = (w <= 0.0f) && drop > 0.0f;
        // 已经进入「黑底 + 少亮像素」区（淡到深处 / 过场全黑 / 从黑淡入）时，只要平均亮度还在动
        // 就整帧冻结：这段没有可信纸白，任何重定目标都会在回到画面时被看见（先亮后收）。
        // 稳态黑底（星空：不动）不冻结，仍然按暗场落地 0.02 扩张——那是刻意的行为，有单测守着。
        const int deep_dark = avg < S2A_HDR_DIM_APL && s2a_hdr_lit_frac(hist) < S2A_HDR_DARK_FRAC;
        const int dark_passage = deep_dark && fade_rate > S2A_HDR_FADE_MIN_S;
        // 已经黑到测不出纸白、但上一帧的 knee 明显不是暗场落地值 ⇒ 这是「淡到黑/过场黑」，
        // 保持着色前的 knee 不动。稳态星空从 0 起、prev_lo 一直是 0.02，不受这条影响。
        // 这条覆盖整个「暗区进出」过程（含淡到黑后快速回亮的那几帧——那时一帧就能升 2 stop，
        // 任何速度判据都会把它看成切镜）。
        const int dim_any = dim || dim_prev;
        const int dark_hold = dim_any && prev_lo > 5.0f * S2A_HDR_DARK_FLOOR;
        if (uniform_fade || uniform_rise || goes_black || dark_passage || dark_hold) {
            // 淡出中：冻结（不要跟着掉下去把中调抬亮）
            out->white = prev_white; out->avg = avg; out->first = 0;
            out->scene = 0.0f; out->knee_lo = prev_lo; out->knee_hi = prev_hi;
            out->frozen = 1;
            return 1;
        }
        if (!dim && dim_prev) {
            // 从暗区回到有内容的画面：这就是一次切镜，别让它爬
            out->white = w; out->avg = avg; out->first = 0;
            out->scene = 1.0f;
            out->knee_lo = lo; out->knee_hi = hi;
            if (out->knee_hi <= out->knee_lo + 1e-4f) out->knee_hi = out->knee_lo + 1e-4f;
            return 1;
        }
    }
    out->white = w; out->avg = avg;
    out->first = !have_prev;
    out->scene = have_prev ? s2a_hdr_scene_fuse(s2a_hdr_scene_weight(prev_avg, avg),
                                                s2a_hdr_scene_weight_white(prev_white, w))
                           : 0.0f;
    out->knee_lo = out->first ? lo : s2a_hdr_auto_step_l1b_hz(prev_lo, lo, out->scene, hz);
    out->knee_hi = out->first ? hi : s2a_hdr_auto_step_l1b_hz(prev_hi, hi, out->scene, hz);
    if (out->knee_hi <= out->knee_lo + 1e-4f) out->knee_hi = out->knee_lo + 1e-4f;
    return 1;
}

static inline int s2a_hdr_auto_l1b_step_fused(const uint32_t hist[S2A_HDR_HIST_BINS], float white_override,
                                        float prev_lo, float prev_hi, float prev_avg, float prev_white,
                                        int have_prev, s2a_hdr_auto_l1b *out) {
    return s2a_hdr_auto_l1b_step_fused_hz(hist, white_override, prev_lo, prev_hi, prev_avg, prev_white,
                                          have_prev, S2A_HDR_SMOOTH_HZ, out);
}

// GPU 侧只有 float4 回读（hi, white, avg, scene），拿不到直方图：
// 用 white 推出 L1b 的两个目标端（lo = 纸白，hi = SDR 裁切点），让上屏路径也遵守「纸白不动」。
// 暗场判据用代理：白点低于 2% 白（S2A_HDR_DARK_WHITE）就当黑底画面，lo 落到 DARK_FLOOR。
// 返回 0 = white 不可用，调用方退回 L1 的分位路径。
static inline int s2a_hdr_l1b_targets_from_white(float white, float *lo, float *hi) {
    if (!(white > 0.0f)) return 0;
    float l = (white < S2A_HDR_DARK_WHITE) ? S2A_HDR_DARK_FLOOR : white;
    if (l < 0.002f) l = 0.002f;
    if (l > 0.95f) l = 0.95f;                 // 白点贴顶时留出可扩张区间
    float h = S2A_HDR_SDR_CLIP;
    if (!(h > l + 0.02f)) h = l + 0.02f;      // 退化保护：区间不许塌
    *lo = l;
    *hi = h;
    return 1;
}

// 60 Hz 兼容包装（老调用点 / GPU 侧历史行为不变）
static inline int s2a_hdr_auto_l1b_step_white(const uint32_t hist[S2A_HDR_HIST_BINS], float white_override,
                                        float prev_lo, float prev_hi, float prev_avg, int have_prev,
                                        s2a_hdr_auto_l1b *out) {
    return s2a_hdr_auto_l1b_step_white_hz(hist, white_override, prev_lo, prev_hi, prev_avg, have_prev,
                                          S2A_HDR_SMOOTH_HZ, out);
}

#endif

// 兼容入口：不带白点覆盖，等于自算白点。
static inline int s2a_hdr_auto_l1b_step(const uint32_t hist[S2A_HDR_HIST_BINS],
                                        float prev_lo, float prev_hi, float prev_avg, int have_prev,
                                        s2a_hdr_auto_l1b *out) {
    return s2a_hdr_auto_l1b_step_white(hist, 0.0f, prev_lo, prev_hi, prev_avg, have_prev, out);
}
