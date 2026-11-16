# 给上游的 PR 说明（可直接粘贴）

> 目标仓库：`jhaizhou-ops/S2Arm`（Apache-2.0）。本目录不是上游源码，而是**一个补丁包**：
> `patch/hdr-auto-l1.patch` 基于 `main` 的 `75eb430240dae472ec834f98b47194d7ff2ff7e5`（2026-11-16）制作。
> 更早的版本基于 `v0.4.0`（`dcd05e5fa84c8c5c68b6ba66e3b1c6de016636ae`），基线迁移时把 8 处补帧改动
> 重放到上游新版 `nvn_present_fi.inc` 与 `nvn_mtl.m`（上游在这两个文件里新增了 `fi_may_take_drawable()`、
> `s2a_cpatch_present_aspect()` 自适应画幅等，共 256 行），重放后反向摘除逐字节还原为上游原文。
>
> 合入目标应是默认分支：已实测它能**干净地打到 `main`（`0db515053e07`）**，并在那棵树上跑通
> 31 套单测 904 项 + 落点自检 49 项 + MSL/常数核对。上游仍在自己前进（同日实测通过的还有
> `6d01b7897c0d`、`8874b92d071f`、`2237da8ca323`、`3f549467b00d`、`e77fca7b949b`、`b9bd76a202ab`、
> `1c31fdc0`、`ddfef415`、`2b5aca92`）。上游代码里 HDR/EDR 意图标记
> 为 0，本补丁是纯增量——`./scripts/check_upstream.sh` 每次都复核这两件事。

## 标题

`graphics/nvn: 自动 SDR→HDR（类 RTX HDR）——直方图驱动纸白/拐点 + 16F 上屏`

## 一句话

S2Arm 现在把 SDR 内容直接按 8 位送上屏。这个补丁让内部上屏层变成 `RGBA16Float` + EDR，
并用每帧直方图**自动**决定「纸白在哪、拐点在哪」：SDR 里本来就白的地方保持原亮度，
只把 SDR 之上的高光空间用来扩张，UI/HUD 不会把整幅画面按死。

## 动机

NS2 的《S2Arm》把主机画面直接 blit 到 layer，`host_adapt.m` 里写死
`MTLPixelFormatBGRA8Unorm`。于是：

- 亮部一律顶在 1.0，没有高光余量；
- 想要「HDR 观感」只能整帧乘个系数，UI 会一起过曝。

参考实现（NVIDIA RTX Video SDK / libplacebo `inverse_tonemapping` / SpecialK HDR Retrofit）
的共同做法是：**先从画面统计里判出「漫反射白」**，再以它做参考点决定扩张曲线。
本补丁把这条链路做成每帧一次 GPU 直方图 + 16 字节回读，不引入 CPU 全帧扫描。

## 做了什么

| 文件 | 内容 |
|---|---|
| `nvn_hdr.h` | sRGB⇄线性、ITM（smoothstep 拐点）、Display P3 混色、保亮度/保色相饱和度 |
| `nvn_hdr_auto.h` / `nvn_hdr_auto_l1.h` | 直方图 → 白点 / 重心 / 暗场占比 / 场景权重 → 目标拐点（两条算法：L1 与 L1b） |
| `shaders/hdr_hist.metal` | 逐像素 log2 亮度直方图（256 格，atomic），外加一个只回读 16 B 的收尾 kernel |
| `nvn_hdr_hist.inc` | 每帧编码 + blit 回读 + 平滑（帧率归一化、白点死区、场景切换检测） |
| `nvn_mtl.m` | 16F 上屏管线、`HdrCrop{rect,edge,knee,extra}` 16 float 常量、knee/head 接通 |
| `nvn_hdr.h` 的 `s2a_hdr_status_line()` | `S2A_HDR_LOG=1` 时的诊断行（600 帧一行，字段与小数位固定，供真机远程判读） |
| `host_adapt.m` | layer 改 `RGBA16Float` + `wantsExtendedDynamicRangeContent`（EDR 上界运行期由 `nvn_mtl.m:16311 hdr_head_tick()` 每帧读 layer 的 `maximumExtendedDynamicRangeColorComponent`，host 不重复读数） |
| `nvn_present_fi.inc` | MetalFX 补帧链的四处 8 位硬编码参数化（HDR 下补帧不再破坏格式） |

