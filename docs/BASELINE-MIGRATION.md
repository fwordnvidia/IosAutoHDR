# 基线迁移工单（自动生成）

目标上游提交：`75eb430240dae472ec834f98b47194d7ff2ff7e5`（2026-11-15T20:44:31-08:00 Merge fix/seal-rm-readonly into main（gate2））

本文件由 `./scripts/rebase_patch.sh <sha>` 在重放失败时自动生成，内容是 **`patch` 拒绝的每一个 hunk**：
每条给出「我们的上下文 / 我们要插入的行 / 上游现在这段是什么」。按单子逐条手工合并，
再把这个提交写进 `patch/BASELINE`。**不要**用 `patch --fuzz` 硬塞——上下文对不上说明代码结构变了。


共 2 个文件冲突。


## `graphics/nvn/gfx/nvn/nvn_present_fi.inc`

### 上游第 335 行附近 — `static void fi_job(NvPresentFI *f, CAMetalLayer *layer, id<MTLTexture> src, fi_c`

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+        if (dm && fi_commit(f, 1, dm, ^int(id cb) { return dup ? present_blit_m4(cb, src, dm.texture, crop, f->uv[2], f->tab[2], flip)
+                                                         : (f->cfmt == MTLPixelFormatRGBA16Float
+                                                            // 16F：补出的帧也必须过同一遍扩张。mid 里是 MetalFX 在**未扩张**域里插出来的值
+                                                            //（in_tex 是 src 的原样缩拷），所以这里再做一次扩张与真帧对称，不多不少。
+                                                            // 用 psoCopy（纯拷贝）会让补出的帧比真帧暗 head 倍——60→120 Hz 隔帧一亮一暗。
+                                                            ? present_blit_m4(cb, mid, dm.texture, NULL, f->uv[2], f->tab[2], flip)
+                                                            : fi_pass(cb, f->psoCopy, f->tab[2], f->uv[2], mid, nil, dm.texture, NULL, NO, vw, vh)); }, nil)) { fi_count_on(dm, &g_fi_on_mid); [dm present]; mid_shown = 1; f->shown_mid++; } }
-        if (dm && fi_commit(f, 1, dm, ^int(id cb) { return dup ? present_blit_m4(cb, src, dm.texture, crop, f->uv[2], f->tab[2], flip) : fi_pass(cb, f->psoCopy, f->tab[2], f->uv[2], mid, nil, dm.texture, NULL, NO, vw, vh); }, nil)) { fi_count_on(dm, &g_fi_on_mid); [dm present]; mid_shown = 1; f->shown_mid++; } }
```
上游现在 327–349 行：
```objc
    if (seq > 0 && !allow) f->declined++;
    else if (seq > 0) { double ms = (now_ns() - t0) / 1e6; f->ms_sum += ms; if (ms > f->ms_max) f->ms_max = ms; if (have_mid) f->made++; else f->skipped++; }
    // 先上补出的帧，再隔半个游戏周期上真帧（游戏原图直接画进画布）。图层已归显示链（正在切过去）：两张都不取，只记票完成
    pthread_mutex_lock(&g_pq_mu); int mine = fi_may_take_drawable(); pthread_mutex_unlock(&g_pq_mu);
    if (!mine) { static _Atomic int said; if (!atomic_fetch_add(&said, 1)) MLOG("present frame interpolation: the display link owns the layer; this job takes no drawable (frame %llu dropped, no two presenters)", (unsigned long long)seq);
                 fi_ticket_done(ticket); fi_inflight_add(-1); return; }
    double half = period > 0 ? period / 2 : 1.0 / 60; int mid_shown = 0;
    // 补出帧的摆放比例要和真帧一致（#1165：自适应满屏补丁开着时真帧走 present_blit_m4 按 s2a_cpatch_present_aspect 拉满到屏幕画幅，补出帧按输入图 16:9 摆，
    //   两侧黑边隔一帧一有，看着闪）：有补丁画幅就按它，没有照旧按输入图比例
    double pa = s2a_cpatch_present_aspect();
    if (have_mid) { id<CAMetalDrawable> dm = [layer nextDrawable]; NSUInteger vh = (NSUInteger)f->h, vw = pa > 0 ? (NSUInteger)llround(f->h * pa) : (NSUInteger)f->w;
        if (dm && fi_commit(f, 1, dm, ^int(id cb) { return dup ? present_blit_m4(cb, src, dm.texture, crop, f->uv[2], f->tab[2], flip, 1) : fi_pass(cb, f->psoCopy, f->tab[2], f->uv[2], mid, nil, dm.texture, NULL, NO, vw, vh); }, nil)) { fi_count_on(dm, &g_fi_on_mid); [dm present]; mid_shown = 1; f->shown_mid++; } }
    id<CAMetalDrawable> dr = [layer nextDrawable];
    if (dr && fi_commit(f, 2, dr, ^int(id cb) { return present_blit_m4(cb, src, dr.texture, crop, f->uv[3], f->tab[3], flip, 1); }, ^(int ok) { (void)ok; fi_ticket_done(ticket); })) {
#if !TARGET_OS_SIMULATOR
        present_note(dr, ticket); fi_count_on(dr, &g_fi_on_real);
        if (mid_shown) [dr presentAfterMinimumDuration:half]; else [dr present];
#else
        [dr present];
#endif
    } else fi_ticket_done(ticket);
    if (++f->jobs % 300 == 0) { MLOG("present frame interpolation up to game frame %llu: interpolated %llu, skipped %llu (over %.1f ms or failed), not interpolated by the rule %llu (60 fps tier or frame late), shown %llu; job mean %.2f ms, max %.2f ms, MetalFX GPU mean %.2f ms; on screen (system reported) interpolated %llu, game frames %llu",
                                     (unsigned long long)f->jobs, (unsigned long long)f->made, (unsigned long long)f->skipped, g_fi_budget_ms, (unsigned long long)f->declined, (unsigned long long)f->shown_mid, f->ms_sum / 300, f->ms_max, f->made > f->made_win0 ? f->gpu_sum / (double)(f->made - f->made_win0) : 0,
```


## `graphics/nvn/gfx/nvn/nvn_mtl.m`

### 上游第 17185 行附近 — `static id<MTLTexture> present_raw_src(id<MTLTexture> src) {`

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+    hdr_head_tick();
```
上游现在 17177–17199 行：
```objc
    pthread_mutex_lock(&mu);
    id<MTLTexture> v = nil;
    for (int i = 0; i < 4 && !v; i++) if (views[i] && views[i].parentTexture == src) v = views[i];
    if (!v && (v = [src newTextureViewWithPixelFormat:pf])) { if (views[next]) defer_obj_unref(views[next]); views[next] = v; next = (next + 1) & 3; }
    pthread_mutex_unlock(&mu);
    if (!v) { RED_ONCE(69, "present: cannot create a non-sRGB view of the sRGB swapchain texture (format %lu); presenting the decoded values", (unsigned long)src.pixelFormat); return src; }
    return v;
}
__attribute__((weak)) double s2a_cpatch_present_aspect(void) { return 0; }   // native runtime/src/cpatch.c: current aspect while an adaptive community widescreen patch is on; graphics unit tests without the host have none
#include "nvn_present_fit.h"   // pf_crop_fit (present_now) / pf_viewport (both blits); plain C, tested by graphics/nvn/tests/t_present_fit.c
static int present_blit(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], BOOL flip, int screen) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【红】 present crop out of bounds: %d,%d %dx%d / source texture %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("present crop: source texture %lux%lu, valid %lu,%lu %lux%lu → target %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
```

### 上游第 16688 行附近 — `static int present_blit(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTextu`（行号来自旧树，新树里没搜到上下文锚点）

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+    hdr_head_tick();
```
上游现在 16680–16702 行：
```objc
    id<MTLCommandBuffer> cb2 = !buf ? nil : g_m3 ? [g_q3 commandBuffer] : [g_q commandBuffer];   // retained references: out / buf / src live until it is done
    if (!cb2) { MLOG("【红】 full-window screenshot: no command buffer (%zux%zu)", w, h); cb(NULL, 0, 0, 0); return; }
    if (!g_m3 && g_ev && g_fence) [cb2 encodeWaitForEvent:g_ev value:g_fence];   // the frame that drew src (and anything committed before it); the old queue is not ordered with g_q4
    if (g_ext_cur && g_ext_ev) [cb2 encodeWaitForEvent:g_ext_ev value:g_ext_cur];   // an external producer's frame (the Vulkan layer)
    if (!present_encode(cb2, src, out, crop)) { MLOG("【红】 full-window screenshot: offscreen encoding of the game image failed (%zux%zu)", w, h); cb_put(cb2); cb(NULL, 0, 0, 0); return; }
    id<MTLBlitCommandEncoder> b = [cb2 blitCommandEncoder];
    [b copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding];
    uint64_t tok = fit_pick();   // the content check itself runs on the consumer's thread (mtl_fit_judge_shot)
    id<MTLBuffer> bb = buf;
    atomic_store(&g_grab_busy, 1);
    pthread_mutex_lock(&g_pq_mu); g_grab_tk = ticket ? ticket : 1; pthread_mutex_unlock(&g_pq_mu);
    [cb2 addCompletedHandler:^(id<MTLCommandBuffer> c) {
        pthread_mutex_lock(&g_pq_mu); g_grab_tk = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);   // the copy is done: src may be handed back (nvn_be_present_wait)
        // S2Arm: the bytes as they are (BGRA8, sRGB), valid during the call; the caller copies them once. buf stays ours until the callback returns
        if (c.error) { if (!mtl_gpu_cb_error(c.error, "screenshot", 0)) MLOG("【红】 full-window screenshot: copy failed: %s", c.error.description.UTF8String); cb(NULL, 0, 0, 0); }
        else { g_fj_tok_now = tok; cb((const uint8_t *)bb.contents, w, h, row); g_fj_tok_now = 0; }
        atomic_store(&g_grab_busy, 0);
    }];
    if (!cb_put(cb2)) { pthread_mutex_lock(&g_pq_mu); g_grab_tk = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); atomic_store(&g_grab_busy, 0); cb(NULL, 0, 0, 0); }   // GPU 安全停：没交，回调不会来
}
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
```

### 上游第 17289 行附近 — `static const int *rs_aa_crop(id<MTLTexture> src, id<MTLTexture> dst, const int c`

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+#include "nvn_hdr_hist.inc"   // 自动 HDR：每 8 帧一次 直方图 → L1 → g_hdr_knee（S2A_HDR_AUTO=1 才真跑）
+    hdr_hist_tick(cb, src);
+    // HDR 画布（16F）：AA / MetalFX 空间放大的内部图与上屏管线仍是 8 位，直接叠上去会管线/画布不匹配。
+    //   本轮先按「不启用」处理并告警一次；16F 版的 AA / 放大留到下一步（见 hdr-auto/docs/DESIGN.md 未做项）
+    int hdr_dst = (dst && dst.pixelFormat == MTLPixelFormatRGBA16Float);
+    if (hdr_dst && (g_present_aa || g_present_fx)) { static int once; if (!once++) MLOG("【黄】HDR：16F 画布下暂不叠加 AA / MetalFX 空间放大（8 位管线还没做 16F 版）"); }
+    NvPresentFX *fx = hdr_dst ? NULL : present_fx_for(slot, src, dst, crop, NO);
+    if ((!g_present_aa || hdr_dst) && !fx) return present_blit(cb, src, dst, crop, flip) && (!mirror || present_blit(cb, src, mirror, crop, flip));
-    NvPresentFX *fx = present_fx_for(slot, src, dst, crop, NO);
-    if (!g_present_aa && !fx) return present_blit(cb, src, dst, crop, flip) && (!mirror || present_blit(cb, src, mirror, crop, flip));
```
上游现在 17281–17303 行：
```objc
static const int *rs_aa_crop(id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], NvPresentFX *fx, int tmp[4]) {
    if (fx || !rs_up_on() || rs_state(src) != RS_SCALED) return crop;
    NSUInteger w = crop && (crop[2] || crop[3]) ? (crop[2] > 0 ? (NSUInteger)crop[2] : 0) : src.width, h = crop && (crop[2] || crop[3]) ? (crop[3] > 0 ? (NSUInteger)crop[3] : 0) : src.height;
    if (!w || !h) return crop;   // an invalid crop is reported where it always was
    pf_vp pv = pf_viewport(dst.width, dst.height, w, h); NSUInteger vw = (NSUInteger)pv.w, vh = (NSUInteger)pv.h;
    if (!vw || !vh || vw > w || vh > h || (vw == w && vh == h)) return crop;
    tmp[0] = tmp[1] = 0; tmp[2] = (int)vw; tmp[3] = (int)vh; return tmp;
}
// mirror (iOS split-view reflection, may be nil): the final picture drawn a second time into the small reflection drawable,
//   by the same blit with the same inputs as the one into dst (ios/play_reflection.m s2a_ios_reflection_drawable)
static int present_encode_slot(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, id<MTLTexture> mirror, const int crop[4], int slot) {
    present_options();
    BOOL flip = atomic_load_explicit(&g_origin_ll, memory_order_relaxed) != 0;
    NvPresentFX *fx = present_fx_for(slot, src, dst, crop, NO);
    if (!g_present_aa && !fx) return present_blit(cb, src, dst, crop, flip, 1) && (!mirror || present_blit(cb, src, mirror, crop, flip, 1));
    id<MTLTexture> out;
    if (g_present_aa) {
        int ac[4]; NvPresentAA *s = present_aa_prepare(slot, src, rs_aa_crop(src, dst, crop, fx, ac));   // rs_aa_crop
        if (!s || !present_blit(cb, src, s->input, crop, flip, 0) || !present_aa_encode(cb, s, NO)) return 0;
        out = s->output;
    } else {
        if (!present_blit(cb, src, fx->input, crop, flip, 0)) return 0;
        out = fx->input;
```

### 上游第 17319 行附近 — `API_AVAILABLE(macos(26.0), ios(26.0))`

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+    int hdr_dst = (dst && dst.pixelFormat == MTLPixelFormatRGBA16Float);
+    if (hdr_dst && (g_present_aa || g_present_fx)) { static int once; if (!once++) MLOG("【黄】HDR：16F 画布下暂不叠加 AA / MetalFX 空间放大（8 位管线还没做 16F 版）"); }
+    NvPresentFX *fx = hdr_dst ? NULL : present_fx_for(slot, src, dst, crop, YES);
+    if ((!g_present_aa || hdr_dst) && !fx) return present_blit_m4(cb, src, dst, crop, g_present_uv[slot], g_present_args, flip) && (!mirror || present_blit_m4(cb, src, mirror, crop, g_present_uv[slot], g_present_args, flip));
-    NvPresentFX *fx = present_fx_for(slot, src, dst, crop, YES);
-    if (!g_present_aa && !fx) return present_blit_m4(cb, src, dst, crop, g_present_uv[slot], g_present_args, flip) && (!mirror || present_blit_m4(cb, src, mirror, crop, g_present_uv[slot], g_present_args, flip));
```
上游现在 17311–17333 行：
```objc
        out = fx->output;
    }
    return present_blit(cb, out, dst, NULL, NO, 1) && (!mirror || present_blit(cb, out, mirror, NULL, NO, 1));
}
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]) {
    return present_encode_slot(cb, src, dst, nil, crop, 3); // synchronous screen captures
}
API_AVAILABLE(macos(26.0), ios(26.0))
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, id<MTLTexture> mirror, const int crop[4], int slot) {
    present_options();
    BOOL flip = atomic_load_explicit(&g_origin_ll, memory_order_relaxed) != 0;
    NvPresentFX *fx = present_fx_for(slot, src, dst, crop, YES);
    // The mirror blit writes the same values into the same parameter buffer / table as the blit before it
    if (!g_present_aa && !fx) return present_blit_m4(cb, src, dst, crop, g_present_uv[slot], g_present_args, flip, 1) && (!mirror || present_blit_m4(cb, src, mirror, crop, g_present_uv[slot], g_present_args, flip, 1));
    id<MTLTexture> out;
    if (g_present_aa) {
        int ac[4]; NvPresentAA *s = present_aa_prepare(slot, src, rs_aa_crop(src, dst, crop, fx, ac));   // rs_aa_crop
        if (!s || !present_blit_m4(cb, src, s->input, crop, s->cropUV, s->cropTable, flip, 0) || !present_aa_encode(cb, s, YES)) return 0;
        out = s->output;
    } else {
        if (!present_blit_m4(cb, src, fx->input, crop, fx->cropUV, fx->cropTable, flip, 0)) return 0;
        out = fx->input;
    }
```

