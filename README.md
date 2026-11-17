# hdr-auto — 逐场景自动 SDR→HDR（S2Arm 出图层）

给 S2Arm 这类 iOS/NS 壳在**出图这一层**做自动 SDR→HDR：不改游戏、不读游戏内存，只看最终画面。

> **English summary.** Per-frame automatic SDR→HDR for the *present* stage of an iOS/NS game shell — no game
> patching, no memory reading, only the final frame. A GPU pass builds a 256-bin log2 luminance histogram of the
> presented frame; the diffuse white (highest bin holding ≥0.5% of pixels) and a black-clip + scene-aware
> smoothing stage derive two knee points, and the present shader expands everything above the knee toward the panel's
> EDR headroom while keeping BT.2087 hue and paper-white luminance fixed. Nine C test suites (458 assertions),
> a reference CLI plus 20 self-checks run on plain Linux — no Xcode, no device needed. Patch applies cleanly on
> upstream `d5cbf7ff53f7` (2026-11-17). Everything is documented in Chinese; code identifiers and comments are bilingual
> enough to follow the math.
>
> **状态 / Status:** 算法层、C 侧接线、ObjC 语法闸门、上屏 MSL 编译均已通过；**未**做链接级与真机验证。
> 真机设备本身可达（装/跑/抓日志都通），卡在「本机编不出 ipa」——盘点与成本见 `docs/DEVICE-PLAN.md`。
> See `docs/DESIGN.md` §7.

- `src/nvn_hdr.h` — 出屏侧：sRGB→线性、保色相增益曲线、EDR 层接线
- `src/nvn_hdr_auto.h` — L0：256 格 log2 直方图 → 99.5 分位 → knee，含平滑与死区
- `src/nvn_hdr_auto_l1.h` — **L1（本补丁新增）**：漫反射白点 + 黑场截断 + 场景感知平滑
- `src/nvn_hdr_hist.inc` / `src/hdr_hist.metal` — GPU 侧直方图与 kernel
- `src/t_hdr_hud.c` — HUD/孤立高光剔除：1% 的 HUD 白字不再掐死整帧的扩张（白点 0.7424 → 0.2381）
- `src/t_hdr_color.c` — 色域（BT.2087 sRGB→P3）与保亮度饱和度：灰恒等、色相守住、缺省逐位不变
- `src/t_hdr.c` / `src/t_hdr_auto.c` / `src/t_hdr_auto_l1.c` / `src/t_hdr_pipeline.c` / `src/t_hdr_color.c` / `src/t_hdr_quality.c` / `src/t_hdr_stability.c` / `src/t_hdr_banding.c` / `src/t_hdr_robust.c` / `src/t_hdr_hud.c` / `src/t_hdr_framerate.c` / `src/t_hdr_gpu_mirror.c` — **564 条断言（另有使用者命令 15 项、报告一致性 3 项）**，纯 C，Linux 可跑
  （最后一个是端到端：合成帧 → 直方图 → L1 → ITM，断言中性不偏色、色相不变、峰值不越 head、中间调不动）

`docs/VERIFICATION.md` 是**验证矩阵**：每条结论配一条命令和期望输出，并列出明确没验的四项（MSL 编译、ObjC 编译、链接、运行期）。
`./run_all.sh` 一条命令跑完全部本地闸门。

`scripts/extract_msl.py` 把 `nvn_mtl.m` 里两段上屏 MSL 字符串抽成独立 `.metal`，好在 CI 里真编译
（ObjC 语法闸门看不见字符串里的 MSL；两段都有 `vs`/`fs`，必须分文件）。

设计动机与业界对照见 [docs/SOURCES.md](docs/SOURCES.md)：RTX Video SDK / BT.2446 三个方法 / libplacebo /
SpecialK / BT.2087 / BT.2408-8 / Apple EDR / Microsoft scRGB，每条都写「建议什么、采纳与否、为什么」
（方法 B 只用于交叉验证「参考白 = 203 nit」，方法 C 因参数为 1000→100 拟合而不反用）。
算法与边界见 [docs/DESIGN.md](docs/DESIGN.md)。

## 怎么提 PR

