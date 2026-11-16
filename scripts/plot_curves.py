#!/usr/bin/env python3
"""从 hdr_ref --curve 的 CSV 画两条曲线，输出自包含 SVG（不依赖 numpy/matplotlib）。

图里每个数字都来自实际代码（hdr_ref 直接调 nvn_hdr.h 的同一份实现），不是手抄的示意曲线。
用法：python3 scripts/plot_curves.py out.svg [hdr_ref 路径]
"""
import subprocess
import sys

W, H = 900, 340
PAD_L, PAD_R, PAD_T, PAD_B = 64, 210, 28, 64


def run(binary, head, knee_lo):
    out = subprocess.run([binary, "--curve", "--head", str(head), "--knee-lo", str(knee_lo)],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.strip().splitlines()[1:]:
        srgb, gain, olin, onit = line.split(",")
        rows.append((float(srgb), float(gain), float(olin), float(onit)))
    return rows


def path(points, sx, sy):
    return "M " + " L ".join(f"{sx(x):.2f} {sy(y):.2f}" for x, y in points)


def main():
    out_svg = sys.argv[1] if len(sys.argv) > 1 else "docs/curve.svg"
    binary = sys.argv[2] if len(sys.argv) > 2 else "build/hdr_ref"

    series = [
        ("head 2 / knee_lo 0.35 (game)", run(binary, 2, 0.35), "#2563eb"),
        ("head 2 / knee_lo 0.74 (UI white)", run(binary, 2, 0.74), "#dc2626"),
        ("head 4 / knee_lo 0.35", run(binary, 4, 0.35), "#16a34a"),
    ]
    gmax = max(r[1] for _, rows, _ in series for r in rows) * 1.05
    nmax = max(r[3] for _, rows, _ in series for r in rows) * 1.05

    sx = lambda x: PAD_L + x * (W - PAD_L - PAD_R)
    sy = lambda g: H - PAD_B - (g / gmax) * (H - PAD_T - PAD_B)

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" font-family="ui-sans-serif, system-ui, sans-serif" font-size="11">',
             f'<rect width="{W}" height="{H}" fill="#fff"/>']

    # 网格
    for i in range(0, 5):
        g = gmax * i / 4
        y = sy(g)
        parts.append(f'<line x1="{PAD_L}" y1="{y:.1f}" x2="{W-PAD_R}" y2="{y:.1f}" stroke="#e5e7eb"/>')
        parts.append(f'<text x="{PAD_L-8}" y="{y+4:.1f}" text-anchor="end" fill="#6b7280">{g:.1f}×</text>')
    for i in range(0, 6):
        x = sx(i / 5)
        parts.append(f'<line x1="{x:.1f}" y1="{PAD_T}" x2="{x:.1f}" y2="{H-PAD_B}" stroke="#f3f4f6"/>')
        parts.append(f'<text x="{x:.1f}" y="{H-PAD_B+16}" text-anchor="middle" fill="#6b7280">{i/5:.1f}</text>')

    parts.append(f'<text x="{PAD_L}" y="{H-PAD_B+38}" fill="#374151">input (sRGB code)</text>')
    parts.append(f'<text x="14" y="{PAD_T-10}" fill="#374151">luminance gain</text>')
    parts.append(f'<text x="{W-PAD_R+10}" y="{PAD_T+12}" fill="#111827" font-weight="600">'
                 f'增益曲线（实测）</text>')

    # 曲线 + 图例（图例里带峰值 nit）
    for k, (label, rows, color) in enumerate(series):
        pts = [(r[0], r[1]) for r in rows]
        parts.append(f'<path d="{path(pts, sx, sy)}" fill="none" stroke="{color}" stroke-width="2"/>')
        peak = rows[-1][3]
        parts.append(f'<text x="{W-PAD_R+10}" y="{PAD_T+36+k*34}" fill="{color}">■ {label}</text>')
        parts.append(f'<text x="{W-PAD_R+22}" y="{PAD_T+50+k*34}" fill="#6b7280">峰值 {peak:.0f} nit</text>')

    parts.append(f'<text x="{PAD_L}" y="{H-12}" fill="#9ca3af" font-size="10">'
                 f'source: hdr_ref --curve (same nvn_hdr.h as the patch); SDR white = 203 nit per BT.2408</text>')
    parts.append("</svg>")

    with open(out_svg, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print(f"{out_svg}: {len(series)} 条曲线，峰值 " + " / ".join(f"{s[1][-1][3]:.0f} nit" for s in series))


if __name__ == "__main__":
    main()
