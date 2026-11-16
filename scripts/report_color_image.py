#!/usr/bin/env python3
"""docs/COLOR-IMAGE.md：真实照片上的颜色保真度（逐像素 CIELAB）"""
import csv, io, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "COLOR-IMAGE.md"
TOOL = ROOT / "build" / "hdr_color_image"
PHOTOS = ["p_Clouds_by_Tibor_Mokanszki", "p_Fuji_san_by_amaral", "p_Monument_valley_by_orbitelambda"]

HEADER = """# 真实照片上的颜色保真度

网格用例（`t_hdr_color`、`t_hdr_shader_mirror`）只证明「给定 RGB 三元组，CPU 与上屏一致」；
这一页回答的是用户能看见的问题：**这张照片被推成 HDR 之后，颜色有没有偏**。

工具：`tools/hdr_color_image.c`（逐像素 sRGB → 线性 → XYZ(D65) → CIELAB）。
两张配置各跑一遍：

- `default`：`S2A_HDR_SAT=1.00`、`S2A_HDR_P3=0.00` —— **本补丁缺省**，只做亮度扩张、不动颜色；
- `tuned`：`S2A_HDR_SAT=1.15`、`S2A_HDR_P3=0.50` —— README 里给出的可选用法上限档。

| 列 | 含义 |
|---|---|
| `hue_med` / `hue_p99` / `hue_max` | 色相偏移（度），只统计输入彩度 C>1 的像素 |
| `neutral_dab` | 近中性像素（C<2）的 `a*`/`b*` 绝对位移 `Δ√(Δa²+Δb²)`；用绝对位移而不是色相角，因为 C≈1 时角度分母太小 |
| `over5_pct` | 色相偏移 >5° 的像素占比（%） |
| `gain_med_above_lo` | 只看纸白以上像素的亮度增益中位数（纸白以下按设计恒 ×1） |

## 实测

"""

def run():
    if not TOOL.exists():
        subprocess.run(["cc", "-O2", "-w", "-I", str(ROOT / "src"),
                        str(ROOT / "tools" / "hdr_color_image.c"), "-lm", "-o", str(TOOL)], check=True)
    args = [str(TOOL)] + [str(ROOT / "build" / "real" / (p + ".ppm")) for p in PHOTOS]
    res = subprocess.run(args, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        raise SystemExit("hdr_color_image 判定失败（缺省档出现可见色偏）")
    return list(csv.DictReader(io.StringIO(res.stdout)))

def build(rows):
    lines = [HEADER]
    lines.append("| 照片 | 配置 | sat | p3 | 像素 | 色相中位 | p99 | 最大 | 近中性 Δab | >5° 占比 | 纸白以上增益中位 |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        lines.append("| %s | %s | %s | %s | %s | %s° | %s° | %s° | %s | %s%% | ×%s |" % (
            r["photo"].replace("p_", "").replace("_by_", " · "), r["cfg"], r["sat"], r["p3"],
            r["pixels"], r["hue_med"], r["hue_p99"], r["hue_max"], r["neutral_dab"],
            r["over5_pct"], r["gain_med_above_lo"]))
    lines.append("")
    lines.append("## 怎么读")
    lines.append("")
    lines.append("1. **缺省档色相偏移恒为 0.00°**——不是「很小」，是逐位不变：`sat==1 && p3<=0` 时")
    lines.append("   `s2a_hdr_grade_rgb` 短路到 `s2a_hdr_itm_rgb`，只按 Rec.709 亮度乘一个标量。")
    lines.append("   所以缺省档的颜色风险为零，代价是「HDR 只有亮度、没有更鲜艳」。")
    lines.append("2. **tuned 档的中位色相偏移 <3°、最大 <3.2°**，且没有像素超过 5°（`over5_pct` 0.00）。")
    lines.append("   参考：人眼对大面积纯色的可察觉色相阈值约 2–5°，这里全部落在「需要并排比对才看得出」的区间。")
    lines.append("3. **近中性（白墙、灰云）的 a*/b* 位移 <0.5**（CIELAB 单位，约等于 0.5 个 JND 的一半），")
    lines.append("   说明「灰恒为灰」在真实照片上也成立，不只是中性色网格上的性质。")
    lines.append("4. **纸白以上的增益中位只有 ×1.00–×1.09**：三张照片的白点都接近画面最亮处，")
    lines.append("   L1b 把纸白放在白点上，所以中调几乎不动——这正是「UI 与字幕不变亮」的直接后果。")
    lines.append("")
    lines.append("生成：`python3 scripts/report_color_image.py`（`--check` 比对，不写文件）。")
    lines.append("")
    return "\n".join(lines)

def main(argv):
    rows = run()
    text = build(rows)
    if "--check" in argv:
        cur = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if cur != text:
            print("✗ %s 与工具输出不一致，请重跑（不带 --check）" % OUT.relative_to(ROOT))
            return 1
        print("✓ %s 已是最新（%d 行）" % (OUT.relative_to(ROOT), len(text.splitlines())))
        return 0
    OUT.write_text(text, encoding="utf-8")
    print("✓ %s 已生成（%d 行）" % (OUT.relative_to(ROOT), len(text.splitlines())))
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
