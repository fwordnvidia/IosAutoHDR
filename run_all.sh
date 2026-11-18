#!/bin/sh
# 一条命令跑完全部本地闸门。CI（.github/workflows/verify.yml，只能手动派发）跑其中 1~5 步的子集，
# 因为 CI 只认 C 侧可验证的部分（MSL/ObjC 需 Apple 工具链，见 docs/VERIFICATION.md）。
#
# 用法：./run_all.sh [干净树根]
#   不给树根：只跑不需要 S2Arm 树的部分（hdr_ref 自检 / 敏感度扫描 / 曲线图）。
#   给树根：额外把补丁打进那棵树并跑全部单测（树必须是干净的 v0.4.0 sparse graphics）。
set -u
cd "$(dirname "$0")" || { echo "✗ 进不去脚本所在目录：$(dirname "$0")" >&2; exit 1; }
fail=0
mkdir -p build          # 解包后（包里不带 build/）第一个 cc -o build/xxx 否则会失败
# 真实照片素材随包发布在 testdata/（三张 480x270 PPM，来源见 docs/SOURCES.md），而单测与工具
# 一律读 build/real/（本机产物目录）。以前这一步是手工 convert 出来的、不入库也不入包，
# 于是「解包即用」在 t_hdr_oracle 这里破功：真实照片 0 张，一类对账直接判失败。
mkdir -p build/real
for f in testdata/p_*.ppm; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    [ -e "build/real/$b" ] || cp "$f" "build/real/$b"
done
nphoto=$(ls build/real/p_*.ppm 2>/dev/null | wc -l | tr -d ' ')
[ "$nphoto" -ge 3 ] || echo "  · 只有 $nphoto 张真实照片（testdata/ 与 build/real/ 都查了）"
step() { printf '\n=== %s ===\n' "$1"; }
# 与 scripts/*.sh 保持同一套辅助函数名。缺了它们，调用方只会在 stderr 打一行
# "ok: not found"，而 fail 不会被置 1 ⇒ 整体仍是「✓ 全部通过」。0w16/5 第一版就这么假绿过。
ok()  { echo "  ✓ $*"; }
bad() { echo "  ✗ $*"; fail=1; }

step "0/5 量化报告（docs/QUALITY.md 必须与实测一致）"
if ./build/hdr_ref --synth uiwhite --stats >/dev/null 2>&1; then
    python3 scripts/report.py --check || fail=1
else
    echo "  （跳过：build/hdr_ref 还没编译，先跑第 1 步）"
fi

step "0b/5 真实内容验证（docs/REAL-CONTENT.md 必须与实测一致）"
python3 scripts/report_real.py --check || fail=1

step "0c/5 时间行为（真实照片 60 帧序列，判缓慢运动不抖、纸白不动）"
python3 scripts/report_video.py --check || fail=1

step "0d/5 使用者命令 hdr_convert（值域 / 纸白不动 / 色域真生效 / 抖动只动最低位）"
python3 scripts/check_convert.py || fail=1

step "0e/5 色相误差（docs/COLOR.md：Lab 缩放 vs 线性锚定）"
python3 scripts/report_color.py --check || fail=1

step "0f/5 MSL 静态守卫（没有 Metal 编译器时挡已知雷区）"
# 曾经是 `check_msl.py | tail -1` —— 管道让 traceback 只剩最后一行，而 `tail` 的退出码
# 永远是 0，于是「脚本崩了」与「守卫通过」在日志里长得一样。2026-11-17 在投影 clone 里
# 就是靠 `bash -x` 才看出那行 `fail=1` 来自 check_msl 的 CalledProcessError。
python3 scripts/check_msl.py >/tmp/msl.log 2>&1 || { tail -3 /tmp/msl.log; fail=1; }
tail -1 /tmp/msl.log

step "0g/5 消融报告（四档策略对照）"
python3 scripts/report_ablate.py --check || fail=1

step "0h/5 转场序列（300 帧：硬切 / 交叉淡入 / 淡到黑 / 黑场 / 淡入）"
cc -O2 -w -I src tools/hdr_seq.c -lm -o build/hdr_seq || fail=1
python3 scripts/report_sequence.py --check || fail=1

step "0i/5 每帧 CPU 开销（控制器一次调用的实测）"
cc -O2 -w -I src tools/hdr_cost.c -lm -o build/hdr_cost || fail=1
python3 scripts/report_cost.py --check || fail=1

step "0q/5 直方图回读控制流（节拍 / 双槽 / 代号作废）"
cc -O2 -w -I src src/t_hdr_tick.c -lm -o build/t_hdr_tick || fail=1
./build/t_hdr_tick || fail=1