```sh
./scripts/verify_upstream.sh     # 在上游当前头上从零验：打补丁 + 31 套 + MSL/常数 + 落点自检
./build/hdr_ref --synth hud --frames 8 --stat-line   # 打与设备日志同格式的一行（比对用）
./scripts/check_device_log.py --log <logcat> --expect t2   # 真机三格矩阵的判据（--selftest 自测）
./scripts/report_review.py           # docs/REVIEW.md：按这个顺序读（--check 挡漂移）
./scripts/report_fidelity.py         # docs/FIDELITY.md：真实照片上到底改了多少（纸白以下必须 0）
./scripts/report_options.py          # docs/OPTIONS.md：47 个常数 + 9 个环境变量（说明取自源码注释）
./scripts/check_counts.py            # 文档里的套数/断言数/fragment 字节数 必须等于实测
./scripts/check_upstream.sh      # 上游默认分支/tag 各在哪、补丁还能不能干净打上、是否纯增量
./scripts/publish.sh --preflight # 只读核对：上游是否又前进、正文 sha 是否过期、gh 能不能用
./scripts/publish.sh             # dry-run：只打印要执行的命令
./scripts/publish.sh --yes       # fork → 分支 hdr-auto/l1b → push → gh pr create（正文用 docs/PR.md）
```

`publish.sh` 拒绝三种操作：push 到 upstream、push 到 main/master、force push（分支名必须以
`hdr-auto/` 开头）。本机账号当前被停用（`gh api user` → `Sorry. Your account was suspended`），
`gh auth login` 换成可用账号后即可直接跑。

## 为什么不是「拉曲线」

固定曲线的自动 HDR 在游戏里会翻车在三处，L1 各给一个机制：

| 翻车 | 现象 | L1 |
|---|---|---|
| 一颗太阳当白点 | 夜景整体被当成「已经够亮」，HDR 感消失 | 白点按**面积**（≥0.5% 像素）取，不看极值 |
| 黑边/纯黑背景 | 分位被拽向暗端，暗部一片灰 | `-6 stops` 以下不计入统计 |
| 硬切镜头 | 平滑把新场景拖成 4–8 秒渐变 | 按 log2 重心差分级，>1.6 stops 直接跳 |

## 上游前进之后（`rebase_patch.sh`）

补丁只对 `patch/BASELINE` 里那个提交负责。上游比你手上的基线新时，先跑这个，别硬 apply：

```sh
./scripts/rebase_patch.sh <sha>     # 或 --head；成功→输出候选补丁；冲突→出逐 hunk 工单
./scripts/baseline_report.sh        # 只看距离：差多少提交、我们改过且上游也动过的文件与行数
```

**无损判据是往返，不是 `--dry-run`**：正向 `patch -p1` → 反向 `patch -R -p1` →
`git status --porcelain` 必须为空。为空才说明「我们的 N 处改动都进了正确位置，上游新增内容也没被覆盖」；
`--dry-run` 只证明上下文对得上。在基线上跑 `--check` 是恒等变换（候选补丁与主补丁 `cmp` 逐字节相同）。

冲突时自动生成 `docs/BASELINE-MIGRATION.md`：每个被拒的 hunk 一节，给出「我们要插入什么（diff）」
与「上游现在这段是什么（objc）」，行号按 hunk 的上下文锚点在新树里现搜（不用 `.rej` 里记的旧行号）。

实测两次（都是结构性合并，不是文本偏移）：

- **2026-11-16**：`0db51505` → `75eb4302`（371 个提交），上游给 `present_blit` 新增 `int screen`（多屏）；
  `nvn_mtl.m` 4/9 hunk 冲突、`nvn_present_fi.inc` 1/7 冲突，靠工单 + 手工补第 6 个实参完成。
- **2026-11-17**：`75eb4302` → `d5cbf7ff53f7`（**1082 个提交**），上游删掉了下区倒影功能
  （`ios/play_reflection.m` 152→64 行、`s2a_ios_reflection_drawable` 全树 0 命中）并去掉
  `present_encode_slot`/`_m4` 的 `mirror` 参数；`nvn_mtl.m` 3/9 hunk 冲突，工单 + 四处手工合并完成，
  同时把本补丁里那条改 `ios/play_reflection.m` 的 EDR 补丁**删掉**（载体没了）。

当前基线 d5cbf7ff53f7（`verify_upstream.sh` 全过）。上游仍在走，这是常态：**基线只对写明的那个提交成立**。

## 合规（可验证，不是承诺）

