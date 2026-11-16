// 自动 HDR 的 GPU 侧：每 N 帧扫一遍画面亮度分布，算 99.5 分位 → 改 g_hdr_knee。
//   算法与 nvn_hdr_auto.h（CPU 参考，t_hdr_auto 29 项守着）同一组常数；
//   kernel 在 graphics/nvn/shaders/hdr_hist.metal，Makefile 嵌成 k_hdr_hist。
// 开关 S2A_HDR_AUTO=1（缺省 0）：关着时 present 用 s2a_hdr_itm_params() 的固定 knee，与之前逐字节一致。
#include "nvn_hdr_hist_msl.h"
#include "nvn_hdr_auto.h"      // L0 常数（HIST_BINS / KNEE_RATIO / HIST_EVERY）
#include "nvn_hdr_auto_l1.h"
#include <time.h>   // clock_gettime：帧率估计（Darwin 上同样是 CLOCK_MONOTONIC）   // L1：白点 / 黑场 / 场景平滑（s2a_hdr_auto_step_l1）

@interface NvHdrHist : NSObject {
@public
    id<MTLBuffer> hist;                          // 256 × uint32，GPU 私有
    id<MTLBuffer> out;                           // float4(hi, white, avg, scene)，shared，CPU 可读
    id<MTLBuffer> histcpu[2];                    // 256 × uint32 的 CPU 副本（blit 拷回），shared；双槽防跨帧竞争
    unsigned      hs;                            // 双槽轮转
    id<MTLComputePipelineState> ps_clear, ps_add, ps_l1;
    NSUInteger w, h;
    float prev_avg;                              // 上一帧的 log2 重心（场景判据用）
    float prev_white;                            // 上一帧的白点（第二个场景判据：重心不动也认）
    int have_prev;
    double last_t;                               // 上一帧的时间戳（秒）：帧率估计用
    float hz;                                    // 估计出的帧率（平滑过），喂给 coef
    uint64_t frame;                              // 已跳过多少帧
    uint64_t logf;                               // 诊断行节拍（S2A_HDR_LOG）
    unsigned gen;                                // 每次重建 +1：回调里用它判断「这份数据是否还属于当前对象」
}
@end
@implementation NvHdrHist @end

static NvHdrHist *g_hh;
static unsigned g_hh_gen;   // 单调递增：重建对象时换一代，旧命令缓冲的回调据此作废

static int hdr_log_setting(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("S2A_HDR_LOG"); v = (e != NULL && *e != '\0' && *e != '0'); }
    return v;
}

static int hdr_auto_setting(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("S2A_HDR_AUTO"); v = !(e == NULL || *e == '0'); }
    return v;
}

static id<MTLComputePipelineState> hh_pipeline(id<MTLLibrary> lib, NSString *name) {
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) { MLOG("【红】自动 HDR：shader 里没有 %s", name.UTF8String); return nil; }
    NSError *e = nil;
    id<MTLComputePipelineState> ps = [g_dev newComputePipelineStateWithFunction:fn error:&e];
    if (!ps) MLOG("【红】自动 HDR：%s 建不了管线 %s", name.UTF8String, e.localizedDescription.UTF8String);
    return ps;
}

static NvHdrHist *hdr_hist_prepare(NSUInteger w, NSUInteger h) {
    if (g_hh && g_hh->w == w && g_hh->h == h && g_hh->ps_l1) return g_hh;
    if (g_hh) {
        if (g_hh->hist) res_remove(g_hh->hist);
        if (g_hh->out) res_remove(g_hh->out);
        if (g_hh->histcpu[0]) res_remove(g_hh->histcpu[0]);
        if (g_hh->histcpu[1]) res_remove(g_hh->histcpu[1]);
        g_hh = nil;
    }
    if (!w || !h) return nil;
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_hdr_hist) options:nil error:&e];
    if (!lib) { MLOG("【红】自动 HDR：shader 编译失败 %s", e.localizedDescription.UTF8String); return nil; }
    NvHdrHist *f = [NvHdrHist new];
    f->ps_clear = hh_pipeline(lib, @"hdr_hist_clear");
    f->ps_add = hh_pipeline(lib, @"hdr_hist_add");
    f->ps_l1 = hh_pipeline(lib, @"hdr_hist_l1");
    if (!f->ps_clear || !f->ps_add || !f->ps_l1) return nil;
    f->hist = [g_dev newBufferWithLength:256 * sizeof(uint32_t) options:MTLResourceStorageModePrivate];
    f->out = [g_dev newBufferWithLength:sizeof(float) * 4 options:MTLResourceStorageModeShared];
    // 直方图本体留在 GPU 私有内存（原子加最快），每 S2A_HDR_HIST_EVERY 帧用 blit 拷 1 KB 回 CPU：
    //   有了真直方图，上屏判据就和 t_hdr_* 测的是同一份代码，不再需要「白点 < 2%」这种代理。
    for (int i = 0; i < 2; i++)
        f->histcpu[i] = [g_dev newBufferWithLength:256 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    if (!f->hist || !f->out || !f->histcpu[0] || !f->histcpu[1]) { MLOG("【红】自动 HDR：建不了 buffer"); return nil; }
    res_add(f->hist); res_add(f->out); res_add(f->histcpu[0]); res_add(f->histcpu[1]);
    f->w = w; f->h = h;
    f->gen = ++g_hh_gen;
    g_hh = f;
    MLOG("自动 HDR：直方图就绪 %lux%lu（每 %d 帧一次）", (unsigned long)w, (unsigned long)h, S2A_HDR_HIST_EVERY);
    return f;
}