缺省**全关**（`S2A_HDR_FMT` / `S2A_HDR_OUTPUT` 都未设时行为与上游一致），
逐项开关见 `README.md`。

## 怎么自证

```sh
./scripts/check_upstream.sh          # 补丁还能不能打到最新 tag
./run_all.sh <干净 v0.4.0 树>         # 打补丁 + 三十套单测 + 嵌入 + 常数一致 + 取 MSL
./scripts/report_options.py --check   # 47 个常数 / 9 个环境变量 与源码注释逐一对应
```

- 三十套单测 **885 条断言**，全部纯 C、`-lm` 即可编，无第三方依赖；
- 诊断行格式有 19 条断言（`t_hdr_stat`）：字段名/小数位/截断语义/非有限值打 `n/a`——
  真机三格矩阵的取证全靠这一行，所以它本身也当契约来测；
  参考实现 `hdr_ref --stat-line` 打的是**同一份格式化代码**，所以「设备日志」与「本机复算」
  可以逐字符 diff（`tools/ref_check.sh` 里另有 3 项交叉校验：前缀/字段顺序、`knee=lo+delta` 与
  `--stats` 的 `knee=lo..hi` 一致、`--hudguard` 换白点后诊断行跟随）；
- 十份报告全部由脚本生成、`--check` 会因数字漂移而失败（防止手抄）：
  `QUALITY` / `REAL-CONTENT` / `TEMPORAL` / `COLOR` / `COLOR-IMAGE` / `ABLATION` /
  `SEQUENCE` / `COST` / `GALLERY` / `CURVE-REF`；
- 真实内容：三张系统壁纸 + 60 帧推镜序列 + 300 帧转场序列（硬切 / 交叉淡入 / 淡黑 / 淡入），
  判「纸白不动」「慢推不抖」「色相不偏」「淡出不越黑越亮」。

## 设备专属代码的本地对账（这类 bug 只有它在设备上跑才现形）

MSL kernel、ObjC `.inc` 这些只在设备上执行的代码，编译不进本地单测；为此把每一处**逐行翻成 C**
再用断言对账（`src/t_hdr_*_mirror.c`、`src/t_hdr_tick.c`）：

| 路径 | 对账方式 | 抓到的真 bug |
|---|---|---|
| `hdr_hist.metal` 分箱 | 256 个 8 位码值 + 256 个 bin 边界 + 20 万 log2 样本 | 黑像素口径不同（`lit_frac` 设备恒 1.0 / CPU 0.04 ⇒ 暗场逻辑在真机上从不执行）；`(int)inf` 是 UB |
| `hdr_hist_knee` 分位 | 像素总数 1..8,300,000 的阈值 + 4K 直方图 | `(uint)((float)total*0.995f)` 与整数口径差一格（1,515,877 个总数不同）⇒ `hi` 差 0.0547 stop |
| 上屏 `fragment` shader | 177,147 组（编码值³ × 拐点 × head × 饱和度 × 色域） | 退化拐点 `lo==hi` 时 shader 给 `×head`、CPU 给 `×1`（差整整一个 head）|
| 直方图回读控制流 | 节拍 / 双槽 / 代号作废 | 换分辨率后旧命令缓冲的回调改写新帧的 `knee` |
| 16F 降级 / 回头路 | 60 帧降级、退避重试探、恢复三件套 | 只有降没有升 ⇒ 拔掉外接屏后永久 8 位 |

这五处现在最大通道差分别是 `0.0000000`（上屏）、逐位相同（分箱 / 分位），两条控制流分别由 15 与 28 条断言钉住。

## 关键实测（详见 docs/）

- 满屏 UI 白：纸白增益 **×1.00**（旧 L1 是 ×1.57 —— 那正是「UI 过曝」的成因）；
- 纯白：×2.00（head=2 时到顶）；暗场星点：×1.77；
- 硬切：切镜权重 **1 帧**到 1.0（只比平均亮度会在同 APL 切镜上错判、要 26 帧；现加白点一路）；
  30 fps 转场序列里 60 帧处 knee `0.4665 → 0.3898`，单帧最大跳 0.0146、方向翻转 0；
  中等切镜（0.66 stop）拿 31% 权重、7 帧走完 95%（旧阈值 0.5/1.6 时只有 5.5%、要 24 帧）；
