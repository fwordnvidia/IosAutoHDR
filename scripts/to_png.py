#!/usr/bin/env python3
"""把 hdr_convert 写的 16 位线性 HDR 预览转成能直接看的 PNG。

注意这是**预览映射**，不是上屏结果：真机上 head 以上的能量由 EDR 显示器还原，
这里只能压到 8 位。默认用「Reinhard 压缩到 [0,1] + sRGB 编码」，所以暗部亮部都会
比真机扁一些——它的用途是让你在普通屏幕上看出「哪些区域被抬起来了」。

用法：python3 scripts/to_png.py in.ppm out.png [--head 2] [--gamma]
     --gamma：不做压缩，直接把值当 sRGB 编码值（看「线性能量有多大」更直观但会过曝）
"""
import struct
import sys

from PIL import Image


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
    assert toks[0] == b"P6" and int(toks[3]) == 65535, "不是 hdr_convert 的 16 位输出"
    w, h = int(toks[1]), int(toks[2])
    n = w * h * 3
    return w, h, struct.unpack(">%dH" % n, b[i + 1:i + 1 + n * 2])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        return 2
    head = 2.0
    if "--head" in sys.argv:
        head = float(sys.argv[sys.argv.index("--head") + 1])
    gamma_only = "--gamma" in sys.argv
    w, h, px = read_ppm16(args[0])

    out = bytearray(w * h * 3)
    for i, q in enumerate(px):
        v = q / 65535.0 * head                       # 线性能量
        if gamma_only:
            v = min(v, 1.0)
        else:
            v = v / (1.0 + v)                        # Reinhard：单调、保序、不过曝
        # 线性 → sRGB 编码
        e = v * 12.92 if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
        out[i] = max(0, min(255, int(round(e * 255.0))))
    Image.frombytes("RGB", (w, h), bytes(out)).save(args[1])
    print("%s → %s  %dx%d（%s 映射，head %.1f）" % (args[0], args[1], w, h,
                                                   "直接截断" if gamma_only else "Reinhard 压缩", head))
    return 0


if __name__ == "__main__":
    sys.exit(main())
