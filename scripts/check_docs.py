#!/usr/bin/env python3
"""README 里承诺能跑的东西必须真的存在，且用户会直接跑的脚本必须出现在 README。

起因（真事）：round 4 往 README 插「上游前进之后（rebase_patch.sh）」一节，用的锚点字符串在
README 里并不存在 ⇒ `str.replace` 什么也没替换，脚本却照常打印「已加」并退出 0。
那一节的缺失一路带到公开仓库：README 说「见 rebase_patch.sh」，读者翻遍 README 找不到。
所以任何「文档里提到 X」都必须有断言，断言写进闸门。

检查三件事：
  1. README 提到的每个 scripts/*.sh、scripts/*.py、tools/*.c 都真实存在
  2. 用户会直接跑的入口脚本必须在 README 出现（加了脚本却没人知道怎么用，等于没加）
  3. README 引用的 docs/*.md 与 patch/* 真实存在
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
readme = (ROOT / 'README.md').read_text(encoding='utf-8')
fails = []

# 1) README 里出现的路径必须存在
refs = set(re.findall(r'(?:scripts|tools|docs|patch)/[A-Za-z0-9_.\-]+\.(?:sh|py|c|md|patch)\b', readme))
for r in sorted(refs):
    if not (ROOT / r).exists():
        fails.append(f'README 提到 {r}，但文件不存在')

# 2) 入口脚本必须在 README 出现（这些是使用者直接敲的）
# 使用者在本仓库（hdr-auto 子项目 / 公开投影）里会直接敲的入口。
# 仓库级脚本（mirror_vultr.sh、check_repo_hygiene.sh）住在完整工作树的 scripts/，不进投影，不在本表。
ENTRY = ['rebase_patch.sh', 'verify_upstream.sh', 'baseline_report.sh', 'make_patch.sh',
         'sanitize.sh', 'check_projection.sh', 'publish.sh', 'sync_release.sh', 'check_callsites.py', 'check_device_coverage.py',
         'check_device_log.py', 'check_msl.py']
for e in ENTRY:
    if e not in readme:
        fails.append(f'入口脚本 {e} 没在 README 出现（加了没人知道怎么用）')
    if not ((ROOT / 'scripts' / e).exists()):
        fails.append(f'README 承诺的 scripts/{e} 不存在')

# 3) README 里的行内命令块引用的脚本（`./scripts/xxx`）必须存在
for m in set(re.findall(r'\./scripts/([A-Za-z0-9_.\-]+)', readme)):
    if not (ROOT / 'scripts' / m).exists():
        fails.append(f'README 的示例命令用了 scripts/{m}，文件不存在')

if fails:
    for f in fails:
        print(f'::error::{f}')
    print(f'✗ README 与仓库不一致：{len(fails)} 处')
    sys.exit(1)
print(f'✓ README 与仓库一致（引用 {len(refs)} 个路径、{len(ENTRY)} 个入口脚本全在）')
