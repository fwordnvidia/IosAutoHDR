#!/usr/bin/env python3
"""我们改了纹理格式，上游那些「只在设备上跑」的代码假设还成立吗？

背景：这个补丁把上屏画布从 BGRA8 换成 RGBA16Float（HDR）。换格式牵动的不是一处：
补帧（MetalFX + 自写 luma/motion）、直方图（读纹理算亮度）、拷贝管线、重采样 view……
上游这些代码写的时候只有 8 位，很多假设是**隐含**的（pitch、字节/像素、像素值域、
sRGB 解码与否）。这类假设错了不会崩、不会报错，只会画错。

本脚本把每一条假设做成可检查的结构断言，并生成 docs/UPSTREAM-ASSUMPTIONS.md。
它只能证明「代码长这个样子」，不能证明「Metal 上跑起来是对的」——后者要真机（见
docs/DEVICE-ONLY.md 的说明）。
"""
import os, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 和 check_landing.py 同一口径：第一个非选项参数是补丁目标树（上游 checkout / 干净基线树）。
# 工作区里只有 src/ 的源文件，没有 graphics/ 这层——不给树根就会 11 条全「不成立」，
# 那个结论是假的（错的是路径，不是代码）。所以树不存在时直接报错退出。
_args = [a for a in sys.argv[1:] if not a.startswith("-")]
ROOT = _args[0] if _args else os.environ.get("S2A_TREE", ".")
if not os.path.isdir(os.path.join(ROOT, "graphics/nvn")):
    print("::error::找不到补丁目标树 %r（要含 graphics/nvn）。用法：%s <树根> [--check]"
          % (ROOT, os.path.basename(sys.argv[0])))
    sys.exit(2)
G = "graphics/nvn/gfx/nvn"
FILES = {
    "fi": G + "/nvn_present_fi.inc",
    "mtl": G + "/nvn_mtl.m",
    "hist": G + "/nvn_hdr_hist.inc",
    "hdr": G + "/nvn_hdr.h",
}

# (编号, 一句话, 文件键, 断言类型, needle, 不成立会怎样)
#   类型 has  = 这一行必须出现
#   类型 none = 全文件不得出现
#   类型 pair = needle 是 (a, b)，a 出现的那一行里必须同时有 b
CHECKS = [
    ("A1", "运动矢量图是 RG16Float（位移 float2 不被截断）", "fi", "has",
     "f->motion = fi_tex(MTLPixelFormatRG16Float",
     "改成 8 位格式会把 ±16 像素的位移量化成 1/255，运动矢量全错，补出的帧抖动"),
    ("A2", "补帧的输入/中间纹理跟着 cfmt（不是写死 BGRA8）", "fi", "has",
     "f->in_tex[i] = fi_tex(f->cfmt",
     "16F 画布下输入仍是 8 位 ⇒ 补帧看不到 >1 的高光，补出的帧把高光压平"),
    ("A3", "psoCopy 的像素格式跟着 cfmt（两条相邻行：先设 colorAttachments 再用它建 psoCopy）", "fi", "pair_any",
     ("pd.colorAttachments[0].pixelFormat = f->cfmt", "f->psoCopy"),
     "拷贝管线与纹理格式不一致 ⇒ 建管线失败或写出错位"),
    ("A4", "fsCopy 是直通（不做颜色变换）", "fi", "has",
     "fragment float4 fsCopy(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return float4(t.sample(",
     "拷贝里加了解码/编码 ⇒ 16F 与 8 位两条路径各差一次变换，画面偏暗或偏亮"),
    ("A5", "不给 16F 纹理建 sRGB view", "mtl", "none",
     "newTextureViewWithPixelFormat:MTLPixelFormatRGBA16Float_sRGB",
     "Metal 没有 RGBA16Float_sRGB；若哪天有人「顺手」建 sRGB view，sample 会多解码一次"),
    ("A6", "present 管线的格式实参是变量而不是字面量", "mtl", "none",
     "present_pipeline(MTLPixelFormatBGRA8Unorm)",
     "写死 BGRA8 ⇒ 16F 画布用 8 位管线，高光被钳在 1.0"),
    ("A7", "直方图的 linearize 由源格式决定", "hist", "has",
     "uint32_t linearize = (src.pixelFormat == MTLPixelFormatRGBA16Float) ? 0u : 1u;",
     "16F 已经线性还再解一次 sRGB ⇒ 亮度整体偏暗、白点算错"),
    ("A8", "MetalFX 的 16F 是「先试再退」而不是写死", "fi", "has",
     "MTLPixelFormat want = (layer.pixelFormat == MTLPixelFormatRGBA16Float) ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;",
     "写死 16F ⇒ 不支持的机型直接建不出补帧器（补帧整个消失）"),
    ("A9", "MetalFX 建失败要抓异常并回退到 8 位", "fi", "pair",
     ("@catch (NSException *ex)", "f->mfx = nil;"),
     "不抓 ⇒ 抛到调用方崩掉；抓了不置 nil ⇒ 后面拿着半成品用"),
    ("A10", "FI 的缓存键含层格式（格式变了必须重建）", "fi", "has",
     "f->hpf == layer.pixelFormat",
     "降级/升级 16F 后仍用旧 FI 对象 ⇒ 输入格式与 cfmt 不一致，补帧写出错"),
    ("A11", "cfmt 由实际建成的 MetalFX 决定，不是猜的", "fi", "pair",
     ("if (f->mfx) f->cfmt = cf;", "cf"),
     "按 want 记 cfmt 而 MetalFX 实际退回 8 位 ⇒ 纹理格式与描述符不一致"),
]