- 同场景 60 帧噪声 1–20%：逐帧 knee 变化 **0.00%**；
- 8 位链路的台阶放大最坏 3.35×，16F 输出余量 96×（不需要抖动）；
- 饱和度：Lab 缩放 p99 色相误差 **≤0.43°**（线性锚定最差 12.34°），上屏 shader 与 C 侧同算法；
- 淡出/淡入：淡出 30 帧里 `0.50` 线性增益恒 **×1.0000**（修前 ×1.000→×1.46，越黑越亮），
  跨黑场不跳、出暗区 1 帧到位；
- 每帧开销：控制器一次调用 **0.57 µs**（30/60/120 Hz 相同），占 120 Hz 帧预算 0.007%；
- 运行期 headroom：每帧读一次 EDR 上界（1% 死区），用户拖亮度/换屏后不再按旧 head 扩张。

## 基线迁移（2026-11-16：`0db51505` → `75eb4302`，371 个提交）

这次不是机械重放，是**结构性合并**——上游给上屏路径加了多屏参数，我们的注入点上下文全失效。

| 上游改动 | 我们要跟着改的地方 |
| --- | --- |
| `present_blit(…, BOOL flip)` → `(…, BOOL flip, int screen)`；`present_blit_m4` 同样 | 两个函数首行的 `hdr_head_tick();` 重新插；**`screen=1` 只在画进画布那一次**（`0` 是拷进 AA/放大中间图，必须原样拷满，否则 #1169 SMAA 中/高左右留黑） |
| `present_encode_slot` / `present_encode_m4` 重写（AA / MetalFX 分支结构变化） | `hdr_hist_tick(cb, src)`、`#include "nvn_hdr_hist.inc"`、`hdr_dst` 判定（16F 画布下 `fx=NULL`、不走 AA、告警一次）重新插进新结构，早退条件变成 `(!g_present_aa \|\| hdr_dst) && !fx` |
| `fi_job` 中间帧分支带上了 `screen` 实参 | 16F 下中间帧改走 `present_blit_m4(cb, mid, dm.texture, NULL, …, flip, 1)`（`mid` 是 MetalFX 在**未扩张**域插出来的值，用 `fi_pass` 纯拷贝会让补出的帧比真帧暗 head 倍） |

合并后必须核对的：`present_blit` / `present_blit_m4` 的**所有调用点**都带上了第 6 个实参（冲突列表不会报调用点，但不改就编译不过）。

验证：`check_landing.py` 49/49（按函数范围断言，不按行号）、`run_all.sh /tmp/v08` rc=0（31 套 904 项 + ASan/UBSan + 10 份报告对账），补丁 41 文件 / 6673 行 / `sha256 f2e996e9771bda94…`。

### 历史：`dcd05e5` → `0db51505`（机械重放，9 个 hunk 全带 offset、反向 `cmp` 逐字节相同）

## 基线迁移（上游前进后怎么跟）

补丁是**对某一个提交**验证的，`patch/BASELINE` 就是那个提交，且它是唯一真相源：

```sh
cat patch/BASELINE                       # 一行 sha + 日期
./scripts/verify_upstream.sh             # 默认验基线，可复现
./scripts/verify_upstream.sh --head      # 追上游当前头；打不上会打印诊断而不是普通失败
./scripts/baseline_report.sh             # 基线离上游 head 多远、哪些文件要重放、各多少行
```

为什么默认验基线而不是 head：上游一天能走几百个提交（2026-11-16 一天内 `2b5aca92`→`ddfef415`→
`1c31fdc0`→`b9bd76a2`→`6d01b789`→`e77fca7b`→`3f549467`→`2237da8c`→`8874b92d`→`0db51505`→`1a6eda79`）。
head 只说明「此刻」，而基线是「任何人 clone 这个 sha 都能复现」的承诺。追 head 是需要人工重放的
工程动作，不该混进日常验证。

实测的迁移成本（从 v0.4.0 `dcd05e5` → `0db51505`）：

