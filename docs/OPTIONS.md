# 可调项一览

补丁引入的全部旋钮，由 `scripts/report_options.py` 从源码生成（`--check` 挡漂移）。说明直接取宏上方的注释，所以**文档不会比代码新**。

## 环境变量（运行期生效，进程内首次读取后缓存）

| 变量 | 读取处 | 说明 |
|---|---|---|
| `S2A_HDR_AUTO` | `nvn_hdr_hist`（nvn_hdr_hist.inc） | 1 = 打开自动判定（直方图 → 纸白 → 拐点）；缺省关，关时整条 HDR 链路逐字节同以前 |
| `S2A_HDR_FMT` | `host_adapt`（host_adapt.m） | 1 = 只换 16F 画布、不声明 EDR（T1，分辨黑屏归因）；0 = 只声明 EDR、画布留 8 位（T3） |
| `S2A_HDR_HEADROOM` | `s2a_hdr_itm_params`（nvn_hdr.h） | 覆盖 EDR 上界（钳 [1,4]）；不设则每帧读屏幕 EDR 上界（iOS：UIScreen.currentEDRHeadroom；macOS：NSScreen.maximumExtendedDynamicRangeColorComponentValue） |
| `S2A_HDR_LOG` | `nvn_hdr_hist`（nvn_hdr_hist.inc） | 1 = 每 600 帧打一行 `HDR stat:`（真机判读与三格矩阵全靠它） |
| `S2A_HDR_MAP` | `s2a_hdr_map`（nvn_hdr.h） | 0/缺省 = 拐点档；1/bt2408/content = BT.2408 内容映射（整幅抬起，knee 只作日志） |
| `S2A_HDR_OUTPUT` | `host_adapt`（host_adapt.m） | 1 = 总开关：16F 画布 + 声明 EDR（三格矩阵的 T2，head 才会 >1） |
| `S2A_HDR_P3` | `s2a_hdr_p3`（nvn_hdr.h） | Display P3 混合比例 [0,1]，0 = 不动（BT.2087 矩阵按行归一化，灰不变） |
| `S2A_HDR_SAT` | `s2a_hdr_sat`（nvn_hdr.h） | 饱和度倍数，钳 [0.5,2]，1 = 不动（CIELAB 里缩放 a*/b*，保色相） |
| `S2A_HDR_SAT_SPACE` | `s2a_hdr_sat_space`（nvn_hdr.h） | 1 = 在 CIELAB 里缩放色度（缺省）；0 = 旧的线性锚定（会偏色相） |

## 编译期常数（可用 `-D` 覆盖，覆盖后需重编）

