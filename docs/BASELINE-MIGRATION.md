# 基线迁移工单（自动生成）

目标上游提交：`d5cbf7ff53f75b6132b17ce3a442d431bbbc8726`（2026-11-17T04:33:42-08:00 Merge fix/foreground-preempts-auto into main（gate2））

> **本次迁移已完成（2026-11-17）**：下面两个文件的失败 hunk 已按本单手工合并、基线升到
> `d5cbf7ff53f7`；判读结论见 `docs/MIGRATION-NOTES.md` 的 2026-11-17 段，改动清单见 `docs/PR.md`
> 的迁移表。本文件保留原始工单供事后核对——下次 `rebase_patch.sh` 失败会整体覆盖它。

本文件由 `./scripts/rebase_patch.sh <sha>` 在重放失败时自动生成，内容是 **`patch` 拒绝的每一个 hunk**：
每条给出「我们的上下文 / 我们要插入的行 / 上游现在这段是什么」。按单子逐条手工合并，
再把这个提交写进 `patch/BASELINE`。**不要**用 `patch --fuzz` 硬塞——上下文对不上说明代码结构变了。


共 2 个文件冲突。


## `graphics/nvn/gfx/nvn/nvn_mtl.m`

### 上游第 17290 行附近 — `static const int *rs_aa_crop(id<MTLTexture> src, id<MTLTexture> dst, const int c`（行号来自旧树，新树里没搜到上下文锚点）

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+#include "nvn_hdr_hist.inc"   // 自动 HDR：每 8 帧一次 直方图 → L1 → g_hdr_knee（S2A_HDR_AUTO=1 才真跑）
+    hdr_hist_tick(cb, src);
+    // HDR 画布（16F）：AA / MetalFX 空间放大的内部图与上屏管线仍是 8 位，直接叠上去会管线/画布不匹配。
+    //   本轮先按「不启用」处理并告警一次；16F 版的 AA / 放大留到下一步（见 hdr-auto/docs/DESIGN.md 未做项）
+    int hdr_dst = (dst && dst.pixelFormat == MTLPixelFormatRGBA16Float);
+    if (hdr_dst && (g_present_aa || g_present_fx)) { static int once; if (!once++) MLOG("【黄】HDR：16F 画布下暂不叠加 AA / MetalFX 空间放大（8 位管线还没做 16F 版）"); }
+    NvPresentFX *fx = hdr_dst ? NULL : present_fx_for(slot, src, dst, crop, NO);
+    if ((!g_present_aa || hdr_dst) && !fx) return present_blit(cb, src, dst, crop, flip, 1) && (!mirror || present_blit(cb, src, mirror, crop, flip, 1));
-    NvPresentFX *fx = present_fx_for(slot, src, dst, crop, NO);
-    if (!g_present_aa && !fx) return present_blit(cb, src, dst, crop, flip, 1) && (!mirror || present_blit(cb, src, mirror, crop, flip, 1));
```
上游现在 17282–17304 行：
```objc
// DM_NVN_SHOT_OUT=WIDTHxHEIGHT (for picture quality metrics): the present-frame screenshot instead captures the result "upscaled to this resolution by present_encode", i.e. the picture the player sees on screen; screen=0 (texture dump) is unaffected
static id<MTLTexture> present_raw_src(id<MTLTexture> src);   // shot reads an enlarged sRGB window texture through it
static void shot(id<MTLTexture> src, const char *path, int screen, const int crop[4]) {
    static int out_w = -1, out_h;
    if (out_w < 0) {
        out_w = 0;
        const char *e = getenv("DM_NVN_SHOT_OUT"); int w = 0, h = 0; char sep = 0, extra = 0;
        if (e && sscanf(e, "%d%c%d%c", &w, &sep, &h, &extra) == 3 && sep == 'x' && w > 0 && h > 0) { out_w = w; out_h = h; }
    }
    id<MTLTexture> read = src;
    if (out_w && screen) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:out_w height:out_h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
        if (!out) { MLOG("【红】 cannot create the screenshot output texture %dx%d", out_w, out_h); return; }
        id<MTLCommandBuffer> cb = q_cb();
        if (!present_encode(cb, src, out, crop) || !cb_run(cb)) return;
        read = out;
    }
    if (read.pixelFormat != MTLPixelFormatRGBA8Unorm && read.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        read.pixelFormat != MTLPixelFormatBGRA8Unorm && read.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) { MLOG("【红】 screenshot does not support format %lu", (unsigned long)read.pixelFormat); return; }
    size_t w = read.width, h = read.height, row = w * 4;
    id<MTLBuffer> buf = nil;
