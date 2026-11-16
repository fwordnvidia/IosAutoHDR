#!/usr/bin/env python3
"""对真实照片跑一遍自动判定，生成 docs/REAL-CONTENT.md。

合成场景是可控的，但真实照片会带来它们没有的东西：大块天空、雾、雪地、反光、JPEG 噪声。
这份报告回答两个问题：
  1. 自然内容上，HUD/孤立簇剔除会不会误伤？（判据：white_bulk 必须等于 white）
  2. 输出会不会越界 head？（判据：clipped_frac 必须为 0）
使用方法：
  python3 scripts/report_real.py            # 生成 / 覆盖 docs/REAL-CONTENT.md
  python3 scripts/report_real.py --check    # 只比对，不一致就 rc=1（CI 用）
没有可用的源图时（例如裸 CI 容器），报告里写明「跳过」而不是失败。
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "docs", "REAL-CONTENT.md")
WORK = os.path.join(HERE, "build", "real")
SOURCES = [
    "/usr/share/backgrounds/Clouds_by_Tibor_Mokanszki.jpg",
    "/usr/share/backgrounds/Fuji_san_by_amaral.png",
    "/usr/share/backgrounds/Monument_valley_by_orbitelambda.jpg",
]
# 没有那几张系统壁纸时，退回到任何可用的大图（保证在别的机器上也能跑起来）
FALLBACK = "/usr/share/backgrounds/*.jpg"


def prep():
    if not shutil.which("convert"):
        return []
    os.makedirs(WORK, exist_ok=True)
    paths = [p for p in SOURCES if os.path.exists(p)]
    if not paths:
        paths = sorted(glob.glob(FALLBACK))[:3]
    made = []
    for p in paths:
        name = os.path.splitext(os.path.basename(p))[0]
        out = os.path.join(WORK, "p_%s.ppm" % name)
        subprocess.run(["convert", p, "-resize", "480x270!", "-colorspace", "sRGB",
                        "-depth", "8", out], check=True)
        made.append((name, out))
    return made


def build_tool():
    binp = os.path.join(HERE, "build", "hdr_batch")
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(HERE, "src"),
                    os.path.join(HERE, "tools", "hdr_batch.c"), "-lm", "-o", binp], check=True)
    return binp


def main():
    check = "--check" in sys.argv
    made = prep()
    lines = ["# 真实内容验证", "",
             "合成的 city/uiwhite/dark/ramp/star 是可控的，但真实照片会带来它们没有的东西：",
             "大块天空、雾、雪地、镜面反光、JPEG 噪声。这张表就是拿真实照片核对两件事：",
             "",
             "1. **不误伤**：`white_bulk`（HUD 剔除后的白点）必须等于 `white`（普通白点）——",
             "   自然内容里没有「孤立的一小撮亮像素」，一旦不等就说明剔除判据在照片上误伤。",
             "2. **不越界**：输出任何通道都不得超过 `head`（`clipped_frac` 必须为 0）。",
             ""]
    if not made:
        lines += ["_本机没有可用的源图（缺 ImageMagick 或 /usr/share/backgrounds），本表跳过。_", ""]
        text = "\n".join(lines)
    else:
        binp = build_tool()
        rows = subprocess.run([binp] + [p for _, p in made], capture_output=True, text=True,
                              check=True, cwd=WORK).stdout.strip().split("\n")
        hdr = rows[0].split(",")
        body = [r.split(",") for r in rows[1:]]
        lines += ["| 图 | APL | 黑占比 | 白点 = 剔除后 | L1b 纸白 | 重心 (log2) | 越界像素 | 色相偏差 |",
                  "|---|---|---|---|---|---|---|---|"]
        bad = 0
        for r in body:
            name = os.path.basename(r[0]).replace("p_", "").replace(".ppm", "")
            while len(r) < len(hdr):
                r.append("")
            if abs(float(r[5]) - float(r[6])) > 1e-6:
                bad += 1
            if float(r[10]) > 0:
                bad += 1
            lines.append("| %s | %.3f | %.4f | %.4f | %.4f | %+.2f | %.5f | %.1e |"
                         % (name, float(r[3]), float(r[4]), float(r[5]), float(r[8]),
                            float(r[7]), float(r[10]), float(r[11])))
        lines += ["",
                  "两列的判据都通过：%s（不误伤：白点两列逐位相同；不越界：越界像素 0）"
                  % ("是" if bad == 0 else "**否**"),
                  "",
                  "复现：`python3 scripts/report_real.py`（需要 ImageMagick；没有源图时本表写「跳过」）。",
                  ""]
        text = "\n".join(lines)
        if bad:
            print("::error:: 真实内容验证不通过：%d 项" % bad)
            if check:
                return 1

    old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else None
    if check:
        if old != text:
            print("::error:: docs/REAL-CONTENT.md 与实测不一致（重跑 scripts/report_real.py）")
            return 1
        print("✓ docs/REAL-CONTENT.md 与实测一致（%d 行）" % len(text.split("\n")))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write(text)
    print("✓ 已生成 %s（%d 行）" % (OUT, len(text.split("\n"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
