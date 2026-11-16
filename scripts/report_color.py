#!/usr/bin/env python3
"""生成 / 校验 docs/COLOR.md：真实照片上两种饱和度算子的色相误差。

为什么不写在单测里：单测只能证明「某个具体颜色对」的性质，而色相误差是分布问题
（中位数还好、p99 和长尾才是肉眼看到的「颜色跑偏」）。这里直接跑使用者命令
`hdr_convert`，从它写出的 16 位线性图反推 CIELAB 色相角，与输入比。

两种算子：
  S2A_HDR_SAT_SPACE=0  线性锚定 y + (c-y)*s（旧行为）
  S2A_HDR_SAT_SPACE=1  在 Lab 里缩放 a*/b*（保色相，默认）
"""
import glob
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REAL = os.path.join(HERE, "build", "real")
DOC = os.path.join(HERE, "docs", "COLOR.md")

M = [[0.4124564, 0.3575761, 0.1804375],
     [0.2126729, 0.7151522, 0.0721750],
     [0.0193339, 0.1191920, 0.9503041]]
W = (0.95047, 1.0, 1.08883)


def read_ppm(path):
    b = open(path, "rb").read()
    i, toks = 0, []
    while len(toks) < 4:
        while b[i:i + 1].isspace():
            i += 1
        j = i
        while not b[j:j + 1].isspace():
            j += 1
        toks.append(b[i:j])
        i = j
    w, h, mx = int(toks[1]), int(toks[2]), int(toks[3])
    n = w * h * 3
    if mx == 65535:
        return w, h, struct.unpack(">%dH" % n, b[i + 1:i + 1 + n * 2]), 65535.0
    return w, h, tuple(b[i + 1:i + 1 + n]), 255.0


def to_linear(v, mx):
    c = v / mx
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lab(rgb):
    xyz = [sum(M[i][j] * rgb[j] for j in range(3)) for i in range(3)]

    def f(t):
        return t ** (1.0 / 3.0) if t > 216.0 / 24389.0 else (24389.0 / 27.0 * t + 16.0) / 116.0

    fx, fy, fz = [f(xyz[i] / W[i]) for i in range(3)]
    return (116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz))


def hue_delta_deg(a, b):
    d = (math.atan2(a[2], a[1]) - math.atan2(b[2], b[1]) + math.pi) % (2 * math.pi) - math.pi
    return abs(math.degrees(d))


def measure(binp, src, space, sat):
    out = "/tmp/color_%d.ppm" % space
    env = dict(os.environ, S2A_HDR_SAT_SPACE=str(space))
    subprocess.run([binp, src, out, "--head", "2", "--sat", "%.2f" % sat],
                   check=True, capture_output=True, env=env)
    w, h, px, mx = read_ppm(out)
    w2, h2, ip, imx = read_ppm(src)
    ds = []
    for k in range(0, len(ip), 3):
        a = [to_linear(ip[k + c], imx) for c in range(3)]
        if max(a) < 1e-3:
            continue
        b = [px[k + c] / mx * 2.0 for c in range(3)]
        ds.append(hue_delta_deg(lab(b), lab(a)))
    ds.sort()
    return ds


def build():
    srcs = sorted(glob.glob(os.path.join(REAL, "p_*.ppm")))
    binp = os.path.join(HERE, "build", "hdr_convert")
    if not srcs:
        return None
    # 每次都重编：只判「文件是否存在」会拿旧二进制量新算法，两张表会一模一样（踩过）。
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(HERE, "src"),
                    os.path.join(HERE, "tools", "hdr_convert.c"), "-lm", "-o", binp], check=True)
    rows = []
    for src in srcs:
        name = os.path.basename(src)[2:-4]
        for sat in (1.6, 2.0):
            for space, label in ((0, "线性锚定（旧）"), (1, "Lab 缩放（默认）")):
                ds = measure(binp, src, space, sat)
                rows.append((name, sat, label, ds[len(ds) // 2], ds[int(len(ds) * 0.99)], ds[-1]))
    return rows


def render(rows):
    L = ["# 真实照片上的色相误差（饱和度算子对照）", "",
         "由 `scripts/report_color.py` 生成。输入：`/usr/share/backgrounds` 里三张 480×270 缩图；"
         "测得的是 `hdr_convert --sat` 输出的 16 位线性图与输入的 **CIELAB 色相角**差（`atan2(b*, a*)`）。",
         "",
         "| 照片 | sat | 算子 | 中位 | p99 | 最大 |", "|---|---|---|---|---|---|"]
    for name, sat, label, med, p99, mx in rows:
        L.append("| %s | %.1f | %s | %.2f° | %.2f° | %.2f° |" % (name, sat, label, med, p99, mx))
    lab_med = max(r[3] for r in rows if r[2].startswith("Lab"))
    lin_p99 = max(r[4] for r in rows if r[2].startswith("线性"))
    L += ["",
          "结论：Lab 缩放的中位误差 **%.2f°**（按构造应为 0，残差全是 float 往返），"
          "线性锚定的 p99 最差 **%.2f°**。" % (lab_med, lin_p99),
          "",
          "为什么线性锚定会偏：`y + (c-y)*s` 只保证「三通道同比偏离 Rec.709 亮度」，"
          "而 Lab 的色相是 `(a*, b*)` 的夹角；亮度锚定在非线性变换下会把色相推走，"
          "越接近色域边、`s` 越大越明显（Monument 的岩壁/天空渐变最吃亏）。",
          "",
          "上屏（Metal）：`src_msl_hdr` 的 `s2a_sat()` 已移植同一套 Lab 算式（同一组常数、"
          "同一个「中性色短路」、同样 14 次二分的色域守卫），`t_hdr_gpu_mirror` 用 30 组样本"
          "逐通道对照两侧，最大差 0.000000。",
          "",
          "仍未验证的两条：MSL 从未被 `xcrun metal` 真编过（GitHub CI 随账号停用而不可触发，"
          "只剩 `scripts/check_msl.py` 的静态守卫），`s2a_sat` 的 GPU 端耗时也没有实测"
          "（它只在 `S2A_HDR_SAT != 1` 时进入，且仅当缩放把某通道推成负值才走二分）。" ]
    return "\n".join(L) + "\n"


def main():
    rows = build()
    if rows is None:
        print("跳过：没有真实照片（先跑 scripts/report_real.py）")
        return 0
    text = render(rows)
    if "--check" in sys.argv:
        if not os.path.exists(DOC) or open(DOC, encoding="utf-8").read() != text:
            print("::error::docs/COLOR.md 与实测不一致（改过饱和度算法没重新生成报告？）")
            return 1
        print("✓ docs/COLOR.md 与实测一致（%d 行）" % text.count("\n"))
        return 0
    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    open(DOC, "w", encoding="utf-8").write(text)
    print("写出 docs/COLOR.md（%d 行）" % text.count("\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
