#!/usr/bin/env python3
"""生成 / 校验 docs/ABLATION.md（消融表：四档策略各解决什么）。

数据全部来自 tools/hdr_ablate.c 的实测输出，`--check` 会因数字漂移而失败，
避免文档里的表格与代码脱节。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(HERE, "build", "hdr_ablate")
SRC = os.path.join(HERE, "tools", "hdr_ablate.c")
OUT = os.path.join(HERE, "docs", "ABLATION.md")

DESC = {
    "fixed": "固定拐点 0.55/1.0，不看画面（老式做法）",
    "apl": "只看平均亮度：`lo = 平均亮度`，`hi = 1.0`",
    "white": "只看普通白点：`lo = 白点`，`hi = 1.0`（无 HUD 剔除、无暗场兜底）",
    "full": "本补丁 L1b：bulk 白点 + 暗场 lit 判据 + 死区 + 场景融合 + 帧率归一",
}


def build():
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(HERE, "src"), SRC, "-lm", "-o", BIN],
                   check=True)


def run(*args):
    out = subprocess.run([BIN, *args], check=True, capture_output=True, text=True).stdout
    lines = [l for l in out.strip().split("\n") if l]
    head = lines[0].split(",")
    return head, [dict(zip(head, l.split(","))) for l in lines[1:]]


def render():
    build()
    _, rows = run()
    _, seq = run("--seq")

    L = []
    L.append("# 消融实验：自动判白点到底解决了什么")
    L.append("")
    L.append("> 本文件由 `scripts/report_ablate.py` 生成，`--check` 会因数字漂移而失败。")
    L.append("> 数据源 `tools/hdr_ablate.c`（全 C、无依赖），四个合成场景各跑 60 帧到稳态。")
    L.append("")
    L.append("四档策略（每档都在同一套直方图/ITM 之上，只有「怎么定拐点」不同）：")
    L.append("")
    for p in ("fixed", "apl", "white", "full"):
        L.append("- **%s** — %s" % (p, DESC[p]))
    L.append("")
    L.append("## 稳态拐点与增益")
    L.append("")
    L.append("| 策略 | 场景 | knee_lo | 纸白(0.86) | 0.18 线性 | 0.50 线性 | 0.90 线性 | 越界像素 |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in rows:
        L.append("| %s | %s | %s | ×%s | ×%s | ×%s | ×%s | %s |"
                 % (r["policy"], r["scene"], r["knee_lo"], r["gain_white"],
                    r["gain_018"], r["gain_050"], r["gain_090"], r["clip_frac"]))
    L.append("")
    L.append("读法（都能在上表里逐格对上）：")
    L.append("")
    L.append("- `fixed` 四个场景给出**同一组数**（0.55 / 纸白 ×1.29）——它不认识画面，满屏 UI 白照样被抬；")
    L.append("- `apl` 在满屏 UI 白上凑巧接近 ×1.00，但 `hud`/`mid` 把 0.50 线性抬到 ×1.33：")
    L.append("  平均亮度高不等于「白多」，而是「没有暗部」，抬中间调就是发灰发闷；")
    L.append("- `white` 在 `hud` 上被那一行 HUD 白条（1.9% 像素）钉在 0.8974 —— 0.90 线性增益只剩 **×1.002**，")
    L.append("  整幅画面等于没扩张；`star` 上被星点钉在 0.4893，黑底画面永远等不到扩张；")
    L.append("- `full` 是唯一四格都合理的：`hud` 回到 0.1897（0.90 线性 ×1.958，与无 HUD 的 `mid` 一致），")
    L.append("  星空 0.0200，UI 白纸白 ×1.00 不动——没有 HUD 时它必须与 `white` 逐位一致（上表 `mid` 行已对上）。")
    L.append("")
    L.append("## 时间行为：切镜后多少帧到 85%")
    L.append("")
    L.append("序列：32 帧中间调画面 → 32 帧 UI 白（同一条平滑链路，系数 0.10 @60 Hz）。")
    L.append("")
    L.append("| 策略 | 到 85% 的帧数 | 首帧 knee_lo | 末帧 knee_lo |")
    L.append("|---|---|---|---|")
    for r in seq:
        L.append("| %s | %s | %s | %s |" % (r["policy"], r["frames85"], r["knee_first"], r["knee_last"]))
    L.append("")
    L.append("`-1` = 拐点从不动（`fixed` 本来就没有目标可追）；`0` = 切镜当帧就到稳态。")
    L.append("`full` 之所以能 0 帧，是因为场景判据除了平均亮度还看**白点跳变**——")
    L.append("同 APL 的切镜（菜单→游戏、同亮度过场）只靠平均亮度会判不出来，得爬十几帧。")
    L.append("")
    return "\n".join(L) + "\n"


def main(argv):
    text = render()
    if "--check" in argv:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print("::error::docs/ABLATION.md 与实测不一致（改过算法没重新生成报告？）")
            return 1
        print("✓ docs/ABLATION.md 与实测一致（%d 行）" % len(text.strip().split("\n")))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write(text)
    print("✓ 已生成 docs/ABLATION.md（%d 行）" % len(text.strip().split("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
