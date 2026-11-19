# 资料与出处（SOURCES）

这份模块不是凭空设计的：每一条参数都能对应到公开实现里的一个具体旋钮。下面按「读到什么 / 用在哪」列。

## 1 NVIDIA RTX HDR / RTX Video SDK

- https://developer.nvidia.com/rtx-video-sdk

RTX HDR 的定位是**逐帧**把 SDR 内容转成 HDR：不是全片一个固定曲线，而是对当前画面分析亮度分布，再按显示器的 HDR 能力把高光抬上去。它跑在 tensor core 上（神经网络推理），所以本机复刻不了同等质量；能复刻的是**它必须解决的那几个问题**：

| RTX HDR 要解决的问题 | 本模块的对应 |
|---|---|
| 画面里「白」在哪（白点/漫反射白） | `s2a_hdr_sdr_white()`，取计数 ≥0.5% 的最高格 |
| 单帧最优会闪 | 一阶平滑 + 死区（L0 `s2a_hdr_auto_step`） |
| 硬切镜头要立刻跟上 | `s2a_hdr_scene_weight()` 按 log2 重心差分级 |
| 暗场景不该被拉成灰 | 黑场截断 `S2A_HDR_BLACK_CUT` |

结论：不做神经网络，做**同一组控制变量的经典实现**。

## 2 ITU-R BT.2446（SDR→HDR 的官方方法 A/B/C）

- https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2446-1-2021-PDF-E.pdf

这份报告定义了 SDR（BT.709，100 nit 参考）到 HDR（BT.2020/PQ）的三种转换方法。方法 A 的核心是**亮度驱动的分段映射**：暗部基本线性、高光段按内容亮度做滚降，而不是全画面统一 gamma。我们的 `s2a_hdr_itm_gain()`（`nvn_hdr.h`）就是这个形状：

```
gain(y) = 1 + (head-1) * smoothstep(knee_lo, knee_hi, y)
```

- `knee_lo/hi` 决定「哪一段开始扩张」，等价于方法 A 里的拐点；
- `head` 是目标峰值相对 SDR 白的倍数，对应方法 A 的输出峰值设定；
- 色相保持用 Rec.709 亮度算 gain 再乘 RGB（`s2a_hdr_itm_rgb`），避免直接乘通道造成偏色。

L1 的贡献是**把拐点从常数变成逐场景量**：`knee_hi = 白点 / 0.8`。BT.2446 方法 A 的拐点也是跟着内容走的，只是它的输入是整段视频的统计，我们这里是逐帧直方图。

数值对照见 `docs/CURVE-REF.md`（`scripts/report_curveref.py` 生成）：把它取逆做 SDR→HDR 时，
中间调比本案亮 **+0.76 stop**（203 nit 目标）、顶端两侧重合到 **0.000 stop**。也就是说
「形状取自方法 A」只对高光段成立，中低亮度我们刻意保持 ×1.0000（UI/字幕不变亮）。

## 3 libplacebo（mpv 的渲染器）

- 选项文档：https://libplacebo.org/options/
- FFmpeg 的封装：https://github.com/FFmpeg/FFmpeg/blob/8bea614d/libavfilter/vf_libplacebo.c

libplacebo 的 peak detection 是公开实现里最完整的一套，我们直接对齐它的四个旋钮：

| libplacebo 选项 | 语义 | 本模块 |
|---|---|---|
| `peak_percentile` | 取直方图哪个分位当场景峰值（默认 100，high_quality 99.995） | L0 的 `S2A_HDR_QUANTILE=0.995`；L1 再加白点判据 |
| `peak_smoothing_period` | 峰值检测的 IIR 时间常数（默认 20 帧） | `S2A_HDR_SMOOTH=0.1`（≈ 10 帧时间常数，比它快一档） |
| `scene_threshold_low/high` | 场景亮度差超过阈值就**关掉低通**，避免硬切拖影 | `S2A_HDR_SCENE_LOW/HIGH = 0.5/1.6`（log2 stops，libplacebo 用 1% PQ 单位） |
| `black_cutoff` | 低于阈值的像素不计入统计（默认 1% PQ） | `S2A_HDR_BLACK_CUT = -6.0`（log2，≈ 0.0156 线性） |