step "0p/5 运行期 headroom（EDR 上界变化、纸白不跳）"
cc -O2 -w -I src src/t_hdr_head.c -lm -o build/t_hdr_head || fail=1
./build/t_hdr_head || fail=1

step "0o/5 上屏 shader 对账（src_msl_hdr 逐像素翻成 C）"
cc -O2 -w -I src src/t_hdr_shader_mirror.c -lm -o build/t_hdr_shader_mirror || fail=1
./build/t_hdr_shader_mirror || fail=1

step "0n/5 L0 分位对账（MSL hdr_hist_knee 逐行翻成 C）"
cc -O2 -w -I src src/t_hdr_knee_mirror.c -lm -o build/t_hdr_knee_mirror || fail=1
./build/t_hdr_knee_mirror || fail=1

step "0m/5 GPU/CPU 分箱对账（MSL 算式逐行翻成 C）"
cc -O2 -w -I src src/t_hdr_bin_mirror.c -lm -o build/t_hdr_bin_mirror || fail=1
./build/t_hdr_bin_mirror || fail=1

step "0l/5 画幅与黑边（信箱 / 脏黑边 / 画幅切换）"
cc -O2 -w -I src src/t_hdr_letterbox.c -lm -o build/t_hdr_letterbox || fail=1
./build/t_hdr_letterbox || fail=1

step "0k/5 BT.2408 display-referred 放置方式（与 knee 并列的第二条路）"
cc -O2 -w -I src src/t_hdr_bt2408.c -lm -o build/t_hdr_bt2408 || fail=1
./build/t_hdr_bt2408 || fail=1

step "0j/5 对照图（真实照片：SDR vs 自动 HDR；--check 连图内容一起核）"
python3 scripts/make_gallery.py --check || fail=1

step "0r/5 与方法 A 的定量对照（逆函数自检 + 逐码值表）"
cc -O2 -w -I src tools/hdr_curve_bt2446.c -lm -o build/hdr_curve_bt2446 || fail=1
[ -x build/hdr_curve_bt2446 ] && ./build/hdr_curve_bt2446 >/dev/null || fail=1
python3 scripts/report_curveref.py --check || fail=1

step "0s/5 整幅图像上屏/CPU 对账（3 张真实照片 × 3 档旋钮）"
cc -O2 -w -I src tools/hdr_shader_image.c -lm -o build/hdr_shader_image || fail=1
img_ok=1
for f in build/real/p_*.ppm; do
  for a in "--sat 1.0 --p3 0.0" "--sat 1.6 --p3 0.5" "--sat 3.0 --p3 1.0"; do
    ./build/hdr_shader_image $a "$f" || img_ok=0
  done
done
[ "$img_ok" = 1 ] && echo "  ✓ 9 组 × 129,600 像素：最大差 0，无通道超 head" || { echo "  ✗ 整幅对账不过"; fail=1; }

step "0z/5 保真（真实照片：纸白以下必须逐位不动）"
python3 scripts/report_fidelity.py --check || fail=1

step "0y/5 review 地图（docs/REVIEW.md 必须与补丁文件清单/行数一致）"
python3 scripts/report_review.py --check || fail=1

step "0w2/5 可调项（docs/OPTIONS.md 必须与源码里的常数/注释一致）"
python3 scripts/report_options.py --check || fail=1

step "0w4/5 随机直方图不变量（t_hdr_fuzz：12 条性质 × 20 万次判定）"
cc -O2 -w -I src src/t_hdr_fuzz.c -lm -o build/t_hdr_fuzz && ./build/t_hdr_fuzz || fail=1

step "0w5/5 白点/重心 与独立参考实现对账（t_hdr_oracle：4000 组随机直方图 + 真实照片）"
cc -O2 -w -I src src/t_hdr_oracle.c -lm -o build/t_hdr_oracle && ./build/t_hdr_oracle || fail=1

step "0w6/5 全关态逐位一致（t_hdr_identity：穷举 256 个码值 × 5 条路径）"
cc -O2 -w -I src src/t_hdr_identity.c -lm -o build/t_hdr_identity && ./build/t_hdr_identity || fail=1

step "0e7/5 内存与未定义行为（30 套 × ASan+UBSan，halt_on_error=1 / detect_leaks=1）"
./scripts/sanitize.sh || fail=1

step "0w7/5 基线一致（patch/BASELINE 是唯一真相源，README/PR 必须与它对齐）"
python3 scripts/check_baseline.py || fail=1

