// 自动 HDR 的 L0 直方图：log2 域 256 bin → 99.5 分位 → knee。
//   参数与判据与 nvn_hdr_auto.h 完全对齐（那边是 CPU 参考实现，t_hdr_auto 29 项守着）：
//   bin = clamp((log2(y) - LO) / (HI - LO) * 256)，LO=-10（线性 0.001）、HI=+4（线性 16）。
//   三个 kernel 顺序跑：clear → add（每 stride×stride 采一点）→ knee（单线程扫 CDF）。
#include <metal_stdlib>
using namespace metal;

constant int   kHistBins = 256;
constant float kHistLo   = -10.0f;
constant float kHistHi   = 4.0f;

// 1) 清零
kernel void hdr_hist_clear(device atomic_uint *hist [[buffer(0)]],
                           uint tid [[thread_position_in_grid]]) {
    if (tid < (uint)kHistBins) atomic_store_explicit(&hist[tid], 0u, memory_order_relaxed);
}

// 2) 累加：Rec.709 亮度，log2 量化；stride 由 CPU 按分辨率给（1080p 用 4，4K 用 8）
// sRGB 编码 → 线性（与 present shader 的 s2a_lin 同一组常数）
static inline float s2a_lin_h(float c) { return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f); }

// linearize=1 时先把编码值转线性（8 位游戏源）；16F 源本身已是线性，传 0
kernel void hdr_hist_add(texture2d<float, access::read> src [[texture(0)]],
                         device atomic_uint *hist [[buffer(0)]],
                         constant uint &stride [[buffer(1)]],
                         constant uint &linearize [[buffer(2)]],
                         uint2 gid [[thread_position_in_grid]]) {
    uint w = src.get_width(), h = src.get_height();
    if (gid.x >= w || gid.y >= h) return;
    if ((gid.x % stride) != 0u || (gid.y % stride) != 0u) return;
    float3 c = src.read(gid).rgb;
    if (linearize) c = float3(s2a_lin_h(c.r), s2a_lin_h(c.g), s2a_lin_h(c.b));
    float y = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    // 黑 / 负数 / NaN 一律记进 0 号格，而不是丢掉。
    //   口径必须与 CPU 的 s2a_hdr_hist_bin() 一致（那边同样返回 0）：
    //   丢掉的话 GPU 侧 lit_frac 的分母不含黑像素，暗场判据（lit < 5%）永远不会成立，
    //   于是「星空 / 夜景」这类画面在设备上走的是亮场分支，而全部单测走的是暗场分支。
    //   这一条是 t_hdr_bin_mirror 逐样本对账抓出来的。
    if (!(y > 0.0f)) { atomic_fetch_add_explicit(&hist[0], 1u, memory_order_relaxed); return; }
    float l = clamp((log2(y) - kHistLo) / (kHistHi - kHistLo), 0.0f, 0.9999999f);
    uint bin = (uint)(l * (float)kHistBins);
    atomic_fetch_add_explicit(&hist[bin], 1u, memory_order_relaxed);
}

// 3) CDF → knee。输出 float4(lo, hi-lo, head, 0)，直接当 present 的 fragment buffer 用
kernel void hdr_hist_knee(device atomic_uint *hist [[buffer(0)]],
                          device float4 *out [[buffer(1)]],
                          constant float &head [[buffer(2)]],
                          uint tid [[thread_position_in_grid]]) {
    if (tid != 0u) return;
    ulong total = 0ul;
    for (int i = 0; i < kHistBins; i++) total += (ulong)atomic_load_explicit(&hist[i], memory_order_relaxed);
    if (total == 0ul) { out[0] = float4(0.55f, 0.45f, head, 0.0f); return; }   // 空帧：保持缺省 0.55 / 2.0
    // 分位阈值用整数千分，与 CPU 的 s2a_hdr_auto_knee 逐位一致。
    //   原来写 (uint)((float)total * 0.995f)：1..8,300,000 个像素总数里 1,515,878 个会少 1，
    //   同一帧在 GPU/CPU 上可能落相邻 bin（hi 差 0.0547 stop）。见 t_hdr_knee_mirror。
    ulong target = (total * 995ul) / 1000ul;
    if (target < 1ul) target = 1ul;
    ulong acc = 0ul, hit = (ulong)(kHistBins - 1);
    for (int i = 0; i < kHistBins; i++) {
        acc += (ulong)atomic_load_explicit(&hist[i], memory_order_relaxed);
        if (acc >= target) { hit = (ulong)i; break; }
    }
    // bin 的高边 → 线性亮度；knee_hi 让 99.5 分位落在扩张后约 0.8 的位置
    float edge = exp2(kHistLo + (kHistHi - kHistLo) * ((float)hit + 1.0f) / (float)kHistBins);
    float hi = clamp(edge / 0.8f, 0.25f, 1.0f);
    out[0] = float4(0.55f * hi, hi - 0.55f * hi, head, 0.0f);
}

