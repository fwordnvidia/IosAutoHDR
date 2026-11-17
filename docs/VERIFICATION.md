# 验证矩阵（每条结论 → 命令 → 期望输出）

这份文件的规矩：**任何一条结论都必须能用一条命令重跑，并且期望输出写在这里**。
没有命令支撑的说法不进这张表（会写进 §7 未验证）。

## 一条命令跑完

```bash
./run_all.sh                      # 不需要 S2Arm 树：自检 + 敏感度 + 曲线图
./run_all.sh /tmp/v09             # 额外：把补丁打进干净 d5cbf7ff53f7 树跑 31 套单测（904 项）
```

`run_all.sh` 返回 0 才算通过。步骤：`0/5` 量化报告 → `1/5` 参考 CLI → `2/5` HUD 平台区 →
`3/5` 曲线图 → `3b/5` 平台阶 → `4/5` 补丁自洽 + 十四套单测 → `0g/5` 消融 → `0h/5` 转场序列 →
`0i/5` 每帧开销（实测控制器一次调用 0.57 µs，上限 5 µs）→ `0j/5` 对照图（12 张图的指纹清单）→ `0k/5` BT.2408 放置方式 → `0l/5` 画幅与黑边 → `0m/5` GPU/CPU 分箱对账 → `0n/5` L0 分位对账 → `0o/5` 上屏 shader 对账 → `0p/5` 运行期 headroom → `0q/5` 回读控制流 → `0u/5` knee 发布/消费 → `0v/5` 图像级用例 → `0w/5` 颜色保真 → `0e7/5` ASan+UBSan（31 套 15s）→ `0w7/5` 基线一致 → `0w8/5` README 与仓库一致（引用的路径都存在、10 个入口脚本都被写进 README）→ `0w9/5` 调用点实参个数（16 个定义 / 63 个调用点，跨文件合并定义表）。 → `0w10/5` `hdr_hist_l1` 逐行对账（MSL 的 float 累加 vs CPU 的 double：avg 差 5.96e-07 stop、white 同格、scene 0.66 stop 两版都是 31.1%；**已知差异**：GPU 侧没有 HUD 簇剔除，HUD 画面白点被顶格拉到 13.24 ⇒ hi 打满 2.0 ⇒ 这条 fallback 等于不扩张）。 → `0w11/5` 设备侧代码登记完整（13 项：12 项有对账、1 项写明理由；新增 kernel 不登记就红；`docs/DEVICE-ONLY.md` 由脚本生成并 `--check` 挡漂移）→ `0w12/5` 补帧运动估计的域（`tools/fi_domain` 实测：静止判定在 16F 线性域只有 44–64% 命中、转回编码域回到 100%，见 `docs/FI-DOMAIN.md`）→ `0w13/5` 换格式后上游隐含假设（13 条结构断言 + `docs/UPSTREAM-ASSUMPTIONS.md`）→ `0w14/5` 补丁统计三处一致（README / VERIFICATION / PR 里写的文件数、±行数、sha256 必须等于对 `patch/hdr-auto-l1.patch` 的现算，见 `docs/PATCH-STATS.md`）

它不做的事：
编译 MSL、编译 ObjC、链接、真机——见文末。

## 每帧开销

| 结论 | 命令 | 期望 |
|---|---|---|
| 控制器一次调用亚微秒级 | `python3 scripts/report_cost.py --check` | `✓ … 最慢 0.5~0.6 µs（上限 5.0 µs）` |
| 报告与实测同量级 | 同上（`--check` 只校验用例集合、量级与上限，不比对逐位数字） | 0 退出 |

## 补丁本身

| 结论 | 命令 | 期望 |
|---|---|---|
| 补丁能在干净 `d5cbf7ff53f7` 上无冲突应用 | `git apply --check patch/hdr-auto-l1.patch`（在干净树） | 0 退出；42 个文件 |
| 补丁文件指纹 | `sha256sum patch/hdr-auto-l1.patch` | `3fa19b23…`（42 文件，+6889 −26 行） |
| MSL 能按 Makefile 规则嵌入 | `python3 graphics/metal/tools/embed_msl.py graphics/nvn/shaders/hdr_hist.metal k_hdr_hist out.h` | 7,948 B |