step "0w8/5 README 与仓库一致（引用的路径存在、入口脚本都被写进 README）"
python3 scripts/check_docs.py || fail=1

step "0w9/5 调用点实参个数（上游改签名时 patch 只报定义、不报调用点）"
python3 scripts/check_callsites.py || fail=1

step "0w10/5 hdr_hist_l1（MSL）与 CPU 参考实现逐行对账"
cc -O2 -w -I src src/t_hdr_gpu_l1.c -lm -o build/t_hdr_gpu_l1 && ./build/t_hdr_gpu_l1 || fail=1

step "0w11/5 设备侧代码登记完整（每个只在 Metal 上跑的 kernel 都必须有对账或写明理由）"
python3 scripts/check_device_coverage.py --check || fail=1

step "0w12/5 补帧的运动估计与 HDR 在同一个域（tools/fi_domain + docs/FI-DOMAIN.md）"
cc -O2 -w -I src tools/fi_domain.c -lm -o build/fi_domain || fail=1
./build/fi_domain build/real/p_*.ppm >/dev/null || fail=1
python3 scripts/report_fi_domain.py --check || fail=1

step "0w14/5 补丁统计三处一致（文件数 / ±行数 / sha256 必须与补丁本体现算一致）"
python3 scripts/check_patch_stats.py --check || fail=1

step "0w15/5 CI workflow 只手动跑（无 push/PR 触发、有 concurrency、timeout ≤20 分钟、基线不漂移）"
# 投影默认不带 .github（推 workflow 要 PAT 的 workflow scope，不为一个 CI 文件扩权），
# 所以这里不加 --require：没有 workflow 时脚本打印「跳过」并返回 0，而不是让 clone 的人
# 以为仓库坏了。开发树缺 workflow 由 scripts/check_repo_hygiene.sh 的第 2b 组判红。
python3 scripts/check_workflow_guard.py || fail=1

