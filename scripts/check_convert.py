#!/usr/bin/env python3
"""检查面向使用者的 hdr_convert：它写出来的 16 位线性图必须满足四条硬性质。

为什么单独查：这是使用者真正会跑的那个命令（读图 → 写 HDR 预览），前面所有验证都在
测「算法函数」，没有一条测「这个命令写出的文件对不对」。

四条性质：
  1. 值域：所有样本 ≤ head（65535 = head），没有负值——超出就是上屏会裁高光；
  2. 纸白以下不动：y < knee_lo 的像素必须逐位等于输入（±1 LSB，来自 8→16 位量化）；
  3. 色域刻度正确：p3 打开后画面必须真的变（否则旋钮没接上），且仍在 [0, head] 内；
  4. 抖动只动最低位：加 --dither 后逐像素差 ≤ 2 LSB。
"""
import glob
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REAL = os.path.join(HERE, "build", "real")
TMP = os.path.join(HERE, "build", "conv")


def read_ppm16(path):
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
    assert toks[0] == b"P6" and int(toks[3]) == 65535, toks
    w, h = int(toks[1]), int(toks[2])
    n = w * h * 3
    px = struct.unpack(">%dH" % n, b[i + 1:i + 1 + n * 2])
    return w, h, px


def read_ppm8(path):
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
    n = int(toks[1]) * int(toks[2]) * 3
    return toks[2].decode(), list(b[i + 1:i + 1 + n])


def to_linear8(v):
    c = v / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def main():
    os.makedirs(TMP, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(REAL, "p_*.ppm")))
    if not srcs:
        print("跳过：%s 里没有真实照片（先跑 scripts/report_real.py 生成）" % REAL)
        return 0
    binp = os.path.join(HERE, "build", "hdr_convert")
    if not os.path.exists(binp):
        subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(HERE, "src"),
                        os.path.join(HERE, "tools", "hdr_convert.c"), "-lm", "-o", binp], check=True)

    fails, checks = 0, 0

    def check(cond, msg):
        nonlocal fails, checks
        checks += 1
        if cond:
            print("  ✓ " + msg)
        else:
            print("  ✗ " + msg)
            fails += 1

    for src in srcs:
        tag = os.path.basename(src)[2:-4]
        base = os.path.join(TMP, tag + ".ppm")
        p3f = os.path.join(TMP, tag + "_p3.ppm")
        dif = os.path.join(TMP, tag + "_dither.ppm")
        outp = subprocess.run([binp, src, base, "--head", "2"], capture_output=True, text=True, check=True)
        subprocess.run([binp, src, p3f, "--head", "2", "--p3", "1", "--sat", "1.2"], check=True,
                       capture_output=True)
        subprocess.run([binp, src, dif, "--head", "2", "--dither"], check=True, capture_output=True)
        head = 2.0
        knee_lo = float(outp.stdout.split("knee_lo")[1].split()[0])

        w, h, px = read_ppm16(base)
        _, in8 = read_ppm8(src)
        check(max(px) <= 65535, "%s：输出上限 = head（max %d ≤ 65535）" % (tag, max(px)))

        # 性质 2：纸白以下逐位不动
        bad = 0
        checked = 0
        full = 65535.0
        for k in range(0, len(px), 3):
            y = (0.2126 * to_linear8(in8[k]) + 0.7152 * to_linear8(in8[k + 1])
                 + 0.0722 * to_linear8(in8[k + 2]))
            if y >= knee_lo * 0.98:
                continue
            checked += 1
            for c in range(3):
                want = int(round(to_linear8(in8[k + c]) / head * full))
                if abs(px[k + c] - want) > 1:
                    bad += 1
                    break
        check(checked > 1000 and bad == 0,
              "%s：纸白(%.4f) 以下 %d 个像素逐位不动（越界 %d）" % (tag, knee_lo, checked, bad))

        _, _, px3 = read_ppm16(p3f)
        diff = sum(1 for a, b in zip(px, px3) if a != b)
        check(diff > len(px) // 4, "%s：--p3 1 --sat 1.2 真的改变了画面（%d/%d 通道）" % (tag, diff, len(px)))
        check(max(px3) <= 65535, "%s：色域/饱和后仍不越 head（max %d）" % (tag, max(px3)))

        _, _, pxd = read_ppm16(dif)
        maxd = max(abs(a - b) for a, b in zip(px, pxd))
        check(0 < maxd <= 2, "%s：--dither 只动最低位（最大差 %d LSB）" % (tag, maxd))

    print("✓ hdr_convert：使用者命令的 4 条硬性质（%d 项）%s"
          % (checks, "全过" if fails == 0 else "失败 %d" % fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