// 每 S2A_HDR_HIST_EVERY 帧跑一次：clear → add（4×4 采样）→ hdr_hist_l1，结果回读 16 B 进 g_hdr_knee
static void hdr_hist_tick(id<MTLCommandBuffer> cb, id<MTLTexture> src) {
    if (!hdr_auto_setting() || !src) return;
    NvHdrHist *f = hdr_hist_prepare(src.width, src.height);
    if (!f) return;
    if ((f->frame++ % S2A_HDR_HIST_EVERY) != 0) return;
    @autoreleasepool {
        id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
        if (!en) return;
        // 双槽：本帧 blit 进 slot，下一个 tick（S2A_HDR_HIST_EVERY 帧后）进另一个槽，
        //   CPU 读完前不会被覆盖；落后超过两个 tick 时只会读到较新的数据，不会读到半写状态。
        unsigned slot = (f->hs++) & 1u;
        id<MTLBuffer> hb = f->histcpu[slot];
        if (!hb) return;
        uint32_t stride = (src.width >= 2560 || src.height >= 1440) ? 8u : 4u;
        uint32_t linearize = (src.pixelFormat == MTLPixelFormatRGBA16Float) ? 0u : 1u;
        [en setComputePipelineState:f->ps_clear];
        [en setBuffer:f->hist offset:0 atIndex:0];
        [en dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [en setComputePipelineState:f->ps_add];
        [en setTexture:src atIndex:0];   // read() 不解码 sRGB，直接读存储原字节，不用转 view
        [en setBuffer:f->hist offset:0 atIndex:0];
        [en setBytes:&stride length:sizeof stride atIndex:1];
        [en setBytes:&linearize length:sizeof linearize atIndex:2];
        [en dispatchThreads:MTLSizeMake(src.width, src.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        // hdr_hist_l1 不 dispatch。它算出来的 f->out 没有任何读者——knee 全部在 CPU 侧按下面
        // 回读的真直方图算（s2a_hdr_auto_l1b_step_fused_hz），GPU 只负责数格子。
        // 曾经它每 S2A_HDR_HIST_EVERY（8）帧被 dispatch 一次、写进一个没人看的 buffer：白算。
        // 管线 ps_l1 仍然建着，因为 src/t_hdr_gpu_l1.c 拿它当参考实现对账；将来真要在 GPU 上
        // 算 knee，把这段 dispatch 连同读取 f->out 一起加回来——那时对账里「GPU 白点比 CPU bulk
        // 高 1 stop 以上」那条断言会先红，提醒 HUD 簇剔除也得补上。
        [en endEncoding];
        // 把 1 KB 直方图拷回 CPU 可见缓冲：判定全部在 CPU 侧按「真直方图」算，
        // GPU 只负责数格子。blit 在 30 帧里发生一次，开销可忽略。
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        if (be) {
            [be copyFromBuffer:f->hist sourceOffset:0
                      toBuffer:hb destinationOffset:0
                          size:256 * sizeof(uint32_t)];
            [be endEncoding];
        }
        NvHdrHist *owner = f; unsigned gen = f->gen;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            // 以「对象 + 代号」双重判定：分辨率改变会释放并重建 buffer，此时旧命令缓冲的回调
            //   既不该读新对象的 prev_*，也不该把旧直方图的结论写进 g_hdr_knee。
            if (b.error || !g_hh || g_hh != owner || owner->gen != gen || !owner->out) return;
            float v[4]; memcpy(v, g_hh->out.contents, sizeof v);   // (hi, white, avg, scene)：GPU 版本的结果，留作对账
            if (!(v[0] > 0.0f)) return;                            // 空帧 / 全黑：保持上一帧
            NvHdrHist *h = g_hh;
            const uint32_t *hist = (const uint32_t *)hb.contents;
            float cur_lo = g_hdr_knee[0];
            float cur_hi = g_hdr_knee[0] + g_hdr_knee[1];
            s2a_hdr_auto_l1b b;
            // 真直方图路径：与 t_hdr_* 完全同一份判定（纸白、暗场、HUD 视角都能精确判）
            // 有直方图就不必再靠代理：HUD 孤立高光剔除（sdr_white_bulk）也能在 CPU 侧先做，
            // 于是上屏路径与 t_hdr_* 走的是同一份「纸白 / 暗场 / HUD」判定。
            float wov = 0.0f, wbulk = 0.0f;
            if (s2a_hdr_sdr_white_bulk(hist, &wbulk)) wov = wbulk;
            // 帧率：从两次回调的时间差估（拿不到层刷新率）。掉帧/切档时 coef 跟着变，
            // 否则 30 Hz 下每秒只走一半、120 Hz 下四倍，观感就是「切档后过渡速度变了」。
            struct timespec ts;
            double now = 0.0;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
            if (now > 0.0) {
                h->hz = s2a_hdr_hz_from_dt(now - h->last_t, h->hz);
                h->last_t = now;
            }
            float hz = (h->hz > 0.0f) ? h->hz : S2A_HDR_SMOOTH_HZ;
            if (s2a_hdr_auto_l1b_step_fused_hz(hist, wov, cur_lo, cur_hi, h->prev_avg, h->prev_white,
                                               h->have_prev, hz, &b)) {
                // 先发原子槽（渲染线程只认它），再更新 CPU 侧记账，两处值一致
                s2a_hdr_knee_publish(&g_hdr_knee_pub, b.knee_lo, b.knee_hi - b.knee_lo);
                g_hdr_knee[0] = b.knee_lo;
                g_hdr_knee[1] = b.knee_hi - b.knee_lo;
                g_hdr_knee[3] = 1.0f;
                // 一次对账日志：GPU 的 float4 与 CPU 直方图算出的白点/重心应一致（差 >0.02 就是两边分叉了）
                if (!h->have_prev ||
                    fabsf(b.white - v[1]) > 0.02f || fabsf(b.avg - v[2]) > 0.05f) {
                    MLOG("自动 HDR：GPU/CPU 对账 white %.4f/%.4f avg %.4f/%.4f（差大就是 kernel 与头文件分叉）",
                         (double)v[1], (double)b.white, (double)v[2], (double)b.avg);
                }
                if (hdr_log_setting() && (h->logf++ % S2A_HDR_LOG_EVERY) == 0) {
                    s2a_hdr_stat st;
                    memset(&st, 0, sizeof st);
                    st.fmt16 = 1;              // 能跑到这里说明 HDR 链路在跑（画布是 16F 且开了 S2A_HDR_AUTO）
                    st.itm = 1;
                    st.frozen = b.frozen;
                    st.head = g_hdr_knee[2];
                    st.knee_lo = b.knee_lo; st.knee_hi = b.knee_hi;
                    st.white = b.white; st.avg = b.avg; st.scene = b.scene;
                    st.map = s2a_hdr_map();   // 0 = 拐点（缺省），1 = BT.2408 内容映射（此时 knee 只是日志）
                    char line[192];
                    s2a_hdr_status_line(line, sizeof line, &st);
                    MLOG("%s", line);
                }
                h->prev_avg = b.avg; h->prev_white = b.white; h->have_prev = 1;
                return;
            }
            // 直方图读不出来（blit 没跑 / 缓冲被回收）：退回旧的 float4 代理路径
            float scene = h->have_prev
                        ? s2a_hdr_scene_fuse(v[3], s2a_hdr_scene_weight_white(h->prev_white, v[1]))
                        : 0.0f;
            float tgt_lo = 0.0f, tgt_hi = 0.0f;
            if (s2a_hdr_l1b_targets_from_white(v[1], &tgt_lo, &tgt_hi)) {
                float lo = (g_hdr_knee[3] != 0.0f) ? s2a_hdr_auto_step_l1b_hz(cur_lo, tgt_lo, scene, hz) : tgt_lo;
                float hi = (g_hdr_knee[3] != 0.0f) ? s2a_hdr_auto_step_l1b_hz(cur_hi, tgt_hi, scene, hz) : tgt_hi;
                if (!(hi > lo + 1e-4f)) hi = lo + 1e-4f;
                if (fabsf(lo - cur_lo) > 1e-6f || fabsf(hi - cur_hi) > 1e-6f || g_hdr_knee[3] == 0.0f) {
                    s2a_hdr_knee_publish(&g_hdr_knee_pub, lo, hi - lo);
                    g_hdr_knee[0] = lo;
                    g_hdr_knee[1] = hi - lo;
                    g_hdr_knee[3] = 1.0f;
                }
            } else {
                float nxt_hi = (g_hdr_knee[3] != 0.0f) ? s2a_hdr_auto_step_l1(cur_hi, v[0], scene) : v[0];
                if (fabsf(nxt_hi - cur_hi) > 1e-6f || g_hdr_knee[3] == 0.0f) {
                    float lo = S2A_HDR_KNEE_RATIO * nxt_hi;
                    s2a_hdr_knee_publish(&g_hdr_knee_pub, lo, nxt_hi - lo);
                    g_hdr_knee[0] = lo;
                    g_hdr_knee[1] = nxt_hi - lo;
                    g_hdr_knee[3] = 1.0f;
                }
            }
            h->prev_avg = v[2]; h->prev_white = v[1]; h->have_prev = 1;
        }];
    }
}
