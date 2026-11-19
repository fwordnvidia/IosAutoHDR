#!/usr/bin/env python3
"""时间行为验证：真实照片 + 慢速运动，按上屏同一条链路跑整段序列。

为什么需要：单帧批测（report_real.py）只看「这一帧判得对不对」，看不出
  · 慢速平移会不会被场景判据误判成切镜（画面来回跳）；
  · 每帧 knee 的变化幅度（理论上被 smoothing 吸收到 <2%）；
  · 方向是否来回翻转（来回抖 = 观感上的呼吸）。
做法：ffmpeg 的 zoompan 把一张真实壁纸推成 60 帧（30 fps 的 2 秒慢推），
用 tools/hdr_seq 跑整段——它内部走的就是 nvn_hdr_hist.inc 的
s2a_hdr_auto_l1b_step_fused，所以量到的就是上屏真实行为。

用法：python3 scripts/report_video.py           # 生成 docs/TEMPORAL.md
      python3 scripts/report_video.py --check   # 只比对（CI）
源图缺 / 无 ffmpeg 时报告写「跳过」而不是失败。
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "docs", "TEMPORAL.md")
WORK = os.path.join(HERE, "build", "seq")
SOURCES = [
    "/usr/share/backgrounds/Clouds_by_Tibor_Mokanszki.jpg",
    "/usr/share/backgrounds/Fuji_san_by_amaral.png",
    "/usr/share/backgrounds/Monument_valley_by_orbitelambda.jpg",
]
FPS = 30
FRAMES = 60
# 判据（都来自「人要看着舒服」这个目标，不是拟合出来的数）
MAX_STEP = 0.02          # 单帧 knee_lo 相对变化上限 2%
MAX_SCENE = 0.05         # 慢速运动不许被判成切镜
MAX_REVERSAL = 2         # 方向翻转次数（允许数值抖动，不许来回呼吸）


def have_ffmpeg():
    return shutil.which("ffmpeg") is not None


def build_tool():
    binp = os.path.join(HERE, "build", "hdr_seq")
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(HERE, "src"),
                    os.path.join(HERE, "tools", "hdr_seq.c"), "-lm", "-o", binp], check=True)
    return binp


def clip_frames(src, tag, binp):
    """把一张图推成 60 帧并跑 hdr_seq，返回统计字典。"""
    d = os.path.join(WORK, tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-loop", "1", "-i", src,
                    "-vf", "scale=960:540,zoompan=z='min(zoom+0.0012,1.25)':d=%d:"
                           "x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':s=480x270:fps=%d" % (FRAMES, FPS),
                    "-frames:v", str(FRAMES), os.path.join(d, "f_%03d.ppm")], check=True)
    files = sorted(glob.glob(os.path.join(d, "f_*.ppm")))
    rows = subprocess.run([binp, "--hz", str(FPS)] + files, capture_output=True, text=True,
                          check=True, cwd=HERE).stdout.strip().split("\n")
    hdr = rows[0].split(",")
    recs = [dict(zip(hdr, r.split(","), strict=True)) for r in rows[1:]]
    ok = [r for r in recs if r["lo"] != "nan"]
    if not ok:
        return None
    lo = [float(r["lo"]) for r in ok]
    steps = [float(r["dlo"]) for r in ok if r["dlo"] not in ("nan", "")]
    scene = [float(r["scene"]) for r in ok]
    g083 = [float(r["g083"]) for r in ok]
    g050 = [float(r["g050"]) for r in ok]
    # 纸白增益只看稳态（后 1/3 帧）：真实切换的过渡期里 knee_lo 还没追上白点，
    # 那一瞬间「gain(white) > 1」是对的（平滑的本意），不该算不合格。
    tail = ok[-max(1, len(ok) // 3):]
    gw = [float(r["gwhite"]) for r in tail]
    reversals = sum(1 for a, b in zip(steps, steps[1:], strict=False) if a * b < -1e-9)
    maxstep = max((abs(s) / max(abs(x), 1e-6) for s, x in zip(steps, lo[1:], strict=False)), default=0.0)
    return {
        "name": tag, "frames": len(ok), "lo0": lo[0], "lo1": lo[-1],
        "lo_min": min(lo), "lo_max": max(lo),
        "maxstep": maxstep, "maxscene": max(scene), "reversals": reversals,
        "g050_min": min(g050), "g050_max": max(g050),
        "gw_max": max(gw), "g083_max": max(g083),
        "nan": len(recs) - len(ok),
    }


def main():
    check = "--check" in sys.argv
    lines = ["# 时间行为（真实照片 + 慢速运动）", "",
             "单帧批测回答「这一帧判得对不对」，这张表回答「连续播的时候稳不稳」：",
             "ffmpeg 的 `zoompan` 把一张真实壁纸推成 %d 帧（%d fps，2 秒慢推），" % (FRAMES, FPS),
             "用 `tools/hdr_seq` 跑整段——它内部调用 `nvn_hdr_hist.inc` 用的同一条",
             "`s2a_hdr_auto_l1b_step_fused`，所以量到的就是上屏会发生的过渡。", "",
             "判据与理由：", "",
             "| 指标 | 上限 | 理由 |", "|---|---|---|",
             "| 单帧 `knee_lo` 相对变化 | %.0f%% | 平滑的时间常数（60 Hz 每帧 10%%）应吸收掉内容微动 |" % (MAX_STEP * 100),
             "| 慢速运动的最大场景权重 | %.2f | 平移不是切镜，误判会让画面「先暗后亮」 |" % MAX_SCENE,
             "| 方向翻转次数 | %d | 允许数值抖动，不允许来回呼吸 |" % MAX_REVERSAL, ""]

    sources = [p for p in SOURCES if os.path.exists(p)]
    if not sources or not have_ffmpeg():
        if check:
            print("· 本机缺源图或 ffmpeg，跳过 docs/TEMPORAL.md 对账")
            return 0
        lines += ["_本机没有源图或 ffmpeg，本表跳过。_", ""]
        text = "\n".join(lines)
        return finish(text, check)

    binp = build_tool()
    os.makedirs(WORK, exist_ok=True)
    stats = []
    for p in sources:
        tag = os.path.splitext(os.path.basename(p))[0]
        s = clip_frames(p, tag, binp)
        if s:
            stats.append(s)
    if not stats:
        lines += ["_序列生成失败，本表跳过。_", ""]
        return finish("\n".join(lines), check)

    lines += ["| 片 | 帧数 | knee_lo 起→止 | 单帧最大变化 | 最大场景权重 | 方向翻转 | 纸白增益（稳态） | 0.50 增益 | 0.832 增益 | 空帧 |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    bad = []
    for s in stats:
        lines.append("| %s | %d | %.4f → %.4f | %.2f%% | %.3f | %d | %.4f | %.4f | %.3f | %d |"
                     % (s["name"], s["frames"], s["lo0"], s["lo1"], s["maxstep"] * 100,
                        s["maxscene"], s["reversals"], s["gw_max"], s["g050_max"], s["g083_max"], s["nan"]))
        if s["maxstep"] > MAX_STEP:
            bad.append("%s: 单帧变化 %.1f%% > %.0f%%" % (s["name"], s["maxstep"] * 100, MAX_STEP * 100))
        if s["maxscene"] > MAX_SCENE:
            bad.append("%s: 场景权重 %.2f > %.2f" % (s["name"], s["maxscene"], MAX_SCENE))
        if s["reversals"] > MAX_REVERSAL:
            bad.append("%s: 方向翻转 %d 次" % (s["name"], s["reversals"]))
        if s["gw_max"] > 1.001:
            bad.append("%s: 纸白增益 %.4f > 1.001（纸白必须不动）" % (s["name"], s["gw_max"]))

    lines += ["", "三条判据：%s" % ("**全部通过**" if not bad else "**未通过**：" + "；".join(bad)), "",
              "复现：`python3 scripts/report_video.py`（需 ffmpeg；缺源图时写「跳过」）。", ""]
    text = "\n".join(lines)
    if bad:
        print("::error:: 时间行为不达标：%s" % "；".join(bad))
        if check:
            return 1
    return finish(text, check)


def finish(text, check):
    old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else None
    if check:
        if old != text:
            print("::error:: docs/TEMPORAL.md 与实测不一致（重跑 scripts/report_video.py）")
            return 1
        print("✓ docs/TEMPORAL.md 与实测一致（%d 行）" % len(text.split("\n")))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write(text)
    print("✓ 已生成 %s（%d 行）" % (OUT, len(text.split("\n"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