| 上游改了我们也要改的文件 | 我们重放几处 | 怎么验 |
| --- | --- | --- |
| `nvn_mtl.m` | 9 个 hunk（HDR 上屏、管线缓存、headroom tick） | `patch --dry-run` 全 succeed；反向摘除后 `cmp` 逐字节等于上游原文 |
| `nvn_present_fi.inc` | 8 处（`cfmt` 参数化、16F 试探、uv 16 float、中间帧走 ITM） | 同上 |
| `host_adapt.m` | 2 处（16F 画布 + EDR 声明） | 同上 |
| `Makefile` | 1 处（`GENH` 生成规则） | 上游未动，无需重放 |

重放步骤（三个文件都一样）：

```sh
# 1) 取上游新版原文
git -C <新基线树> show HEAD:graphics/nvn/gfx/nvn/nvn_mtl.m > /tmp/up_mtl.m
# 2) 从旧补丁里抽出该文件的 diff，打到上游新版上（patch 会自动找偏移）
python3 - <<'EOF'
import re; t=open('patch/hdr-auto-l1.patch').read()
for s in re.split(r'(?m)^(?=diff --git )', t):
    if 'nvn_mtl.m' in s[:120]: open('/tmp/our_mtl.diff','w').write(s)
EOF
mkdir -p /tmp/replay/graphics/nvn/gfx/nvn && cp /tmp/up_mtl.m /tmp/replay/graphics/nvn/gfx/nvn/
cd /tmp/replay && patch -p1 -i /tmp/our_mtl.diff      # 看 Hunk #n succeeded at … (offset N)
# 3) 反向守卫：把我们改动反向摘掉，必须逐字节等于上游原文
mkdir -p /tmp/rev/graphics/nvn/gfx/nvn && cp graphics/nvn/gfx/nvn/nvn_mtl.m /tmp/rev/graphics/nvn/gfx/nvn/
cd /tmp/rev && patch -R -p1 -i /tmp/our_mtl.diff && cmp graphics/nvn/gfx/nvn/nvn_mtl.m /tmp/up_mtl.m
# 4) 写回 src/（src 是唯一真相源），改 patch/BASELINE，重建补丁，跑闸门
cp /tmp/replay/graphics/nvn/gfx/nvn/nvn_mtl.m <repo>/src/nvn_mtl.m
./scripts/make_patch.sh <新基线树> --force
python3 scripts/report_review.py && python3 scripts/report_options.py && python3 scripts/report.py
python3 scripts/check_counts.py && ./scripts/check_baseline.py && ./run_all.sh <新基线树>
```

第 3 步的反向守卫是这一轮唯一真正省时间的东西：`patch --dry-run` 说「succeeded with offset」
只证明上下文能对上，证明不了「我们的改动一处没丢」。反向摘除后 `cmp` 逐字节等于上游原文，
才说明重放是无损的（这一轮 `nvn_mtl.m` 的 9 个 hunk 偏移 10～202 行，反向 `cmp` 通过）。

## 两种放置方式，默认是保守那个

| 档 | 开关 | 数学 | 观感 | 改动面（三张真实照片实测） |
|---|---|---|---|---|
| 拐点（默认） | — | 纸白以下 ×1，纸白以上 smoothstep 到 `head` | 保住 SDR 观感，只把高光顶出去 | 码值有变化 0.02%–0.41% |
| 内容映射 | `S2A_HDR_MAP=1` | `L_out = head · L^1.15`（BT.2408-8 §5.1，黑仍 0、白落 `head`） | 整幅抬起来，接近 RTX HDR | 码值有变化 98%–100%，平均增益 1.39–1.57 |

默认保持拐点档是因为它逐位安全（`docs/FIDELITY.md`：纸白以下最大改动 `0.000000000`）；
内容档会把暗部一起抬起来（`max_dlo_below` 0.29–0.58），所以它是选项而不是默认。
两档共用同一套判定链路（直方图 → L1b → 拐点），只在「乘多少」处分开；
上屏走同一个 fragment 的 `c.extra.z` 分支，`src/t_hdr_map.c` 把 C 与 MSL 的逐通道差压到 2.4e-7。

## 随机输入下的不变量（`t_hdr_fuzz`）

