// 自动 HDR 的 GPU 侧：每 N 帧扫一遍画面亮度分布，blit 1 KB 回 CPU → L1b → 原子发布到 g_hdr_knee_pub。
//   算法与 nvn_hdr_auto.h / nvn_hdr_auto_l1.h（CPU 参考实现）完全一致；
//   kernel 在 graphics/nvn/shaders/hdr_hist.metal，Makefile 嵌成 k_hdr_hist。
// 开关 S2A_HDR_AUTO=1（缺省 0）：关着时 present 用 s2a_hdr_itm_params() 的固定 knee，与之前逐字节一致。
#include "nvn_hdr_hist_msl.h"
#include "nvn_hdr_auto.h"      // L0 常数（HIST_BINS / KNEE_RATIO / HIST_EVERY）
#include "nvn_hdr_auto_l1.h"   // L1：白点 / 黑场 / 场景平滑 / HUD 剔除
#include <pthread.h>
#include <time.h>              // clock_gettime：帧率估计（Darwin 上同样是 CLOCK_MONOTONIC）

@interface NvHdrHist : NSObject {
@public
    id<MTLBuffer> hist;                          // 256 × uint32，GPU 私有
    id<MTLBuffer> histcpu[2];                    // 256 × uint32 的 CPU 副本（blit 拷回），shared；双槽 + 占槽标记防跨帧竞争
    int           slot_busy[2];                  // 1 = 该槽有在途 blit/回调，尚未读完（g_hh_mu 下）
    unsigned      hs;                            // 双槽轮转
    id<MTLComputePipelineState> ps_clear, ps_add;
    NSUInteger w, h;
    float prev_avg;                              // 上一帧的 log2 重心（场景判据用）
    float prev_white;                            // 上一帧的白点（第二个场景判据：重心不动也认）
    int have_prev;
    double last_t;                               // 上一次直方图回调的时间戳（秒）：帧率估计用
    float hz;                                    // 估计出的游戏帧率（平滑过，单位 fps）
    uint64_t frame;                              // 已看到的帧数
    uint64_t logf;                               // 诊断行节拍（单位 = tick；每 8 帧一个 tick）
    unsigned gen;                                // 每次重建 +1：回调里用它判断「这份数据是否还属于当前对象」
}
@end
@implementation NvHdrHist @end

static pthread_mutex_t g_hh_mu = PTHREAD_MUTEX_INITIALIZER;
static NvHdrHist *g_hh;
static unsigned g_hh_gen;   // 单调递增：重建对象时换一代，旧命令缓冲的回调据此作废

static int hdr_log_setting(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("S2A_HDR_LOG"); v = (e != NULL && *e != '\0' && *e != '0'); }
    return v;
}