上游跟随：`./scripts/check_upstream.sh` 会同时报**默认分支**与**最新 tag**，并以默认分支上能否干净打上为准
（只看 tag 会得出「没前进」的错结论：2026-11-16 tag 仍是 v0.4.0，而 `main` 已是 `2b5aca92`，前进 353 个提交）。
该分支上补丁可干净打上，`./run_all.sh <该树>` 全过。

补丁落点自检：`scripts/check_landing.py <树根>` **56 项**——能打上不等于插对地方。逐处断言注入仍落在
预期函数体内（`hdr_head_tick()` 必须出现在 `present_blit` 与 `present_blit_m4` 里、`knee` 走
`uv[8..11]`、`nvn_hdr_hist.inc` 只 include 一次、EDR 上界只在 `hdr_head_tick` 读一次、Makefile 的
`GENH` 与两个构建依赖文件都在）。`verify.sh` 与 `run_all.sh` 第 4 步都会调它。

`verify.sh` 的套件清单改成 glob：它曾硬编码 13 个名字，后来新增的 8 套没被加进去，
于是**「打过补丁 + 跑过测试」这条路会静默少跑 8 套**（`-ge 21` 的断言在 `run_all.sh` 里，
`verify.sh` 现在也自带同样的下限）。

实测（2026-11-16）：在 `main`（`2b5aca92`，比 v0.4.0 前进 353 个提交）的干净 sparse 树上
`./verify.sh <该树>` —— 补丁干净打上、**31 套全过**、落点自检 56 项全过。同一天上游又前进到
`ddfef415`，`check_upstream.sh` 复核仍能干净打上（行为未重跑，因为上游还在动）。
`./scripts/verify_upstream.sh [提交号]` 把上面这套在上游当前头上从零跑一遍（取 HEAD → sparse 克隆 →
`verify.sh`），实测 `1c31fdc005cf` 通过；结论只对那一个提交成立，所以它会把提交号打出来。
`check_upstream.sh` 还会报「上游有没有自己的 HDR/EDR 意图标记」——本次是 **0**（纯增量）；
`RGBA16Float` 在 21 个文件里出现过，但那是通用浮点格式处理，不构成 HDR 实现，所以不进判据。

## 诊断行（`S2A_HDR_LOG=1`）

HDR 生效与否在画面上看不出来，真机三格矩阵也只有日志可依，所以这一行的格式本身就是契约：
每 `S2A_HDR_LOG_EVERY`（600 帧 ≈10 s）打一次

```
HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1
```

- `fmt` 是**当前画布**（`16F`/`8B`），`itm` 是上屏是否真走扩张；`fmt=16F itm=0` 说明管线没重建；
- `head` 是运行期 EDR 上界（`maximumExtendedDynamicRangeColorComponent`，钳 `[1,4]`）；
- `knee` 打成 `lo+delta`，与原子槽打包口径一致；`frozen=1` 表示本帧被判淡出/暗场、拐点保持；
- `avg`/`scene` 非有限时打 `n/a`（首帧、空直方图不灌 `nan` 进日志）。

`t_hdr_stat` 19 项把整行逐字节钉死，另测 8 位降级形态、`NaN`/`-inf`、截断返回完整长度且仍 NUL 结尾、
`NULL`/`n=0`/`st=NULL`、退化拐点 `+0.0000`。落点自检另有 3 项（开关、调用、节流）。

### 本机与设备的同一行

`tools/hdr_ref.c` 打的是**同一份** `s2a_hdr_status_line()`，所以两处输出可以逐字符对比：

```
./build/hdr_ref --synth hud --frames 8 --stat-line
HDR stat: fmt=16F head=2.00 knee=0.7424+0.2576 white=0.7424 avg=-3.89 scene=0.00 frozen=0 itm=1
```

`tools/ref_check.sh` 里「场景十六」三条守着它：前缀/字段顺序固定、`knee=lo+delta` 与 `--stats` 的
`knee=lo..hi` 一致、同一帧换 `--hudguard`（white 0.7424→0.2125）后诊断行跟着变（排除缓存）。
`hdr_ref` 的 `st` 结构为此补了 `scene`/`frozen`——否则这两个字段只能写死 0，那这行就是装饰。

### 真机三格矩阵的判据（`scripts/check_device_log.py`）

设备侧日志拿回来后：

