#!/usr/bin/env python3
"""make_gallery.py：生成 docs/GALLERY.md 与 docs/img/ 里的对照图（`--check` 校验清单）。

为什么需要：仓库里的全部结论都是数字（docs/*.md 的表），但 HDR 这东西不看一眼很难判断
「抬起来的部分对不对」。这里用真实照片跑参考管线（`hdr_convert`），把 SDR 与自动 HDR 的
**同一套预览映射**并排出图，并附一张高光区域的 4× 放大裁切（看点在哪一块被抬起来）。

重要说明（也会写进 GALLERY.md）：
  - 图是「预览映射」（Reinhard 压到 8 位 + sRGB），不是真机上屏的样子；真机上 head 以上的
    能量由 EDR 屏幕还原，普通显示器只能压。
  - 因此这些图可用于「哪些区域被抬、是否偏色、有没有硬裁」，不能用于判断最终观感。
"""
import hashlib
import os
import subprocess
import sys

from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "docs", "img")
OUT = os.path.join(ROOT, "docs", "GALLERY.md")
REAL = os.path.join(ROOT, "build", "real")
CONV = os.path.join(ROOT, "build", "hdr_convert")

PHOTOS = [
    ("Clouds_by_Tibor_Mokanszki", "云层：大面积极亮天空 + 暗地景"),
    ("Fuji_san_by_amaral", "富士山：雪面高光与蓝天渐变"),
    ("Monument_valley_by_orbitelambda", "纪念碑谷：暖色岩壁，色相最容易跑偏"),
]
HEAD = "2"
SAT = "1.15"          # 让「抬起来」和「饱和一点」两件事同时可见


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def main():
    check = "--check" in sys.argv
    os.makedirs(IMG, exist_ok=True)
    manifest = []
    mapdiff = {}
    for name, why in PHOTOS:
        src = os.path.join(REAL, "p_%s.ppm" % name)
        sdr_ppm = os.path.join(ROOT, "build", "gal_%s_sdr.ppm" % name)
        hdr_ppm = os.path.join(ROOT, "build", "gal_%s_hdr.ppm" % name)
        sdr_png = os.path.join(IMG, "%s-sdr.png" % name)
        hdr_png = os.path.join(IMG, "%s-hdr.png" % name)
        pair_png = os.path.join(IMG, "%s-pair.png" % name)
        crop_png = os.path.join(IMG, "%s-crop.png" % name)
        con_ppm = os.path.join(ROOT, "build", "gal_%s_content.ppm" % name)
        con_png = os.path.join(IMG, "%s-content.png" % name)
        map_pair_png = os.path.join(IMG, "%s-map-pair.png" % name)
        if not check:
            if not os.path.exists(CONV):
                run(["make", "-s"])   # 构建工具（run_all 会先建好）
            run([CONV, src, sdr_ppm, "--head", HEAD, "--sat", "1"])
            run([CONV, src, hdr_ppm, "--head", HEAD, "--sat", SAT])
            # 内容档（BT.2408 纸白）：缺省档只在白点以上扩张，内容档把 98% 以上像素一起抬。
            # 两者放同一张对照图里，reviewer 一眼能看出「这是选项不是默认」的原因。
            run([CONV, src, con_ppm, "--head", HEAD, "--sat", SAT, "--map", "bt2408"])
            run([sys.executable, os.path.join(ROOT, "scripts", "to_png.py"), sdr_ppm, sdr_png, "--head", HEAD])
            run([sys.executable, os.path.join(ROOT, "scripts", "to_png.py"), hdr_ppm, hdr_png, "--head", HEAD])
            run([sys.executable, os.path.join(ROOT, "scripts", "to_png.py"), con_ppm, con_png, "--head", HEAD])
            a = Image.open(sdr_png).convert("RGB")
            b = Image.open(hdr_png).convert("RGB")
            w, h = a.size
            pair = Image.new("RGB", (w * 2 + 4, h), (0, 0, 0))
            pair.paste(a, (0, 0))
            pair.paste(b, (w + 4, 0))
            pair.save(pair_png, optimize=True)
            # 裁切区域不取「最亮块」而取「SDR 与 HDR 差异最大的一块」：
            # HDR 侧的差异集中在被抬起来的高光与饱和处，最亮块可能是一块本来就压平的灰岩。
            sa = a.resize((w // 8, h // 8), Image.BILINEAR).tobytes()
            sb = b.resize((w // 8, h // 8), Image.BILINEAR).tobytes()
            bw = w // 8
            best, best_d = 0, -1
            for i in range(bw * (h // 8)):
                d = abs(sa[i * 3] - sb[i * 3]) + abs(sa[i * 3 + 1] - sb[i * 3 + 1]) + \
                    abs(sa[i * 3 + 2] - sb[i * 3 + 2])
                if d > best_d:
                    best, best_d = i, d
            cx, cy = (best % bw) * 8, (best // bw) * 8
            cw, ch = 60, 34
            x0 = max(0, min(w - cw, cx - cw // 2))
            y0 = max(0, min(h - ch, cy - ch // 2))
            ca = a.crop((x0, y0, x0 + cw, y0 + ch)).resize((cw * 4, ch * 4), Image.NEAREST)
            cb = b.crop((x0, y0, x0 + cw, y0 + ch)).resize((cw * 4, ch * 4), Image.NEAREST)
            crop = Image.new("RGB", (cw * 8 + 4, ch * 4), (0, 0, 0))
            crop.paste(ca, (0, 0))
            crop.paste(cb, (cw * 4 + 4, 0))
            crop.save(crop_png, optimize=True)
            c = Image.open(con_png).convert("RGB")
            mp = Image.new("RGB", (w * 2 + 4, h), (0, 0, 0))
            mp.paste(b, (0, 0))
            mp.paste(c, (w + 4, 0))
            mp.save(map_pair_png, optimize=True)
            # 记下两档预览的**实测**差（平均值与「差 > 4 级」的像素占比）。
            # 不写「看得出差别」这种话：800px 宽的并排图在普通屏幕上一眼看不出来，
            # 给数字才能让人知道该看哪、也能在别人机器上复现。
            dd = ImageChops.difference(b, c)
            px = list(dd.convert("L").getdata())
            mapdiff[name] = (sum(px) / len(px), 100.0 * sum(1 for v in px if v > 4) / len(px))
        for p in (sdr_png, hdr_png, pair_png, crop_png, con_png, map_pair_png):
            if not os.path.exists(p):
                print("✗ 缺少 %s（重跑不带 --check）" % os.path.relpath(p, ROOT))
                return 1
            manifest.append((os.path.relpath(p, ROOT), hashlib.sha256(open(p, "rb").read()).hexdigest()))

    lines = [
        "# 对照图（真实照片）", "",
        "由 `scripts/make_gallery.py` 生成，`--check` 校验清单与图片指纹。",
        "",
        "**这些是预览映射，不是上屏结果**：脚本用 Reinhard 把 HDR 输出压到 8 位再按 sRGB 编码，"
        "真机上 head 以上的能量由 EDR 屏幕还原。所以这些图能说明「哪些区域被抬起来、有没有偏色、"
        "有没有硬裁」，不能说明最终观感。",
        "",
        "参数：`--head %s`（2× headroom）、SDR 侧 `--sat 1`、HDR 侧 `--sat %s`（同时展示抬亮与提饱和）。"
        % (HEAD, SAT),
        "",
    ]
    for name, why in PHOTOS:
        lines += [
            "## %s" % why, "",
            "| SDR（`--sat 1`） | 自动 HDR（head %s，sat %s） |" % (HEAD, SAT),
            "|---|---|",
            "| ![%s sdr](img/%s-sdr.png) | ![%s hdr](img/%s-hdr.png) |" % (name, name, name, name),
            "",
            "并排：[%s-pair.png](img/%s-pair.png) ・ 高光 4× 裁切：[%s-crop.png](img/%s-crop.png)"
            % (name, name, name, name),
            "",
            "**两档对照**（左：缺省档，只在白点以上扩张 ｜ 右：内容档 `--map bt2408`，纸白 203 nit 线性化）："
            "![%s 两档](img/%s-map-pair.png)。两档预览的实测差：平均 **%.1f 级**、**%.0f%%** 的像素差 > 4 级"
            "（这两张图用同一套 Reinhard 预览映射，所以差值是真实的内容差，不是曝光归一化后的错觉）。"
            "内容档把绝大部分像素一起抬高（实测 98%%–100%% 的码值变化），所以它是 `S2A_HDR_MAP=1` "
            "的可选项，不是默认。" % (name, name, mapdiff.get(name, (0.0, 0.0))[0], mapdiff.get(name, (0.0, 0.0))[1]),
            "",
        ]
    lines += ["## 图片指纹", "", "| 文件 | sha256 |", "|---|---|"]
    for path, sha in manifest:
        lines.append("| `%s` | `%s…` |" % (path, sha[:16]))
    text = "\n".join(lines) + "\n"

    if check:
        old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        for path, sha in manifest:
            if ("`%s…`" % sha[:16]) not in old:
                print("✗ %s 的指纹与 GALLERY.md 不一致（重跑不带 --check）" % path)
                return 1
        print("✓ docs/GALLERY.md 与 %d 张图一致" % len(manifest))
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("✓ 写入 docs/GALLERY.md（%d 张图）" % len(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
