#!/usr/bin/env python3
"""生成 / 校验 docs/FIDELITY.md：真实照片上「这个功能到底改了多少画面」。

自动 HDR 最容易被骂的两种错不是精度问题：
  ① 该不动的地方动了（纸白以下被抬亮，画面像蒙了灰）
  ② 该动的地方没动（拐点定得太高，整幅图几乎是恒等，开了跟没开一样）

`tools/hdr_fidelity.c` 逐像素回答这两个问题，这里把答案落成表，并把两条不变量当成断言：
  - 纸白以下的最大改动必须是 **0**（不是「很小」）；
  - 任何通道都不得超过 `head`。
另外跑一档「更激进」的对照（`-DS2A_HDR_WHITE_FRAC=0.05f`，纸白下沉到 5 分位），
用来说明默认档保守到什么程度、放开之后代价在哪——**不是**建议改默认值。
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(HERE, "docs", "FIDELITY.md")
CC = os.environ.get("CC", "cc")
HEAD = 2.0
AGGR = "0.05f"          # 5 分位：只用于对照
CONTENT = "1"           # S2A_HDR_MAP_DEFAULT=1：BT.2408 内容映射


def build(out, extra=()):
    cmd = [CC, "-O2", "-w", "-I", "src", *extra, "tools/hdr_fidelity.c", "-lm", "-o", out]
    r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("编译 %s 失败：%s" % (out, r.stderr.strip()[:400]))


def run(binary, photos):
    r = subprocess.run([binary, "--head", str(HEAD), *photos], cwd=HERE,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("%s 运行失败：%s" % (binary, r.stderr.strip()[:400]))
    rows = []
    for line in r.stdout.splitlines()[1:]:
        p = line.split(",")
        if len(p) < 14:
            raise SystemExit("列数不对（%d）：%s" % (len(p), line))
        rows.append({
            "file": os.path.basename(p[0])[2:-4],       # 去掉 p_ 前缀与 .ppm
            "knee_lo": float(p[1]), "knee_hi": float(p[2]),
            "pct_below": float(p[3]), "max_dlo_below": float(p[4]),
            "pct_ident": float(p[5]), "pct_gain5": float(p[6]),
            "gain_mean": float(p[7]), "gain_p50": float(p[8]), "gain_max": float(p[9]),
            "code_changed": float(p[10]),
            "above_p05": float(p[11]), "above_p20": float(p[12]), "above_p50": float(p[13]),
        })
    return rows


def render(def_rows, aggr_rows, content_rows):
    L = []
    L.append("# 真实内容：这个功能到底改了多少画面\n")
    L.append("自动 HDR 最容易被骂的两种错不是数值精度：**该不动的地方动了**（纸白以下被抬亮，"
             "画面像蒙了灰）或**该动的地方没动**（拐点太高，整幅几乎恒等，开了等于没开）。"
             "下表把两件事都量出来，由 `scripts/report_fidelity.py` 生成（`--check` 挡漂移）。\n")
    L.append("## 不变量（默认档）\n")
    L.append("| 判据 | 结果 |\n|---|---|")
    worst = max(r["max_dlo_below"] for r in def_rows)
    L.append("| 纸白以下（`knee_lo` 及以下）的最大改动 | **%.9f**（要求 0） |" % worst)
    gmax = max(r["gain_max"] for r in def_rows)
    L.append("| 全图最大增益 | %.4f（要求 ≤ head = %.2f） |" % (gmax, HEAD))
    ident = min(r["pct_ident"] for r in def_rows)
    L.append("| 输出码值完全不变的像素占比（最小） | %.2f%% |" % (ident * 100))
    L.append("")
    L.append("## 默认档（纸白 = 99.5 分位，`S2A_HDR_WHITE_FRAC=0.005`）\n")
    L.append("| 图 | 纸白 `knee_lo` | 截断点 `knee_hi` | 纸白以下占比 | 码值有变化的像素 | 增益 >5% 的像素 | 平均增益 | 最大增益 |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in def_rows:
        L.append("| %s | %.4f | %.4f | %.2f%% | %.2f%% | %.2f%% | %.4f | %.2f |"
                 % (r["file"], r["knee_lo"], r["knee_hi"], r["pct_below"] * 100,
                    r["code_changed"] * 100, r["pct_gain5"] * 100, r["gain_mean"], r["gain_max"]))
    L.append("")
    mean_gain = sum(r["gain_mean"] for r in def_rows) / len(def_rows)
    changed = [r["code_changed"] * 100 for r in def_rows]
    L.append("读法：默认档在真实照片上**只动高光**——码值有变化的像素 %.2f%%–%.2f%%，整幅平均增益 "
             "%.4f。这不是 bug，是「纸白以下一律不动」的直接后果：纸白定在 99.5 分位，"
             "所以 99%% 以上的像素落在拐点之下。\n"
             % (min(changed), max(changed), mean_gain))
    L.append("## 敏感度：把纸白下移到 5 / 20 / 50 分位会有多少像素进入增益区\n")
    L.append("纯直方图算术（不走判定逻辑），用来回答「默认是不是太保守」：\n")
    L.append("| 图 | 5 分位 | 20 分位 | 50 分位 |")
    L.append("|---|---|---|---|")
    for r in def_rows:
        L.append("| %s | %.1f%% | %.1f%% | %.1f%% |"
                 % (r["file"], r["above_p05"] * 100, r["above_p20"] * 100, r["above_p50"] * 100))
    L.append("")
    L.append("## 对照：纸白下沉到 5 分位（`-DS2A_HDR_WHITE_FRAC=%s`，**只作对照，不建议当默认**）\n" % AGGR)
    L.append("| 图 | 纸白 | 截断点 | 纸白以下占比 | 码值有变化的像素 | 增益 >5% | 平均增益 | 最大增益 |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in aggr_rows:
        L.append("| %s | %.4f | %.4f | %.2f%% | %.2f%% | %.2f%% | %.4f | %.2f |"
                 % (r["file"], r["knee_lo"], r["knee_hi"], r["pct_below"] * 100,
                    r["code_changed"] * 100, r["pct_gain5"] * 100, r["gain_mean"], r["gain_max"]))
    L.append("")
    a_changed = [r["code_changed"] * 100 for r in aggr_rows]
    L.append("代价：改动面从 %.2f%%–%.2f%% 涨到 %.2f%%–%.2f%%，但 `knee_hi` 也跟着下沉"
             "（例如 Clouds 从 %.4f 掉到 %.4f）——意味着更多内容在截断点之上被压平成恒定的 ×head，"
             "高光层次变少。默认档取 0.5 分位就是在这条曲线上选了保守的一端。\n"
             % (min(changed), max(changed), min(a_changed), max(a_changed),
                def_rows[0]["knee_hi"], aggr_rows[0]["knee_hi"]))
    L.append("## 另一档：内容映射（`S2A_HDR_MAP=1`，BT.2408：`L_out = head * L^1.15`）\n")
    L.append("上面那档只抬高光，适合「画面里还有系统 UI 要保护」的场合。游戏内容没有这个约束，"
             "用户要的是「整幅抬起来」——这一档就是 RTX HDR 那种观感，黑仍为 0、白正好落在 head。\n")
    L.append("| 图 | 纸白以下最大改动 | 码值有变化的像素 | 增益 >5% | 平均增益 | 最大增益 | 18% 灰的增益 |")
    L.append("|---|---|---|---|---|---|---|")
    for r in content_rows:
        L.append("| %s | %.4f | %.2f%% | %.2f%% | %.4f | %.2f | %.4f |"
                 % (r["file"], r["max_dlo_below"], r["code_changed"] * 100,
                    r["pct_gain5"] * 100, r["gain_mean"], r["gain_max"],
                    2.03 * (0.18 ** 0.15)))
    L.append("")
    L.append("读法：这一档**有意**改掉纸白以下（`max_dlo_below` 不再是 0），代价是暗部也被抬起来——"
             "原本压在黑的细节会浮出来、对比度观感变化。所以它是选项而不是默认：默认必须逐位安全，"
             "这一档是给「就是要 HDR 观感」的场合。\n")
    L.append("复现：`python3 scripts/report_fidelity.py`（需要 `build/real/p_*.ppm`）\n")
    return "\n".join(L) + "\n"


def check(def_rows, aggr_rows, content_rows, text):
    bad = []
    for tag, rows in (("默认", def_rows), ("5% 对照", aggr_rows)):
        for r in rows:
            if r["max_dlo_below"] != 0.0:
                bad.append("%s %s：纸白以下有改动 %.9f" % (tag, r["file"], r["max_dlo_below"]))
            if r["gain_max"] > HEAD + 1e-4:
                bad.append("%s %s：最大增益 %.4f 超过 head" % (tag, r["file"], r["gain_max"]))
    for r in content_rows:
        if r["gain_max"] > HEAD + 1e-4:
            bad.append("内容映射 %s：最大增益 %.4f 超过 head" % (r["file"], r["gain_max"]))
        if r["pct_ident"] > 0.5:
            bad.append("内容映射 %s：一半以上像素没变（pct_ident %.3f）——这一档就是要整幅抬起来"
                       % (r["file"], r["pct_ident"]))
    d = [r["code_changed"] for r in def_rows]
    a = [r["code_changed"] for r in aggr_rows]
    if not (sum(d) / len(d) < sum(a) / len(a)):
        bad.append("默认档的改动面没有小于 5%% 对照档（敏感度关系不对）")
    if max(r["pct_ident"] for r in def_rows) < 0.99:
        bad.append("默认档在真实照片上改动面过大（pct_ident 最小值 %.4f < 0.99）"
                   % min(r["pct_ident"] for r in def_rows))
    if bad:
        for b in bad:
            print("::error::" + b)
        return 1
    print("✓ 纸白以下逐位不动、无通道超 head、默认档确实比 5% 档保守")
    return 0


def main():
    photos = sorted(glob.glob(os.path.join(HERE, "build", "real", "p_*.ppm")))
    if len(photos) < 3:
        print("跳过：没有三张真实照片（先跑 scripts/report_real.py 拉源图）")
        return 0
    build("build/hdr_fidelity")
    build("build/hdr_fidelity_aggr", ("-DS2A_HDR_WHITE_FRAC=" + AGGR,))
    build("build/hdr_fidelity_content", ("-DS2A_HDR_MAP_DEFAULT=" + CONTENT,))
    def_rows = run("./build/hdr_fidelity", photos)
    aggr_rows = run("./build/hdr_fidelity_aggr", photos)
    content_rows = run("./build/hdr_fidelity_content", photos)
    text = render(def_rows, aggr_rows, content_rows)
    if "--check" in sys.argv:
        rc = check(def_rows, aggr_rows, content_rows, text)
        if rc:
            return rc
        if not os.path.exists(DOC) or open(DOC, encoding="utf-8").read() != text:
            print("::error::docs/FIDELITY.md 与实测不一致（改了切点逻辑没重新生成？）")
            return 1
        print("✓ docs/FIDELITY.md 与实测一致（%d 行）" % text.count("\n"))
        return 0
    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    open(DOC, "w", encoding="utf-8").write(text)
    print("写出 docs/FIDELITY.md（%d 行）" % text.count("\n"))
    return check(def_rows, aggr_rows, content_rows, text)


if __name__ == "__main__":
    sys.exit(main())
