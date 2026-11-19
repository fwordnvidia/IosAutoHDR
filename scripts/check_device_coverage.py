#!/usr/bin/env python3
"""只在设备上跑的代码：逐项登记「有没有对账、凭什么」。

背景：这里没有 macOS、没有 Xcode、没有 `xcrun metal`，`clang -fsyntax-only` 连 Metal 头都缺。
所以凡是只在 Metal/设备上执行的代码，本机能给它的**唯一**保护就是对账——把 MSL 逐行翻成 C，
用同一份输入跑 CPU 参考实现。这件事已经抓出 5 个真 bug（黑像素口径、inf UB、分位单侧偏移一格、
退化拐点、换分辨率后旧回调写新帧）。

问题不在于「有没有做对账」，而在于**没人知道自己漏了哪一项**。这个脚本把这件事变成一张表：
`src/hdr_hist.metal` 里每个 kernel、以及上屏的两条 fragment 路径，都必须登记状态；
登记「有对账」的套件必须真的存在；登记「无对账」的必须写清理由（≥20 字，空话会被看出来）。

新增设备侧代码而不登记 ⇒ 这里红。想清楚为什么不做，比忘记做要好。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (符号, 所在文件, 是否被调度, 对账套件 or None, 理由/说明)
#
# 「被调度」不是靠嘴说：下面会去源码里找 setComputePipelineState/dispatchThreads 或
# present_pipeline 的调用证据，找不到就报「你说它被调度，但源码里没有 dispatch 证据」。
DEVICE = [
    ("hdr_hist_clear", "src/hdr_hist.metal", True, None,
     "三行：tid<256 才清零，dispatch 正好 256 线程（nvn_hdr_hist.inc:105），线程数与 bin 数对齐是"
     "构造保证的，没有可以算错的分支；真出错的表现是「上一帧的计数残留」，会被白点逐帧抖动立刻看出来。"),
    ("hdr_hist_add", "src/hdr_hist.metal", True, "src/t_hdr_bin_mirror.c",
     "逐样本对账（含黑像素入 0 号格、inf/NaN、sRGB 线性化的分段点）；它抓出过 lit_frac 设备 1.0 / CPU 0.04 的分歧。"),
    ("hdr_hist_knee", "src/hdr_hist.metal", False, "src/t_hdr_knee_mirror.c",
     "L0 的 99.5 分位；对账抓出 float 单侧偏多一格（1,515,877 个 total 值不同 ⇒ hi 差 0.0547 stop），已改整数千分。"
     "该 kernel 建了管线但从未被调度（check_landing 断言），保留作 fallback 与参考。"),
    ("hdr_hist_l1", "src/hdr_hist.metal", False, "src/t_hdr_gpu_l1.c",
     "L1 白点/重心/场景权重。avg 差 5.96e-07 stop、white 同格、need 量化 0 差异；"
     "已知差异是它没有 HUD 簇剔除（HUD 画面白点被顶格拉到 13.24 ⇒ hi 打满 2.0 ⇒ 等于不扩张），写在 DESIGN。"
     "**曾经被 dispatch 但结果无人读**（每 8 帧白算一次），已删掉那段 dispatch，管线留着给对账当参考。"),
    ("src_msl_hdr（HDR 上屏 fragment）", "src/nvn_mtl.m", True, "src/t_hdr_shader_mirror.c",
     "177,147 组（9³ 编码值 × 3 拐点 × head × sat × p3）对账，另有一份整幅照片的 tools/hdr_shader_image.c；"
     "抓出过「knee_lo==knee_hi 时 shader 给 ×head、CPU 给 ×1」的分歧。"),
    ("src_msl_8bit（8 位直通 fragment）", "src/nvn_mtl.m", True, "src/t_hdr_identity.c",
     "穷举 256 码值 × 5 条路径逐位一致；它守的是「HDR 关掉时画面必须一个字节都不变」。"),
    ("nvn_hdr_hist.inc（回读控制流）", "src/nvn_hdr_hist.inc", True, "src/t_hdr_tick.c",
     "换分辨率时旧 addCompletedHandler 会读新对象、把旧直方图结论写进 g_hdr_knee；对账用 gen 计数挡住，15 项。"),
    ("present_blit / present_blit_m4（取 knee 与 16F 分支）", "src/nvn_mtl.m", True, "src/t_hdr_publish.c",
     "knee 的 64 位原子发布/消费（撕裂时读到「新 lo + 旧 delta」）；另有 check_landing 按函数范围断言两条路径都 take。"),
    ("nvn_present_fi.inc（HDR + 补帧共存）", "src/nvn_present_fi.inc", True, "src/t_hdr_frames.c",
     "uv 缓冲 8→16 float（曾经 8 ⇒ knee 恒 0 ⇒ 16F 画布全黑且日志「渲染成功」）、16F 中间帧改走 present_blit_m4。"),
    ("host_adapt.m 的 s2a_hdr_out_plan（输出侧三格）", "src/host_adapt.m", True, "src/t_hdr_env.c",
     "四格语义表；原先 S2A_HDR_OUTPUT=1 会让 FMT=1 永远拿不到 EDR，把唯一能判黑屏归因的实验判据自己废掉。"),
    ("nvn_mtl.m 的 hdr_head_tick（运行期 headroom）", "src/nvn_mtl.m", True, "src/t_hdr_head.c",
     "每帧读屏幕 EDR 上界（iOS：UIScreen.currentEDRHeadroom；macOS：NSScreen.maximumExtendedDynamicRangeColorComponentValue），死区 1%；未声明 EDR（T1）时锁 1.0。"),
    ("16F 降级 / 回头路（fmt guard）", "src/nvn_mtl.m", True, "src/t_hdr_fmtguard.c",
     "连续 60 帧拿不到 drawable 才降级、降级时像素格式+EDR+颜色空间三者一起改、600 帧后试探回来。"),
    ("s2a_out_layer_hdr（图层的 EDR 声明与颜色空间）", "src/host_adapt.m", True, None,
     "纯 UIKit/QuartzCore 属性赋值，没有可对账的算术：它做的事就是两个属性 + 一个颜色空间对象。"
     "本机能给它的保护是结构断言——`0w20/5` 的 H8 钉住「iOS 与 Mac 两个分支各调一次、且函数体里"
     "同时有 EDR 声明与扩展线性 sRGB」。这条与 §129 的 Metal 3 静默失效同一个病：漏设一侧时"
     "画面发灰或高光被裁，却没有一行日志。"),
    ("s2a_hdr_status_line（诊断行）", "src/nvn_hdr.h", True, "src/t_hdr_stat.c",
     "固定格式逐字符可 diff；tools/hdr_ref.c --stat-line 调同一函数，任何一侧改了格式另一边立刻红。"),
]

def read(rel):
    p = os.path.join(ROOT, rel)
    return open(p, encoding="utf-8", errors="replace").read() if os.path.exists(p) else ""

def kernels_in_metal():
    src = read("src/hdr_hist.metal")
    return re.findall(r"kernel\s+void\s+(\w+)", src)

def dispatch_evidence(symbol):
    """去源码里找真正把它跑起来的证据（不是注释里的名字）。"""
    inc = read("src/nvn_hdr_hist.inc")
    if symbol.startswith("hdr_hist_"):
        short = {"hdr_hist_clear": "ps_clear", "hdr_hist_add": "ps_add",
                 "hdr_hist_knee": "ps_knee", "hdr_hist_l1": "ps_l1"}.get(symbol, "")
        if not short:
            return False
        # 需要有 setComputePipelineState:<ps_xxx> 且同文件里有 dispatchThreads
        return (f"setComputePipelineState:{short}" in inc or f"setComputePipelineState:f->{short}" in inc) \
               and "dispatchThreads" in inc
    if symbol.startswith("src_msl_hdr"):
        return "src_msl_hdr" in read("src/nvn_mtl.m")
    if symbol.startswith("src_msl_8bit"):
        return "src_msl_8bit" in read("src/nvn_mtl.m") or "present_pipeline" in read("src/nvn_mtl.m")
    if symbol.startswith("present_blit"):
        return "present_blit(" in read("src/nvn_mtl.m")
    if symbol.startswith("nvn_hdr_hist.inc"):
        return "hdr_hist_tick" in read("src/nvn_mtl.m")
    if symbol.startswith("nvn_present_fi.inc"):
        return "fi_pass" in read("src/nvn_present_fi.inc")
    if symbol.startswith("host_adapt.m"):
        return "s2a_hdr_out_plan" in read("src/host_adapt.m")
    if symbol.startswith("nvn_mtl.m 的 hdr_head_tick"):
        return "hdr_head_tick" in read("src/nvn_mtl.m")
    if symbol.startswith("16F 降级"):
        return "s2a_hdr_fmt_guard" in read("src/nvn_mtl.m")
    if symbol.startswith("s2a_hdr_status_line"):
        return "s2a_hdr_status_line" in read("src/nvn_hdr.h")
    return True

def main():
    fails = []
    registered = {d[0]: d for d in DEVICE}

    # 投影形态：登记表里一半的条目指向上游整文件（src/nvn_mtl.m 1.9 MB、
    # src/nvn_present_fi.inc、src/host_adapt.m），而公开仓库按设计只交付差分。
    # 没有这些文件时逐条报「声明的文件不存在」是错的——那不是登记缺失，是没有对象可查。
    # 2026-11-17 之前在 clone 下来的投影里跑 ./run_all.sh 会看到 14 处 ::error::。
    missing_upstream = [r for r in ('src/nvn_mtl.m', 'src/nvn_present_fi.inc', 'src/host_adapt.m')
                        if not os.path.exists(os.path.join(ROOT, r))]
    if missing_upstream:
        print("· 投影形态：没有上游整文件（%s）⇒ 跳过设备侧登记核对" % "、".join(missing_upstream))
        print("  这一层只在带上游树的开发树里有效：登记表核对的是「声明的调度点在源码里真的存在」")
        return 0

    # 1) hdr_hist.metal 里每个 kernel 都必须登记（漏登记 = 新增了没人管的设备侧代码）
    for k in kernels_in_metal():
        if k not in registered:
            fails.append(f"src/hdr_hist.metal 的 kernel {k} 没有登记对账状态（在 DEVICE 表里补一行）")

    # 2) 登记「有对账」的套件必须存在
    for sym, f, sched, suite, why in DEVICE:
        if not os.path.exists(os.path.join(ROOT, f)):
            fails.append(f"{sym}: 声明的文件 {f} 不存在")
        if suite:
            if not os.path.exists(os.path.join(ROOT, suite)):
                fails.append(f"{sym}: 对账套件 {suite} 不存在")
        else:
            if len(why) < 20:
                fails.append(f"{sym}: 不做对账必须写清理由（现在只有 {len(why)} 字）")

    # 3) 声明「被调度」的必须有调度证据；声明「未调度」的必须真的没有
    for sym, f, sched, suite, why in DEVICE:
        ev = dispatch_evidence(sym)
        if sched and not ev:
            fails.append(f"{sym}: 声明被调度，但源码里找不到 dispatch/pipeline 证据")
        if not sched and ev:
            fails.append(f"{sym}: 声明未被调度，但源码里找到了调度证据（请改表或改代码）")

    # 4) 生成表
    lines = ["# 只在设备上执行的代码：逐项状态\n",
             "这份表由 `scripts/check_device_coverage.py` 生成（`run_all.sh` 的 `0w11/5` 会挡漂移）。",
             "这里没有 macOS、没有 Xcode、没有 `xcrun metal`，所以对账是这些代码在本机能拿到的**唯一**保护。\n",
             "| 代码 | 被调度 | 对账 | 为什么 / 不做的理由 |", "| --- | --- | --- | --- |"]
    for sym, f, sched, suite, why in DEVICE:
        s = "是" if sched else "**否**"
        d = f"`{suite}`" if suite else "**无**"
        lines.append(f"| `{sym}`<br>{f} | {s} | {d} | {why} |")
    lines.append("")
    lines.append("## 表外的东西\n")
    lines.append("- 上游自己的 kernel/fragment（`ms`、`ixc_*`、`wide_u8`、`mdx_mask`、`cp`、`dt_fs` 等）不在本表："
                 "它们不是我们加的，改动只经由补丁的上下文，`check_landing.py` 负责断言我们没动到它们。")
    lines.append("- 类型正确性（`float` vs `half`、buffer 对齐、attribute 序号）不在对账范围："
                 "`check_msl.py` 72 项做静态检查，其余交给真机（未执行）。")
    out = os.path.join(ROOT, "docs/DEVICE-ONLY.md")
    text = "\n".join(lines) + "\n"

    if "--check" in sys.argv:
        old = read("docs/DEVICE-ONLY.md")
        if old != text:
            fails.append("docs/DEVICE-ONLY.md 与 DEVICE 表不一致（重新生成：python3 scripts/check_device_coverage.py）")
    else:
        open(out, "w", encoding="utf-8").write(text)
        print(f"写出 docs/DEVICE-ONLY.md（{len(DEVICE)} 项）")

    if fails:
        print(f"✗ {len(fails)} 处问题：")
        for x in fails:
            print(f"  ::error::{x}")
        return 1
    covered = sum(1 for d in DEVICE if d[3])
    print(f"✓ 设备侧代码登记完整：{len(DEVICE)} 项，{covered} 项有对账，{len(DEVICE)-covered} 项写明理由")
    return 0

if __name__ == "__main__":
    sys.exit(main())
