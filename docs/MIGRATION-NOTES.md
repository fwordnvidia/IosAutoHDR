# 当前迁移阻塞点的机器判读（人写的，与自动工单互补）

`docs/BASELINE-MIGRATION.md` 是 `./scripts/rebase_patch.sh` 失败时**自动生成**的：每个被 `patch`
拒绝的 hunk 一条，给出「我们要插入什么」与「上游现在这段是什么」。本文件是**人对那些原始信息的判读**
——自动工单不写结论，因为它不该替你判断。

## 2026-11-16 实测：基线 `0db51505` → 上游头 `75eb4302`（371 个提交）

```
patch/hdr-auto-l1.patch 正向应用：41 个文件里 2 个失败
  graphics/nvn/gfx/nvn/nvn_mtl.m            9 hunk 中 4 个 FAILED（#3 #5 #7 #8）
  graphics/nvn/gfx/nvn/nvn_present_fi.inc   7 hunk 中 1 个 FAILED（#7）
```

### 根因不是「上下文漂移」，是**上游给 `present_blit` 加了参数**

上游 `0db51505`：

```objc
static int present_blit(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst,
                        const int crop[4], BOOL flip)
```

上游 `75eb4302`：

```objc
static int present_blit(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst,
                        const int crop[4], BOOL flip, int screen)
```

新增 `int screen`（多屏支持）。我们的 hunk 里恰好有 `present_blit_m4(cb, mid, dm.texture, NULL,
f->uv[2], f->tab[2], flip)` 这条调用，以及与 `present_blit` 同处的 `hdr_head_tick()` 插入点，
所以上下文都不再匹配。

### 因此需要改的地方（合并时要一起改，不全在冲突列表里）

| 位置 | 要做的事 |
| --- | --- |
| `present_blit` 定义 | 我们的 `hdr_head_tick();` 插到新签名的函数体首行 |
| `present_blit` 的**所有调用点** | 冲突只报插入点，不报调用点——但签名变了，调用点也必须补第 6 个实参，否则编译不过 |
| `present_blit_m4` 定义与调用 | 上游若同步改了 m4 版签名，我们那条「16F 中间帧走 `present_blit_m4`」的调用要跟着改 |
| `present_encode_slot` | 上游重写了这个函数（AA/MetalFX 分支结构变了），我们的 `hdr_dst` 判定要重新插进新结构里 |
| `nvn_present_fi.inc` 的 `fi_job` | 中间帧那条分支要与上游新结构对齐 |

结论：这是**结构性合并**（一行代码在 5 个以上的地方要改），不是文本偏移问题。
`patch --fuzz` 会把插入塞到错误位置而不报错，所以脚本拒绝用 fuzz，只出工单。

## 为什么这不影响「现在就能用」

`patch/BASELINE` 钉在 `0db51505`，任何人 clone 那个提交 + `git apply` 都是 rc=0、885 项全过。
上游前进只影响「直接对着最新 main 应用」这一个场景，而那个场景有 `./scripts/rebase_patch.sh`
先跑一遍：成功就直接给候选补丁并双向验证无损；失败就出工单，不会产出一个半对的补丁。

## 2026-11-17 实测：基线 `75eb4302` → 上游头 `d5cbf7ff`（**1082 个提交**）

```
patch/hdr-auto-l1.patch 正向应用：42 个文件里 2 个失败
  graphics/nvn/gfx/nvn/nvn_mtl.m            9 hunk 中 3 个 FAILED（#7 #8 #9）
  ios/play_reflection.m                     1 hunk 中 1 个 FAILED（#1）
```

### 根因：上游**删功能**，不是参数漂移

| 上游 `d5cbf7ff` 做了什么 | 我们的注入点怎么了 |
| --- | --- |
| 下区倒影（第二个 `CAMetalLayer` + 模糊 + 渐暗）**整个删除**：`ios/play_reflection.m` 152 → 64 行，全树 `s2a_ios_reflection_drawable` **0 命中** | 我们在这里补的 `wantsExtendedDynamicRangeContent` 跟随**没有载体了**。这不是「修不了」，是代码没了 ⇒ 把这一处改动从补丁里删掉，连带删 `src/play_reflection.m`、`check_callsites` 的 SPOT 条目、`report_review` 的说明项、`make_patch.sh` 的路径映射 |
| `present_encode_slot` / `present_encode_m4` 去掉 `id<MTLTexture> mirror` 参数（上游不再「同画一份到镜像层」） | 我们的早退行 `if (…) return present_blit(cb, src, dst, crop, flip, 1) && (!mirror || present_blit(cb, src, mirror, …));` 里的第二遍要删掉，只留 dst |
| `present_blit` / `present_blit_m4` 签名**没变**（仍带 `int screen`） | 两处 `hdr_head_tick();` 与全部调用点都不用动 |

### 合并方式

`nvn_mtl.m` 四处手工插入（每次替换都断言「匹配恰好 1 次」）：`#include "nvn_hdr_hist.inc"`
（`rs_aa_crop` 的 `}` 之后）、`hdr_hist_tick(cb, src);`（`present_options();` 之后）、`hdr_dst`
判定块（`BOOL flip = …` 之后）、`if (!dr)` 的降级/回头路大块。结果：补丁 42 文件 /
+6870 −26 行 / `sha256 012798703ed19814…`，`check_landing.py` 56/56、
`check_callsites.py`（16 定义 / 63 调用点）、`check_format_assumptions.py` 13/13 全过。

### 这一轮买到的教训

1. `rebase_patch.sh` 失败后**必须回退树**——否则留下「上游 + 半套补丁」，下一次读源码会把半套
   补丁当成上游代码（本轮就误判过 `present_blit` 里已有 `hdr_head_tick()`）。已修。
2. 冲突列表只报**定义/插入点**，不报调用点；签名变了要自己把所有调用点补齐（2026-11-16 那次
   的 `int screen` 就是这么漏的）。
3. 「上游删功能」这种迁移，正确答案是**把改动删掉**，不是硬让它编译过——补丁里不该留死代码。