```

### 上游第 17321 行附近 — `API_AVAILABLE(macos(26.0), ios(26.0))`（行号来自旧树，新树里没搜到上下文锚点）

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+    // HDR 画布（16F）：AA / MetalFX 空间放大的内部图与上屏管线仍是 8 位，直接叠上去会管线/画布不匹配。
+    //   本轮先按「不启用」处理并告警一次；16F 版的 AA / 放大留到下一步（见 hdr-auto/docs/DESIGN.md 未做项）
+    int hdr_dst = (dst && dst.pixelFormat == MTLPixelFormatRGBA16Float);
+    if (hdr_dst && (g_present_aa || g_present_fx)) { static int once; if (!once++) MLOG("【黄】HDR：16F 画布下暂不叠加 AA / MetalFX 空间放大（8 位管线还没做 16F 版）"); }
+    NvPresentFX *fx = hdr_dst ? NULL : present_fx_for(slot, src, dst, crop, YES);
+    if ((!g_present_aa || hdr_dst) && !fx) return present_blit_m4(cb, src, dst, crop, g_present_uv[slot], g_present_args, flip, 1) && (!mirror || present_blit_m4(cb, src, mirror, crop, g_present_uv[slot], g_present_args, flip, 1));
-    NvPresentFX *fx = present_fx_for(slot, src, dst, crop, YES);
-    if (!g_present_aa && !fx) return present_blit_m4(cb, src, dst, crop, g_present_uv[slot], g_present_args, flip, 1) && (!mirror || present_blit_m4(cb, src, mirror, crop, g_present_uv[slot], g_present_args, flip, 1));
```
上游现在 17313–17335 行：
```objc
        w = cw; h = ch; row = w * 4;
    }
    if (!buf) {
    buf = [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:read sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding]; if (!cb_run(cb)) return;
    }
    if (screen && read == src && atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) {   // lower-left origin: the window image is stored bottom-up (see g_origin_ll)
        uint8_t *p = buf.contents, *tmp = malloc(row);
        if (tmp) { for (size_t y = 0; y < h / 2; y++) { memcpy(tmp, p + y * row, row); memcpy(p + y * row, p + (h - 1 - y) * row, row); memcpy(p + (h - 1 - y) * row, tmp, row); } free(tmp); } }
    int bgra = read.pixelFormat == MTLPixelFormatBGRA8Unorm || read.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGBitmapInfo bi = bgra ? (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little) : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    // If any step yields null (directory does not exist / not writable, bitmap context cannot be created), report a failure and return; previously it went straight to CFRelease(NULL) and crashed in CFRelease.cold (hit during a 1952-wide TEXDUMP)
    int ok = 0;
    if (!ctx || !img || !dst) MLOG("【红】 screenshot %s failed: %s", path, !ctx ? "cannot create the bitmap context" : !img ? "cannot get the image" : "cannot write (directory missing or not writable)");
```

