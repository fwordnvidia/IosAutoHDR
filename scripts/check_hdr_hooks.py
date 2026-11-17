#!/usr/bin/env python3
"""HDR 钩子不变量：设备侧那几处「一旦被挪进分支就静默失效」的代码。

为什么要有这个脚本：`nvn_mtl.m` 在 Linux 侧不编译，这些钩子写错了**没有任何闸门会红** ——
  它们全都长这样：功能还在、日志还在、`run_all.sh` 全绿，只是那行永远走不到。
  真实案例（2026-11-18 审出来的）：`g_present_layer_resident = layer;` 原来和
  `[g_q4 addResidencySet:…]` 挤在同一个 `if (!g_m3 && …)` 里，而 Metal 3 后端
  （iOS 26.x 一律、GPU 不在 Metal 4 家族、系统 <26 —— 也就是测试机 iPhone SE 3 / iOS 27）
  永远进不去那个分支 ⇒ `hdr_head_tick()` 每帧在 `layer == nil` 处早退 ⇒
  EDR 上界跟随完全不生效，`g_hdr_knee[2]` 停在起局值（XDR 屏只用一半、SDR 屏过度扩张）。

判据都是「结构断言」：不读语义，只保证这几行**还在能被走到的位置**上。
  H1 图层指针每帧都记（不能和 residencySet 的加挂挤在 !g_m3 分支里）
  H2 两个 present 入口都调 hdr_head_tick()
  H3 hdr_head_tick() 里「显式 S2A_HDR_HEADROOM 就让位」早于读图层
  H4 取不到 drawable 的块里同时有降级（step）与回头路（rearm）
  H5 降级块同时改像素格式与颜色空间（只改格式 ⇒ 画面发灰发亮，比黑屏更难查）
  H6 回头路块把 16F / EDR / 扩展线性 sRGB 三件一起装回去

用法：scripts/check_hdr_hooks.py [树根]（缺省用 src/ 里的真相源，不需要树）
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 带树根 ⇒ 查交付状态（打完补丁的树）；不带 ⇒ 查 src/ 里的真相源。
# 不带也必须能跑：投影 clone 里没有上游整文件，但 src/nvn_mtl.m 是随包发的。
if len(sys.argv) > 1:
    ROOT = os.path.abspath(sys.argv[1])
    MTL = os.path.join(ROOT, "graphics", "nvn", "gfx", "nvn", "nvn_mtl.m")
    HDR = os.path.join(ROOT, "graphics", "nvn", "gfx", "nvn", "nvn_hdr.h")
    if not os.path.exists(MTL):  # 投影形态：没有上游整文件
        print("· 投影形态：没有 graphics/nvn/gfx/nvn/nvn_mtl.m（上游整文件按设计不入库）⇒ 跳过 HDR 钩子检查")
        sys.exit(0)
else:
    MTL = os.path.join(HERE, "src", "nvn_mtl.m")
    HDR = os.path.join(HERE, "src", "nvn_hdr.h")
    if not os.path.exists(MTL):
        # 公开投影里只有补丁（上游 1.9 MB 的整文件按设计不搬）⇒ 这里没有可查的源。
        # 开发树与 CI 都有 src/nvn_mtl.m，那两处照跑；投影靠 4e/5（src ↔ 补丁 ↔ 树）兜底。
        print("· 投影形态：只有补丁、没有 src/nvn_mtl.m（上游整文件按设计不入库）⇒ 跳过 HDR 钩子检查")
        sys.exit(0)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().split("\n")


def block(lines, start_pat, what):
    """从含 start_pat 的行开始，按花括号配平取整块。"""
    for i, line in enumerate(lines):
        if start_pat in line:
            depth = 0
            for j in range(i, len(lines)):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0 and j > i:
                    return i, j, lines[i:j + 1]
            return i, len(lines) - 1, lines[i:]
    raise SystemExit("::error::找不到 %s（%s）—— 代码结构变了？" % (what, start_pat))


def main():
    lines = read(MTL)
    fails = []

    # H1：图层指针必须每帧都记 —— 不能和 residencySet 加挂挤在 !g_m3 分支里
    add = [n + 1 for n, l in enumerate(lines) if "addResidencySet:layer.residencySet" in l]
    if not add:
        fails.append("H1 找不到 addResidencySet:layer.residencySet（present 路径变了？）")
    for n in add:
        line = lines[n - 1]
        if "g_present_layer_resident = layer" in line:
            fails.append("H1:%d 图层指针和 residencySet 挤在同一句里 —— Metal 3 下这行走不到，"
                         "hdr_head_tick() 会每帧早退（EDR 上界跟随静默失效）" % n)
    if not any(l.strip() == "g_present_layer_resident = layer;" for l in lines):
        fails.append("H1 没有独立的 `g_present_layer_resident = layer;` 行（必须每帧无条件记一次）")

    # H2：两个 present 入口都要 tick
    tick_re = re.compile(r"^\s*hdr_head_tick\(\);\s*(//.*)?$")
    ticks = [n + 1 for n, l in enumerate(lines) if tick_re.match(l)]
    if len(ticks) < 2:
        fails.append("H2 hdr_head_tick() 只有 %d 处调用（普通与 m4 两个 present 入口都要有），行号 %s"
                     % (len(ticks), ticks))

    # H3：显式 HEADROOM 让位的判据要排在读图层之前
    i0, i1, body = block(lines, "static void hdr_head_tick(void)", "hdr_head_tick()")
    pin = next((k for k, l in enumerate(body) if "s2a_hdr_env_pinned()" in l), -1)
    lay = next((k for k, l in enumerate(body) if "g_present_layer_resident" in l), -1)
    if pin < 0:
        fails.append("H3:%d hdr_head_tick() 里没有 s2a_hdr_env_pinned() 早退 —— "
                     "显式 S2A_HDR_HEADROOM 会在第一帧后被屏幕读数覆盖，A/B 开关看起来没用" % i0)
    elif lay >= 0 and pin > lay:
        fails.append("H3:%d hdr_head_tick() 的让位判据排在读图层之后（顺序反了也可以，但别写成两个分支）" % i0)

    # H4/H5/H6：取不到 drawable 的块
    j0, j1, nil = block(lines, "if (!dr) {", "if (!dr) 块")
    nil_text = "\n".join(nil)
    for pat, why in (("s2a_hdr_fmt_guard_step", "降级"),
                     ("s2a_hdr_fmt_guard_rearm", "回头路")):
        if pat not in nil_text:
            fails.append("H4:%d 取不到 drawable 的块里缺 %s（%s）—— 只剩一半守卫" % (j0, pat, why))
    downgrade = [k for k, l in enumerate(nil) if "MTLPixelFormatBGRA8Unorm" in l]
    if not downgrade:
        fails.append("H5:%d 取不到 drawable 的块里没有降级到 BGRA8Unorm" % j0)
    else:
        near = "\n".join(nil[max(0, downgrade[0] - 2):downgrade[0] + 14])
        if "kCGColorSpaceSRGB" not in near:
            fails.append("H5:%d 降级块改了像素格式却没把颜色空间退回 sRGB —— "
                         "系统会把 8 位 sRGB 字节当线性值解释（画面发灰发亮，比黑屏更难查）" % (j0 + downgrade[0]))
    rearm = [k for k, l in enumerate(nil) if "s2a_hdr_fmt_guard_rearm" in l]
    if rearm:
        rb = "\n".join(nil[rearm[0]:rearm[0] + 12])
        for pat, why in (("MTLPixelFormatRGBA16Float", "像素格式"),
                         ("wantsExtendedDynamicRangeContent = YES", "EDR 声明"),
                         ("kCGColorSpaceExtendedLinearSRGB", "扩展线性 sRGB 颜色空间")):
            if pat not in rb:
                fails.append("H6:%d 回头路块没把 %s 装回去 —— 重新试探的 16F 画布会缺一角" % (j0 + rearm[0], why))

    # 附：纯 C 侧的让位判据必须与 itm_params 同一套解析（两者都在 nvn_hdr.h，改了要对齐）
    hdr = "\n".join(read(HDR))
    if "s2a_hdr_env_pinned" not in hdr:
        fails.append("H3 nvn_hdr.h 里没有 s2a_hdr_env_pinned()")

    for f in fails:
        print("::error::" + f)
    if fails:
        print("✗ HDR 钩子检查：%d 处" % len(fails))
        return 1
    print("✓ HDR 钩子：图层每帧记、两处 tick、让位判据、降级与回头路成对（%d 条）" % 6)
    return 0


if __name__ == "__main__":
    sys.exit(main())
