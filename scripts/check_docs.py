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
         'sanitize.sh', 'check_projection.sh', 'publish.sh', 'sync_release.sh', 'check_callsites.py', 'check_device_coverage.py', 'report_fi_domain.py', 'check_format_assumptions.py',
         'check_patch_symbols.py', 'check_msl_symbols.py', 'check_patch_fresh.py', 'check_patch_tools.sh', 'check_github.sh',
         'check_device_log.py', 'check_msl.py', 'fetch_device_log.sh', 'check_shell_helpers.py', 'check_c_warnings.sh', 'check_py_lint.sh', 'rehearse_ci.py']
for e in ENTRY:
    if e not in readme:
        fails.append(f'入口脚本 {e} 没在 README 出现（加了没人知道怎么用）')
    if not ((ROOT / 'scripts' / e).exists()):
        fails.append(f'README 承诺的 scripts/{e} 不存在')

# 3) README 里的行内命令块引用的脚本（`./scripts/xxx`）必须存在
for m in set(re.findall(r'\./scripts/([A-Za-z0-9_.\-]+)', readme)):
    if not (ROOT / 'scripts' / m).exists():
        fails.append(f'README 的示例命令用了 scripts/{m}，文件不存在')

# 4) 文档里引用的步骤号（`0w15/5` 这类）必须真的存在于 run_all.sh。
#    踩过的实例：`3b/5` 与 `0w13/5` 在 VERIFICATION.md 里挂了很久 —— 前者那个步骤早就
#    不存在了（「平台阶」现在并进 0d/5 与 0l/5），后者上一轮改名成 `4b/5` 后文档没跟着改。
#    读者按文档核步骤时会找不到，而没有任何检查发现过这件事。
_real_steps = set(re.findall(r'step "([0-9a-z]+/5)', (ROOT / 'run_all.sh').read_text(encoding='utf-8')))
for _md in sorted((ROOT / 'docs').glob('*.md')):
    for _m in re.finditer(r'`([0-9a-z]+/5)`', _md.read_text(encoding='utf-8')):
        if _m.group(1) not in _real_steps:
            fails.append(f'{_md.name} 引用了不存在的步骤 {_m.group(1)}（run_all.sh 里没有这个 step）')

# 5) 提到 `check_projection.sh` 的地方，如果写了组数（「11 组检查」/「第 9 组」），必须与
#    脚本里真实的组数一致。踩过的实例：GITHUB-COMPLIANCE.md 一直写「一条命令，9 组检查」，
#    而脚本早就加到 11 组 —— 合规文档自己报的数不准，读者按它去核对时会以为少跑了两组。
#    这里只认「N 组检查/自检」与同一行里的「第 N 组」，因为别处的「第 13 组」指的是
#    C 单测内部的断言分组，与投影自检无关。
_proj = (ROOT / 'scripts' / 'check_projection.sh').read_text(encoding='utf-8')
_n_groups = len(re.findall(r"^\s*say ['\"]\d+\)", _proj, re.M))
for _md in [ROOT / 'README.md'] + sorted((ROOT / 'docs').glob('*.md')):
    for _i, _line in enumerate(_md.read_text(encoding='utf-8').split('\n'), 1):
        if 'check_projection.sh' not in _line and '投影自检' not in _line:
            continue
        for _m in re.finditer(r'(\d+)\s*组(?:检查|自检)', _line):
            if int(_m.group(1)) != _n_groups:
                fails.append(f'{_md.name}:{_i} 说投影自检有 {_m.group(1)} 组，实际 {_n_groups} 组')
        for _m in re.finditer(r'第\s*(\d+)\s*组', _line):
            if int(_m.group(1)) > _n_groups:
                fails.append(f'{_md.name}:{_i} 引用第 {_m.group(1)} 组，但投影自检只有 {_n_groups} 组')

if fails:
    for f in fails:
        print(f'::error::{f}')
    print(f'✗ README 与仓库不一致：{len(fails)} 处')
    sys.exit(1)
print(f'✓ README 与仓库一致（引用 {len(refs)} 个路径、{len(ENTRY)} 个入口脚本全在）')