这个仓库是**公开投影**：只有补丁、自写源码、文档、工具、脚本，加上上游的 `LICENSE`/`NOTICE`。
不含上游整文件、不含构建产物。**没有任何自动触发的 CI**：源码树里的 `.github/workflows/verify.yml` 只接受 `workflow_dispatch`，而且默认**不同步进投影**（推它要求 PAT 带 `workflow` scope，不为一个 CI 文件去扩权；需要时用 `SYNC_CI=1 ./scripts/sync_release.sh`）。改回自动触发会让 `./run_all.sh` 在 `0w15/5` 变红。
逐条纪律与实名事故复盘见 [docs/GITHUB-COMPLIANCE.md](docs/GITHUB-COMPLIANCE.md)，自检：`./scripts/check_projection.sh`。

维护者把工作树同步到投影时不该手抄（会漏、会多）：

```sh
./scripts/sync_release.sh /path/to/projection --commit "说明"   # 白名单复制 → 投影自检 → 提交
```

白名单而不是通配，是踩出来的：`cp -r src/*.inc` 曾把上游整文件 `nvn_present_fi.inc` 拷进公开仓库。

## 打补丁

目标基线：S2Arm `main` 的 **d5cbf7ff53f7**（`d5cbf7ff53f75b6132b17ce3a442d431bbbc8726`，2026-11-17；v0.4.0 `dcd05e5` 之后的提交；上一基线 `75eb430240da`、`0db515053e07` 的历史实测仍保留在 docs/PR.md）。
注意上游 tag 仍停在 v0.4.0 而 `main` 已前进数百个提交，本补丁对**这一个提交**验证通过，上游再动需重跑 `./scripts/verify_upstream.sh <新 sha>`。
0.4.0 上游**没有**任何 HDR 代码（`nvn_hdr*` 全缺、全树 `S2A_HDR` 零命中），所以这个补丁是纯新增。

```bash
git clone https://github.com/jhaizhou-ops/S2Arm.git s2arm && cd s2arm
git checkout d5cbf7ff53f75b6132b17ce3a442d431bbbc8726
git apply -v /path/to/hdr-auto/patch/hdr-auto-l1.patch
```

## 验证

```bash
./verify.sh /path/to/s2arm     # 打补丁 → 编三套单测 → 跑
./run_all.sh /path/to/s2arm    # 全部闸门：31 套 904 项 + 落点 + MSL 静态 + 10 份报告对账 + ASan/UBSan
./scripts/sanitize.sh          # 只跑内存/UB 闸门（ASan+UBSan，halt_on_error=1，约 15 s）
./scripts/check_callsites.py    # 调用点实参个数（上游改签名时 patch 只报定义，调用点全靠这条）
./scripts/check_device_coverage.py  # 设备侧代码登记表（哪些有对账、哪些写明不做的理由）
./scripts/report_fi_domain.py --check  # 补帧的运动估计与 HDR 是否在同一个域
./scripts/check_format_assumptions.py <树>  # 换格式后上游那些隐含假设（pitch/值域/sRGB）还成立吗
```

改过 `src/` 之后**必须**重建补丁，否则所有验证都在验旧代码（踩过：补丁静默回退，全部结论作废）：

```bash
vim src/nvn_hdr.h              # src/ 是唯一真相源
./scripts/make_patch.sh /path/to/s2arm --force   # 回写树 → 重建 patch/hdr-auto-l1.patch → 自检
./run_all.sh /path/to/s2arm
```

本机实测（干净 0db515053e07 树，`cc -O2 -Wall -Wextra`）：

```
✓ t_hdr：HDR 出屏线性化 + 扩张曲线 134 项 → 全过
✓ t_hdr_auto：自适应 knee（量化 / 分位 / 边界 / 死区 / 平滑）32 项 → 全过
✓ t_hdr_auto_l1：白点 / 黑场截断 / 场景平滑 92 项 → 全过
✓ t_hdr_pipeline：内容侧端到端（合成帧 / 色相 / 峰值 / 稳定性）68 项 → 全过
✓ t_hdr_color：色域 / 饱和度 / 与 ITM 的组合 62 项 → 全过
✓ t_hdr_quality：四场景量化 + L1/L1b 纸白对照 + 硬切收敛 23 项 → 全过
✓ t_hdr_stability：60 帧噪声 1%→20% 下 knee 抖动 13 项 → 全过
✓ t_hdr_banding：8 位台阶放大 / 16F 余量 / head 扫描 19 项 → 全过
✓ t_hdr_robust：下标 / 空直方图 / uint32 溢出 / 垃圾上一帧状态 / 20 万次 fuzz 15 项 → 全过
```