```
./scripts/check_device_log.py --log device.log                 # 通用一致性
./scripts/check_device_log.py --log device.log --expect t1     # 或 t2 / t3
./scripts/check_device_log.py --selftest                       # 8 项合成日志自测
```

- 通用检查抓「格式换了 shader 没换」的四种指纹：`fmt=8B` 却 `itm=1`、`fmt=16F` 却 `itm=0`、
  `head` 出 `[1,4]`、`knee_hi` 超 1.0（都报到具体行号）；
- 单格判据：T1 = `16F` 且 `head` 停在 1.00；T2 = `16F` 且 `head>1.0`（SDR 屏上被判「测不到」而非 bug）；
  T3 = `8B` 且 `itm=0`；
- 日志里只有「16F 连续 60 帧取不到 drawable」而无 stat 行 ⇒ 直接判「**黑屏假设成立**」。

`run_all.sh` 的 `0x/5` 会跑自测，并用 `hdr_ref --stat-line` 的真行验两个方向（T2 必须通过、
T3 必须失败），避免判据只会点头。

## 算法（31 套单测，904 项）

| 套 | 命令（在打过补丁的树） | 期望 |
|---|---|---|
| 出屏曲线 | `cc -I graphics/nvn/gfx/nvn graphics/nvn/tests/t_hdr.c -lm -o /tmp/t && /tmp/t` | `134 项 → 全过` |
| L0 自适应 | 同上换 `t_hdr_auto.c` | `32 项` |
| L1 白点/黑场 | `t_hdr_auto_l1.c` | `93 项` |
| 端到端 | `t_hdr_pipeline.c` | `68 项` |
| 色域/饱和度 | `t_hdr_color.c` | `62 项` |
| 量化（四场景） | `t_hdr_quality.c` | `23 项` |
| 噪声稳定性 | `t_hdr_stability.c` | `13 项` |
| 台阶放大 | `t_hdr_banding.c` | `19 项` |
| 鲁棒性 + fuzz | `t_hdr_robust.c` | `15 项` |
| HUD 剔除 | `t_hdr_hud.c` | `20 项` |
| 帧率归一化 + 帧率估计 | `t_hdr_framerate.c` | `28 项` |
| CPU/GPU 同口径 | `t_hdr_gpu_mirror.c` | `14 项` |
| 上屏接线（纸白/head/帧率） | `t_hdr_wiring.c` | `49 项` |
| 淡出/黑场/淡入 | `t_hdr_fade.c` | `20 项` |
| BT.2408 放置方式 | `t_hdr_bt2408.c` | `16 项` |
| 画幅与黑边 | `t_hdr_letterbox.c` | `22 项` |
| GPU/CPU 分箱对账 | `t_hdr_bin_mirror.c` | `21 项` |
| L0 分位对账 | `t_hdr_knee_mirror.c` | `13 项` |
| 上屏 shader 对账 | `t_hdr_shader_mirror.c` | `13 项` |
| 运行期 headroom | `t_hdr_head.c` | `17 项` |
| 回读控制流 | `t_hdr_tick.c` | `15 项` |

## 内存与未定义行为（`0e7/5`，31 套 × ASan+UBSan）

`run_all.sh` 用 `-O2` 跑单测，编译器的首要假设就是「没有 UB」——一旦真有越界或未定义行为，
它往往表现为「在别的机器上算错几个数」，而不是当场崩溃。所以另有一遍带 sanitizer 的：

```sh
./scripts/sanitize.sh
# · 31 套 × ASan+UBSan（halt_on_error=1, detect_leaks=1），15s
# ✓ 无内存错误、无未定义行为、无泄漏（31 套）
```

两个选项是**必须**写死的，少一个就变成假绿：

| 选项 | 少了会怎样 |
| --- | --- |
| `UBSAN_OPTIONS=halt_on_error=1` | UBSan **默认只打印不中止**，退出码仍是 0 ⇒ 有 UB 也判过（这个坑踩过） |
| `ASAN_OPTIONS=detect_leaks=1` | 泄漏不报。这些测试是纯计算，不该有任何泄漏 |

脚本还有一层「退出码 0 也查」：`grep -E 'runtime error|Sanitizer'` 命中同样算失败，
因为报告可能在 halt 生效之前就打印出来。本机 `cc` 不支持 `-fsanitize` 时脚本打印一行跳过、
**不算失败**（不让工具链差异变成红）。

