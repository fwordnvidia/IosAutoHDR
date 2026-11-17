#!/usr/bin/env python3
"""生成 / 校验 docs/REVIEW.md：给 review 的人一张「按这个顺序读」的地图。

为什么需要它：补丁 36 个文件、5,700+ 行，其中 `nvn_mtl.m` 单个就有 17,459 行——
如果 reviewer 从头读，前 16,000 行跟他要判断的东西没关系。这张表回答三个问题：

  1. 这个文件负责什么（一句话）；
  2. 不加它会怎样（这是 review 时唯一真正需要判断的东西：缺陷是否真实存在）；
  3. 它被哪套单测守着（想验证某个说法，直接跑那一套，不用读完文件）。

内容由仓库当前状态算出（文件清单来自补丁本身、行数来自 src/），所以不会漂：
new 文件没写说明、说明指向不存在的文件、行数变了，`--check` 都会红。
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(HERE, "patch", "hdr-auto-l1.patch")
DOC = os.path.join(HERE, "docs", "REVIEW.md")

# 文件 → (一句话职责, 不加会怎样, 守护它的测试套件)
# 顺序即建议阅读顺序：算法核心 → 设备接线 → 宿主/格式 → 对账 → 工具。
INFO = {
    "nvn_hdr.h": (
        "参数层与全部微积分：`s2a_hdr_to_linear`、`s2a_hdr_itm_*`、`s2a_hdr_grade_rgb`、"
        "`s2a_hdr_clip_head`、`s2a_hdr_status_line`、`s2a_hdr_fmt_guard`、原子 knee 打包。",
        "整个补丁没有可复用的纯函数层，任何一处上屏代码都得各自实现一遍 ITM，"
        "GPU/CPU 两侧数值分叉无法避免。",
        ["t_hdr", "t_hdr_stat", "t_hdr_fmtguard", "t_hdr_publish"],
    ),
    "nvn_hdr_auto.h": (
        "直方图与分箱定义（`S2A_HDR_HIST_BINS` 256 格、`s2a_hdr_hist_bin`、"
        "`S2A_HDR_HIST_EVERY 8`、`s2a_hdr_lit_frac`）。",
        "分箱口径（log2 格 / 上沿 / 每 8 帧回读）散落在各处，CPU 与 MSL 各写一份后必然不同。",
        ["t_hdr_auto", "t_hdr_bin_mirror", "t_hdr_gpu_mirror"],
    ),
    "nvn_hdr_auto_l1.h": (
        "自适应判定本体：L1 白点、L1b 拐点（`knee_lo`=纸白、`knee_hi`=截断点）、"
        "HUD 剔除、切镜融合、淡入淡出冻结、按帧率换平滑系数。",
        "拐点只能写死常量（上游 `nvn_present_fi.inc` 的 0.55/1.0 就是这样），"
        "HDR、SDR、暗场、字幕全用同一组数——这就是补丁要解决的原始问题。",
        ["t_hdr_auto_l1", "t_hdr_hud", "t_hdr_fade", "t_hdr_framerate", "t_hdr_frames"],
    ),
    "nvn_hdr_hist.inc": (
        "设备侧接线：建直方图纹理/缓冲、每 8 帧 `copyFromBuffer` 回 CPU、算 L1b、"
        "原子发布 knee、打 `HDR stat` 诊断行。",
        "算法存在但没人调用：画面上不会有任何变化。**这个文件是「补丁是否真的生效」的唯一证据源**，"
        "`check_landing.py` 有 8 项只盯它。",
        ["t_hdr_tick", "t_hdr_wiring", "t_hdr_pipeline"],
    ),
    "nvn_mtl.m": (
        "宿主：16F 画布与 EDR 三件套的启用/降级/回头路、`present_blit` 两条路径取 knee、"
        "每帧 `hdr_head_tick()` 跟随屏幕上界、diagnostic 覆盖。",
        "没有 HDR 画布、没有上屏扩张，且降级路径（外接 SDR 屏、老设备）会永久黑屏或永久发灰。",
        ["t_hdr_pipeline", "t_hdr_head", "t_hdr_fmtguard", "t_hdr_stability"],
    ),
    "nvn_present_fi.inc": (
        "上游补帧（MetalFX）的上屏环节，补丁只做一件事：把两处写死的 `BGRA8Unorm` 参数化，"
        "让 16F 画布也能过补帧。",
        "HDR 与补帧二选一：开补帧就得回 8 位，HDR 被关掉。",
        ["t_hdr_gpu_mirror"],
    ),
    "nvn_hdr_msl.h": (
        "上屏 fragment 的 C 转写（与 `hdr_hist.metal` 里的 shader 逐行对应），供对账用。",
        "上屏 shader 没有任何本机可执行的形式，成了「改了也没人知道」的黑盒。",
        ["t_hdr_shader_mirror"],
    ),
    "nvn_hdr_msl_hist.h": (
        "直方图 kernel 的 C 转写（分箱 / 分位 / L1 三段），逐行对应 MSL。",
        "MSL 与 CPU 的分箱、分位、黑像素口径会各自漂移；历史上正是这里抓到 `lit_frac` 设备恒 1.0。",
        ["t_hdr_bin_mirror", "t_hdr_knee_mirror"],
    ),
    "hdr_hist.metal": (
        "直方图 kernel：`hdr_hist_clear/add`（256 格原子累加，黑像素也记进 0 号格）、"
        "`hdr_hist_l1`（仅用于对账日志与 blit 失败 fallback）。",
        "设备上没有任何亮度统计，自适应无从谈起。",
        ["t_hdr_bin_mirror", "t_hdr_gpu_mirror"],
    ),
    "host_adapt.m": (
        "诊断入口：`S2A_HDR_FMT` / `S2A_HDR_OUTPUT` 两个环境变量覆盖像素格式与 EDR 声明，"
        "供真机三格矩阵使用。",
        "真机只能靠改代码重编才能试一格，三格矩阵要编三个包。",
        ["t_hdr_pipeline"],
    ),
    "Makefile": (
        "把全部单测（当前 %d 套）接进上游的 `make test`，与会话里 `verify.sh` 的 glob 保持同一份名单。" % sum(1 for f in os.listdir(os.path.join(HERE, "src")) if re.match(r"t_hdr.*\.c$", f)),
        "单测存在但没人跑（`verify.sh` 曾硬编码 13 个套件名，新增的 8 套静默漏跑了很久）。",
        [],
    ),
}

# 对账类与工具类不在补丁里（只随包发布），单独一段说明。
EXTRA = [
    ("tools/hdr_ref.c", "脱离 S2Arm 的参考 CLI：合成场景、读 PPM、`--stat-line` 打与设备同格式的一行。", "tools/ref_check.sh（43 项）"),
    ("tools/hdr_seq.c", "逐帧跑一段序列，输出拐点/白点/冻结列，用于时间行为（转场、淡入淡出）。", "scripts/report_sequence.py"),
    ("tools/hdr_ablate.c", "四档定拐点策略（fixed / apl / white / full）在同一批帧上的对照。", "scripts/report_ablate.py"),
    ("tools/hdr_cost.c", "每帧 CPU 开销（`step_fused` 0.57 µs）。", "scripts/report_cost.py"),
    ("tools/hdr_device_image.c", "整幅跑「MSL 链路」与「CPU 链路」两遍，逐像素对账。", "run_all.sh 0t/5"),
    ("tools/hdr_shader_image.c", "上屏 fragment 的 C 转写作用到整幅照片，与 CPU 版逐像素对账。", "run_all.sh（整幅对账）"),
    ("tools/hdr_color_image.c", "逐像素 CIELAB：色相角误差与 a*/b* 位移。", "scripts/report_color_image.py"),
    ("tools/hdr_fidelity.c", "逐像素：纸白以下有没有动、多少像素真吃到增益、增益分布。", "scripts/report_fidelity.py"),
    ("scripts/check_landing.py", "在打好的树上断言 36 处注入点仍在（按函数范围而不是行号）。", "run_all.sh 4/5"),
    ("scripts/check_msl.py", "没有 Metal 编译器时挡 MSL 已知雷区（72 项 / 4 个 MSL 文件）。", "run_all.sh 0f/5"),
    ("scripts/check_device_log.py", "把设备日志判读成 T1/T2/T3 结论（8 项自测）。", "run_all.sh 0x/5"),
]


def patch_files():
    """补丁涉及的文件（只留 basename，用来对应 src/）。

    为什么不用 `git apply --numstat`：那是**相对 cwd** 解析的。本仓库根是 `ios-build`，
    `hdr-auto/` 是它的子目录，补丁里的路径却是 `graphics/nvn/…`（相对上游树根）——
    于是每个片段都被「跳过」，numstat 返回 0 行、rc 仍是 0。后果是**静默**的：
    `missing`（新增文件没写说明）恒空、「stale」把 INFO 里全部条目当过期。
    这里直接读补丁头，不碰工作区，也不受 cwd 影响。
    """
    names = []
    for line in open(PATCH, encoding="utf-8", errors="replace"):
        if line.startswith("diff --git "):
            names.append(line.rstrip("\n").split(" b/")[-1].split("/")[-1])
    if len(names) < 40:          # 真值 42；掉了说明补丁头格式变了或文件被截断，宁红不静默
        raise SystemExit("补丁里只解析出 %d 个文件（预期 ≥40），补丁头格式变了？" % len(names))
    return names


def src_lines(name):
    for d in ("", "tests", "gfx/nvn", "shaders"):
        p = os.path.join(HERE, "src", name) if not d else os.path.join(HERE, "src", os.path.basename(name))
        if os.path.exists(p):
            return sum(1 for _ in open(p, encoding="utf-8", errors="replace"))
    return None


def render():
    files = patch_files()
    have = set(files)
    missing = [f for f in files if f not in INFO and not f.startswith("t_hdr")]
    if missing:
        raise SystemExit("补丁新增了没写说明的文件：%s（在 scripts/report_review.py 的 INFO 里补一行）"
                         % ", ".join(sorted(missing)))
    stale = [f for f in INFO if f not in have]
    if stale:
        raise SystemExit("INFO 里有补丁不再涉及的文件：%s" % ", ".join(sorted(stale)))

    total = sum(l for l in (src_lines(f) for f in INFO) if l)
    tests = sorted(f for f in have if f.startswith("t_hdr"))
    L = []
    L.append("# Review 地图\n")
    L.append("补丁 %d 个文件（其中 %d 个新增 src 文件、%d 套单测），"
             "新增/改动的 src 合计 **%d 行**（含 `nvn_mtl.m` 的 17,459 行存量）。"
             "本表由 `scripts/report_review.py` 按仓库当前状态生成，`--check` 会挡漂移。\n"
             % (len(files), len(INFO), len(tests), total))
    L.append("## 先读这五个（约 20 分钟）\n")
    L.append("按这个顺序读，就能判断「缺陷是否真实、修法是否成立」；其余文件只在需要核对细节时再看。\n")
    order = ["nvn_hdr.h", "nvn_hdr_auto_l1.h", "nvn_hdr_hist.inc", "nvn_mtl.m", "nvn_hdr_auto.h"]
    for i, f in enumerate(order, 1):
        duty, why, guards = INFO[f]
        L.append("%d. **`%s`**（%s 行）—— %s" % (i, f, _n(src_lines(f)), duty))
        L.append("   不加会怎样：%s" % why)
    L.append("")
    L.append("## 逐文件\n")
    L.append("| 文件 | 行数 | 职责 | 不加会怎样 | 守它的套件 |")
    L.append("|---|---|---|---|---|")
    for f, (duty, why, guards) in INFO.items():
        g = "、".join("`%s`" % x for x in guards) if guards else "（由 `run_all.sh` 步骤覆盖）"
        L.append("| `%s` | %s | %s | %s | %s |" % (f, _n(src_lines(f)), duty, why, g))
    L.append("")
    L.append("## 单测（%d 套）\n" % len(tests))
    L.append("`verify.sh` 用 glob 收集 `src/t_hdr*.c` 并断言套数下限，所以新增一套不会被静默漏跑。"
             "每套只做一件事：把某个「只在设备上执行」的实现（MSL kernel、上屏 fragment、"
             "回读控制流）逐行翻成 C，与参考实现对账。\n")
    L.append("| 套件 | 行数 | 对的是哪一段 |")
    L.append("|---|---|---|")
    for f in tests:
        L.append("| `%s` | %s | %s |" % (f, _n(src_lines(f)), _target(f)))
    L.append("")
    L.append("## 不在补丁里、但 review 时用得上的工具\n")
    L.append("| 文件 | 作用 | 谁在跑 |")
    L.append("|---|---|---|")
    for f, what, who in EXTRA:
        L.append("| `%s` | %s | %s |" % (f, what, who))
    L.append("")
    return "\n".join(L) + "\n"


def _n(v):
    return "—" if v is None else str(v)


TARGET = {
    "t_hdr": "线性化/ITM/增益（参数层）",
    "t_hdr_auto": "分箱与亮度统计口径",
    "t_hdr_auto_l1": "L1 白点、黑场截断与平滑",
    "t_hdr_auto_l1.c": "L1 白点与平滑",
    "t_hdr_auto_l1b": "L1b 拐点",
    "t_hdr_banding": "16F→8 位预览的台阶",
    "t_hdr_bin_mirror": "MSL 分箱（逐行 C 转写）",
    "t_hdr_bt2408": "BT.2408 放置方式（100→203 nit）",
    "t_hdr_bt2408.c": "BT.2408 放置方式",
    "t_hdr_color": "饱和度算子色相误差",
    "t_hdr_env": "出屏格式 / EDR 声明两个开关的组合（三格矩阵的语义真相）",
    "t_hdr_identity": "全关态逐位一致（sRGB 解码→×1→编码 穷举 256 码值）——「没开就跟上游一样」的数值证据",
    "t_hdr_oracle": "白点/重心 与独立参考实现对账（不验场景，验实现是否偏离头文件里的定义）",
    "t_hdr_fuzz": "随机直方图不变量（12 条性质）：不预设场景，只要求任何输入下都成立",
    "t_hdr_map": "BT.2408 内容映射（C 与 MSL 转写对账）",
    "t_hdr_fade": "淡入淡出冻结判据",
    "t_hdr_fmtguard": "16F 降级与回头路",
    "t_hdr_framerate": "按帧率换平滑系数",
    "t_hdr_frames": "图像级用例（夜景/HUD/字幕/淡出）",
    "t_hdr_gpu_mirror": "设备链路整链（分箱→L1→上屏）",
    "t_hdr_head": "EDR 上界跟随",
    "t_hdr_hud": "HUD/字幕簇剔除",
    "t_hdr_knee_mirror": "MSL 分位阈值",
    "t_hdr_letterbox": "画幅/黑边与拐点无关",
    "t_hdr_pipeline": "管线格式自适应",
    "t_hdr_publish": "knee 原子发布（撕裂）",
    "t_hdr_quality": "量化台阶",
    "t_hdr_robust": "退化输入（NaN/inf/全黑）",
    "t_hdr_shader_mirror": "上屏 fragment 逐行",
    "t_hdr_stability": "长时间运行稳定性",
    "t_hdr_stat": "诊断行格式",
    "t_hdr_tick": "直方图回读控制流（gen 防串帧）",
    "t_hdr_wiring": "注入点自洽",
}


def _target(f):
    stem = f[:-2] if f.endswith(".c") else f
    if stem in TARGET:
        return TARGET[stem]
    if stem.startswith("t_hdr_auto_l1b"):
        return "L1b 拐点"
    return "（未登记，补 scripts/report_review.py）"


def main():
    text = render()
    if "--check" in sys.argv:
        if not os.path.exists(DOC) or open(DOC, encoding="utf-8").read() != text:
            print("::error::docs/REVIEW.md 与仓库状态不一致（改了 src/ 或补丁文件清单没重新生成？）")
            return 1
        print("✓ docs/REVIEW.md 与仓库一致（%d 行）" % text.count("\n"))
        return 0
    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    open(DOC, "w", encoding="utf-8").write(text)
    print("写出 docs/REVIEW.md（%d 行）" % text.count("\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