其余各套都在验「某个场景算得对不对」；真机遇到的输入无法预知。`t_hdr_fuzz` 换一种问法：
不管丢进来什么直方图，12 条性质必须恒成立（输出有限、拐点范围、增益单调不超 head、
逐像素不超 head 且非负、白点范围、返回值合法）。五类输入（均匀/孤立尖峰/单格全满/单调斜坡/
病态计数）+ 固定种子，缺省 20 万次判定，`./build/t_hdr_fuzz 400000` 可跑到 1000 万次。

写这套时改了两条不变量而非改代码——因为第一版把它们写错了：
① 白点是**线性亮度**（直方图范围 log2 −10..4 ⇒ 线性 0.001..16），不是 [0,1]；
真实 8 位输入的线性上限是 1.0，但取的是格子**上沿**，所以最亮那格会到 1.04；
② `knee_lo` 是纸白，暗场可以低到 0.02（`KNEE_MIN=0.25` 只约束 L0 导出的 `hi`）。
同时把「上一帧状态」限定在真会出现的范围——第一版给 `prev_white=rf(0,1)`，冻结分支把它原样
带出来，看着像越界，其实是喂了不存在的历史。

判别力实测：在临时副本里让 `s2a_hdr_clip_head()` 直接返回，500 帧就报 96 处「通道超 head」。

## 与上游 0.4.0 自带的补帧共存（`nvn_present_fi.inc`）

上游 0.4.0 自带 MetalFX 补帧，它会**再插一帧**上屏，所以 HDR 的扩张必须发生在每一张上屏的帧上，
不只是游戏交来的真帧。本补丁在这条链上做两件事，都在 `nvn_present_fi.inc`：

1. **uniform 缓冲 12 → 16 float**。`present_blit_m4` 只在 `uvBuffer.length >= 16*sizeof(float)`
   时才写 knee/extra（`uv[8..15]`）；补帧原来给的是 12 float，于是这条路上 knee 恒 0，
   16F 的扩张 shader 走退化分支（`knee_hi <= knee_lo`）返回 `a * knee.z(=0)` ⇒
   **HDR + 补帧同时开时画布全黑**。缓冲区是共享内存且归零，日志只显示「渲染成功」，看不出来。
2. **补出的帧也过同一遍扩张**。`mid` 里是 MetalFX 在**未扩张**域里插出来的值（输入是 src 的原样缩拷），
   所以 16F 时中间帧改走 `present_blit_m4(cb, mid, …)` 而不是纯拷贝 `fsCopy`；
   否则补出的帧比真帧暗 `head` 倍，60→120 Hz 隔帧一亮一暗。

`scripts/check_landing.py` 有 3 条断言钉住这两点（缓冲必须是 16 float、不得再出现 12 float、
16F 分支必须调 `present_blit_m4(cb, mid`）。

## 与 RTX HDR 的对照（这一节回答「像不像 RTX HDR」）