实测：31 套 904 项在 `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` 下全过，
无一次 sanitizer 报告。

## 交付包（消费者视角）

| 结论 | 命令 | 期望 |
|---|---|---|
| 使用者命令写的文件对 | `python3 scripts/check_convert.py` | `✓ hdr_convert：… 15 项 全过` |
| 慢速运动不抖、纸白不动 | `python3 scripts/report_video.py --check` | `✓ docs/TEMPORAL.md 与实测一致` |
| 饱和度不拧色相（CIELAB 对照） | `python3 scripts/report_color.py --check` | `✓ docs/COLOR.md 与实测一致` |
| 使用者命令写出的文件对不对 | `python3 scripts/check_convert.py` | `✓ hdr_convert：… 15 项 全过` |
| MSL 无已知雷区（double / 括号 / include） | `python3 scripts/check_msl.py` | `✓ MSL 静态守卫：2 个文件 28 项全过` |
| 四档策略的消融表（每步各解决什么） | `python3 scripts/report_ablate.py --check` | `✓ docs/ABLATION.md 与实测一致（57 行）` |
| 补丁还能打到上游最新 tag | `./scripts/check_upstream.sh` | `✓ 上游没有前进，补丁按原样可用` |
| 解包即用、包里不含本机产物 | `TARBALL=… ./scripts/consumer_test.sh` | 末行 `✓ 消费者测试通过` |
| 解包 + 干净树跑单测 | `TARBALL=… ./scripts/consumer_test.sh /tmp/v04-clean` | `✓ 全部通过` |

## 行为（参考 CLI 自检 40 项）

| 结论 | 命令 | 期望 |
|---|---|---|
| 满屏 UI 白纸白不动 | `./tools/ref_check.sh` | `场景一` 通过；纸白 1.0000 |
| 暗场星空会扩张 | 同上 | 星点 ×1.722 |
| L1b 修掉 L1 的纸白抬升 | 同上 | ×1.2913 vs ×1.0000 |
| 同参数两次逐字节相同 | 同上 | `场景五` |
| 渐变的平台阶：无抖动 178 / 抖动 0 | 同上 | `场景八` |
| 16 位输出不合并输入码 | 同上 | 78 = 78 |
| 淡入 64 帧不抽动 | 同上 | 方向翻转 0；最大单帧跳变 0.443% |
| 1% HUD 不掐死扩张 | 同上 | 白点 0.7424 → 0.2381 |
| 剔除常数在平台区 | 同上 + `./build/sweep_hud` | 默认行 12/12 & 0/8 |
| 切镜阈值在中间 | 同上 + `./build/sweep_scene --response/--lag` | 0.30→sw 0；1.50→sw 0.977；lag 0.662 / 1.964 |

## 明确**没有**验证的（不要在别处声称已验证）

1. **MSL 编译**：需要 Apple Metal 编译器。最后一次通过是 CI run `43729862796`（GitHub 账号停用前），
   覆盖 `hdr_hist.metal` 与两段上屏着色器；此后 `hdr_hist_l1` 的 HUD/lit_frac 扩展**没有**再编译过。
2. **ObjC 编译**：最后一次 rc=0 是 CI run `43728394548`（`nvn_mtl.m` / `host_adapt.m` 的 `-fsyntax-only`）。
3. **链接与整包构建**：`build-ios-shell` 最后一次成功是 run `43724773168`，但它打的是**旧补丁**（无 HDR）。
4. **运行期**：没有一次在本机或真机上跑过带 HDR 的壳。黑屏假设（T1/T2/T3 三格矩阵）未验。
5. **画质**：所有画质结论都是**合成内容**上的数值结论，没有真实游戏截图/录像的对照。

`.github/workflows/verify.yml` 能补上前两条：它 `xcrun metal` 真编 4 个 MSL 文件、核常数、
对两个改过的 ObjC 文件跑 `clang -fsyntax-only`。**注意它只能手动派发**，而且默认不进公开投影
（推动它需要给 token 加 `Workflows` 权限，见 `docs/GITHUB-COMPLIANCE.md` 的取舍）。
运行期与画质两条仍然只有真机能给。

## 切镜

| 结论 | 命令 | 期望 |
|---|---|---|
| 切镜（同 APL）1 帧到位 | `./build/hdr_ref --synth cut --seq --frames 64 --scene-fuse` | 第 33 帧起 `lo` ≈ 0.897（不融合则要 26 帧） |

