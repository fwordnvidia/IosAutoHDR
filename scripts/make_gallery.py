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
import glob
import hashlib
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageChops, ImageStat

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


def _pixel_diff_frac(a, b):
    """同尺寸 RGB 两图：至少一个通道不同的像素占比（%）。

    注意不能数「不同的字节」再除以像素数 —— 那样三个通道全变会算出 300%，
    本次就真的打印过 224.5% 这种不可能的数（守卫自己得先对）。
    """
    da, db = a.tobytes(), b.tobytes()
    total = len(da) // 3
    hit = 0
    for i in range(0, len(da), 3):
        if da[i:i + 3] != db[i:i + 3]:
            hit += 1
    return 100.0 * hit / total


PANEL = (480, 270)                      # 实拍素材是 480x270
PAIR = (PANEL[0] * 2 + 4, PANEL[1])      # 并排：中间 4 像素黑缝
CROP_W, CROP_H = 60, 34                  # 裁切窗口
CROP = (CROP_W * 2 * 4 + 4, CROP_H * 4)  # 4x 放大后左右并排


def verify_images(gallery_text):
    """核对图**内容**：尺寸、是否真是照片、左右两半是不是各自的输入、文字里的数字。

    为什么光有指纹不够：指纹只证明「文件没被动过」，不证明「文件里的东西是对的」。
    换掉一张空白图再更新指纹，指纹一样绿；把 pair 的左右两半贴反，指纹也绿。
    这里全部从已提交的 PNG 现算，不需要 build/、不需要 cc。
    """
    bad = 0
    for name, _why in PHOTOS:
        paths = {role: os.path.join(IMG, "%s-%s.png" % (name, role))
                 for role in ("sdr", "hdr", "content", "pair", "crop", "map-pair")}
        imgs = {}
        for role, p in paths.items():
            im = Image.open(p).convert("RGB")
            want = CROP if role == "crop" else (PAIR if role in ("pair", "map-pair") else PANEL)
            if im.size != want:
                print("✗ %s-%s.png 尺寸 %dx%d，预期 %dx%d"
                      % (name, role, im.size[0], im.size[1], want[0], want[1]))
                bad = 1
            imgs[role] = im
        if bad:
            continue
        sdr, hdr, con = imgs["sdr"], imgs["hdr"], imgs["content"]
        w, h = PANEL

        # ① 不是空白图 / 不是纯色图：亮度标准差够大
        for role, im in (("sdr", sdr), ("hdr", hdr)):
            sd = ImageStat.Stat(im.convert("L")).stddev[0]
            if sd < 20.0:
                print("✗ %s-%s.png 亮度标准差只有 %.1f —— 这不像照片（空白/纯色图？）"
                      % (name, role, sd))
                bad = 1

        # ② SDR 与 HDR 必须真有差别：贴成同一张图的话，整个对照图失去意义
        changed = _pixel_diff_frac(sdr, hdr)
        if changed < 50.0:
            print("✗ %s：SDR 与 HDR 只有 %.1f%% 的通道字节不同 —— 两张图贴错了？" % (name, changed))
            bad = 1

        # ③ 并排图的左右两半必须就是各自的输入（贴反/贴重都会被这条抓住）
        for role, left, right in (("pair", sdr, hdr), ("map-pair", hdr, con)):
            im = imgs[role]
            if im.crop((0, 0, w, h)).tobytes() != left.tobytes():
                print("✗ %s-%s.png 左半不是 %s" % (name, role, "sdr" if role == "pair" else "hdr"))
                bad = 1
            if im.crop((w + 4, 0, w + 4 + w, h)).tobytes() != right.tobytes():
                print("✗ %s-%s.png 右半不对" % (name, role))
                bad = 1

        # ④ 4x 裁切的左右两半必须不同（同一块猜两次就等于没给看点）
        crop = imgs["crop"]
        hw = CROP_W * 4
        if crop.crop((0, 0, hw, CROP_H * 4)).tobytes() ==                 crop.crop((hw + 4, 0, hw + 4 + hw, CROP_H * 4)).tobytes():
            print("✗ %s-crop.png 左右两半一模一样 —— 裁的是没变化的地方" % name)
            bad = 1

        # ⑤ GALLERY.md 里那句「平均 X 级、Y% 的像素差 > 4 级」必须能从这两张图算回来
        dd = ImageChops.difference(hdr, con)
        lum = dd.convert("L").tobytes()
        avg = sum(lum) / len(lum)
        pct = 100.0 * sum(1 for v in lum if v > 4) / len(lum)
        for needle in ("平均 **%.1f 级**" % avg, "**%.0f%%** 的像素差" % pct):
            if needle not in gallery_text:
                print("✗ %s：GALLERY.md 里找不到 %s（按两张现成图算出来的值）" % (name, needle))
                bad = 1
        # 「98%%–100%% 的码值变化」这句也得站得住
        anydiff = _pixel_diff_frac(hdr, con)
        if anydiff < 98.0:
            print("✗ %s：内容档只改了 %.2f%% 的像素，而 GALLERY.md 写的是 98%%–100%%"
                  % (name, anydiff))
            bad = 1
        print("  ✓ %s：%dx%d，SDR/HDR 差 %.1f%% 像素，两档差 平均 %.1f 级 / %.0f%% >4 级 / %.1f%% 像素"
              % (name, w, h, changed, avg, pct, anydiff))
    if bad:
        return 1
    print("✓ 6 类图 × 3 张：尺寸、非空白、左右两半、两档差值都能从现成 PNG 复算")
    return 0


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
            # 干净克隆里既没有 build/ 也没有工具，必须能自己把前置条件凑出来：
            # 之前这里写的是 run(["make", "-s"])，而仓库根本没有 Makefile ⇒
            # GALLERY.md 让读者「重跑不带 --check」，那条路一执行就崩在 make 上。
            os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
            os.makedirs(REAL, exist_ok=True)
            for f in sorted(glob.glob(os.path.join(ROOT, "testdata", "p_*.ppm"))):
                dst = os.path.join(REAL, os.path.basename(f))
                if not os.path.exists(dst):
                    shutil.copyfile(f, dst)
            if not os.path.exists(CONV):
                run(["cc", "-O2", "-w", "-I", os.path.join(ROOT, "src"),
                     os.path.join(ROOT, "tools", "hdr_convert.c"), "-lm", "-o", CONV])
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
            # 用 tobytes()：L 模式一像素一字节，且 Pillow 14 会删掉 Image.getdata()。
            px = dd.convert("L").tobytes()
            mapdiff[name] = (sum(px) / len(px), 100.0 * sum(1 for v in px if v > 4) / len(px))
        for p in (sdr_png, hdr_png, pair_png, crop_png, con_png, map_pair_png):
            if not os.path.exists(p):
                print("✗ 缺少 %s（重跑不带 --check）" % os.path.relpath(p, ROOT))
                return 1
            manifest.append((os.path.relpath(p, ROOT), hashlib.sha256(open(p, "rb").read()).hexdigest()))

    lines = [
        "# 对照图（真实照片）", "",
        "由 `scripts/make_gallery.py` 生成。`--check` 不只看指纹，还核对图**内容**："
        "尺寸（480×270 / 并排 964×270 / 裁切 484×136）、亮度标准差（挡空白图）、"
        "并排图左右两半是否就是各自的输入（挡贴反/贴重）、裁切左右是否真有差别，"
        "以及下面每段写的「平均 X 级 / Y%」能不能从这两张现成 PNG 算回来。",
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
        return verify_images(old)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("✓ 写入 docs/GALLERY.md（%d 张图）" % len(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