补丁本体：`patch/hdr-auto-l1.patch`，**42 文件 +6889 行**（−26），`sha256 3fa19b237182e0833116122e32276f53e31256960b8e5b01f6e4a3111dbbce5c`。

补丁同时修了一个会直接编译失败的缺口：`nvn_hdr_hist.inc` 要 `#include "nvn_hdr_hist_msl.h"`，而该基线的 `graphics/nvn/Makefile` 里**没有**生成它的规则（`GENH` 只列 `gal_tess_msl.h`）。补丁补上 `GENH` 条目与 `embed_msl.py` 规则，否则这条链路一被 include 就断。

## 状态（诚实标注）

| 项 | 状态 |
|---|---|
| 参数层（纯 C） | ✅ **904** 条断言（31 套）全绿，可在 Linux/CI 复跑；含四场景 nit 量化（`t_hdr_quality`） |
| 补丁可打性 | ✅ 干净 `d5cbf7ff53f7` 树 `git apply` rc=0 |
| MSL 语法 | ✅ CI `hdr-check` 用 `xcrun -sdk iphoneos metal -std=metal3.0 -c` 真编译（第一次就抓出 MSL 无 `double`，已改 float） |
| 嵌入规则 | ✅ 本机 `embed_msl.py` 产出 7,756 B 头文件 |
| GPU 侧接线 | ✅ `present_encode_slot` 每 8 帧（`S2A_HDR_HIST_EVERY`）跑 `clear→add→hdr_hist_l1` + blit 1 KB 直方图回 CPU；判定全在 CPU（真直方图），`float4` 只留作对账日志 |
| 层变 16F + EDR | ✅ `host_adapt.m` 按 `S2A_HDR_OUTPUT` 选格式并声明 EDR（`S2A_HDR_FMT=1` 只换格式不声明 EDR，用来分辨黑屏归因） |
| 16F 上屏 ITM | ✅ `present_pipeline` 按目标格式选管线：16F 用 `HdrCrop`（rect+edge+knee+extra）走线性化+扩张，8 位保持直通 |
| L1b（纸白不动） | ✅ **已接进上屏路径**：1 KB 直方图经 blit 回 CPU，`s2a_hdr_auto_l1b_step_fused()` 与单测/`hdr_ref` 同一份代码（纸白 ×1.00、纯白 ×head、星空从 0.02 起） |
| HUD 剔除 | ✅ 1.2% 的 HUD 白字能把 98.8% 画面的扩张掐死（纸白 0.74 vs 0.24）；`s2a_hdr_sdr_white_bulk` 按簇剔除，平台区 `max_frac` 3%–20% / `gap` 2–5 bin 全部 12/12 |
| 鲁棒性 | ✅ NaN/Inf/`uint32` 拉满/垃圾上一帧状态 + 20 万次 fuzz：无 NaN 外泄、下标不出界（修掉 3 处入口缺守卫） |
| 带阶 | ✅ 实测：中间调台阶放大 **1.000×**，最坏 **3.35×**（高白点时 6.6×）；16F 相对余量 96×，**不需要输出抖动** |
| 时间稳定性 | ✅ 实测：噪声 1%→20% 下 knee 逐帧变化 **0.00%**（量化+smoothing 足够，不需迟滞）；噪声越重白点上移越多（20% 时 +0.55 stop） |
| 色域 / 饱和度 | ✅ `S2A_HDR_P3`（BT.2087 sRGB→P3，按行归一化保证灰恒定）与 `S2A_HDR_SAT`（保亮度）；两级都带恒等短路，缺省逐位等同没有这两个旋钮 |
| MSL 编译 | ⚠️ **只编过两处**：`hdr_hist.metal`（run 43727751120，去 `double` 后 `air: 8736 B`）与 8 位直通上屏 shader。**HDR fragment（3555 B）与 16F 上屏路径从未过 `xcrun metal`**（GitHub 账号停用后 CI 不可触发），本地只有 `scripts/check_msl.py` 72 项结构守卫（4 个 MSL 文件）——2026-11-17 起把 `nvn_present_fi.inc` 里补帧的两段（`fi_mv_setup` 的 down/match/spread 与 `fi_copy_setup` 的缩拷）也纳进来了：它们此前既不进静态守卫、也不进真编译 |
| ObjC 编译 | ✅ `hdr-check` 第 6 步：生成 gen 头后用 `clang -fsyntax-only -fobjc-arc -arch arm64 -isysroot iphoneos` 编 `nvn_mtl.m`（16k 行）与 `host_adapt.m`，两者 **rc=0**（run 43728394548）。这是语法/类型级验证，不是链接与整包构建 |
| 与 0.4.0 补帧共存 | ✅ 补帧链四处格式参数化（含 `fi_commit` 的 `present_pipeline`），MetalFX 不收 16F 时退回 8 位并告警（run 43729167355 语法闸门 rc=0） |
| 16F 下 AA / MetalFX 放大 | ⬜ 内部图仍是 8 位，当前跳过并打黄字 |
| 诊断行 | ✅ `S2A_HDR_LOG=1` 每 600 帧打一行 `HDR stat: fmt=16F head=2.00 knee=… white=… avg=… scene=… frozen=… itm=…`，格式由 `t_hdr_stat` 19 项钉死（真机三格矩阵靠它取证） |
| 真机画质/性能 | ⬜ 未上机——设备其实可达（iPhone SE 3 / iOS 27.3，装、跑、抓日志三条通道都通），缺的只是 ipa：本机无 iOS SDK/ld64，编不出来。盘点与四条路线的成本见 `docs/DEVICE-PLAN.md` |

