#!/usr/bin/env python3
"""补丁「插在哪」自检：能打上 != 插对了地方。

为什么需要它：补丁按 v0.4.0 制作，而上游 main 已前进几百个提交（2026-11-16 实测 353 个）。
`git apply` 只看上下文行，上下文恰好还在、但那个函数已经被挪走/改名/改写的情况它照样 rc=0。
于是每处注入都单独校验「它是否还落在预期的函数体内」，以及几处跨文件不变量。

用法：check_landing.py <已打过补丁的树根>
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
MTL = os.path.join(ROOT, "graphics/nvn/gfx/nvn/nvn_mtl.m")
FI = os.path.join(ROOT, "graphics/nvn/gfx/nvn/nvn_present_fi.inc")
HOST = os.path.join(ROOT, "graphics/nvn/host_adapt.m")
MK = os.path.join(ROOT, "graphics/nvn/Makefile")

ok = bad = 0


def check(cond, what, detail=""):
    global ok, bad
    if cond:
        ok += 1
        print("  ✓ %s" % what)
    else:
        bad += 1
        print("  ✗ %s%s" % (what, ("  —— " + detail) if detail else ""))


def read(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read().split("\n")


def func_range(lines, name):
    """返回 (start, end) 行号（0 基，闭区间）：列 0 起头的函数定义到列 0 的 '}'。"""
    pat = re.compile(r"^[\w\s\*<>\[\]]*\b" + re.escape(name) + r"\s*\(")
    for i, ln in enumerate(lines):
        if pat.match(ln) and ln.rstrip().endswith("{"):
            for j in range(i + 1, len(lines)):
                if lines[j].startswith("}"):
                    return i, j
            return i, len(lines) - 1
    return None


def contains(lines, fname, needle, nth=1):
    r = func_range(lines, fname)
    if not r:
        return False, "找不到函数 %s" % fname
    hits = [i for i in range(r[0], r[1] + 1) if needle in lines[i]]
    if len(hits) < nth:
        return False, "%s 里没有第 %d 处 %r（函数 %d-%d 行）" % (fname, nth, needle, r[0] + 1, r[1] + 1)
    return True, ""


print("· 树根：%s" % ROOT)
lines = read(MTL)
src = "\n".join(lines)

# 1) 定义恰好一处，调用恰好两处，且两处分别在 MTL3 / MTL4 的两个 blit 里
check(len(re.findall(r"^static void hdr_head_tick\(void\)", src, re.M)) == 1,
      "hdr_head_tick 定义恰好一处")
calls = [i for i, ln in enumerate(lines) if re.search(r"^\s*hdr_head_tick\(\);", ln)]
check(len(calls) == 2, "hdr_head_tick 被调用恰好两处", "实际 %d 处" % len(calls))
for fn in ("present_blit", "present_blit_m4"):
    good, why = contains(lines, fn, "hdr_head_tick();")
    check(good, "调用点落在 %s 函数体内" % fn, why)

# 2) 上屏常量由 g_hdr_knee 每帧填，且 [2] 在 hdr_extra_once 与 hdr_head_tick 里都被写过
check(contains(lines, "hdr_extra_once", "g_hdr_knee[2]")[0],
      "hdr_extra_once 写 g_hdr_knee[2]")
check(contains(lines, "hdr_head_tick", "g_hdr_knee[2]")[0],
      "hdr_head_tick 写 g_hdr_knee[2]")

# 3) 直方图回读只在 nvn_mtl.m 里被 include 一次
check(len(re.findall(r'#include "nvn_hdr_hist\.inc"', src)) == 1,
      "nvn_hdr_hist.inc 只被 include 一次")

# 4) 16F 常量块与 8 位块分属两个 blit：knee 走 uv[8..11]（MTL4）与后 32 字节（MTL3）
check("uv[8] = m4lo" in src and "uv[9] = m4de" in src and "uv[10] = g_hdr_knee[2]" in src,
      "MTL4 上屏把 knee 写进 uv[8..11]（lo/delta 来自原子槽，head/flag 来自 g_hdr_knee）")

# 5) 补帧链的参数化仍在（cfmt 声明 + 至少两处使用）
fi = "\n".join(read(FI))
check(re.search(r"MTLPixelFormat\s+cfmt", fi) is not None, "nvn_present_fi.inc 仍有 cfmt 参数")
check(fi.count("cfmt") >= 3, "cfmt 被使用（>=3 处）", "实际 %d 处" % fi.count("cfmt"))
# 5b) 补帧的 uniform 缓冲必须够 MTL4 上屏写 knee+extra（16 float），否则 16F 那条路上 knee 恒 0
#     ⇒ 扩张 shader 走退化分支返回 a*knee.z(=0) ⇒ HDR + 补帧同时开时画布全黑。
check("newBufferWithLength:16 * sizeof(float)" in fi,
      "补帧的 uniform 缓冲是 16 float（8 裁剪 + 4 knee + 4 extra），与 present_blit_m4 的 HdrCrop 一致")
check(re.search(r"newBufferWithLength:12 \* sizeof\(float\)", fi) is None,
      "补帧不再用 12 float 的缓冲（那个长度会让 present_blit_m4 跳过 knee 写入）")
# 5c) 16F 下补出的帧也要过同一遍扩张（走 present_blit_m4），否则隔帧一亮一暗
check(re.search(r"f->cfmt == MTLPixelFormatRGBA16Float[\s\S]{0,400}?present_blit_m4\(cb, mid", fi) is not None,
      "16F 时补出的帧走 present_blit_m4（与真帧同一遍扩张），不是纯拷贝")

# 6) layer 侧：16F + EDR 三件套仍在同一文件
host = "\n".join(read(HOST))
check("MTLPixelFormatRGBA16Float" in host, "host_adapt.m 仍有 RGBA16Float")
check("wantsExtendedDynamicRangeContent" in host, "host_adapt.m 仍设 EDR 内容标志")
# 格式/EDR 的决策必须在纯函数里（有单测 t_hdr_env），host_adapt.m 只许取值 ——
# 以前两份手写分支里 FMT=1 会把 OUTPUT=1 的 EDR 关掉，而三格矩阵的 T2 恰好那么写。
check("s2a_hdr_out_plan(" in host, "host_adapt.m 走 s2a_hdr_out_plan（纯函数、有单测），不自己判分支")
check("if (f && *f == '1') return 0;" not in host,
      "host_adapt.m 不再有「FMT=1 就把 EDR 关掉」那条分支（那会让 T2 永远不成立）")
check(host.count("s2a_out_plan(&w16, &edr)") >= 2, "两个出屏决策都从同一个 plan 取值")
# EDR 上界的读数不在 host_adapt.m，而在 nvn_mtl.m 的 hdr_head_tick 里（按 Apple 文档读 layer 的
# maximumExtendedDynamicRangeColorComponent；potentialEDRHeadroom 是屏幕级预测量，上屏用不到）。
# 断言：读数存在、带 respondsToSelector 守卫、不在 host_adapt.m 里重复一份。
check("maximumExtendedDynamicRangeColorComponent" in src,
      "nvn_mtl.m 每帧读 maximumExtendedDynamicRangeColorComponent")
check(contains(lines, "hdr_head_tick", "respondsToSelector")[0],
      "hdr_head_tick 读到之前先 respondsToSelector 守卫")
check("EDRHeadroom" not in host and "maximumExtendedDynamicRange" not in host,
      "host_adapt.m 不重复做 EDR 上界读数（单一真相源在 hdr_head_tick）")

# 7) 构建链：GENH 里嵌了直方图 MSL，且规则引用的两个文件真实存在
mk = "\n".join(read(MK))
check("nvn_hdr_hist_msl.h" in mk, "Makefile 的 GENH 含 nvn_hdr_hist_msl.h")
for p in ("graphics/nvn/shaders/hdr_hist.metal", "graphics/metal/tools/embed_msl.py"):
    check(os.path.exists(os.path.join(ROOT, p)), "构建依赖存在：%s" % p)

# 16F 画布取不到 drawable 时的降级路径必须真的接在 present 里（否则又是静默黑屏）
for needle, what in (
    ("s2a_hdr_fmt_guard_step(&g_hdr_fmt_guard, 0", "present_now 里报「拿不到」给守卫"),
    ("s2a_hdr_fmt_guard_step(&g_hdr_fmt_guard, 1", "present_now 里报「拿到了」给守卫"),
    ("MTLPixelFormatBGRA8Unorm", "present_now 里确实把像素格式改回 8 位"),
    ("present_pipeline(layer.pixelFormat)", "降级后立即按新格式重建管线"),
    ("kCGColorSpaceSRGB", "降级时把颜色空间一起退回 sRGB（否则 8 位字节被当线性值）"),
    ("wantsExtendedDynamicRangeContent = NO", "降级时关掉 EDR 内容标志"),
):
    good, why = contains(lines, "present_now", needle)
    check(good, what, why)

# knee 走 64 位原子发布：写入方在直方图回调里（三处更新都要发），读取方在两条 present 路径里
inc_path = os.path.join(ROOT, "graphics/nvn/gfx/nvn/nvn_hdr_hist.inc")
inc_lines = read(inc_path) if os.path.exists(inc_path) else []
pubs = sum(1 for ln in inc_lines if "s2a_hdr_knee_publish(&g_hdr_knee_pub" in ln)
check(pubs >= 3, "nvn_hdr_hist.inc 的每处 knee 更新都走原子发布（≥3 处）", "实际 %d 处" % pubs)
for fn in ("present_blit", "present_blit_m4"):
    good, why = contains(lines, fn, "s2a_hdr_knee_take(&g_hdr_knee_pub")
    check(good, "%s 里 knee 从原子槽取（不直接读 g_hdr_knee[0..1]）" % fn, why)

# hdr_hist_knee 这条 kernel 在本补丁里**从未被调度**（只建 ps_clear/ps_add/ps_l1 三条管线）：
# 设备上的 knee 一律来自 CPU 的 s2a_hdr_auto_l1b_step_fused_hz。若将来有人把它接上去，
# 这条会变红，提醒同步补「GPU 侧 L0 与 CPU 的逐位对账」（t_hdr_knee_mirror 目前守的是算式，不是调度）。
knee_used = any("ps_knee" in ln or ("hdr_hist_knee" in ln and "kernel" not in ln and "//" not in ln)
                for ln in inc_lines)
check(not knee_used, "hdr_hist_knee 仍然未被调度（若接上去，需同步加 GPU/CPU L0 对账）")

# hdr_hist_l1 同理：管线建着（ps_l1），但**不 dispatch**——它算出的 f->out 没有读者，
# knee 全部在 CPU 侧按回读的真直方图算。曾经每 8 帧白跑一次单线程 256 格循环。
l1_dispatched = any("setComputePipelineState:f->ps_l1" in ln for ln in inc_lines)
check(not l1_dispatched, "hdr_hist_l1 不 dispatch（结果无人读；接回去要先补 HUD 簇剔除，见 t_hdr_gpu_l1）")

# 降级之后必须能回来：present_now 里要有 rearm 试探，并且把三样状态一起恢复
# （像素格式、EDR 开关、颜色空间）。少恢复一样就会出现「画面回来了但颜色发灰」。
for needle, what in (
    ("s2a_hdr_fmt_guard_rearm(&g_hdr_fmt_guard, g_hdr_want16)", "present_now 里降级后按退避试探 16F"),
    ("layer.pixelFormat = MTLPixelFormatRGBA16Float;", "试探时把像素格式切回 16F"),
    ("kCGColorSpaceExtendedLinearSRGB", "试探时把颜色空间切回 ExtendedLinearSRGB"),
    ("g_hdr_want16 = 1;", "确实要过 16F 时才记得「想要 16F」"),
):
    good, why = contains(lines, "present_now", needle)
    check(good, what, why)

# 「缺省关 ⇒ 与上游逐字节一致」的结构性依据：8 位画布不走进 HDR fragment，走直通。
good, why = contains(lines, "present_pipeline", "int itm = (format == MTLPixelFormatRGBA16Float)")
check(good, "只有 16F 画布才建 HDR fragment（8 位画布保持直通）", why)

# 诊断行必须真的接在直方图主路径上（S2A_HDR_LOG=1 才有输出；八位降级时打的是 fmt=8B/itm=0）
inc_all = "\n".join(inc_lines)
check("hdr_log_setting()" in inc_all, "nvn_hdr_hist.inc 有 S2A_HDR_LOG 开关")
check("s2a_hdr_status_line(line, sizeof line, &st)" in inc_all, "主路径每 S2A_HDR_LOG_EVERY 帧打诊断行")
check("S2A_HDR_LOG_EVERY" in inc_all, "诊断行按 S2A_HDR_LOG_EVERY 节流")
check("st.map = s2a_hdr_map();" in inc_all, "诊断行里带上放置方式（map=1 时 knee 只是日志）")

mtl_all = "\n".join(lines)
hdr_all_ = open(os.path.join(ROOT, "graphics/nvn/gfx/nvn/nvn_hdr.h"), encoding="utf-8", errors="replace").read()

# 内容映射：环境变量 → uniform extra.z → 上屏分支，三段都必须接上。
#   少任何一段都会得到「设了 S2A_HDR_MAP 却看不出变化」，而日志里只在解析处打一行，很容易漏。
check("g_hdr_extra[2] = (float)s2a_hdr_map()" in mtl_all, "把 S2A_HDR_MAP 解析结果放进 extra.z")
check("g_hdr_extra[2], g_hdr_extra[3]}" in mtl_all, "MTL3 的 HdrCrop 把 extra.z 传给上屏")
check("uv[14] = g_hdr_extra[2]" in mtl_all, "MTL4 的 uv[14] 也传 extra.z（两条 present 路径都要）")
check('"  if (c.extra.z > 0.5f) {\\n"' in mtl_all, "上屏 MSL 有 c.extra.z 分支")
check('c.knee.z * pow(y, 0.15f)' in mtl_all, "上屏 MSL 的 bt2408 增益算式（head * y^0.15）")
check("s2a_hdr_grade_rgb_map(" in hdr_all_, "C 侧有 map 版本的 grade（工具/测试共用）")

print("· %d 项通过，%d 项失败" % (ok, bad))
sys.exit(1 if bad else 0)
