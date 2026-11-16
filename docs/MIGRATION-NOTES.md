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