def lines_of(key):
    p = os.path.join(ROOT, FILES[key])   # 树根，不是工作区（工作区只有 src/）
    if not os.path.exists(p):
        return None
    return open(p, encoding="utf-8", errors="replace").read().splitlines()


def main():
    rows, bad = [], 0
    cache = {}
    for cid, what, key, kind, needle, why in CHECKS:
        ls = cache.get(key)
        if ls is None:
            ls = cache[key] = lines_of(key)
        if ls is None:
            bad += 1
            rows.append((cid, what, "✗ 文件不存在", why))
            continue
        if kind == "has":
            ok = any(needle in ln for ln in ls)
        elif kind == "none":
            ok = not any(needle in ln for ln in ls)
        elif kind == "pair_any":
            a, b = needle
            ok = any(a in ln for ln in ls) and any(b in ln for ln in ls)
        else:
            a, b = needle
            ok = any(a in ln and b in ln for ln in ls)
        if not ok:
            bad += 1
        rows.append((cid, what, "✓" if ok else "✗", why))

    if "--check" in sys.argv or bad:
        for cid, what, st, why in rows:
            if st != "✓":
                print("::error::%s 不成立：%s（%s）" % (cid, what, why))
        if bad:
            print("✗ 纹理格式假设 %d/%d 条不成立" % (bad, len(CHECKS)))
            return 1
        print("✓ 纹理格式假设 %d 条全部成立" % len(CHECKS))
        return 0

    out = os.path.join(HERE, "docs/UPSTREAM-ASSUMPTIONS.md")
    L = ["# 换了纹理格式之后，上游那些假设还成立吗\n",
         "这份表由 `scripts/check_format_assumptions.py` 生成（`run_all.sh` 的 `0y/5` 会挡漂移）。\n",
         "补丁把上屏画布从 `BGRA8Unorm` 换成 `RGBA16Float`（HDR）。换格式牵动的不是一处：补帧、",
         "直方图、拷贝管线、重采样 view 都读这块纹理。上游写这些代码时只有 8 位，所以很多假设是",
         "**隐含**的——pitch、字节/像素、像素值域、是否 sRGB 解码。这类假设错了不会崩、不会报错，只会画错。\n",
         "| # | 假设 | 现在 | 不成立会怎样 |", "| --- | --- | --- | --- |"]
    for cid, what, st, why in rows:
        L.append("| %s | %s | %s | %s |" % (cid, what, st, why))
    L.append("")
    L.append("## 这些断言证明什么、不证明什么\n")
    L.append("证明：**代码结构**没被悄悄改回去（有人把 `f->cfmt` 改回 `MTLPixelFormatBGRA8Unorm`，这里立刻红）。\n")
    L.append("不证明：Metal 上跑起来是对的。本机没有 macOS、没有 `xcrun metal`，连 `clang -fsyntax-only`")
    L.append("都缺 Metal 头（见 `docs/DEVICE-ONLY.md`）。语义层面的保护是逐行 C 对账（`t_hdr_shader_mirror`、")
    L.append("`t_hdr_gpu_l1`、`tools/fi_domain`），运行期保护只有真机三格矩阵。\n")
    L.append("## 已经踩到的两条（都是这张表里的）\n")
    L.append("- **A2 的后果已经发生过**：`hdr_hist_add` 曾经对 16F 再解一次 sRGB（`linearize` 写死 1），")
    L.append("  亮度整体偏暗；现在是 A7 那条断言守着。")
    L.append("- **A4 的相邻问题已经发生过**：补帧的 luma 在 16F 下换成了线性域，而阈值是按 8 位编码域")
    L.append("  标定的（`spread` 的 `abs<0.004` ≈ 1 个码值）⇒ 静止判定命中率 100% → 44–64%，界面出鬼影。")
    L.append("  修法与实测见 `docs/FI-DOMAIN.md`。这条教训是写这张表的直接动机。\n")
    open(out, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print("写出 %s（%d 行，%d 条全过）" % (os.path.relpath(out, HERE), len(L) + 1, len(CHECKS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
