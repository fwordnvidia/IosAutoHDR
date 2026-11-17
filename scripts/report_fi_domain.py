#!/usr/bin/env python3
"""把 tools/fi_domain 的实测结果写成 docs/FI-DOMAIN.md，--check 挡漂移。

为什么单独一份报告：这是「HDR 打开以后，补帧的运动估计还对不对」的唯一证据。
它不是单测（要读真实照片、跑两条路径），所以结果落文档、由 run_all 的 0x/5 复核。
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "docs/FI-DOMAIN.md")


def measure():
    exe = os.path.join(HERE, "build/fi_domain")
    if not os.path.exists(exe):
        raise SystemExit("先编译：cc -O2 -w -I src tools/fi_domain.c -lm -o build/fi_domain")
    imgs = sorted(
        os.path.join("build/real", f) for f in os.listdir(os.path.join(HERE, "build/real"))
        if f.endswith(".ppm")
    ) if os.path.isdir(os.path.join(HERE, "build/real")) else []
    if not imgs:
        raise SystemExit("找不到 build/real/*.ppm（真实照片）")
    r = subprocess.run([exe] + imgs, cwd=HERE, text=True, capture_output=True)
    if r.returncode != 0:
        raise SystemExit("fi_domain 失败：%s" % r.stderr[-400:])
    return r.stdout


def render(out):
    rows = [ln for ln in out.splitlines() if ln.strip()]
    L = []
    L.append("# 补帧的运动估计与 HDR：同一个域吗？\n")
    L.append("这份表由 `scripts/report_fi_domain.py` 跑 `tools/fi_domain` 生成（`run_all.sh` 的 `0x/5` 会挡漂移）。\n")
    L.append("## 为什么这个问题值得单独一份报告\n")
    L.append("上游的补帧在 `fi_mv_setup` 里写死了两个常数，都是在 **8 位 sRGB 编码域** 上标定的：\n")
    L.append("- `match`：`if (zsad <= best * 1.02 + 0.05) bd = int2(0);` —— SAD 的绝对偏移 0.05")
    L.append("- `spread`：`if (all(abs(cur.read(gp).rgb - prev.read(gp).rgb) < 0.004)) v = float2(0);`")
    L.append("  —— 逐通道差 0.004，约等于「差不到 1 个 8 位码值」（1/255 = 0.00392）\n")
    L.append("我们把 HDR 的补帧输入换成了 `RGBA16Float`（线性、可 > 1）。`texture.read()` **不解码**，")
    L.append("所以 luma 提取读到的值从「sRGB 编码」变成「线性」——两者的数值尺度完全不同：")
    L.append("线性 0.18 == 编码 0.46；线性 0.004 == 编码 0.06（约 15 个码值）。\n")
    L.append("## 实测\n")
    L.append("同一张照片 + ±1 码值确定性噪声（代表压缩/时域抖动下最温和的帧间差异）：\n")
    L.append("```")
    L.extend(rows)
    L.append("```\n")
    L.append("列的含义：")
    L.append("- **SAD中位**：逐 8×8 块同位置的 SAD 中位数（就是 `zsad` 的量级）")
    L.append("- **SAD零比**：SAD 恰好为 0 的块占比")
    L.append("- **静止命中**：「前后两帧这个像素一样」（`<0.004`）的像素占比 —— 这张图前后内容相同，**理想是 100%**")
    L.append("- **偏零可用**：`0.05` 相当于多少个噪声块 SAD（>1 说明偏零偏移大于噪声底，仍能挡住乱跳）\n")
    L.append("## 结论\n")
    L.append("1. **`0.004` 这条真坏了**：8 位路径 100% 命中，16F 线性域只有 **44–64%** ——")
    L.append("   界面、字幕这类静止区域有一半被判成「动了」，会被运动矢量搬走（鬼影）。")
    L.append("2. **`0.05` 这条没坏**：两域的 SAD 中位都在 0.018–0.028，`0.05` 相当于 1.8–2.8 倍噪声底，两域都够用。")
    L.append("3. **修法**：16F 输入时先把线性值转回 sRGB 编码域再算 luma / 比差（`to_enc`，`enc` 由")
    L.append("   `cur.pixelFormat == MTLPixelFormatRGBA16Float` 决定）。静止命中回到 **100%**，")
    L.append("   SAD 中位回到 0.026–0.028 —— 与 8 位路径一致。\n")
    L.append("## 边界\n")
    L.append("这个工具不跑 Metal：它假设 `down` 的 4×4 平均与 `match` 的 8×8 块布局（与 MSL 同构），")
    L.append("但**不能**证明 Metal 编译器对 `select`/`pow` 的实现与我这里的 float 一致。")
    L.append("真正的保护是 `scripts/check_landing.py` 的 6 条结构断言（`to_enc` 存在、down/spread 都用它、")
    L.append("`enc` 由源格式决定、两处 dispatch 都绑了 buffer）——它挡的是「有人把这段改回去」。\n")
    return "\n".join(L) + "\n"


def main():
    text = render(measure())
    if "--check" in sys.argv:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print("::error::docs/FI-DOMAIN.md 与实测不一致（改了 tools/fi_domain.c 或真实照片没重新生成？）")
            return 1
        print("✓ docs/FI-DOMAIN.md 与实测一致")
        return 0
    open(OUT, "w", encoding="utf-8").write(text)
    print("写出 %s（%d 行）" % (os.path.relpath(OUT, HERE), text.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
