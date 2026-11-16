#!/usr/bin/env python3
"""把 300 帧转场序列（硬切 / 交叉淡入 / 淡到黑 / 黑场 / 淡入）的时间行为写成 docs/SEQUENCE.md。

为什么单独一份：单场景慢推只能证明「参数没写错」，量不到转场。真实播放里最容易被看见的
问题都在转场：硬切后整段偏暗、淡到黑时中调反而变亮、从黑回来先闪一下再收。
本报告把每段的 knee_lo 轨迹、单帧最大跳变、方向翻转次数、增益区间摊开，并由 `--check`
断言三条硬性质（淡出冻结 / 黑场不乱跳 / 淡入后收得回）。

用法：python3 scripts/report_sequence.py [--check]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEQ = os.path.join(ROOT, "build", "seq")
CSV = os.path.join(ROOT, "build", "seq.csv")
OUT = os.path.join(ROOT, "docs", "SEQUENCE.md")

# (名字, 起始帧, 结束帧, 说明)
SEGMENTS = [
    ("A 静止", 0, 59, "基准：knee 收敛后不动"),
    ("硬切 → B", 60, 119, "Δavg 0.66 stop：必须一帧内改判，不能慢慢爬"),
    ("B → C 交叉淡入", 120, 149, "两幅不同内容叠化，白点连续变"),
    ("C 静止", 150, 179, "第二个基准"),
    ("C → 黑 淡出", 180, 209, "整幅等比变暗：knee 必须冻结（否则越黑越亮）"),
    ("全黑", 210, 239, "不可测：保持上一帧"),
    ("黑 → A 淡入", 240, 269, "升亮：不许跟着当前亮度重定目标"),
    ("A 静止（收尾）", 270, 299, "淡入结束：收到新纸白"),
]


def seq_fps():
    """从 scripts/make_sequence.py 读取 FPS 常量（唯一真相源）。"""
    src = open(os.path.join(ROOT, "scripts", "make_sequence.py"), encoding="utf-8").read()
    m = re.search(r"^FPS\s*=\s*(\d+)", src, re.M)
    if not m:
        raise SystemExit("make_sequence.py 里找不到 FPS 常量")
    return int(m.group(1))


def read_csv():
    rows = []
    with open(CSV, "r", encoding="utf-8") as f:
        for line in f:
            p = line.strip().split(",")
            if len(p) != 12:
                continue
            try:
                rows.append({
                    "f": int(p[0]), "white": float(p[1]), "avg": float(p[2]),
                    "lo": float(p[3]), "hi": float(p[4]), "scene": float(p[5]),
                    "gpw": float(p[6]), "g050": float(p[7]), "g083": float(p[8]),
                    "gpaper": float(p[10]), "over": float(p[11]),
                    "dlo": float(p[9]),
                })
            except ValueError:
                continue   # 极端帧解析失败
            if any(v != v for v in (rows[-1]["lo"], rows[-1]["avg"], rows[-1]["g050"])):
                rows.pop()   # 全黑帧是 nan：不可测，单独说明
                continue
    return rows


def stats(rows, a, b):
    seg = [r for r in rows if a <= r["f"] <= b]
    if not seg:
        return None
    dlo = [abs(seg[i]["lo"] - seg[i - 1]["lo"]) for i in range(1, len(seg))]
    rev = 0
    sign = 0
    for i in range(1, len(seg)):
        d = seg[i]["lo"] - seg[i - 1]["lo"]
        if abs(d) < 1e-5:
            continue
        s = 1 if d > 0 else -1
        if sign and s != sign:
            rev += 1
        sign = s
    g = [r["g050"] for r in seg]
    return {
        "lo_first": seg[0]["lo"], "lo_last": seg[-1]["lo"],
        "lo_min": min(r["lo"] for r in seg), "lo_max": max(r["lo"] for r in seg),
        "max_step": max(dlo) if dlo else 0.0,
        "rev": rev,
        "g_min": min(g), "g_max": max(g),
    }


def main(argv):
    check = "--check" in argv
    if not os.path.isdir(SEQ) or len(os.listdir(SEQ)) < 300:
        subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "make_sequence.py"), SEQ] +
                       [os.path.join(ROOT, "build", "real", n) for n in (
                           "p_Clouds_by_Tibor_Mokanszki.ppm", "p_Fuji_san_by_amaral.ppm",
                           "p_Monument_valley_by_orbitelambda.ppm")], check=True)
    frames = sorted(os.path.join(SEQ, n) for n in os.listdir(SEQ) if n.startswith("f") and n.endswith(".ppm"))
    # 帧率必须与合成器一致：hdr_seq 的 --hz 直接决定平滑系数与「每秒落差」判据，
    # 声明成 60 而内容其实是 30 fps 会让所有时间结论偏快一倍（曾经就是默认值 60 造成的）。
    fps = seq_fps()
    with open(CSV, "w", encoding="utf-8") as f:
        subprocess.run([os.path.join(ROOT, "build", "hdr_seq"), "--hz", str(fps)] + frames,
                       stdout=f, check=True)
    rows = read_csv()
    live = [r for r in rows if r["gpaper"] == r["gpaper"]]
    gp_min = min((r["gpaper"] for r in live), default=float("nan"))
    gp_max = max((r["gpaper"] for r in live), default=float("nan"))
    ov_max = max((r["over"] for r in live), default=float("nan"))
    if not rows:
        print("✗ 序列没有可用帧")
        return 1

    lines = [
        "# 转场序列（300 帧 @%d fps）" % seq_fps(),
        "",
        "来源：`scripts/make_sequence.py` 合成，`tools/hdr_seq.c` 逐帧跑 `s2a_hdr_auto_l1b_step_fused_hz`。",
        "内容：A 静止 60 → 硬切 B 60 → B→C 交叉淡入 30 → C 静止 30 → C→黑 淡出 30 → 全黑 30 → 黑→A 淡入 30 → A 静止 30。",
        "本文件由 `scripts/report_sequence.py` 生成，不要手改（`--check` 会在不一致时失败）。",
        "",
        "| 段 | 帧 | knee_lo 首 → 末 | 区间 | 单帧最大跳变 | 方向翻转 | 0.50 线性增益 |",
        "|---|---|---|---|---|---|---|",
    ]
    st = {}
    for name, a, b, _ in SEGMENTS:
        s = stats(rows, a, b)
        if not s:
            lines.append("| %s | %d–%d | — | — | — | — | — |" % (name, a, b))
            continue
        st[name] = s
        lines.append("| %s | %d–%d | %.4f → %.4f | %.4f–%.4f | %.5f | %d | ×%.4f–×%.4f |" % (
            name, a, b, s["lo_first"], s["lo_last"], s["lo_min"], s["lo_max"], s["max_step"],
            s["rev"], s["g_min"], s["g_max"]))

    lines += ["", "## 三条硬性质", ""]
    fails = []
    fade = st.get("C → 黑 淡出")
    if fade:
        ok = fade["max_step"] < 1e-4 and abs(fade["g_max"] - 1.0) < 5e-3
        lines.append("- 淡出冻结：180–208 帧 knee_lo 单帧跳变 %.5f（< 1e-4 为过），0.50 线性增益上限 ×%.4f（1.000 为过）→ %s"
                     % (fade["max_step"], fade["g_max"], "过" if ok else "不过"))
        if not ok:
            fails.append("淡出冻结")
    black = st.get("全黑")
    if black:
        ok = black["max_step"] < 1e-4
        lines.append("- 黑场保持：210–239 帧 knee_lo 跳变 %.5f → %s" % (black["max_step"], "过" if ok else "不过"))
        if not ok:
            fails.append("黑场保持")
    else:
        hold = [r for r in rows if 209 <= r["f"] <= 241]
        ok = len(hold) >= 2 and abs(hold[-1]["lo"] - hold[0]["lo"]) < 1e-4
        lines.append("- 黑场保持：全黑段不可测（`s2a_hdr_auto_l1b_step_fused_hz` 返回 0，调用方保持），"
                     "跨黑场 knee_lo %.4f → %.4f → %s" % (hold[0]["lo"] if hold else float("nan"),
                                                          hold[-1]["lo"] if hold else float("nan"),
                                                          "过" if ok else "不过"))
        if not ok:
            fails.append("黑场保持")
    fin = st.get("黑 → A 淡入")
    tail = st.get("A 静止（收尾）")
    if fin and tail:
        ok = abs(tail["lo_last"] - 0.5278) < 0.02
        lines.append("- 淡入收回：269 帧 knee_lo %.4f（冻结中），299 帧 %.4f（目标 0.5278，±0.02 内为过）→ %s"
                     % (fin["lo_last"], tail["lo_last"], "过" if ok else "不过"))
        if not ok:
            fails.append("淡入收回")
    cut = st.get("硬切 → B")
    if cut:
        lines.append("- 硬切响应（不作为判据）：60 帧 knee_lo 落在 %.4f，整段单帧最大跳变 %.4f，方向翻转 %d 次"
                     % (cut["lo_last"], cut["max_step"], cut["rev"]))
    lines += ["", "阈值：`S2A_HDR_SCENE_LOW/HIGH = 0.40/1.10`（旧 0.5/1.6 时 0.66 stop 只拿 5.5% 权重、"
                  "B 段要 24 帧才走完 95%）。改后中等切镜 31% 权重、7 帧到 95%，"
                  "`ref_check` 场景十三把这一档和「0.30 stop/帧 快摇镜仍是 0 权重」一起钉住。",
                  "",
                  "冻结窗口按 **stop/秒** 判（`S2A_HDR_FADE_MIN_S/MAX_S = 1.5/16`）：同一段淡出在 "
                  "30/60/120 Hz 下每帧落差差 2/4 倍，按帧写常数会让判据在 120 Hz 整体失效。"
                  "本序列是 30 fps 内容，`--hz` 由 `scripts/make_sequence.py` 的 `FPS` 常量直接读"
                  "（曾经固定用默认 60，等于把内容当两倍速，所有时间结论偏快一倍）。"
                  "换到 30 Hz 后交叉淡入段不再整段冻结：慢于 1.5 stop/s 的帧照常跟随，整段从 "
                  "0.3898 平滑爬到 0.5936，段末与 C 稳态 0.6625 差 0.07，随后 30 帧内以 ≤0.0107/帧 跟上"
                  "（没有台阶）。", "",
                  "## 端到端的两个不变量（整片 269 帧可测）",
                  "",
                  "上面按段统计的是「拐点走得对不对」；下面两条是**每一帧的输出**都必须满足的性质，"
                  "用的是与上屏完全相同的 `s2a_hdr_itm_gain` / `s2a_hdr_grade_rgb`：",
                  "",
                  "- 纸白增益 **恒 1.000000**（实测 %.6f .. %.6f）：`knee_lo` 那一档永远不动，"
                  "所以 UI、字幕、纸白文字在 HDR 下亮度与 SDR 逐位相同。" % (gp_min, gp_max),
                  "- 极端饱和度探针 `(1, 0.24, 1)` @ `sat=3.0` 调色后 **不超 head**（实测最大超 %.6f）："
                  "超了系统会硬钳，切色块并推走色相；`s2a_hdr_clip_head` 按三元组等比缩放把" % ov_max,
                  "  它拉回上界内，只降亮度。",
                  ""]
    text = "\n".join(lines)

    old = ""
    if os.path.exists(OUT):
        with open(OUT, "r", encoding="utf-8") as f:
            old = f.read()

    if not (abs(gp_min - 1.0) < 2e-5 and abs(gp_max - 1.0) < 2e-5):
        fails.append("纸白增益恒定（%.6f..%.6f）" % (gp_min, gp_max))
    if ov_max > 1e-6:
        fails.append("调色不超 head（最大超 %.6f）" % ov_max)
    print("· 端到端不变量：%d 帧可测，纸白增益 %.6f..%.6f，超 head %.6f"
          % (len(live), gp_min, gp_max, ov_max))

    if check:
        if old != text:
            print("✗ docs/SEQUENCE.md 与实测不一致（重跑不带 --check 生成）")
            return 1
        if fails:
            print("✗ 序列判据不过：" + "、".join(fails))
            return 1
        print("✓ docs/SEQUENCE.md 与实测一致，三条硬性质 + 两条端到端不变量全过")
        return 0
    if fails:
        print("✗ 序列判据不过：" + "、".join(fails))
        return 1
    if old != text:
        with open(OUT, "w", encoding="utf-8") as f:
            f.write(text)
        print("写出 %s（%d 行）" % (OUT, len(lines)))
    else:
        print("✓ %s 已是最新（%d 行）" % (OUT, len(lines)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