// ---------------------------------------------------------------------------
// 4) L1：白点 + 黑场 + 场景亮度，全在 GPU 上算完，只回读 float4。
//    与 CPU 参考实现 nvn_hdr_auto_l1.h 逐条对齐（t_hdr_auto_l1 / t_hdr_pipeline 守着）：
//      avg  = Σ(bin_center·count)/Σcount，bin_center 取格中心（不是上沿）
//      white= 从亮往暗扫，第一个 count ≥ 0.5%·total 且不在黑场以下的格子的**上沿**
//      hi   = clamp(white/0.8, 0.01, 2.0)   ← L1 的两个界都比 L0 宽/低，别用 0.25/1.0
//      scene= smoothstep(0.40, 1.10, |avg - prev_avg|)   ← 真值在下面常量区；写 0.5/1.6 是过期注释（t_hdr_gpu_l1 对账时发现）
//    out[0] = float4(hi, white, avg, valid)
// ---------------------------------------------------------------------------
constant float kBlackCut   = -6.0f;
constant float kAvgCut     = -9.5f;   // 重心只排真黑：星点常在 0.002..0.01，用 -6 会把它们整片丢掉
constant float kWhiteFrac  = 0.005f;
constant float kKneeTarget = 0.8f;
constant float kL1HiMin    = 0.01f;
constant float kL1HiMax    = 2.0f;
constant float kSceneLow   = 0.40f;   /* 与 S2A_HDR_SCENE_LOW 一致：0.66 stop 的中等切镜要拿 31% 权重 */
constant float kSceneHigh  = 1.10f;

static inline float hist_bin_center(int i) { return kHistLo + ((float)i + 0.5f) * ((kHistHi - kHistLo) / (float)kHistBins); }
static inline float hist_bin_low(int i)    { return kHistLo + (float)i * ((kHistHi - kHistLo) / (float)kHistBins); }
static inline float hist_bin_edge(int i)   { return kHistLo + ((float)i + 1.0f) * ((kHistHi - kHistLo) / (float)kHistBins); }

static inline float scene_weight(float prev_avg, float cur_avg) {
    float d = fabs(cur_avg - prev_avg);
    if (!(d > kSceneLow)) return 0.0f;
    if (d >= kSceneHigh) return 1.0f;
    float t = (d - kSceneLow) / (kSceneHigh - kSceneLow);
    return t * t * (3.0f - 2.0f * t);
}

kernel void hdr_hist_l1(device atomic_uint *hist [[buffer(0)]],
                        device float4 *out [[buffer(1)]],
                        constant float &prev_avg [[buffer(2)]],
                        constant uint &have_prev [[buffer(3)]],
                        uint tid [[thread_position_in_grid]]) {
    if (tid != 0u) return;
    // MSL 没有 double（CI 的 xcrun metal 闸门第一次跑就红在这）：全程 float。
    // 计数上界：1080p 用 stride=4 ⇒ 约 5.2e5 像素，float 尾数 24 位（精确到 1.6e7）够用。
    uint total = 0u;                 // 黑场以下的格不计（与 CPU 的 s2a_hdr_sdr_white 同口径）
    float acc = 0.0f;
    float all = 0.0f;
    for (int i = 0; i < kHistBins; i++) {
        uint c = atomic_load_explicit(&hist[i], memory_order_relaxed);
        if (c == 0u) continue;
        all += (float)c;
        if (hist_bin_low(i) < kAvgCut) continue;    // 注意：这里用 kAvgCut（与 CPU 的 s2a_hdr_hist_avg 同口径）
        total += c;
        acc += hist_bin_center(i) * (float)c;
    }
    if (all <= 0.0f || total == 0u) { out[0] = float4(0.0f, 0.0f, 0.0f, 0.0f); return; }   // 空帧 / 全黑
    float avg = acc / (float)total;

    uint need = (uint)((float)total * kWhiteFrac);
    if (need < 1u) need = 1u;
    float white = 0.0f;
    bool found = false;
    for (int i = kHistBins - 1; i >= 0; i--) {
        if (hist_bin_low(i) < kBlackCut) break;
        if (atomic_load_explicit(&hist[i], memory_order_relaxed) >= need) { white = exp2(hist_bin_edge(i)); found = true; break; }
    }
    float hi;
    if (found) {
        hi = white / kKneeTarget;
        // 白点找不到时退回 L0 的 99.5 分位算法（与 CPU 同分支），此处不算，交给调用方沿用上一帧
        if (!(hi >= kL1HiMin)) hi = kL1HiMin;
        if (hi > kL1HiMax) hi = kL1HiMax;
    } else {
        // 没有漫反射白：用 99.5 分位 + L0 的 [0.25, 1.0] 界
        //    阈值口径必须与 hdr_hist_knee / CPU 的 s2a_hdr_auto_knee 一致：整数千分。
        //    写 (uint)((float)total * 0.995f) 时，1..8,300,000 个像素总数里有 1,515,878 个会少 1
        //    （t_hdr_knee_mirror 抓的同一类分歧），这条 fallback 会落到相邻 bin。
        const ulong target = ((ulong)total * 995ul) / 1000ul;
        uint a2 = 0u; int hit = kHistBins - 1;
        for (int i = 0; i < kHistBins; i++) { a2 += atomic_load_explicit(&hist[i], memory_order_relaxed); if (a2 >= target) { hit = i; break; } }
        hi = clamp(exp2(hist_bin_edge(hit)) / kKneeTarget, 0.25f, 1.0f);
    }
    float sc = have_prev ? scene_weight(prev_avg, avg) : 0.0f;
    out[0] = float4(hi, white, avg, sc);
}
