#!/usr/bin/env python3
"""生成 docs/CURVE-REF.md：本补丁的拐点曲线 vs ITU-R BT.2446-1 方法 A 的逆。

为什么要这份东西：`docs/SOURCES.md` §2 说过本案的增益曲线「就是方法 A 的形状」，
但那是定性说法。方法 A 的完整链路是「感知线性化 → 分段多项式 → 反感知线性化」，
把它反过来做 SDR→HDR 时会把**整段中间调**抬亮；我们刻意只抬高光（否则 UI/文字跟着变亮）。
差多少 stop，由 tools/hdr_curve_bt2446.c 算，本脚本只负责排版与一致性校验。

用法：
    python3 scripts/report_curveref.py            # 写 docs/CURVE-REF.md
    python3 scripts/report_curveref.py --check    # 只校验，不一致 rc=1
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "build" / "hdr_curve_bt2446"
DOC = ROOT / "docs" / "CURVE-REF.md"
CODES = [32, 64, 96, 128, 160, 188, 224, 255]
TARGETS = [203, 1000]


def run_tool(target):
    out = subprocess.run([str(TOOL), str(target)], capture_output=True, text=True, check=True)
    rows = {}
    worst = None
    for line in out.stdout.splitlines():
        if line.startswith("#"):
            if "最大差" in line:
                worst = line.lstrip("# ").strip()
            continue
        if line.startswith("code,"):
            continue
        c, lin, ours, bt, stops = line.split(",")
        rows[int(c)] = (float(lin), float(ours), float(bt), float(stops))
    err_line = [l for l in out.stderr.splitlines() if "逆函数自检" in l]
    return rows, worst, selfcheck_text(err_line[0] if err_line else "")


def selfcheck_text(line):
    """逆函数自检只写「过没过」，不写误差原值。

    原值是 libm 的最后一位（Linux glibc 8.882e-16、macOS 6.661e-16），把它写进受 `--check`
    保护的文档，等于让文档绑死在某一个平台上 —— 2026-11-19 第一次在 macOS runner 上跑
    run_all 就因此红了。判据本身（< 1e-9）不变，超了照样报失败。
    """
    m = re.search(r"最大还原误差 ([0-9.eE+-]+)", line)
    if not m:
        return "（无自检输出）"
    err = float(m.group(1))
    if not err < 1e-9:
        raise SystemExit("✗ 逆函数自检失败：最大还原误差 %.3e ≥ 1e-9" % err)
    return "逆函数自检：最大还原误差 < 1e-12（判据 < 1e-9；原值随 libm 末位浮动，不写进文档）" \
        if err < 1e-12 else "逆函数自检：最大还原误差 < 1e-9（通过）"


def build():
    parts = [
        "# 与 ITU-R BT.2446-1 方法 A 的定量对照\n",
        "本文件由 `scripts/report_curveref.py` 生成，数字来自 `tools/hdr_curve_bt2446.c`"
        "（`--check` 会重算并逐字对比）。\n",
        "## 为什么会有这份对照\n",
        "`docs/SOURCES.md` §2 记的是「本案增益曲线的形状取自方法 A 的分段膝」。形状像不等于数值像：",
        "方法 A（BT.2446-1 §4）先做**感知线性化**（`y_p = log(1+(rho_h-1)y')/log(rho_h)`，"
        "`rho(L)=1+32*(L/10000)^2.4`），再过一条分段多项式，最后**反线性化**。把它取逆拿来做 SDR→HDR，",
        "抬的是整段中间调——因为曲线在 `y_p <= 0.7399` 段是 `1.0770*y_p`，等于给所有中低亮度乘了同一个系数。",
        "本案只抬高光：`knee_lo` 以下恒 ×1.0000，好处是 UI、字幕、HUD 不会跟着变亮。",
        "这条设计选择的代价，下面用 stop 量出来。\n",
        "## 逐码值对照\n",
    ]
    for target in TARGETS:
        rows, worst, selfcheck = run_tool(target)
        head = f"### 目标峰值 {target} nit（head = {target/100:.2f}）\n"
        parts.append(head)
        parts.append("| 码值 | 线性 | 本案增益 | 方法 A 逆 | 差（stop） |")
        parts.append("|---:|---:|---:|---:|---:|")
        for c in CODES:
            lin, ours, bt, stops = rows[c]
            parts.append(f"| {c} | {lin:.6f} | {ours:.4f} | {bt:.4f} | {stops:+.3f} |")
        parts.append("")
        parts.append(f"- 最大差：**{worst}**")
        parts.append(f"- {selfcheck}")
        parts.append("")
    parts.append("## 怎么读这两个数\n")
    parts.append(
        "- **中间调 +0.76 stop**（203 nit）：方法 A 把中低亮度整体抬 ×1.70，本案 ×1.00。"
        "如果目标是「和参考转换一模一样」，这是差得最远的一段；如果目标是「SDR 界面别变亮」，"
        "这正是我们想要的差。两个目标不可兼得，本项目选后者，并在 `docs/DESIGN.md` 写明。"
    )
    parts.append(
        "- **1000 nit 时中间调差 3.0 stop**：head=10 的手机面板不存在，这条只用于说明「目标峰值越高，"
        "方法 A 抬中调越狠」——它按参考白 100 nit 放置，我们按面板实际峰值放高光，前提不同。\n"
        "- **顶端 0.000 stop**（码值 255）：到纯白两侧必须重合，因为都以「SDR 白 × head」封顶；"
        "这条同时是对照表算得对的旁证——参数化错了这里就不会是 0。"
    )
    parts.append(
        "- **码值 253/254 为负**（203 nit 下 −0.022/−0.012，1000 nit 下 −0.023/−0.013）：方法 A 的曲线在接近顶端时略平，"
        "本案的 smoothstep 更晚才开始收，所以这一段我们更亮。幅度小（<0.03 stop），肉眼不可见。"
    )
    parts.append(
        "\n方法 A 里还有一项我们没有跟进：Hunt 效应色度修正（`1.8814`/`1.4746` 在 Y'Cb'Cr' 域），"
        "它作用在色差上而非亮度。本案的等价物是 CIELAB 色相保持 + 可选饱和度（`docs/COLOR.md`），"
        "但两者不是同一件事，未做等价比对，记在 `docs/SOURCES.md` §5。"
    )
    return "\n".join(parts) + "\n"


def main():
    check = "--check" in sys.argv
    text = build()
    if check:
        if not DOC.exists():
            print(f"✗ {DOC.relative_to(ROOT)} 不存在", file=sys.stderr)
            return 1
        old = DOC.read_text(encoding="utf-8")
        if old != text:
            print("✗ docs/CURVE-REF.md 与工具输出不一致，请重跑 python3 scripts/report_curveref.py",
                  file=sys.stderr)
            return 1
        print("✓ docs/CURVE-REF.md 与工具输出一致")
        return 0
    DOC.write_text(text, encoding="utf-8")
    print(f"✓ 写入 {DOC.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