另外两条只做记录、本版未实现：`gamut_expansion`（把 sRGB 扩到 P3/2020）与 `inverse_tone_mapping` 的 `bt2446a`/`spline`/`st2094-40` 曲线族。

## 4 SpecialK 的 HDR Retrofit

- https://wiki.special-k.info/HDR/Retrofit

Windows 上把 SDR 游戏转 HDR 的另一条成熟路线。它的做法与我们的约束最接近：**在出图这一层做转换，不改游戏**，并且明确区分「纸白（paper white）」与「峰值」两个量。我们的 `white` / `head` 二分与它一致。

## 5 本模块没有用到的方向（记录以免重复调研）

- 神经网络反色调映射（Deep Reverse Tone Mapping / ExpandNet）：需要推理引擎与权重，iOS 侧无可用模型，收益不抵复杂度。
- HDR10+ 动态元数据：我们是本地出图，没有元数据通道，iOS 层也不接受 per-frame metadata。
- 硬件 HDR 直通（`wantsExtendedDynamicRangeContent`）：这是**输出侧**的事，已在 `nvn_hdr.h` 的层接线里，与本文的**内容侧**转换分开。

## 6 色域与饱和度（本轮新增）：BT.2087 + libplacebo 的两个旋钮

- **ITU-R BT.2087**（*Colour conversion from Recommendation ITU-R BT.709 to Recommendation ITU-R BT.2020*）：
  给了 sRGB/709 → 宽色域原色的 3×3 线性矩阵推导方法。本模块只用到第一段：**sRGB → Display P3**。
  直接抄文献数值时每行和是 `0.9999 / 1.0001 / 1.0000`（四舍五入残留），
  代入后中性灰会偏 1e-4 量级；所以按行归一化成
  `0.822582 0.177418 0.000000` / `0.033197 0.966803 0.000000` / `0.017100 0.072400 0.910500`，
  让「灰不变」是结构性成立而不是靠容差（`t_hdr_color` 第 1 组断言守着）。
- **libplacebo `gamut_expansion`**（`options.h`，mpv 的 `--gamut-expansion`）：只在目标色域大于源色域时把
  颜色往外推，且**默认关闭**。本模块对应旋钮 `S2A_HDR_P3`（0..1，缺省 0）。
- **libplacebo `saturation`**（着色器里的 `pl_color_adjustment.saturation`）：以亮度为中心缩放色度向量，
  即 `c' = y + (c - y) * s`，权重是 Rec.709 的 `0.2126/0.7152/0.0722`。本模块对应 `S2A_HDR_SAT`（0.5..2，缺省 1）。
  这条与 RTX HDR 面板上的 ``saturation`` 语义一致：**保亮度、灰恒等、只拉色彩**。
- 顺序：先色域后饱和度再 ITM。理由是 ITM 的 gain 只由亮度决定，色域/饱和度改的是「颜色往哪偏」，
  放在 ITM 之前可以让扩张倍数仍然基于**最终**颜色的亮度，避免先扩张后改色导致高光溢出不同步。
- 明确**没有**做：BT.2390/BT.2446 的完整色调映射（我们是反方向）、perceptual gamut mapping 的软裁剪
  （`softclip` / `perceptual` 曲线族）、色适应（chromatic adaptation，换白点才需要）。


## 7 2024–2025 复核：BT.2408-8 §5.1 的 display-referred 放置（本轮新增）

- ITU-R BT.2408-8 (2024)，*Guidance for operational practices in HDR television production*，
  §5.1「display-referred SDR→HDR mapping」：把 SDR 放进 HDR 容器时不是简单乘一个常数，而是三步：
  1. **100 → 203 cd/m²**：SDR 峰值白线性放大 2.03 倍（BT.1886 的 100 cd/m² → HDR Reference White 203 cd/m²）；
  2. **OOTF gamma 校正**：`L_out = L_in^1.15`（BBC 主观测试；ARIB 为 1.16）。理由是只在 100→203 上做线性放大
     会让中调看起来被冲淡，gamma 把中调压回来；
  3. **203 ↔ 100 的互转**：`L^1/1.08`（Annex 11），用于两套 SDR 参考白之间的换算。
  参考实现（Rust，逐条注明出处）：<https://docs.rs/zentone/latest/src/zentone/sdr_hdr.rs.html>。

