#!/usr/bin/env python3
"""把 hdr_ref 的上屏结果量成「多少条可见带」——比「斜率 3.35×」更接近人眼能看见的东西。

做法：输入图必须是横向渐变（每列一个码值，多行相同），于是
  · 每列亮度 = 该码值经过整条链后的输出；
  · 相邻列的亮度差 = 输出台阶；
  · 用 Weber 判据（暗部 ~2%、亮部 ~1%，这里统一取 1.5%）算「这一台阶会不会被看见」；
  · 数「会看见」的台阶有几级 —— 那就是肉眼能数出来的带数。

两种输入：
  python3 tools/measure_banding.py --csv <hdr_ref --columns 输出> [--json]   # 精确（用未量化的线性值）
  python3 tools/measure_banding.py in.ppm out.ppm                            # 只能看 8 位预览，仅作肉眼参考

判据：相邻两列的线性亮度台阶 / 下方亮度 > 1.5% 且下方亮度 > 0.5 nit（BT.1886 暗部以下看不见）算「可见带」。
"""
import json
import sys

import numpy as np


def read_ppm_any(path):
    """读 P6，maxval 255 或 65535 都支持，返回 0..1 浮点。"""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: 只支持 P6")
    parts, i = [], 2
    while len(parts) < 3:
        while data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while data[i:i+1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j+1].isspace():
            j += 1
        parts.append(data[i:j]); i = j
    i += 1
    w, h, mx = (int(p) for p in parts)
    if mx == 255:
        px = np.frombuffer(data[i:i+w*h*3], dtype=np.uint8).reshape(h, w, 3).astype(np.float64)
    elif mx == 65535:
        px = np.frombuffer(data[i:i+w*h*6], dtype=">u2").reshape(h, w, 3).astype(np.float64)
    else:
        raise SystemExit(f"{path}: maxval {mx} 不支持")
    return px / float(mx)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: 只支持 P6")
    # 解析头：P6 <w> <h> <max>\n
    parts, i = [], 2
    while len(parts) < 3:
        while data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while data[i : i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j : j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h, _ = (int(p) for p in parts)
    px = np.frombuffer(data[i : i + w * h * 3], dtype=np.uint8).reshape(h, w, 3)
    return px.astype(np.float64) / 255.0


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def measure_rows(in_lin, out_lin, thresh=0.015, floor_nit=0.5):
    steps = np.diff(out_lin)
    lower = out_lin[:-1]
    keep = steps > 0
    steps, lower = steps[keep], lower[keep]
    rel = steps / np.maximum(lower, 1e-9)
    # 相对台阶只在「看得见的亮度」上统计：接近黑的地方分母趋零，比值会炸成几万 %
    # （8 位预览把 >1 全限幅到 1.0 时尤其明显），那个数字没有物理意义。
    ok = lower * 203.0 > floor_nit
    visible = int(np.sum((rel > thresh) & ok))
    # 「平台阶」= 相邻列输出完全相同（量化把台阶吃掉了）
    flat = int(np.sum(np.diff(np.round(out_lin, 9)) == 0))
    return {
        "columns": int(out_lin.size),
        "distinct_in": int(np.unique(np.round(in_lin, 8)).size),
        "distinct_out": int(np.unique(np.round(out_lin, 8)).size),
        "flat_steps": flat,
        "max_step_rel": float(rel[ok].max()) if np.any(ok) else 0.0,
        "max_step_nit": float(steps[ok].max() * 203.0) if np.any(ok) else 0.0,
        "measured_steps": int(np.sum(ok)),
        "visible_bands": visible,
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_json = "--json" in sys.argv
    if args and args[0] == "--csv":
        raise SystemExit("用法：--csv 后面给文件路径")

    if "--csv" in sys.argv:
        path = sys.argv[sys.argv.index("--csv") + 1]
        with open(path, encoding="utf-8") as f:
            rows = [l.strip().split(",") for l in f if l.strip() and not l.startswith("col")]
        in_lin = np.array([float(r[1]) for r in rows])
        out_lin = np.array([float(r[2]) for r in rows])
        res = measure_rows(in_lin, out_lin)
    else:
        inp, outp = args[0], args[1]
        a, b = read_ppm_any(inp), read_ppm_any(outp)
        if a.shape != b.shape:
            raise SystemExit("输入输出尺寸不一致")
        ry = int(sys.argv[sys.argv.index("--row") + 1]) if "--row" in sys.argv else a.shape[0] // 2
        row_in = srgb_to_lin(a[ry, :, 0])
        ro = b[ry, :, :]
        out_lin = srgb_to_lin(0.2126 * ro[:, 0] + 0.7152 * ro[:, 1] + 0.0722 * ro[:, 2])
        res = measure_rows(row_in, out_lin)

    if want_json:
        print(json.dumps(res, ensure_ascii=False))
    else:
        print(f"  列数 {res['columns']}，不同输入亮度 {res['distinct_in']}，不同输出亮度 {res['distinct_out']}，"
              f"平台阶 {res['flat_steps']}")
        print(f"  最大台阶 {res['max_step_rel']*100:.2f}% 相对（{res['max_step_nit']:.2f} nit）")
        print(f"  超过 1.5% 的可见带 {res['visible_bands']} 级")
    return 0


if __name__ == "__main__":
    sys.exit(main())