| 常数 | 缺省 | 说明 | 读取处 |
|---|---|---|---|
| `S2A_HDR_AVG_CUT` | `(-9.5f)` | 重心的黑场门限：比 BLACK_CUT 更低，只排「真黑」（线性 0.0014 ≈ 8 位码 10/255 之下的死区） | t_hdr_oracle.c |
| `S2A_HDR_BLACK_CUT` | `(-6.0f)` | 黑场截断：log2 线性亮度低于此值的格子不计入统计（-6 stops = 0.0156） | t_hdr_oracle.c |
| `S2A_HDR_BT2408_GAMMA` | `1.15f` | （同上一组）合起来：L_out = head * L^1.15（head 缺省 2.03） | nvn_hdr_msl.h |
| `S2A_HDR_BT2408_HEAD` | `2.03f` | 合起来：L_out = head * L^1.15（head 缺省 2.03） | t_hdr_bt2408.c |
| `S2A_HDR_CLUSTER_GAP` | `3` | 连续 3 个空 bin（约 0.35 stop）才算分开 | t_hdr_oracle.c |
| `S2A_HDR_CLUSTER_MAX_FRAC` | `0.05` | 一簇亮像素少于亮像素的 5% 视为 HUD/孤立高光 | t_hdr_oracle.c |
| `S2A_HDR_DARK_FLOOR` | `0.02f` | 黑底画面从这条线开始扩张（线性亮度，≈ -5.6 stops） | t_hdr_fade.c、t_hdr_wiring.c |
| `S2A_HDR_DARK_FRAC` | `0.05f` | 截断之上的像素少于这个占比就算「黑底画面」：此时白点不可信（星星/孤灯就是唯一亮点） | t_hdr_bin_mirror.c |
| `S2A_HDR_DARK_WHITE` | `0.02f` | 白点低于此值当黑底画面（lit_frac 的代理判据） | （仅本文件） |
| `S2A_HDR_DEADBAND` | `0.02f` | 死区（stop）：小于 2% 的变化直接不动，防止内容微动导致每帧方向翻转 | nvn_hdr_auto_l1.h、t_hdr_auto.c |
| `S2A_HDR_DIM_APL` | `(-4.5f)` | 淡出（fade to black）与暗场的区分 | t_hdr_bin_mirror.c |
| `S2A_HDR_FADE_MAX_S` | `16.0f` | （同上一组）淡出的判据用「下降速度窗口」而不是「亮度水平」： 太慢（< FADE_MIN_S）是同场景微动，正常平滑； 太快（> FADE_MAX_S）是硬切到暗场景，必须让拐点跟过去（否则切镜后整段都错） | （仅本文件） |
| `S2A_HDR_FADE_MIN_S` | `1.5f` | 淡出的判据用「下降速度窗口」而不是「亮度水平」： 太慢（< FADE_MIN_S）是同场景微动，正常平滑； 太快（> FADE_MAX_S）是硬切到暗场景，必须让拐点跟过去（否则切镜后整段都错） | t_hdr_fade.c |
| `S2A_HDR_HEAD_DEADBAND` | `0.01f` | 变化死区：相对差小于 S2A_HDR_HEAD_DEADBAND 就不动（读数抖一下不该让画面闪） | （仅本文件） |
| `S2A_HDR_HIST_BINS` | `256` | 直方图格数：256 格覆盖 14 个 stop（每格 0.0547 stop），1 KB，一次 copyFromBuffer 全取回 | nvn_hdr_auto_l1.h、nvn_hdr_hist.inc、nvn_hdr_msl_hist.h、t_hdr_auto.c… |
| `S2A_HDR_HIST_BIN_STOP` | `((S2A_HDR_HIST_HI - S2A_HDR_HIST_LO) / (float)S2A_HDR_HIST_BINS)` | 白点死区。直方图按 log2 分格（S2A_HDR_HIST_BINS=256 覆盖 -10..+4 ⇒ 每格 0.0547 stop）， 内容微动会让测出的白点在**相邻两格**之间来回跳，knee_lo 就被拽着来回走 （实测真实照片慢推序列 11 次方向翻转… | t_hdr_fade.c、t_hdr_wiring.c |
| `S2A_HDR_HIST_EVERY` | `8` | GPU 直方图的采样间隔（帧） | nvn_hdr_hist.inc、t_hdr_auto.c、t_hdr_tick.c |
| `S2A_HDR_HIST_HI` | `(4.0f)` | log2 线性亮度上界（16） | nvn_hdr_auto_l1.h、t_hdr_fuzz.c、t_hdr_oracle.c |
| `S2A_HDR_HIST_LO` | `(-10.0f)` | log2 线性亮度下界（0.001） | nvn_hdr_auto_l1.h、t_hdr_fuzz.c、t_hdr_oracle.c |
| `S2A_HDR_HZ_MAX` | `240.0f` | （同上一组）帧率估计：上屏路径拿不到「层的刷新率」，只能从两次回调的时间差自己算 | t_hdr_framerate.c |
| `S2A_HDR_HZ_MIN` | `24.0f` | 帧率估计：上屏路径拿不到「层的刷新率」，只能从两次回调的时间差自己算 | t_hdr_framerate.c、t_hdr_tick.c |
| `S2A_HDR_HZ_TAU` | `8.0f` | （同上一组）帧率估计：上屏路径拿不到「层的刷新率」，只能从两次回调的时间差自己算 | （仅本文件） |
| `S2A_HDR_KNEE_L1_MAX` | `2.0f` | L1 的 knee 上界比 L0 宽：白点可能在 1.0（满屏 UI 白），此时 knee_hi = 1.0/0.8 = 1.25， 意思是「画面里没有可扩张的高光了」 | t_hdr_auto_l1.c、t_hdr_pipeline.c |
| `S2A_HDR_KNEE_L1_MIN` | `0.01f` | （同上一组）白点死区。直方图按 log2 分格（S2A_HDR_HIST_BINS=256 覆盖 -10..+4 ⇒ 每格 0.0547 stop）， 内容微动会让测出的白点在**相邻两格**之间来回跳，knee_lo 就被拽着来回走 （实测真实照片慢推序列 11 次方向翻转… | t_hdr_auto_l1.c、t_hdr_pipeline.c |
| `S2A_HDR_KNEE_MAX` | `1.0f` | （同上一组）拐点下界的合法区间：低于 0.25 连 SDR 中调都会被抬（UI 发灰），高于 1.0 等于不扩张 | nvn_hdr_auto_l1.h |
| `S2A_HDR_KNEE_MIN` | `0.25f` | 拐点下界的合法区间：低于 0.25 连 SDR 中调都会被抬（UI 发灰），高于 1.0 等于不扩张 | t_hdr_auto.c、t_hdr_fuzz.c |
| `S2A_HDR_KNEE_RATIO` | `0.55f` | knee_lo : knee_hi，与原来写死的 0.55:1.0 同一比例 | nvn_hdr_auto_l1.h、t_hdr_auto.c、t_hdr_auto_l1.c、t_hdr_wiring.c |
| `S2A_HDR_KNEE_TARGET` | `0.8f` | 99.5 分位落在「可用上沿」的哪里 | nvn_hdr_auto_l1.h |
| `S2A_HDR_LOG_EVERY` | `600` | 600 帧 ≈10 秒 @60 Hz | nvn_hdr_hist.inc、nvn_mtl.m |
| `S2A_HDR_MAP_DEFAULT` | `0` | ---------- 内容映射（RTX HDR 那一路）：把整个 SDR 画面放进 HDR 容器 ---------- 上面两种放置方式以前只有 hdr_convert（工具）能选，设备上永远是 knee 方案 | （仅本文件） |
| `S2A_HDR_NIL_LIMIT` | `60` | 60 帧 ≈ 1 秒 @60 Hz | nvn_mtl.m、nvn_present_fi.inc、t_hdr_fmtguard.c |
| `S2A_HDR_P3_R0` | `0.822582f, 0.177418f, 0.000000f` | 线性光下的 sRGB → Display P3（BT.2087 推导的 3×3 | （仅本文件） |
| `S2A_HDR_P3_R1` | `0.033197f, 0.966803f, 0.000000f` | （同上一组）线性光下的 sRGB → Display P3（BT.2087 推导的 3×3 | （仅本文件） |
| `S2A_HDR_P3_R2` | `0.017100f, 0.072400f, 0.910500f` | （同上一组）线性光下的 sRGB → Display P3（BT.2087 推导的 3×3 | （仅本文件） |
| `S2A_HDR_QUANTILE` | `0.995f` | 让最亮 0.5% 去吃满 headroom（文档用 | （仅本文件） |
| `S2A_HDR_QUANTILE_PERMILLE` | `995u` | 分位阈值的唯一真相源：整数千分，GPU/CPU 都用它 | t_hdr_knee_mirror.c |
| `S2A_HDR_REARM_FRAMES` | `600` | 首次重试间隔：600 帧 ≈ 10 秒 @60 Hz | nvn_mtl.m、t_hdr_fmtguard.c |
| `S2A_HDR_REARM_MAX` | `8` | 退避上限倍数（600*8 ≈ 80 秒 @60 Hz） | t_hdr_fmtguard.c |
| `S2A_HDR_SAT_MAX` | `2.0f` | （同上一组）---------- 色域 / 饱和度（对标 RTX HDR 的 "saturation"，取 BT.2087 的 sRGB→Display P3 矩阵）---------- RTX HDR 除了亮度扩张还会把 SDR 的 sRGB 色彩往宽色域推一点，并用一个饱… | t_hdr_fuzz.c |
| `S2A_HDR_SAT_MIN` | `0.5f` | ---------- 色域 / 饱和度（对标 RTX HDR 的 "saturation"，取 BT.2087 的 sRGB→Display P3 矩阵）---------- RTX HDR 除了亮度扩张还会把 SDR 的 sRGB 色彩往宽色域推一点，并用一个饱… | t_hdr_fuzz.c |
| `S2A_HDR_SCENE_HIGH` | `1.10f` | （同上一组）场景切换：低于 LOW 档完全按平滑走，高于 HIGH 档直接跳（单位 = log2 亮度差） | （仅本文件） |
| `S2A_HDR_SCENE_LOW` | `0.40f` | 场景切换：低于 LOW 档完全按平滑走，高于 HIGH 档直接跳（单位 = log2 亮度差） | hdr_hist.metal |
| `S2A_HDR_SDR_CLIP` | `1.0f` | SDR 编码域的线性上沿（L1b 用它当 knee_hi：白点之上到裁切点之间拿满 head） | t_hdr_wiring.c |
| `S2A_HDR_SMOOTH` | `0.1f` | 一阶平滑系数 | nvn_hdr_auto_l1.h、t_hdr_framerate.c |
| `S2A_HDR_SMOOTH_HZ` | `60.0f` | 帧率归一化：S2A_HDR_SMOOTH 是「60 Hz 下每帧走多少」 | nvn_hdr_hist.inc、t_hdr_framerate.c、t_hdr_wiring.c |
| `S2A_HDR_WHITE_DEADBAND` | `(1.5f * S2A_HDR_HIST_BIN_STOP)` | （同上一组）白点死区。直方图按 log2 分格（S2A_HDR_HIST_BINS=256 覆盖 -10..+4 ⇒ 每格 0.0547 stop）， 内容微动会让测出的白点在**相邻两格**之间来回跳，knee_lo 就被拽着来回走 （实测真实照片慢推序列 11 次方向翻转… | t_hdr_framerate.c、t_hdr_wiring.c |
| `S2A_HDR_WHITE_FRAC` | `0.005f` | 纸白的定义位置：从最亮格往下累计到总像素的 0.5% 为止，那一格之上就是「高光」 | t_hdr_oracle.c |

## 不在这里的东西

- 上游自己的 `DM_*` 调试开关（一百多个）与本补丁无关，不去罗列。

- 三格矩阵（T1/T2/T3）用到的只是 `S2A_HDR_FMT` 与 `S2A_HDR_OUTPUT` 的组合，判据见 `scripts/check_device_log.py`。