本模块把第 1、2 步合成为一条可选映射（`--map bt2408`，`s2a_hdr_bt2408_rgb_lin`）：

```
L_out = head * L^1.15        # head 缺省 2.03，即 BT.2408 的 203/100
```

- 黑仍映射到 0（幂函数在 0 处为 0）—— 这是它与「整体加偏移」的关键差别，`t_hdr_bt2408` 第一条断言守着；
- 白正好落在 `head`，所以纸白落在 203 cd/m² 的定义点；
- 与缺省 knee 方案的实测差异：线性 0.18 → knee ×1.0000 / bt2408 ×1.5696；线性 0.50 → ×1.0000 / ×1.8295；
  线性 1.00 → ×2.0000（head=2）/ ×2.0300。也就是说 bt2408 把整幅按幂函数抬起来，knee 只抬纸白以上。

**自觉偏离文献的一处**：BT.2408 的 gamma 作用在信号（逐通道）上，逐通道做幂运算会改变彩度
（`t_hdr_bt2408` 里有一条对照断言量化了这一点：通道比从 5.00 变到 7.02）。本模块按 Rec.709 亮度算
等比倍数再乘 RGB，与 `s2a_hdr_itm_rgb` 同口径 —— 代价是与文献的字面结果不完全一致，收益是「灰恒为灰、
色相不跑」。两条路都保留：knee（缺省，保住 SDR 观感、只顶高光，接近 RTX HDR）与 bt2408
（官方定义的「把 SDR 放进 HDR 容器」）。

## 8 2024–2026 复核：输出侧的 EDR 上界与参考白（本轮新增）

前 7 节都在「内容侧」——怎么把 SDR 画面变成更亮的线性值。这一轮补的是**输出侧**的官方口径，
它直接决定了我们该往 16F 画布写多大的数。

### 8.1 Apple：Performing your own tone mapping（Metal / EDR）

- 出处：<https://developer.apple.com/documentation/metal/performing-your-own-tone-mapping>
- 原文要点（逐句核对过）：EDR 画布下 **`1.0` 对应 SDR 的最大亮度**，不等于面板峰值；
  面板更亮时「additional range (headroom) may be available」——100 nit 的 UI 亮度 + 400 nit 面板
  ⇒ 值可以到 `4.0`；同一块面板把 UI 亮度调到 200 nit ⇒ `1.0` 是 200 nit、`2.0` 是 400 nit。
  **「The actual upper bound … may vary from moment to moment」**，要读当前的屏幕 EDR headroom
  （iOS：`UIScreen.currentEDRHeadroom`；macOS：`NSScreen.maximumExtendedDynamicRangeColorComponentValue`）；超过上界的值由系统钳位；
  用户调亮时「expect midtones to increase and highlights to be compressed」。
- 对应实现：
  - 已经有的：`host_adapt.m` 读 `potentialEDRHeadroom`，`>= 1.0` 才建 16F 画布（`S2A_HDR_HEADROOM`）；
    运行期上界逐帧读取屏幕当前 EDR headroom（iOS `UIScreen.currentEDRHeadroom` / macOS
    `NSScreen.maximumExtendedDynamicRangeColorComponentValue`，经 KVC 与 `respondsToSelector:` 守卫，
    未声明 `wantsExtendedDynamicRangeContent` 时锁 1.0）。
  - **本轮补上的**：起局时读一次是不够的。`nvn_mtl.m` 新增 `hdr_head_tick()`，在 Metal 3 的
    `present_blit` 与 Metal 4 的 `present_blit_m4` 两个入口每帧读一次屏幕 EDR 上界，变化超过 **1% 死区**才更新
    `s2a_hdr_head_live`（`nvn_hdr.h`），上屏 buffer 的 `knee.z` 与 CPU 侧 `s2a_hdr_itm_params()`
    读同一个值。否则用户在游戏里拖亮度、或把窗口拖到另一块屏，画面会一直按旧 head 扩张
    （过亮或欠亮）。
  - 不做：把内容改成「场景参考的绝对亮度」（Apple 说的是 display-referred 上界，我们按上界
    等比扩张即可）；对超上界的值做我们自己的色调映射——系统已经钳位，再做一次等于双重映射。

