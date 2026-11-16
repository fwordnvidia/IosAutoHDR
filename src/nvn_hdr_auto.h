// HDR 出屏的自适应参数（L0）：从一帧的亮度分布决定 knee 往哪挪，而不是所有场景共用 0.55:1.0。
//
// 为什么需要：固定 knee 在暗场景下等于不扩张——SDR 画面最亮也就到 1.0 附近，knee_hi=1.0 意味着
//   只有纯白吃满 headroom，整体偏暗的一局几乎没有 HDR 观感。让 99.5 分位落在「可用上沿 0.8」处，
//   暗场景的 knee 自动左移，扩张才真的发生在画面里。
//
// 分工：这个头文件只做**参数**（纯 C，Linux 单测）；直方图统计与采样在 MSL 里跑，全留 GPU 上，
//   不回读。present fragment shader 从那三个常量改成读 constant buffer，曲线形式不变。
//
// 时间稳定性：单帧最优会闪（RTX HDR 论文把亮度跳变列为显式设计目标）。两件套压住——
//   一阶平滑（60fps 约 200ms 常数）+ 死区（小于 2% 的变化直接不动）。
#ifndef S2A_NVN_HDR_AUTO_H
#define S2A_NVN_HDR_AUTO_H
#include <math.h>
#include <stdint.h>

// 直方图格数：256 格覆盖 14 个 stop（每格 0.0547 stop），1 KB，一次 copyFromBuffer 全取回
#define S2A_HDR_HIST_BINS 256
#define S2A_HDR_HIST_LO (-10.0f)   // log2 线性亮度下界（0.001）
#define S2A_HDR_HIST_HI (4.0f)     // log2 线性亮度上界（16）
#define S2A_HDR_KNEE_RATIO 0.55f   // knee_lo : knee_hi，与原来写死的 0.55:1.0 同一比例
// 拐点下界的合法区间：低于 0.25 连 SDR 中调都会被抬（UI 发灰），高于 1.0 等于不扩张
#define S2A_HDR_KNEE_MIN 0.25f
#define S2A_HDR_KNEE_MAX 1.0f
#define S2A_HDR_QUANTILE 0.995f    // 让最亮 0.5% 去吃满 headroom（文档用；实际按下面的千分整数算）
#define S2A_HDR_QUANTILE_PERMILLE 995u  // 分位阈值的唯一真相源：整数千分，GPU/CPU 都用它
/* 为什么不用 (double)total * 0.995 也不 (float)total * 0.995f：
   1..8,300,000 个像素总数里有 1,515,878 个会算出不同的阈值（float 版少 1），
   于是同一帧在 GPU 与 CPU 上可能落在相邻 bin，hi 差一格 = 0.0547 stop。
   整数千分在两边都精确，见 t_hdr_knee_mirror。 */
#define S2A_HDR_KNEE_TARGET 0.8f   // 99.5 分位落在「可用上沿」的哪里
#define S2A_HDR_SMOOTH 0.1f        // 一阶平滑系数
// 死区（stop）：小于 2% 的变化直接不动，防止内容微动导致每帧方向翻转
#define S2A_HDR_DEADBAND 0.02f
/* GPU 直方图的采样间隔（帧）。30 帧一次要从 1.0 走到 0.25 要 17.5 秒，太慢；
   8 帧一次是 4.7 秒，且 kernel 只占 0.1–0.3ms，摊到每帧 0.02–0.04ms。 */
#define S2A_HDR_HIST_EVERY 8

// 线性亮度 -> 直方图 bin。0 / 负数 / NaN 一律进最暗一格（不该出现，但别让它越界）。
static inline int s2a_hdr_hist_bin(float y) {
    if (!(y > 0.0f)) return 0;                              // 黑 / 负数 / NaN / -inf
    // +inf 单独处理：原来直接走下面的 (int)f，而 (int)inf 是未定义行为（x86 上得 INT_MIN，
    // 于是 inf 被夹成 0 号格 = 当黑）。MSL 那边 clamp 到 0.9999999 得 255 号格，
    // 两边不一致，且 CPU 侧是 UB。现在统一按「最亮」处理，见 t_hdr_bin_mirror 场景四。
    if (!isfinite(y)) return S2A_HDR_HIST_BINS - 1;
    float f = (log2f(y) - S2A_HDR_HIST_LO) * ((float)S2A_HDR_HIST_BINS / (S2A_HDR_HIST_HI - S2A_HDR_HIST_LO));
    int b = (int)f;
    if (b < 0) b = 0;
    if (b >= S2A_HDR_HIST_BINS) b = S2A_HDR_HIST_BINS - 1;
    return b;
}

// bin 的上界（取「累计到这里就够」的那一格的上沿，宁可多算一点亮度，别把高光算丢）
static inline float s2a_hdr_hist_edge(int bin) {
    if (bin < 0) bin = 0;
    if (bin >= S2A_HDR_HIST_BINS) bin = S2A_HDR_HIST_BINS - 1;
    return powf(2.0f, S2A_HDR_HIST_LO + (float)(bin + 1) * ((S2A_HDR_HIST_HI - S2A_HDR_HIST_LO) / (float)S2A_HDR_HIST_BINS));
}

// 从直方图算目标 knee。返回 0 = 直方图全空（调用方保持上一帧），1 = 算出来了。
static inline int s2a_hdr_auto_knee(const uint32_t hist[S2A_HDR_HIST_BINS], float *hi, float *lo) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) total += hist[i];
    if (!total) return 0;
    uint64_t want = (total * S2A_HDR_QUANTILE_PERMILLE) / 1000u;
    if (want < 1) want = 1;
    uint64_t acc = 0;
    int bin = S2A_HDR_HIST_BINS - 1;
    for (int i = 0; i < S2A_HDR_HIST_BINS; i++) {
        acc += hist[i];
        if (acc >= want) { bin = i; break; }
    }
    float y = s2a_hdr_hist_edge(bin);
    float h = y / S2A_HDR_KNEE_TARGET;
    if (!(h >= S2A_HDR_KNEE_MIN)) h = S2A_HDR_KNEE_MIN;   // 也兜住 NaN
    if (h > S2A_HDR_KNEE_MAX) h = S2A_HDR_KNEE_MAX;
    *hi = h;
    *lo = S2A_HDR_KNEE_RATIO * h;
    return 1;
}

// 一阶平滑 + 死区。prev 是上一帧真正用的 knee_hi。
static inline float s2a_hdr_auto_step(float prev_hi, float target_hi) {
    float d = target_hi - prev_hi;
    if (d < 0) d = -d;
    if (d < S2A_HDR_DEADBAND) return prev_hi;
    return prev_hi + (target_hi - prev_hi) * S2A_HDR_SMOOTH;
}

#endif