## 逐帧输出性质（序列级，不只单测）

`./scripts/report_sequence.py --check` 会对整段 270 帧序列（`build/seq/f000..f269.ppm`）逐帧断言：

| 性质 | 判据 | 实测 |
|---|---|---|
| 纸白那一档的增益恒等于 1 | `s2a_hdr_itm_gain(knee_lo) == 1.0`（±2e-5） | 269 帧 `1.000000..1.000000` |
| 调色不把任何通道推出 EDR 上界 | 探针 `(1,0.24,1)`@`sat=3.0` 经 `s2a_hdr_grade_rgb` 后 `max<=head` | 最大超 `0.000000` |

第一条保证 UI、字幕、纸白文字在 HDR 下与 SDR 逐位同亮；第二条保证超上界时只降亮度（等比缩放），
不会让系统硬钳切出色块。黑场帧是 `nan`（保持上一帧），不计入统计。

## 整幅图像的上屏/CPU 对账

`tools/hdr_shader_image.c` 拿一整幅真实照片，用该帧自己的 L1b 拐点，逐像素同时跑
「MSL 转写」（`src/nvn_hdr_msl.h`）与「CPU 参考」（`s2a_hdr_grade_rgb`），累计最大差与超 head 像素数。

| 输入 | 旋钮 | 像素 | 最大差 | 超 head 像素 |
|---|---|---|---|---|
| Clouds / Fuji / Monument（`build/real/p_*.ppm`） | `sat=1.0,p3=0`；`sat=1.6,p3=0.5`；`sat=3.0,p3=1.0` | 各 129,600 | `0.000000000` | `0` |

九组组合合计约 117 万像素，全部逐位一致。与 `t_hdr_shader_mirror`（色彩空间网格，177,147 组）
互补：网格保证算式一致，整幅保证在真实内容（P3 混色 + Lab 二分 + clip 的交互）上一致。

## 设备链路整幅对账（分箱 → L1 → 上屏）

设备上拿到的 knee 不是 GPU 算的：`hdr_hist_add` 只负责数格子，1 KB 直方图 blit 回 CPU，
`s2a_hdr_auto_l1b_step_fused_hz` 在 CPU 上算 L1b 再写 `g_hdr_knee`；GPU 的 `hdr_hist_l1`
输出只用于对账日志与 blit 失败时的 fallback。所以「设备==CPU」等价于三件事：

1. MSL 分箱与 CPU 分箱在**同一幅图**上逐格相同（`tools/hdr_device_image.c`，256 格 diff = 0）；
2. GPU L1 转写与 CPU 参考的 white/avg 一致（本工具实测逐位相同；运行期日志阈 0.02/0.05）；
3. 由此得到的 knee 与画面完全相同（`max_diff = 0.000000000`，无通道超 head）。

实测 3 张照片 × stride 1/4/8（覆盖 1080p 用 4、4K 用 8 两档）× `sat=1.6,p3=0.5`：
9 组全部 `bin_diff = 0`、`white`/`avg` 逐位相同、画面差 0。`run_all.sh` 的 `0t/5` 步守这条。

## 并发：knee 的发布/消费（`0u/5`）

`t_hdr_publish` 9 项：`(lo, delta)` 打包往返逐位一致（含 ±0、1e-6）、20,000 组随机往返、
发布前后只读到整组、旧做法的撕裂帧 `hi` 既非旧也非新（判别力）、从未发布返回 0、
负 delta 夹 0、1000 组随机 `lo+delta >= lo`。落点自检另断言：`nvn_hdr_hist.inc` 的三处
knee 更新都走 `s2a_hdr_knee_publish`，`present_blit` / `present_blit_m4` 都走 `s2a_hdr_knee_take`。

## 图像级用例：真实像素上的用户可见性质（`0v/5`）

`t_hdr_frames` 20 项：判据算得对（直方图级用例）不等于**画面**对。这组用 320×180 的合成帧
走完整链路（分箱 → HUD 剔除后白点 → L1b 目标 → `s2a_hdr_grade_rgb` 逐像素），断言：