## 测试抓出来的两个真 bug（已修）

端到端测试不是「补个覆盖率」，它把两处钳位错误照出来了：

| 症状 | 根因 | 修法 |
|---|---|---|
| 满屏 UI 白的场景整体过曝（白直接 ×2） | L1 沿用 L0 的 `KNEE_MAX = 1.0`，白点 1.0 时应得 knee 1.25 被钳回 1.0，于是 1.0 落到拐点顶端吃满 head | 新增 `S2A_HDR_KNEE_L1_MAX = 2.0` |
| 夜景/太空场景完全不扩张（L1 等于没开） | `KNEE_MIN = 0.25` 高于暗场景白点（≈0.03），knee 被顶到 0.25，整帧落在 `knee_lo` 以下 | 新增 `S2A_HDR_KNEE_L1_MIN = 0.01`，只兜底不参与观感取舍 |

第二条尤其关键：L1 存在的理由就是让暗场景也能扩张，钳位一卡就等于白做。

## 与 0.4.0 自带补帧共存（原来会打架，已参数化）

0.4.0 的 MetalFX 补帧把四处钉死在 BGRA8；HDR 一旦把画布换成 `RGBA16Float`，
补出的那一帧会被画进 16F 画布（管线/画布不匹配），而且 `present_pipeline` 是单槽格式缓存，
主路径与补帧会互相把缓存打回对方格式（每帧两次重编）。

现在的做法：

| 位置 | 处理 |
|---|---|
| copy 管线的 `colorAttachments[0].pixelFormat` | 跟 `f->cfmt` |
| MetalFX 描述符 `colorTextureFormat` / `outputTextureFormat` | 先试 16F，`@try` 建不出来就退回 BGRA8 + 一次黄字告警 |
| `in_tex[3]` / `mid_tex` | 跟 `f->cfmt` |
| `fi_commit` 的上屏管线 | `present_pipeline(dr ? dr.texture.pixelFormat : f->cfmt)` |

副作用（明写）：16F 画布下**暂不叠加** AA 与 MetalFX 空间放大，因为这两条链的内部图与上屏管线仍是 8 位。

## 许可

与本仓库其余补丁一致：MIT。

## 用法：把一张图转成 HDR 预览

```sh
cc -O2 -w -I src tools/hdr_convert.c -lm -o hdr_convert
./hdr_convert shot.ppm shot_hdr.ppm --head 2            # 默认：纸白不动、裁切点=SDR 白
./hdr_convert shot.ppm shot_hdr.ppm --head 2 --p3 1 --sat 1.2   # 加色域扩展与饱和度
./hdr_convert shot.ppm shot_hdr.ppm --head 2 --map bt2408       # BT.2408 的 display-referred 放置方式
python3 scripts/to_png.py shot_hdr.ppm shot.png --head 2        # 转成能直接看的 PNG
```