static int hdr_auto_setting(void) {
    static int v = -1;
    if (v < 0) { v = s2a_hdr_enabled(getenv("S2A_HDR_AUTO")); }
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

static NvHdrHist *hdr_hist_prepare_locked(NSUInteger w, NSUInteger h) {
    if (g_hh && g_hh->w == w && g_hh->h == h && g_hh->ps_clear && g_hh->ps_add) return g_hh;
    if (g_hh) {
        if (g_hh->hist) res_remove(g_hh->hist);
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
    if (!f->ps_clear || !f->ps_add) return nil;
    f->hist = [g_dev newBufferWithLength:256 * sizeof(uint32_t) options:MTLResourceStorageModePrivate];
    // 直方图本体留在 GPU 私有内存（原子加最快），每 S2A_HDR_HIST_EVERY 帧用 blit 拷 1 KB 回 CPU：
    //   有了真直方图，上屏判据就和 t_hdr_* 测的是同一份代码，不再需要任何 GPU 侧代理。
    for (int i = 0; i < 2; i++)
        f->histcpu[i] = [g_dev newBufferWithLength:256 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    if (!f->hist || !f->histcpu[0] || !f->histcpu[1]) { MLOG("【红】自动 HDR：建不了 buffer"); return nil; }
    res_add(f->hist); res_add(f->histcpu[0]); res_add(f->histcpu[1]);
    f->w = w; f->h = h;
    f->gen = ++g_hh_gen;
    g_hh = f;
    MLOG("自动 HDR：直方图就绪 %lux%lu（每 %d 帧一次）", (unsigned long)w, (unsigned long)h, S2A_HDR_HIST_EVERY);
    return f;
}

// 内部实现：把 clear → add（4×4/8×8 采样）→ blit 1 KB 编进指定的 classic MTLCommandBuffer，
// 完成回调在 CPU 侧按真直方图算 L1b 并原子发布到 g_hdr_knee_pub。
// 返回 1 = 已编进 cb（调用方若自建了 cb 须提交）；0 = 本帧跳过。
static int hdr_hist_encode_cb(id<MTLCommandBuffer> cb, id<MTLTexture> src, NvHdrHist *f, unsigned slot) {
    @autoreleasepool {
        id<MTLBuffer> hb = f->histcpu[slot];
        if (!hb || !cb) {
            pthread_mutex_lock(&g_hh_mu); f->slot_busy[slot] = 0; pthread_mutex_unlock(&g_hh_mu);
            return 0;
        }
        id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
        if (!en) {
            pthread_mutex_lock(&g_hh_mu); f->slot_busy[slot] = 0; pthread_mutex_unlock(&g_hh_mu);
            return 0;
        }
        uint32_t stride = (src.width >= 2560 || src.height >= 1440) ? 8u : 4u;
        uint32_t linearize = (src.pixelFormat == MTLPixelFormatRGBA16Float) ? 0u : 1u;
        // 必须传 present_raw_src(src) 剥掉 _sRGB 视图：
        //   Metal 规范下 texture2d::read() 对 _sRGB 纹理会自动解码 sRGB→线性；
        //   若直接传原 src 且 linearize=1，hdr_hist_add 会再做一次 s2a_lin_h()，导致二次伽马解码（整体压暗 ~2.5 stop）。
        id<MTLTexture> raw_src = present_raw_src(src);
        [en setComputePipelineState:f->ps_clear];
        [en setBuffer:f->hist offset:0 atIndex:0];
        [en dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [en setComputePipelineState:f->ps_add];
        [en setTexture:raw_src atIndex:0];
        [en setBuffer:f->hist offset:0 atIndex:0];
        [en setBytes:&stride length:sizeof stride atIndex:1];
        [en setBytes:&linearize length:sizeof linearize atIndex:2];
        [en dispatchThreads:MTLSizeMake(src.width, src.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        // hdr_hist_l1 不 dispatch：knee 全部在 CPU 侧按回读的真直方图算（s2a_hdr_auto_l1b_step_fused_hz），
        // GPU 只负责数格子。曾因移除 dispatch 却保留了读 f->out 的 `if (!(v[0] > 0)) return;`，导致回调每帧早退失效。
        [en endEncoding];

        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        if (!be) {
            pthread_mutex_lock(&g_hh_mu); f->slot_busy[slot] = 0; pthread_mutex_unlock(&g_hh_mu);
            return 0;
        }
        [be copyFromBuffer:f->hist sourceOffset:0
                  toBuffer:hb destinationOffset:0
                      size:256 * sizeof(uint32_t)];
        [be endEncoding];

        NvHdrHist *owner = f;
        unsigned gen = f->gen;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done_cb) {
            // 持 g_hh_mu 保护 g_hh / prev_* / slot_busy，避免分辨率重建时 ARC 跨线程并发释放导致 UAF。
            // 回调参数命名为 done_cb（旧代码叫 b，与局部 s2a_hdr_auto_l1b b 重定义导致编译失败）。
            pthread_mutex_lock(&g_hh_mu);
            owner->slot_busy[slot] = 0;
            if (done_cb.error || !g_hh || g_hh != owner || owner->gen != gen) {
                pthread_mutex_unlock(&g_hh_mu);
                return;
            }
            NvHdrHist *h = g_hh;
            uint32_t hist_copy[S2A_HDR_HIST_BINS];
            memcpy(hist_copy, hb.contents, sizeof hist_copy);

            float cur_lo = g_hdr_knee[0];
            float cur_hi = g_hdr_knee[0] + g_hdr_knee[1];
            float wov = 0.0f, wbulk = 0.0f;
            if (s2a_hdr_sdr_white_bulk(hist_copy, &wbulk)) wov = wbulk;

            // 帧率与节拍换算：两次回调间隔 S2A_HDR_HIST_EVERY（8）帧。
            //   直接把 8 帧 dt 喂给 s2a_hdr_hz_from_dt 会把 30/60/120 fps（tick 率 3.75/7.5/15 Hz）全部钳死在 24 Hz；
            //   先除以 S2A_HDR_HIST_EVERY 得到真实游戏帧率 fps，再折算成 tick_hz = fps / S2A_HDR_HIST_EVERY
            //   传给控制器，使平滑时间常数与 fade_rate（stop/秒）都按真实墙钟秒数对齐。
            struct timespec ts;
            double now = 0.0;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
            if (now > 0.0) {
                if (h->last_t > 0.0) {
                    double frame_dt = (now - h->last_t) / (double)S2A_HDR_HIST_EVERY;
                    h->hz = s2a_hdr_hz_from_dt(frame_dt, h->hz);
                }
                h->last_t = now;
            }
            float fps = (h->hz > 0.0f) ? h->hz : S2A_HDR_SMOOTH_HZ;
            float tick_hz = fps / (float)S2A_HDR_HIST_EVERY;

            s2a_hdr_auto_l1b res;
            memset(&res, 0, sizeof res);
            int ok = s2a_hdr_auto_l1b_step_fused_hz(hist_copy, wov, cur_lo, cur_hi,
                                                    h->prev_avg, h->prev_white,
                                                    h->have_prev, tick_hz, &res);
            int do_log = 0;
            s2a_hdr_stat st;
            if (ok) {
                s2a_hdr_knee_publish(&g_hdr_knee_pub, res.knee_lo, res.knee_hi - res.knee_lo);
                g_hdr_knee[0] = res.knee_lo;
                g_hdr_knee[1] = res.knee_hi - res.knee_lo;
                g_hdr_knee[3] = 1.0f;
                const uint64_t log_ticks = (uint64_t)((S2A_HDR_LOG_EVERY + S2A_HDR_HIST_EVERY - 1) / S2A_HDR_HIST_EVERY);
                if (hdr_log_setting() && (h->logf++ % log_ticks) == 0) {
                    memset(&st, 0, sizeof st);
                    st.fmt16 = 1;
                    st.itm = 1;
                    st.frozen = res.frozen;
                    st.head = s2a_hdr_f32_load(&g_hdr_head_bits);
                    st.knee_lo = res.knee_lo; st.knee_hi = res.knee_hi;
                    st.white = res.white; st.avg = res.avg; st.scene = res.scene;
                    st.map = s2a_hdr_map();
                    do_log = 1;
                }
                h->prev_avg = res.avg; h->prev_white = res.white; h->have_prev = 1;
            }
            pthread_mutex_unlock(&g_hh_mu);
            if (do_log) {
                char line[192];
                s2a_hdr_status_line(line, sizeof line, &st);
                MLOG("%s", line);
            }
        }];
        return 1;
    }
}

// 公共节拍与占槽入口（Metal 3 / Metal 4 共用）：返回 1 表示本帧应当采样并已占下 *slot_out
static int hdr_hist_claim(id<MTLTexture> src, NvHdrHist **out_f, unsigned *slot_out) {
    if (!hdr_auto_setting() || !src) return 0;
    pthread_mutex_lock(&g_hh_mu);
    NvHdrHist *f = hdr_hist_prepare_locked(src.width, src.height);
    if (!f) { pthread_mutex_unlock(&g_hh_mu); return 0; }
    if ((f->frame++ % S2A_HDR_HIST_EVERY) != 0) { pthread_mutex_unlock(&g_hh_mu); return 0; }
    unsigned slot = (f->hs++) & 1u;
    if (f->slot_busy[slot]) {
        // 上一次同槽回调尚未读完：宁可跳过一次采样，也不覆写正在读取的 CPU 缓冲
        pthread_mutex_unlock(&g_hh_mu);
        return 0;
    }
    f->slot_busy[slot] = 1;
    *out_f = f;
    *slot_out = slot;
    pthread_mutex_unlock(&g_hh_mu);
    return 1;
}

// Metal 3 上屏路径：复用本帧 present 的 MTLCommandBuffer
static void hdr_hist_tick(id<MTLCommandBuffer> cb, id<MTLTexture> src) {
    NvHdrHist *f = nil; unsigned slot = 0;
    if (!hdr_hist_claim(src, &f, &slot)) return;
    (void)hdr_hist_encode_cb(cb, src, f, slot);
}

// Metal 4 上屏路径：在辅助经典队列（q_cb）上异步跑直方图（7/8 帧零开销，每 8 帧一次），
// 避免 Metal 4 设备（!g_m3）开启 S2A_HDR_AUTO=1 时完全不采样。
static void hdr_hist_tick_m4(id<MTLTexture> src) {
    NvHdrHist *f = nil; unsigned slot = 0;
    if (!hdr_hist_claim(src, &f, &slot)) return;
    id<MTLCommandBuffer> qcb = q_cb();
    if (!hdr_hist_encode_cb(qcb, src, f, slot)) return;
    if (!cb_put(qcb)) {
        pthread_mutex_lock(&g_hh_mu); f->slot_busy[slot] = 0; pthread_mutex_unlock(&g_hh_mu);
    }
}