| 用例 | 断言 | 实测 |
|---|---|---|
| 夜景（全黑底 + 400 星点） | 纯黑输出严格 0；无通道超 head；**低于纸白的像素逐位不变**；整幅平均涨幅 < 5% | paper 1.0000、lo 0.6380、平均比 1.0000 |
| HUD 白条（2 行纯白 = 1.1% 像素） | 纸白恒 ×1.0000；**拐点与「没有白条那一帧」逐位相同**；不做剔除的对照组明显不同（判别力） | clean/hud 均 lo 0.3349、naive lo 0.9500 |
| 字幕条（整行白 = 0.55%） | 拐点不受字幕条影响；不剔除时确实被拉走 | clean/sub 0.3349、naive 0.9500 |
| 淡出（60 帧 @2.4 stop/s） | avg 逐帧下掉；`knee_lo` 全程不动；末帧与起始相同 | lo 0.6380 → 0.6380 |

两处踩坑（都写进注释，防后人重复）：
- 夜空里最亮的星簇**就是**白点（L1b 的纸白 = 0.5% 分位白点），所以星点本身不该被放大；
  放大只发生在白点以上。第一版断言「星点吃到增益」是错的，正确性质是「低于纸白逐位不变」。
- sRGB 码值乘 k 对应线性乘 k^2.2。第一版按「每帧码值 ×0.90」想造 9 stop/s，线性上其实是
  20 stop/s（> `S2A_HDR_FADE_MAX_S` 16），被判成硬切、冻结不触发。要造 2.4 stop/s 得取
  `pow(2, -2.4/60)` 的 1/2.2 次幂。

## 真实照片的颜色保真（`0w/5`）

`tools/hdr_color_image.c` 对三张真实照片逐像素算 sRGB → 线性 → XYZ(D65) → CIELAB，
两张配置各跑一遍（`default` = 缺省 `sat 1.00 / p3 0.00`；`tuned` = README 上限档 `1.15 / 0.50`），
输出色相偏移的中位/p99/最大、>5° 像素占比、近中性像素（C<2）的 `a*`/`b*` 绝对位移。
`scripts/report_color_image.py` 写 `docs/COLOR-IMAGE.md`（`--check` 逐字比对）。

实测（129,600 像素/张）：缺省档 **色相偏移恒 0.00°**（`sat==1 && p3<=0` 短路到
`s2a_hdr_itm_rgb`，逐位不变），近中性 Δab ≤0.36；`tuned` 档中位 0.90–2.56°、p99 ≤3.12°、
最大 3.15°，**无像素超过 5°**，近中性 Δab ≤0.49。工具在缺省档出现 >6° 色偏或近中性
Δab >0.5 时以非 0 退出。

落点自检另有一条：`hdr_hist_knee` 这条 kernel **仍然未被调度**（设备 knee 全来自 CPU 的
`s2a_hdr_auto_l1b_step_fused_hz`）。若将来有人把它接上，这条会变红，提醒同步补 GPU/CPU 的
L0 逐位对账——`t_hdr_knee_mirror` 守的是算式，不是调度。

## 16F 画布的回头路（降级不是终点）

只降不升等于「一次意外换来永久 HDR 关」：外接 SDR 屏 → 内建 XDR 屏、系统 HDR 开关被重新打开、
分屏拖回全屏，都会让 16F 画布重新可用。`s2a_hdr_fmt_guard_rearm()` 在降级后按 600 帧（10 秒 @60 Hz）
试探一次，失败就指数退避（1200 / 2400 / 4800 帧，上限 ≈80 秒），16F 稳定拿到 drawable 后由
`s2a_hdr_fmt_guard_ok()` 复位到 10 秒。试探时像素格式、EDR 标志、颜色空间三者一起恢复
（只恢复格式会出现「画面回来了但发灰」）。`g_hdr_want16` 保证 `S2A_HDR_FMT=0` / HDR 关闭的用户
不会被无端反复试探。

`t_hdr_fmtguard` 15→**28** 项：前 59 帧不降级、第 60 帧降一次、降级后不重复触发、出帧清零、
8 位不动作、已降级不回跳、NULL 安全；新增 rearm 组——试探正好在第 600 帧、窗口内只一次、
失败后翻倍到 1200、退避封顶 4800、16F 稳定后复位、锁存期间不动状态、`want16=0` 永不试探。
落点自检 28→**32** 项：`present_now` 里必须有 rearm 调用、切回 16F、切回 ExtendedLinearSRGB，
并且只在 `g_hdr_want16` 时试探。
