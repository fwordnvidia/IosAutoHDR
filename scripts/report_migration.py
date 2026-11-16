#!/usr/bin/env python3
"""把 `patch` 的 .rej 变成给人看的工单：每个失败 hunk 一节——上游现在这段是什么、我们原本要插什么。

为什么要脚本：上游每重写一次 present 路径，我们的 hunk 就会冲突一次。手抄 .rej 到文档
既慢又会漂移（写完就和 .rej 不同步）。这里直接从 .rej 原文与目标树现场取文本。

用法：scripts/report_migration.py <树> <rej文件>... [-o docs/BASELINE-MIGRATION.md]
"""
import re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
args = [a for a in sys.argv[1:]]
out = ROOT / 'docs/BASELINE-MIGRATION.md'
if '-o' in args:
    i = args.index('-o'); out = Path(args[i+1]); del args[i:i+2]
tree = Path(args[0]); rejs = [Path(a) for a in args[1:]]

def sh(*cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=60).stdout
    except Exception:
        return ''

sha = sh('git', '-C', str(tree), 'rev-parse', 'HEAD').strip()
sha12 = sha[:12] or '?'
trip = sh('git', '-C', str(tree), 'log', '-1', '--format=%cI %s').strip()

L = []
L.append('# 基线迁移工单（自动生成）\n')
L.append(f'目标上游提交：`{sha or "?"}`（{trip or "?"}）\n')
L.append('本文件由 `./scripts/rebase_patch.sh <sha>` 在重放失败时自动生成，内容是 **`patch` 拒绝的每一个 hunk**：')
L.append('每条给出「我们的上下文 / 我们要插入的行 / 上游现在这段是什么」。按单子逐条手工合并，')
L.append('再把这个提交写进 `patch/BASELINE`。**不要**用 `patch --fuzz` 硬塞——上下文对不上说明代码结构变了。\n')
L.append(f'\n共 {len(rejs)} 个文件冲突。\n')

for rej in rejs:
    rel = rej.as_posix()
    m = re.search(r'(?:^|/)(graphics/.+?)(?:\.rej)$', rel)
    relpath = m.group(1) if m else rel.replace('.rej', '')
    L.append(f'\n## `{relpath}`\n')
    txt = rej.read_text(encoding='utf-8', errors='replace')
    # 按 hunk 切
    hunks = re.split(r'(?m)^(?=@@ )', txt)
    for h in hunks:
        if not h.startswith('@@'):
            continue
        head = h.splitlines()[0]
        mm = re.match(r'@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@(.*)', head)
        if not mm:
            continue
        lineno = int(mm.group(1)); ctx = mm.group(2).strip()
        ours_ctx, ours_add, ours_del = [], [], []
        for line in h.splitlines()[1:]:
            if line.startswith(' '):
                ours_ctx.append(line[1:])
            elif line.startswith('+'):
                ours_add.append(line[1:])
            elif line.startswith('-'):
                ours_del.append(line[1:])
        # .rej 里记的是「旧树的行号」。上游结构一变这个数字就骗人，所以先用 hunk 的第一条
        # 上下文行在新树里 grep 定位；找不到才退回旧行号，并明确标注「行号来自旧树」。
        anchor = next((c.strip() for c in ours_ctx if len(c.strip()) >= 20), '')
        located = None
        if anchor:
            hit = sh('grep', '-nF', '-m1', '--', anchor, str(tree / relpath), cwd=str(tree)).split(':')
            if hit and hit[0].isdigit():
                located = int(hit[0])
        real = located or lineno
        how = '' if located else '（行号来自旧树，新树里没搜到上下文锚点）'
        L.append(f'### 上游第 {real} 行附近' + (f' — `{ctx[:90]}`' if ctx else '') + how + '\n')
        L.append('我们原本要插入/删除（`+` 是要加进去的）：')
        L.append('```diff')
        for line in ours_add[:40]:
            L.append('+' + line)
        for line in ours_del[:20]:
            L.append('-' + line)
        L.append('```')
        # 上游当前该位置附近
        lo, hi = max(1, real - 8), real + 14
        now = sh('sed', '-n', f'{lo},{hi}p', str(tree / relpath), cwd=str(tree))
        if now.strip():
            L.append(f'上游现在 {lo}–{hi} 行：')
            L.append('```objc')
            L.extend(now.rstrip('\n').splitlines())
            L.append('```')
        else:
            L.append('（目标树里读不到该路径——可能上游把这个文件改名或搬走了）')
        L.append('')

out.write_text('\n'.join(L) + '\n', encoding='utf-8')
print(f'写出 {out}（{len(L)} 行，{len(rejs)} 个冲突文件）')
