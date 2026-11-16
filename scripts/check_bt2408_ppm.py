#!/usr/bin/env python3
"""check_bt2408_ppm.py <16 位 PPM> <head>：BT.2408 放置方式的两条硬性质。

黑必须仍近 0（幂函数在 0 处仍为 0，这是它相对「整体加一个偏移」的关键差别）；
最亮值必须落在 [0.85*head, head]（纸白被放到 203 cd/m² 的定义点）。
用法由 tools/ref_check.sh 调用。
"""
import struct
import sys


def main():
    path, head = sys.argv[1], float(sys.argv[2])
    b = open(path, "rb").read()
    i, t = 0, []
    while len(t) < 4:
        while b[i:i + 1].isspace():
            i += 1
        j = i
        while not b[j:j + 1].isspace():
            j += 1
        t.append(b[i:j])
        i = j
    w, h = int(t[1]), int(t[2])
    px = struct.unpack(">%dH" % (w * h * 3), b[i + 1:i + 1 + w * h * 6])
    scale = 65535.0 / head
    mn, mx = min(px) / scale, max(px) / scale
    if mn > 0.02:
        print("  ✗ bt2408：最暗值 %.4f 被抬起来了（应近 0）" % mn)
        return 1
    if not (0.85 * head <= mx <= head + 1e-3):
        print("  ✗ bt2408：最亮值 %.4f 不在 [%.2f, %.2f]" % (mx, 0.85 * head, head))
        return 1
    print("  ✓ bt2408：最暗 %.4f（近 0）、最亮 %.4f（≈head %.1f）" % (mn, mx, head))
    return 0


if __name__ == "__main__":
    sys.exit(main())