### 8.2 Microsoft：Advanced Color / HDR（Win32）

- 出处：<https://learn.microsoft.com/windows/win32/direct3darticles/high-dynamic-range>
- 原文要点：桌面合成用 **FP16 + scRGB（BT.709/sRGB 基色 + 线性 gamma）**，值可以超出 `[0,1]`；
  **SDR 参考白nominal 是 80 nit，桌面 HDR 显示器通常设到约 200 nit**；
  scRGB `(1,1,1)` = 80 nit 的 D65 白；应用要把 SDR 内容乘 `SdrWhiteLevelInNits / 80` 才不至于偏暗；
  目标是 SDR 屏时 `[0,1]` 之外的值被数值钳位。
- 对应实现：我们的 `head` 与 BT.2408-8 §5.1 的「100 cd/m² → 203 cd/m²」是同一件事的两种说法
  （203/100 = 2.03，与 `S2A_HDR_BT2408_HEAD` 一致）；`s2a_hdr_itm` 的「纸白以下不动、以上按
  smoothstep 抬」正是把纸白钉在 SDR 参考白上的做法。**不采纳**：改写成 scRGB 的绝对亮度语义
  （iOS 侧只有 `wantsExtendedDynamicRangeContent` + linear 色彩空间，没有 scRGB 的
  80 nit 基准，照搬会引入一层不必要的换算）。

### 8.3 仍未采纳、且有明确理由的方向（复核结论）

| 方向 | 出处 | 为什么不采纳 |
|---|---|---|
| ISO 21496-1 / Adaptive HDR gain map（2024，Apple/Adobe/Google 推动） | <https://webstore.ansi.org/standards/iso/isodis214962024> | 它是「SDR 基线 + 增益图」的**容器格式**，用于分发；我们是在设备上逐帧生成 HDR，没有分发通道，收益为 0。 |
| ICC `ADGC`（Adaptive Gain Curve）tag | <https://www.color.org/specification/ICC.1_Adaptive_Gain_Curve.pdf> | 同上，属于色彩管理/分发侧；iOS 的 EDR 路径不接受 per-frame ICC。 |
| HDR10+ / SMPTE ST 2094 动态元数据 | 见 §5 | 本地出图，没有元数据通道。 |
| 神经网络反色调映射 | 见 §5 | 无推理引擎与权重。 |

## 9 2026-11 复核：BT.2446 方法 B / C——「把 HDR→SDR 曲线反过来用」行不行

§2 用的是 **方法 A**（分段二次 + 感知 log 域）的**逆**，并有 `docs/CURVE-REF.md` 的数字对照。
另外两个方法常被提议「反用」，这里把结论写清楚，免得下次重复调研。

### 9.1 方法 B（简化 HLG，§5）

- 原文要点（参考实现注释逐字）：**291 cd/m² 上限**，「for live broadcast content that may contain
  large over-exposed/clipped areas」；**「90% SDR maps to 75% HLG (HDR Reference White)」**，
  所以对已剪顶的素材不会放大；按 BT.2408 Annex 10（NBCU hybrid-log）对齐。
- 对我们的意义：它的 HDR 参考白同样是 **203 cd/m²**（75% HLG @ L_W=291），与 BT.2408-8 §5.1 的
  「100 → 203」、我们的 `S2A_HDR_BT2408_HEAD = 2.03f` 三方一致 ⇒ 我们缺省不是「偏激进」，
  而是标准放置；方法 B 的上限 291 nit（2.91×）比我们宽。
- **不采纳**：它定义在 **HLG 信号域**（BT.2020 + HLG OETF/OOTF），本补丁的输入是 sRGB 编码的
  8 位 SDR 帧，没有 HLG 传输函数；要照搬就得先造一条 HLG 编码链，等于把「设备上逐帧」换成
  「按播出链路推断」，反而引入无法用现有断言守的假设。

### 9.2 方法 C（4 参数分段线性+log，§6）

- 原文要点：`Y_SDR = k1·Y_HDR`（`Y_HDR < Y_ip`）、`k2·ln(Y_HDR/Y_ip) + k4`（其余），
  `Y_ip = k4/k1`（C0 连续推出）；缺省 `k1=0.83802, k2=15.09968, k3=0.74204, k4=78.99439`，
  由 BT.2408 Annex 4 的**皮肤色**分析拟出，「HDR Reference White mapping to 96% SDR」；
  卖点是 **TMO 与 ITMO 互为精确代数逆**，供 gain-map 工作流与标准曲线检测用。
