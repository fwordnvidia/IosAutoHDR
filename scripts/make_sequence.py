#!/usr/bin/env python3
"""造一段「有硬切、有淡入淡出、有全黑」的测试序列，用来量时间行为。

为什么不用 zoompan 单场景：真实播放里最容易出问题的是**转场**——
硬切（菜单→游戏）、交叉淡入（过场）、淡到全黑（死亡/读盘）。单场景慢推量不到这些。

序列（FPS=30，共 300 帧 = 10 秒）：
   0.. 59  A                  （2.0 s）
  60..119  B                  （硬切，2.0 s）
 120..149  B → C 交叉淡入      （1.0 s）
 150..179  C                  （1.0 s）
 180..209  C → 全黑 淡出       （1.0 s）
 210..239  全黑                （1.0 s）
 240..269  全黑 → A 淡入       （1.0 s）
 270..299  A 静止               （1.0 s，用来看淡入结束后 knee 是否收回新纸白）

用法：python3 scripts/make_sequence.py <输出目录> [源图.ppm ...]
"""
import os

# 序列帧率：tools/hdr_seq 必须以同一个值调用（report_sequence.py 会自动读取）
FPS = 30
import sys

W, H = 480, 270


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts, idx = [], 0
    while len(parts) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        parts.append(data[start:idx])
    idx += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[idx:idx + w * h * 3]
    return w, h, px


def resize(px, w, h, tw, th):
    out = bytearray(tw * th * 3)
    for y in range(th):
        sy = min(h - 1, y * h // th)
        for x in range(tw):
            sx = min(w - 1, x * w // tw)
            s = (sy * w + sx) * 3
            d = (y * tw + x) * 3
            out[d:d + 3] = px[s:s + 3]
    return bytes(out)


def blend(a, b, t):
    return bytes(int(a[i] * (1 - t) + b[i] * t + 0.5) for i in range(len(a)))


def scale(a, k):
    return bytes(min(255, int(v * k + 0.5)) for v in a)


def write_ppm(path, px):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(px)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    outdir = argv[1]
    srcs = argv[2:]
    os.makedirs(outdir, exist_ok=True)
    imgs = []
    for s in srcs:
        w, h, px = read_ppm(s)
        imgs.append(resize(px, w, h, W, H))
    while len(imgs) < 3:
        imgs.append(imgs[0])
    A, B, C = imgs[0], imgs[1], imgs[2]
    black = bytes([0]) * (W * H * 3)

    frames = []
    frames += [A] * 60
    frames += [B] * 60
    frames += [blend(B, C, (i + 1) / 30.0) for i in range(30)]
    frames += [C] * 30
    frames += [scale(C, 1.0 - (i + 1) / 30.0) for i in range(30)]
    frames += [black] * 30
    frames += [scale(A, (i + 1) / 30.0) for i in range(30)]
    frames += [A] * 30

    for i, px in enumerate(frames):
        write_ppm(os.path.join(outdir, "f%03d.ppm" % i), px)
    print("✓ 生成 %d 帧（%dx%d）→ %s" % (len(frames), W, H, outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
