#!/usr/bin/env python3
"""把 `run_all.sh` 里的每一个 `step "id 标题"` 列成索引，并核对索引没有腐烂。

为什么要有它：`run_all.sh` 有 51 个步骤，而 `docs/VERIFICATION.md` 是**人写的**叙述，
只挑了其中一部分讲透。于是出现过这样的事：某个闸门步骤存在、在跑、也在挡东西，
但没有任何一份文档提到它 —— 从文档读起来像是根本不存在。反过来，文档里也可能写着
已经删掉的步骤号（这类由 `check_docs.py` 第 4 条挡）。

判据：`--check` 时，`docs/STEPS.md` 必须与 `run_all.sh` 现在的 step 行逐条一致
（数量、顺序、id、标题）。不一致就打印差在哪，并提示重生命令。

用法：
    ./scripts/report_steps.py            # 重写 docs/STEPS.md
    ./scripts/report_steps.py --check    # 不一致就红（run_all.sh 里跑它）
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run_all.sh"
OUT = ROOT / "docs" / "STEPS.md"
STEP = re.compile(r'^step\s+"([0-9a-z]+/[0-9])\s+([^"]*)"', re.M)


def steps():
    return STEP.findall(RUN.read_text(encoding="utf-8"))


def render(rows):
    lines = [
        "# 闸门步骤索引",
        "",
        "由 `scripts/report_steps.py` 从 `run_all.sh` 的 `step` 行生成，**不要手改**：",
        "`run_all.sh` 的 `0w22/5` 会跑 `--check`，手改或忘记重生都会红。",
        "",
        "| 步骤 | 标题 | 叙述在 |",
        "| --- | --- | --- |",
    ]
    prose = {
        p.name: p.read_text(encoding="utf-8")
        for p in sorted((ROOT / "docs").glob("*.md"))
        if p.name != "STEPS.md"
    }
    prose["README.md"] = (ROOT / "README.md").read_text(encoding="utf-8")
    for sid, title in rows:
        where = [n for n, t in prose.items() if sid in t]
        lines.append("| `%s` | %s | %s |" % (sid, title.strip() or "—", "、".join(where) if where else "（仅索引）"))
    lines += ["", "共 %d 个步骤。" % len(rows), ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    rows = steps()
    if not rows:
        print("::error::run_all.sh 里一个 step 都没解析到 —— 格式变了？")
        return 1
    text = render(rows)
    if a.check:
        old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if old != text:
            print("::error::docs/STEPS.md 与 run_all.sh 的 step 行不一致 —— "
                  "有步骤加了/删了/改名了却没重生索引（./scripts/report_steps.py）")
            old_ids = re.findall(r"^\| `([0-9a-z]+/[0-9])`", old, re.M)
            new_ids = [s for s, _ in rows]
            if old_ids != new_ids:
                only_old = [i for i in old_ids if i not in new_ids]
                only_new = [i for i in new_ids if i not in old_ids]
                if only_new:
                    print("  · 索引里缺：%s" % "、".join(only_new))
                if only_old:
                    print("  · 索引里多出：%s" % "、".join(only_old))
            else:
                print("  · 步骤号一致，但标题或顺序变了")
            return 1
        print("✓ 步骤索引：%d 个步骤与 run_all.sh 一致" % len(rows))
        return 0
    OUT.write_text(text, encoding="utf-8")
    print("写出 %s（%d 个步骤）" % (OUT.relative_to(ROOT), len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