step "0w16/5 仓库自己的 shell（shellcheck -S warning + 调用的辅助函数必须有定义）"
# 讽刺过一轮：给 workflow 的 run: 块接了 shellcheck，却没审过仓库自己的 .sh。
# 一开就抓到 4 条 warning（cd 不检查失败、`MODE=head` 被当成命令、sudo 重定向），其中
# `cd "$(dirname "$0")"` 不检查是最实在的一条：进不去时后面每条命令都在调用者 cwd 跑。
# 可选依赖：没装就明确跳过，不静默。
# 先查辅助函数：`ok: not found` 这类错只写 stderr、不置 fail，shellcheck 也看不见。
python3 scripts/check_shell_helpers.py || fail=1
if command -v shellcheck >/dev/null 2>&1; then
    if shellcheck -S warning -f gcc ./*.sh scripts/*.sh; then
        ok "13 个 shell 脚本 shellcheck 无 warning"
    else
        bad "shellcheck 报了 warning（见上）—— 修完再提交"
    fi
else
    echo "  · 未装 shellcheck —— 跳过（brew/apt install shellcheck，或下 release 的静态二进制）"
fi

step "0w17/5 C 警告审计（46 个带 main 的源文件用 -Wall -Wextra 重编一遍）"
# 正式构建处处是 `cc -O2 -w`：-w 关掉全部警告，等于把编译器这个最基础的检查器关掉。
# 一开就抓到 tools/sweep_darkproxy.c 里 proxy_gap() 定义了从没被调用（候选 3 的公式早内联了）。
./scripts/check_c_warnings.sh || fail=1

step "0w18/5 Python 脚本 lint（34 个脚本：E9 运行时错 + F pyflakes + B905 zip 显式 strict）"
# 脚本就是闸门本身，脚本错了闸门就骗人。首次跑抓到 7 条真残留与 8 处没写 strict= 的 zip
# （4 处本该等长 ⇒ strict=True 能抓「比了一半就宣布通过」）。
./scripts/check_py_lint.sh || fail=1

step "0w19/5 社区文件结构（issue 表单写错时 GitHub 会静默忽略整份）"
python3 scripts/check_issue_form.py || fail=1

# 设备侧那几行在 Linux 侧从不编译，写错了没有任何闸门会红：功能还在、日志还在、全绿，
# 只是那行永远走不到。真实案例：`g_present_layer_resident = layer;` 和 residencySet 加挂
# 挤在同一个 `if (!g_m3 && …)` 里 ⇒ Metal 3（iOS 26.x / 非 Metal 4 GPU / 系统 <26，
# 也就是我们自己的测试机）永远进不去 ⇒ EDR 上界跟随静默失效。这里把 6 条结构不变量钉住。
step "0w20/5 HDR 钩子不变量（图层每帧记 / 两处 tick / 显式 headroom 让位 / 降级与回头路成对）"
python3 scripts/check_hdr_hooks.py || fail=1

step "0w21/5 CI 结果判读（CI 绿了但 macOS 那步被跳过，也是绿：判据要认得出）"
python3 scripts/check_ci_result.py --selftest || fail=1

step "0w22/5 步骤索引（docs/STEPS.md 必须与 run_all.sh 的 step 行逐条一致）"
python3 scripts/report_steps.py --check || fail=1

step "0w3/5 文档计数（套数 / 断言总数 / 上屏 fragment 字节数 必须与实测一致）"
python3 scripts/check_counts.py || fail=1

step "0x/5 设备日志判读（三格矩阵 T1/T2/T3 的判据）"
python3 scripts/check_device_log.py --selftest || fail=1
# 这一步在 1/5 之前跑，不能假设 build/hdr_ref 已经存在（解包后第一次跑就是这种情况）
[ -x ./build/hdr_ref ] || cc -O2 -w -I src tools/hdr_ref.c -lm -o build/hdr_ref || fail=1
# 同一份格式化代码产出的真行必须被判读脚本认出来（fmt/head/knee/itm 全部字段）。
# 真机日志里还有一行「EDR 上界跟随已启用」（tick 第一次读到上界时打），判读脚本要求它
# 与 16F+ITM 同时出现 —— hdr_ref 是纯 CPU 参考实现，没有图层，所以这里手工补那一行，
# 模拟「设备上真的跑起来了」的日志形状。
{ echo "I/nvn: HDR: EDR 上界跟随已启用（首帧读数 2.00）"; ./build/hdr_ref --synth hud --frames 2 --stat-line; } > /tmp/dev_selftest.log 2>/dev/null || fail=1
python3 scripts/check_device_log.py --log /tmp/dev_selftest.log --expect t2 >/dev/null || { echo "  ✗ 真行判读失败"; fail=1; }
python3 scripts/check_device_log.py --log /tmp/dev_selftest.log --expect t3 >/dev/null 2>&1 && { echo "  ✗ T3 判据对 16F 行不该通过"; fail=1; }

step "0w/5 真实照片颜色保真（逐像素 CIELAB）"
cc -O2 -w -I src tools/hdr_color_image.c -lm -o build/hdr_color_image || fail=1
python3 scripts/report_color_image.py --check || fail=1

step "0v/5 图像级用例（夜景 / HUD / 字幕 / 淡出）"
cc -O2 -w -I src src/t_hdr_frames.c -lm -o build/t_hdr_frames || fail=1
./build/t_hdr_frames || fail=1

step "0u/5 knee 原子发布（撕裂读写模型）"
cc -O2 -w -I src src/t_hdr_publish.c -lm -o build/t_hdr_publish || fail=1
./build/t_hdr_publish || fail=1

step "0t/5 设备链路整幅对账（分箱逐格 → L1 白点/重心 → 上屏画面）"
cc -O2 -w -I src tools/hdr_device_image.c -lm -o build/hdr_device_image || fail=1
dev_ok=1
for f in build/real/p_*.ppm; do
  for st in 1 4 8; do ./build/hdr_device_image --stride $st --sat 1.6 --p3 0.5 "$f" || dev_ok=0; done
done
[ "$dev_ok" = 1 ] && echo "  ✓ 9 组：256 格全同、GPU/CPU 白点与重心逐位同、画面差 0" || { echo "  ✗ 设备链路对账不过"; fail=1; }

step "1/5 参考 CLI 自检（40 项）"
./tools/ref_check.sh || fail=1

step "2/5 HUD 剔除常数平台区"
./build/sweep_hud > build/sweep_hud.csv 2>/dev/null || cc -O2 -I src -o build/sweep_hud tools/sweep_hud.c -lm
./build/sweep_hud > build/sweep_hud.csv
awk -F, '$1=="0.05" && $2=="3"{printf "  默认 0.05/3 → 剔 %s，误丢 %s\n", $3, $4}' build/sweep_hud.csv
awk -F, 'NR>1 && $1>=0.03 && $1<=0.12 && $2>=2 && $2<=5 && $3=="12/12" && $4=="0/8"' build/sweep_hud.csv | grep -q . || { echo "  ✗ 平台区消失"; fail=1; }

step "3/5 曲线图"
python3 scripts/plot_curves.py docs/curve.svg build/hdr_ref || fail=1

step "4/5 补丁自洽（在干净树上打补丁 + 全部单测）"
if [ $# -ge 1 ]; then
    if ( cd "$1" && git apply --check patch_marker_ignore 2>/dev/null ); then :; fi
    if [ -e "$1/graphics/nvn/gfx/nvn/nvn_hdr_hist.inc" ]; then
        # 树已经打过补丁（例如 make_patch.sh 刚写过）。verify.sh 不幂等（它会 git apply），
        # 但单测本身可以就地跑：把每套 t_*.c 直接编到 build/ 再执行，判据相同（末行含「全过」）。
        echo "  该树已打过补丁 → 跳过 git apply，就地跑全部单测"
        T_INC="-I $1/graphics/nvn/gfx/nvn"
        S2A_REAL_DIR="$PWD/testdata"; export S2A_REAL_DIR
        n=0
        for t in "$1"/graphics/nvn/tests/t_hdr*.c; do
            name=$(basename "$t" .c)
            if cc -O2 -w $T_INC "$t" -lm -o "build/$name" 2>build/$name.err; then
                if "./build/$name" | tail -1 | grep -q '全过\|失败 0'; then n=$((n+1)); else echo "  ✗ $name"; fail=1; fi
            else
                echo "  ✗ $name 编译失败"; head -3 "build/$name.err"; fail=1
            fi
        done
        echo "  就地通过 $n 套"
        [ "$n" -ge 24 ] || fail=1
        python3 scripts/check_landing.py "$1" || fail=1
    else
        ./verify.sh "$1" || fail=1
    fi
    # 0w13/5 挪到这里跑：它断言的是「**打补丁之后**的树上，上游那些格式假设仍成立」。
    # 曾经放在 0wxx 段（打补丁之前），于是带树根跑必然 6 条红——守卫拿没打补丁的上游源码去查
    # 我们插入的行。那不是代码坏了，是跑法错了（2026-11-17 消费端测试首次撞到）。
    step "4b/5 格式假设（对着刚打完补丁的树核 13 条 + docs/UPSTREAM-ASSUMPTIONS.md）"
    python3 scripts/check_format_assumptions.py "$1" --check || fail=1
    # ObjC 与 MSL 在 Linux 侧从不编译：`nvn_mtl.m` 的四处手工合并、`nvn_hdr_hist.inc`
    # 整块设备代码，拼错一个字母在这里不会红，只会在 macOS CI 或真机上炸。
    # 这里做最接近链接的检查：补丁改过的文件里每个前缀符号都得有定义（或上游声明过）。
    step "4c/5 符号对账（补丁改过的 ObjC/MSL 里每个前缀符号都得有定义）"
    python3 scripts/check_patch_symbols.py "$1" || fail=1
    # 4c/5 只看 C 侧（剥掉字符串后，MSL 源码整个被剥成了空白）。MSL 自己那一层的拼写错误
    # 只有 `xcrun metal` 会报，而 3555 B 的 HDR fragment 从未真编过 ⇒ 单独对账一次。
    step "4d/5 MSL 符号对账（着色器里每个前缀符号都得在 MSL 里有定义）"
    python3 scripts/check_msl_symbols.py "$1" || fail=1
    # 补丁是发货内容、src/ 是真相源。两份东西不同步时，4/5 测的是补丁里的旧内容、
    # check_counts/check_msl 核的是 src 里的新内容 —— 一半闸门测旧、一半测新，全绿。
    step "4e/5 补丁新鲜度（src/ 与补丁打出来的树逐字节一致）"
    python3 scripts/check_patch_fresh.py "$1" || fail=1
    # 我们全程用 `git apply`，而使用者手上很可能只有 GNU `patch`。两者「都成功」不等于
    # 「结果一样」：GNU patch 有 fuzz 容忍度、还会写 .orig/.rej。这条把两份结果逐字节比对。
    step "4f/5 两种打补丁工具结果一致（git apply vs GNU patch -p1）"
    ./scripts/check_patch_tools.sh "$1" || fail=1
else
    echo "  跳过（没给树根）；CI 里由 verify.yml 完成这一步"
    echo "  （4b/5 格式假设、4c/5 C 符号对账、4d/5 MSL 符号对账、4e/5 补丁新鲜度、4f/5 双工具一致性也要树根，一并跳过）"
fi

step "5/5 结论"
if [ "$fail" -eq 0 ]; then echo "✓ 全部通过"; else echo "✗ 有失败项，见上"; fi
exit "$fail"