输出是 **16 位线性 PPM，值域 [0, head]（65535 = head）**，不是 sRGB 编码——因为 `RGBA16Float`
上屏写的就是线性能量，>`1.0` 的部分由 EDR 显示器还原。`scripts/to_png.py` 做的是**预览映射**
（Reinhard 压缩到 8 位），只为让你在普通屏幕上看出「哪些区域被抬起来了」，不是真机画面。

四条硬性质由 `python3 scripts/check_convert.py` 守着（三张真实照片上 15 项）：

| 性质 | 判据 |
|---|---|
| 值域 | 所有样本 ≤ head，无负值（否则上屏裁高光） |
| 纸白以下不动 | `y < knee_lo` 的像素逐位等于输入（±1 LSB 量化） |
| 色域/饱和度真生效 | `--p3 1 --sat 1.2` 改变 >1/4 通道，且仍不越 head |
| 抖动只动最低位 | 逐像素差 ≤ 2 LSB |

## 时间行为（真实照片 60 帧序列）

单帧判得对不代表连续播得稳。`scripts/report_video.py` 用 ffmpeg 把一张真实壁纸推成 60 帧
（30 fps 2 秒慢推），交给 `tools/hdr_seq` 跑整段——它走的就是上屏用的
`s2a_hdr_auto_l1b_step_fused`，所以量到的就是真实过渡。三条判据：

| 指标 | 上限 | 理由 |
|---|---|---|
| 单帧 `knee_lo` 相对变化 | 2% | 平滑时间常数应吸收内容微动 |
| 慢速运动的最大场景权重 | 0.05 | 平移不是切镜，误判会让画面先暗后亮 |
| 方向翻转次数 | 2 | 允许数值抖动，不允许来回呼吸 |

三张系统壁纸全过（`docs/TEMPORAL.md`）。这张表抓出过一个真 bug：直方图按 0.0547 stop 分格，
内容微动让白点在**相邻两格**之间来回跳，`knee_lo` 被拽着走（Monument 11 次方向翻转）——
修法是 `S2A_HDR_WHITE_DEADBAND = 1.5 格` 的白点死区（`s2a_hdr_auto_l1b_step_fused_hz`）。

## 消费者测试（解包即用）

开发目录里 `build/` 有编译好的二进制、`patch/src.manifest` 有指纹，「在我这儿能跑」不等于
「别人解包后能跑」。`scripts/consumer_test.sh` 把交付包解到一个空临时目录，检查包里没有 `build/`，
再在里面从零跑一遍 `run_all.sh`（可选：带一个上游干净树，把补丁打进去跑 31 套单测）：

```sh
TARBALL=../hdr-auto-delivery/hdr-auto-<sha>.tar.gz ./scripts/consumer_test.sh          # 只验证包自检
TARBALL=... ./scripts/consumer_test.sh /tmp/v04-clean                                   # 再加单测
```

依赖只有 `cc` / `python3` /（可选）ImageMagick 与一个 upstream 树。已在 `hdr-auto-76de57d.tar.gz`
上实测通过。

## 真实内容验证

合成场景（city / uiwhite / dark / ramp / star）是可控的，但真实照片会带来它们没有的东西：
大块天空、雾、雪地、镜面反光、JPEG 噪声。`docs/REAL-CONTENT.md` 用三张系统壁纸核对两件事：

1. **不误伤**：HUD 剔除后的白点必须等于普通白点。自然照片里没有「孤立的一小撮亮像素」，
   一旦不等就说明剔除判据在照片上误伤——实测过一版「取主簇簇顶」的写法，在 Clouds 上把白点
   从 0.5278 抬到 0.8974（+0.77 stop），纸白被无谓抬高、扩张被削弱，已改成与普通白点同判据。
2. **不越界**：任何通道都不得超过 `head`（`clipped_frac` 为 0）。

复现：`python3 scripts/report_real.py`（需 ImageMagick；没有源图时报告写「跳过」而不是失败）。

## 量化结果与复现

### 色相：饱和度不再拧走颜色

