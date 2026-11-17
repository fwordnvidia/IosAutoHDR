# 只在设备上执行的代码：逐项状态

这份表由 `scripts/check_device_coverage.py` 生成（`run_all.sh` 的 `0w11/5` 会挡漂移）。
这里没有 macOS、没有 Xcode、没有 `xcrun metal`，所以对账是这些代码在本机能拿到的**唯一**保护。

| 代码 | 被调度 | 对账 | 为什么 / 不做的理由 |
| --- | --- | --- | --- |
| `hdr_hist_clear`<br>src/hdr_hist.metal | 是 | **无** | 三行：tid<256 才清零，dispatch 正好 256 线程（nvn_hdr_hist.inc:105），线程数与 bin 数对齐是构造保证的，没有可以算错的分支；真出错的表现是「上一帧的计数残留」，会被白点逐帧抖动立刻看出来。 |
| `hdr_hist_add`<br>src/hdr_hist.metal | 是 | `src/t_hdr_bin_mirror.c` | 逐样本对账（含黑像素入 0 号格、inf/NaN、sRGB 线性化的分段点）；它抓出过 lit_frac 设备 1.0 / CPU 0.04 的分歧。 |
| `hdr_hist_knee`<br>src/hdr_hist.metal | **否** | `src/t_hdr_knee_mirror.c` | L0 的 99.5 分位；对账抓出 float 单侧偏多一格（1,515,877 个 total 值不同 ⇒ hi 差 0.0547 stop），已改整数千分。该 kernel 建了管线但从未被调度（check_landing 断言），保留作 fallback 与参考。 |
| `hdr_hist_l1`<br>src/hdr_hist.metal | **否** | `src/t_hdr_gpu_l1.c` | L1 白点/重心/场景权重。avg 差 5.96e-07 stop、white 同格、need 量化 0 差异；已知差异是它没有 HUD 簇剔除（HUD 画面白点被顶格拉到 13.24 ⇒ hi 打满 2.0 ⇒ 等于不扩张），写在 DESIGN。**曾经被 dispatch 但结果无人读**（每 8 帧白算一次），已删掉那段 dispatch，管线留着给对账当参考。 |
| `src_msl_hdr（HDR 上屏 fragment）`<br>src/nvn_mtl.m | 是 | `src/t_hdr_shader_mirror.c` | 177,147 组（9³ 编码值 × 3 拐点 × head × sat × p3）对账，另有一份整幅照片的 tools/hdr_shader_image.c；抓出过「knee_lo==knee_hi 时 shader 给 ×head、CPU 给 ×1」的分歧。 |
| `src_msl_8bit（8 位直通 fragment）`<br>src/nvn_mtl.m | 是 | `src/t_hdr_identity.c` | 穷举 256 码值 × 5 条路径逐位一致；它守的是「HDR 关掉时画面必须一个字节都不变」。 |
| `nvn_hdr_hist.inc（回读控制流）`<br>src/nvn_hdr_hist.inc | 是 | `src/t_hdr_tick.c` | 换分辨率时旧 addCompletedHandler 会读新对象、把旧直方图结论写进 g_hdr_knee；对账用 gen 计数挡住，15 项。 |
| `present_blit / present_blit_m4（取 knee 与 16F 分支）`<br>src/nvn_mtl.m | 是 | `src/t_hdr_publish.c` | knee 的 64 位原子发布/消费（撕裂时读到「新 lo + 旧 delta」）；另有 check_landing 按函数范围断言两条路径都 take。 |
| `nvn_present_fi.inc（HDR + 补帧共存）`<br>src/nvn_present_fi.inc | 是 | `src/t_hdr_frames.c` | uv 缓冲 8→16 float（曾经 8 ⇒ knee 恒 0 ⇒ 16F 画布全黑且日志「渲染成功」）、16F 中间帧改走 present_blit_m4。 |
| `host_adapt.m 的 s2a_hdr_out_plan（输出侧三格）`<br>src/host_adapt.m | 是 | `src/t_hdr_env.c` | 四格语义表；原先 S2A_HDR_OUTPUT=1 会让 FMT=1 永远拿不到 EDR，把唯一能判黑屏归因的实验判据自己废掉。 |
| `nvn_mtl.m 的 hdr_head_tick（运行期 headroom）`<br>src/nvn_mtl.m | 是 | `src/t_hdr_head.c` | 每帧读 layer 的 maximumExtendedDynamicRangeColorComponent（Apple 文档所指，不是 potentialEDRHeadroom），死区 1%。 |
| `16F 降级 / 回头路（fmt guard）`<br>src/nvn_mtl.m | 是 | `src/t_hdr_fmtguard.c` | 连续 60 帧拿不到 drawable 才降级、降级时像素格式+EDR+颜色空间三者一起改、600 帧后试探回来。 |
| `s2a_hdr_status_line（诊断行）`<br>src/nvn_hdr.h | 是 | `src/t_hdr_stat.c` | 固定格式逐字符可 diff；tools/hdr_ref.c --stat-line 调同一函数，任何一侧改了格式另一边立刻红。 |

## 表外的东西

- 上游自己的 kernel/fragment（`ms`、`ixc_*`、`wide_u8`、`mdx_mask`、`cp`、`dt_fs` 等）不在本表：它们不是我们加的，改动只经由补丁的上下文，`check_landing.py` 负责断言我们没动到它们。
- 类型正确性（`float` vs `half`、buffer 对齐、attribute 序号）不在对账范围：`check_msl.py` 72 项做静态检查，其余交给真机（未执行）。