- 为什么不能直接反用：这套 k1–k4 是**为 1000 → 100 cd/m² 的下变换 + 皮肤色**拟合的；
  我们的目标峰值是 203–400 cd/m²、输入没有 1000-nit 语义。把缺省参数代进逆曲线，
  等于把「1000 nit 内容的形状」贴到「SDR 白只有 203 nit」的屏上，中调会被抬得比任何放置
  规范都狠。要正确用就得**重拟合 4 个参数**——但方法 C 的权威产出是 TMO 侧（HDR→SDR），
  公开数据里没有「SDR→HDR 放置」的验收曲线，重拟合之后我们**无法用断言守住**它是否更对，
  只会多一条无法验证的分支。
- **不采纳**（保留为「已知可选路径」）：`s2a_hdr_itm` 的 L1b 放置（纸白不动、以上 smoothstep）
  有 BT.2408-8 §5.1 与 BT.2446 方法 A 高光段的双重支撑，且每条性质都有断言；方法 C 反用
  拿不到同等强度的支撑。

### 9.3 结论一句话

方法 A 逆**采纳**（只取高光段形状，中低亮度刻意 ×1.0000，见 §2 与 `docs/CURVE-REF.md`）；
方法 B 用来**交叉验证参考白 = 203 nit**；方法 C 只在需要「精确可逆曲线」的 gain-map 场景
才有意义，本补丁不做分发，故不采纳。

## 10 2026-07 第三方实测：RTX HDR 到底改了什么（HDRgameAnalysis）

来源：<https://hdrgameanalysis.com/rtx-hdr>（2026-07，玩家视角实拍对比，不是 NVIDIA 官方文档）。
选它的理由：官方只说「per-frame analysis + neural network」，能对上我们旋钮的是这类实测。

它记下的事实（对我们的取舍有直接约束）：

| RTX HDR 的行为 | 对我们的意义 |
|---|---|
| 逐帧跑一个**神经网络**（Tensor core），开销显著 | 手机上跑不起；我们只做闭式直方图统计（实测每帧 0.57 µs，见 `docs/COST.md`） |
| **不能恢复** SDR 里已被裁掉的高光细节与颜色，只能「重分配并扩张幸存下来的亮度」 | 我们必须同样声明：本补丁不 de-clip。没有引擎内数据/学习先验就没法凭空造细节 |
| 会**主动补中间色阶**（减轻 banding） | 我们做不到「补阶」：8 位出屏的色阶只有 256 级；16F 路径不丢，但显示器仍是 8/10 位 |
| Peak Brightness **输出上限 1000 nit**，滑到 2000 只是把次级高光往 1000 拖（压平层次） | 我们的 `head` **不发明峰值**：只取 layer 声明的 EDR headroom（钳 `[1,4]`），不把次级高光强行拉到峰值 |
| Middle Grey = 纸白，**50 ≈ 203 nit**、25 ≈ 100 nit | 与我们 `S2A_HDR_BT2408_HEAD 2.03f`（100→203 nit）独立吻合——这是除 BT.2408 之外的第二个出处 |
| Contrast 提高会同时抬高中间调、压暗上层阴影，并**联动纸白** | 我们的「纸白 + 拐点」两段式正是为解耦这件事：纸白由 L1b 定，扩张只发生在纸白之上 |
| 默认设置**过饱和**是已知问题，作者建议手动降 | 我们缺省 `S2A_HDR_SAT=1.0`、`S2A_HDR_P3=0`（颜色逐位不动），要饱和度得显式开，见 `docs/FIDELITY.md` |

结论一句话：RTX HDR 的「形状」与我们的 L1b 同源（纸白 203 nit、只扩张不 de-clip、峰值受显示器约束），
差别在三处——它有学习先验（能猜中间调以上）、它会补色阶、它默认过饱和。这三处我们都选择了保守侧，
理由分别是：手机无 Tensor 预算、补阶在 8 位出屏上无意义、过饱和属主观偏好不该做默认。