### 上游第 18058 行附近 — `static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t t`

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+    if (!dr) {
+        // 16F 画布可能根本给不出可绘制对象（格式不被支持 / EDR 被系统关掉）。
+        // 原来这里只是一句 return 0：没有日志、没有回退，表现就是永久黑屏。
+        // 连续一秒拿不到就降级到 8 位，让画面先出来（HDR 关掉，其余功能不受影响）。
+        // 只要**当前确实在用 16F**（无论是否拿到过 drawable），就记下「想要 16F」：
+        // 真机 T1/T2 那种从头黑到尾的情形，用户之后在系统里打开 HDR 时也要能自己回来。
+        if (layer.pixelFormat == MTLPixelFormatRGBA16Float) g_hdr_want16 = 1;
+        // 降级之后还要留回头路。g_hdr_want16 记录「起局时确实要了 16F」，
+        // 否则 S2A_HDR_FMT=0 / HDR 关闭的用户会被无端反复试探。
+        if (!supplied && g_hdr_fmt_guard.latched &&
+            s2a_hdr_fmt_guard_rearm(&g_hdr_fmt_guard, g_hdr_want16)) {
+            layer.pixelFormat = MTLPixelFormatRGBA16Float;
+            layer.wantsExtendedDynamicRangeContent = YES;
+            CGColorSpaceRef hdr_cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
+            if (hdr_cs) { layer.colorspace = hdr_cs; CGColorSpaceRelease(hdr_cs); }
+            else MLOG("HDR: 重建 16F 画布时拿不到 ExtendedLinearSRGB，颜色会按 sRGB 解释");
+            g_hdr_knee[2] = s2a_hdr_itm_params().head;
+            present_pipeline(layer.pixelFormat);
+            MLOG("HDR: 重新试探 16F 画布（降级后第 %d 帧；失败会按退避再等）", S2A_HDR_REARM_FRAMES);
+            return 0;
+        }
+        if (!supplied && s2a_hdr_fmt_guard_step(&g_hdr_fmt_guard, 0,
+                                                layer.pixelFormat == MTLPixelFormatRGBA16Float)) {
+            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
+            layer.wantsExtendedDynamicRangeContent = NO;
+            // 颜色空间必须一起退回来：host 层给 16F 画布设的是 ExtendedLinearSRGB，
+            // 而 8 位直通 shader 写出的是**已 sRGB 编码**的值。只改像素格式不改颜色空间，
+            // 系统会把那些字节当线性值解释 ⇒ 画面发灰发亮（比黑屏更难查）。
+            CGColorSpaceRef back = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
+            if (back) { layer.colorspace = back; CGColorSpaceRelease(back); }
+            g_hdr_knee[2] = 1.0f;              // 8 位画布不做扩张，head 归 1
+            present_pipeline(layer.pixelFormat);   // 立即按 8 位重建，下一帧走直通 shader
+            MLOG("HDR: 16F 画布连续 %d 帧取不到 drawable，已回退 8 位上屏（HDR 关闭）", S2A_HDR_NIL_LIMIT);
+        }
+        return 0;
+    }
+    if (!supplied) {
+        const int is16 = (layer.pixelFormat == MTLPixelFormatRGBA16Float);
+        s2a_hdr_fmt_guard_step(&g_hdr_fmt_guard, 1, is16);
+        if (is16) { g_hdr_want16 = 1; s2a_hdr_fmt_guard_ok(&g_hdr_fmt_guard); }   // 16F 稳了：退避复位
-    if (!dr) return 0;
```
上游现在 18050–18072 行：
```objc
    if (!g_m3 && g_present_layer_resident != layer) { [g_q4 addResidencySet:layer.residencySet]; g_present_layer_resident = layer; }   // the layer's residency set is macOS / iOS 26 on; Metal 3 presents through the command buffer
#if TARGET_OS_OSX
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;   // Windowed mode follows the display's vsync to prevent tearing
#endif
    present_pipeline(layer.pixelFormat);   // Same as the original path: the first frame compiles the pipeline before taking the drawable
    fit_geom(layer.drawableSize.width, layer.drawableSize.height, pcw, pch);   // game units (was the crop / source size read here)
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize, ticket);   // Game picture needed by the whole-window screenshot (peek without the lock first; the real take locks inside the function)
    if (fi_wanted() && !supplied) { if (@available(macOS 26.0, iOS 26.0, *)) { int fr = fi_frame(src, crop, layer, period, ticket); if (fr >= 0) return fr; } }   // 实验：出图补帧接手这一帧
    uint64_t dt0 = now_ns();
    id<CAMetalDrawable> dr = supplied ? supplied : [layer nextDrawable];
    if (!supplied) g_drawable_ns += now_ns() - dt0;
    if (!dr) return 0;
    int alloc_i = (int)(ticket % 3);
    present_options();
    if (g_present_aa || g_present_fx) {
        pthread_mutex_lock(&g_pq_mu);
        while ((g_present_aa_slots[alloc_i] && g_present_aa_slots[alloc_i]->busy) || (g_present_fx_slots[alloc_i] && g_present_fx_slots[alloc_i]->busy)) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
    if (g_present_alloc_fence[alloc_i]) {
        DM_BLOCK_BEGIN(); pthread_mutex_lock(&g_pq_mu);
        while (g_pq_done < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu); DM_BLOCK_END();
```


## `ios/play_reflection.m`

### 上游第 107 行附近 — `id<CAMetalDrawable> s2a_ios_reflection_drawable(CAMetalLayer *game) {`（行号来自旧树，新树里没搜到上下文锚点）

我们原本要插入/删除（`+` 是要加进去的）：
```diff
+    // S2Arm HDR：像素格式与色彩空间跟着游戏层走了，EDR 开关却没跟——16F drawable + extended linear
+    //   colorspace 但 wantsExtendedDynamicRangeContent 仍是 NO，系统按 SDR 合成这块镜像，
+    //   镜像画面里 >1 的高光被钳回 1.0（主屏正常）。这一行少不得：HDR 与「对称视图/反射」同开时
+    //   只有它能让两个层说同一种亮度语言。（像素格式那一行是上游 11-13 加的，见上方注释）
+    if (m.wantsExtendedDynamicRangeContent!=game.wantsExtendedDynamicRangeContent) m.wantsExtendedDynamicRangeContent=game.wantsExtendedDynamicRangeContent;
```
（目标树里读不到该路径——可能上游把这个文件改名或搬走了）