`--sat` 是个「看起来简单、实际很容易做坏」的旋钮。把饱和度乘在「偏离亮度的量」上
（`y + (c-y)*s`）在 Lab 里**不保色相**：三张真实壁纸上 sat=1.6 时 p99 误差 7.8–10.7°、
sat=2.0 到 9.2–12.3°，Monument 的岩壁最吃亏（肉眼就是「天空变紫了」）。

现在默认在 **CIELAB 里缩放 `a*/b*`**（等价于 LCh 里乘 `C*`），色相角按构造不变：
实测中位 0.00–0.01°、p99 ≤0.43°、最大 ≤2.06°。穿出线性 RGB 负区时二分退回最大安全倍数，
所以不会为了「更艳」把通道压平。旧行为可用 `S2A_HDR_SAT_SPACE=0` 拿回。

对照表在 [docs/COLOR.md](docs/COLOR.md)（`scripts/report_color.py` 生成）。

上屏（Metal）里的饱和度已移植同一套 Lab 算式（见 [docs/COLOR.md](docs/COLOR.md) 末段）。

所有数字都在 [docs/QUALITY.md](docs/QUALITY.md)（量化）、[docs/ABLATION.md](docs/ABLATION.md)（四档策略消融）、
[docs/REAL-CONTENT.md](docs/REAL-CONTENT.md)（真实照片）、[docs/TEMPORAL.md](docs/TEMPORAL.md)（时间行为）、
[docs/COLOR.md](docs/COLOR.md)（色相）、[docs/SEQUENCE.md](docs/SEQUENCE.md)（转场：硬切 / 交叉淡入 /
淡到黑 / 黑场 / 淡入）、[docs/COST.md](docs/COST.md)（每帧 CPU 开销）、[docs/GALLERY.md](docs/GALLERY.md)（真实照片对照图）、
[docs/CURVE-REF.md](docs/CURVE-REF.md)（与 ITU-R BT.2446-1 方法 A 的定量对照）、
[docs/COLOR-IMAGE.md](docs/COLOR-IMAGE.md)（真实照片上的逐像素 CIELAB 色保真：缺省档 0.00°、
tuned 档最大 3.15°、近中性 Δab <0.5）里，
**全部由脚本生成、不是手抄**：

```bash
./run_all.sh                  # 0/5 核对 QUALITY.md；0h/5 核对 SEQUENCE.md；0i/5 核对 COST.md；0j/5 核对 GALLERY.md；0r/5 核对 CURVE-REF.md；0w/5 核对 COLOR-IMAGE.md；0l/5 画幅、0m/5 分箱对账自带断言
./scripts/check_upstream.sh    # 只读：补丁还能不能打到上游最新 tag
python3 scripts/report.py     # 改完算法后重新生成
```

表里包含：六个场景的白点 / 重心 / knee / 亮度增益、L1 与 L1b 的对照、head 扫描（纸白不随峰值漂）、
64 帧淡入的逐帧跳变、以及 8 位与 16F 输出下的平台阶与可见带。

## 许可证

本目录是 S2Arm（Apache-2.0）的**补丁与独立参考实现**，不含上游源码：`patch/hdr-auto-l1.patch`
打在上游 `d5cbf7ff53f7` 的干净树上。见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。

`.github/workflows/verify.yml` 是包内 CI：clone 上游干净树 → 跑全部闸门 → 再打补丁做
**MSL 真编译 + 常数对账 + ObjC 语法检查**（本机 Ubuntu 做不到的三件事）。

它**只能手动触发**（`workflow_dispatch`），因为 2026-11-17 之前的 GitHub 号
`Kippythisway` 带 push 触发、并发又没上限，一天起了 114 台 macOS 虚机被停用。
默认它不随投影同步（GitHub 拒绝用没有 `workflow` scope 的 PAT 创建 workflow 文件）；
要给 token 加上 `Workflows: Read and write` 之后用 `SYNC_CI=1 ./scripts/sync_release.sh <投影>` 带上。
现在：
`concurrency` 保证同一 ref 只跑一个、`timeout-minutes: 20` 兜底、`scripts/check_workflow_guard.py`
把「不许出现 push/PR/schedule 触发、不许出现 xcodebuild、写死的基线必须等于 `patch/BASELINE`」
做成闸门（`0w15/5`）。手动跑一次约 10 分钟，公开仓库不消耗配额。