2026-07 有一份玩家侧实测（[hdrgameanalysis.com/rtx-hdr](https://hdrgameanalysis.com/rtx-hdr)），
把我们关心的旋钮都对上了，写进 `docs/SOURCES.md` §10。相同的地方：纸白锚在 **203 nit**（他们滑块的
「Middle Grey 50」，与我们 `S2A_HDR_BT2408_HEAD 2.03f` 独立吻合）；只重分配 survivor 亮度，
**不 de-clip**（SDR 里已经裁平的高光不会回来）；峰值受显示器约束，不凭空发明。

三处**故意**与 RTX HDR 不同，请在 review 时当作设计选择而不是缺失：

| RTX HDR | 本补丁 | 理由 |
|---|---|---|
| 逐帧神经网络（Tensor core） | 闭式直方图统计，实测 0.57 µs/帧 | 手机没有 Tensor 预算；`docs/COST.md` |
| 主动补中间色阶（抗 banding） | 不补 | 8 位出屏只有 256 级，补阶在末端无意义；16F 路径本身不丢，见 `t_hdr_banding` |
| 默认过饱和（公认问题） | 缺省 `SAT=1.0`/`P3=0`，颜色逐位不动 | 过饱和是主观偏好，不该做默认；要就开 `S2A_HDR_SAT` |

另有两点是它的实现细节、我们靠不同手段达成：它的「Peak Brightness 超过 1000 只压平层次」——
我们根本不滑峰值，`head` 只读 layer 声明的 EDR 上界（钳 `[1,4]`，见运行期自愈一节）；
它的 Contrast 会联动纸白——我们用「纸白 + 拐点」两段把两件事解耦（`docs/ABLATION.md` 的
`fixed/apl/white/full` 四档就是这个模型的消融）。

## 全关态逐位一致（`t_hdr_identity`）

「不开启时与上游一样」以前只有结构证据（落点自检钉住 `int itm = (format == MTLPixelFormatRGBA16Float)`）。
但 HDR 上屏是「线性化 → 乘增益 → 再编码」的 fragment：即使拐点两端写成 1.0，像素也走了一遍
sRGB 解码 → float 乘法 → sRGB 编码，任何 1 LSB 偏差都会让 8 位画面「颜色轻微不同」，而这类偏差
在性质测试与对账测试里都看不见（它们只看 HDR 开启时的行为）。

`t_hdr_identity` 穷举 8 位码值空间（256 个）走五条路径，要求**取整后与原码值相等**：
A 线性→×1.0→编码；B 上屏 ITM 公式（拐点退化到单点、head=1，增益必须逐位 1.0）；
C 全关的 `s2a_hdr_grade_rgb_map` 必须逐位返回输入；D 16F 透传（knee_lo 以上才扩张，1.0 以下不动）；
E 编码后单调（分段点 0.04045 附近不回退）。结果：256 × 5 全部精确。

**边界（写在 PR 里，不假装覆盖）**：本机证的是我们自己的 float 公式。真机上 sRGB 解码/编码是
Metal 按纹理格式（`_sRGB`）做的，往返是否同样逐位精确属于硬件行为，只能靠 T3（`S2A_HDR_OUTPUT=1
S2A_HDR_FMT=0`，8 位 + 声明 EDR）与上游截图对比来验——这也是三格矩阵保留 T3 的原因之一。

## 与独立参考实现对账（`t_hdr_oracle`）

`t_hdr_fuzz` 验性质，性质全成立也可能「错得合理」：HUD 剔除的簇分界若错一格，输出仍是
[0,1] 里的某个白点，12 条性质全绿，但白点系统性偏 0.05 stop。所以另有一套 `t_hdr_oracle`：
照头文件注释里的**定义**重写一遍最笨的版本（分箱也不调生产函数，直接按 sRGB 定义算线性值），
再要求两者逐位相同（容差 1e-5 stop）。覆盖 4000 组随机直方图 + 3 张真实照片 + 4 个擦边球构造
（HUD 恰好 5%、簇间隙恰好 =GAP、间隙 =GAP-1、计数恰好等于 need），共 24,043 次比较。

写这套时也先改了自己的参考实现：第一版用**格心**判「黑」（生产用格**下沿**），边界那一格的
进出改变 `total`，进而改变 `need` 的取整，产生 3300 处假失败（重心差 0.04 stop、白点差一格）。
判别力实测：把「主簇内部 0.5% 分位」故意偏一格，立刻报 4007 处差异。

## 运行期自愈（补丁会改 layer 格式，请重点看这一段）

自动 HDR 不是「设一次就不管」：EDR 上界、画布格式都可能在中途变化，所以补丁里有三处运行期逻辑。
它们都会**改变 `CAMetalLayer` 的状态**，是 review 时最需要注意的行为面：

| 场景 | 行为 | 上界 |
|---|---|---|
| 用户拖系统亮度 / 换屏 / 显示器切换 HDR | 每帧读 `maximumExtendedDynamicRangeColorComponent`，1% 死区外才更新 `head` | 钳在 `[1,4]` |
| 16F 画布连续 60 帧拿不到 drawable（起局、切后台不算） | 降级到 `BGRA8Unorm`，**格式 + EDR 标志 + 颜色空间三件套一起改**，head 归 1，重建管线并打日志 | 60 帧 ≈1 秒 |
| 降级之后 16F 又变得可用（外接屏拔掉、系统 HDR 重新打开、分屏拖回全屏） | 600 帧（≈10 秒）试探一次 16F；失败则退避 ×2，封顶 600×8（≈80 秒）；16F 稳定后复位到 10 秒 | 试探不改任何持久设置 |

第三条是因为「只降不升」= 「一次意外换来永久 HDR 关」。`g_hdr_want16` 记录「起局确实要过 16F」，
所以 `S2A_HDR_FMT=0` 的用户不会被无端反复试探；而真机 T1/T2 那种从第一帧起就没有 drawable 的情形
也会在用户后来打开系统 HDR 时自己回来（该标志在拿不到 drawable 的分支里按当前格式置位）。

调色后若超过 EDR 上界，`s2a_hdr_clip_head()` 按 `head/m` **等比缩放**整幅（保色相）、不做逐通道钳位，
否则亮色块会被系统切成色阶（实测 sat=3.0 时超界 2.0457 倍）。

## 已知未验证（写在这里，不藏）

1. **MSL 只做过语法级验证**：本机无 Metal 编译器。通过 `xcrun metal -std=metal3.0 -c` 的是
   `hdr_hist.metal` 与 8 位直通上屏 shader（CI run `43729862796`）；**当前的 3555 B HDR fragment
   shader 与 16F 上屏路径从未过 Apple 编译器**（GitHub 账号停用后 CI 不可触发）。静态守卫
   `scripts/check_msl.py` 42 项只查禁用类型、括号配平、属性重复这类结构问题；
2. **ObjC 只做过 `clang -fsyntax-only`**（对 `nvn_mtl.m` / `host_adapt.m`，无链接、无运行）；
3. 真机三格矩阵尚未跑：T1 `S2A_HDR_FMT=1`（只换格式）、T2 `S2A_HDR_OUTPUT=1`（总开关；`FMT=1` 可叠加）、T3 `FMT=0`（只开 EDR）——组合语义在 `s2a_hdr_out_plan`，单测 `t_hdr_env` 37 项；
   判据已脚本化为 `./scripts/check_device_log.py --log <logcat> --expect t1|t2|t3`——它认的是
   `HDR stat:` 那一行（与参考实现同一份格式化代码），并把「fmt 与 itm 互相矛盾」「head 超 [1,4]」
   「knee_hi 超 1.0」「只有降级日志（⇒ 黑屏假设成立）」分别报成失败；`--selftest` 8 项用合成
   日志覆盖 T1/T2/T3/矛盾/黑屏五种情况；
4. **运行期行为从未观测**：所以 `docs/` 里的所有数字都来自「CPU 参考实现 + 与 MSL 的逐行对账」，
   不是设备截图。对账能证明两侧算得一样，不能证明设备上真的跑到那一步。

## 怎么提交

```sh
./scripts/verify_upstream.sh  # 一条命令：取上游当前头 → 打补丁 → 31 套 + MSL/常数 + 落点自检
./scripts/check_upstream.sh   # 上游默认分支/tag 各在哪、能不能干净打上、是否纯增量
./scripts/publish.sh          # dry-run 只打印命令
./scripts/publish.sh --yes    # fork → 分支 hdr-auto/l1b → push → gh pr create（正文即本文件）
```

## 建议的 review 顺序

1. `nvn_hdr.h`（纯数学，无平台依赖，最容易看）；
2. `shaders/hdr_hist.metal` + `nvn_hdr_hist.inc`（数据流：一帧一次 dispatch + 16 B 回读）；
3. `nvn_mtl.m` 的 `present_blit_m4` / `g_hdr_knee`（16 float 常量怎么到上屏）；
4. `host_adapt.m`（layer 格式 + EDR）；
5. `nvn_present_fi.inc`（与补帧共存，可选）；
6. `tests/*.c`（每条断言都对应 docs 里的一张表）。

### 触及范围（为什么有一行在 `graphics/` 之外）

补丁 43 个文件里 42 个在 `graphics/`，另有 `ios/play_reflection.m` **一行**：

```objc
if (m.wantsExtendedDynamicRangeContent!=game.wantsExtendedDynamicRangeContent) m.wantsExtendedDynamicRangeContent=game.wantsExtendedDynamicRangeContent;
```

上游让镜像/反射层跟随游戏层的像素格式与色彩空间，没有跟 EDR 开关。HDR 开时那块层是
16F + extended linear 但按 SDR 合成，镜像里的高光被钳。理由与后果写在 `docs/DESIGN.md`
「镜像 / 反射层」。若维护者不愿改 ios/，把这一行删掉即可：主屏不受影响，只是镜像退回 SDR。
