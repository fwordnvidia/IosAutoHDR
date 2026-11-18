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
import os, re, subprocess, sys
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
         'check_issue_form.py', 'check_hdr_hooks.py', 'check_ci_result.py', 'dispatch_ci.sh', 'report_steps.py',
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

# 6) NOTICE（许可与出处声明）里的提交号必须等于 patch/BASELINE 的第一列。
#    踩过的实例：NOTICE 一直写 v0.4.0 的 `dcd05e5fa84c`，而基线早迁到 `d5cbf7ff53f7`
#    （2026-11-17）—— 许可文件把下游指到一个错误的补丁适用版本上，这比文档里写错一个
#    步骤号严重：它是分发时的依据。基线只在 patch/BASELINE 里定义一处，别处引用的提交号
#    一律拿它比。
_base_sha = (ROOT / 'patch' / 'BASELINE').read_text(encoding='utf-8').split()[0]
_notice = (ROOT / 'NOTICE').read_text(encoding='utf-8') if (ROOT / 'NOTICE').exists() else ''
for _m in re.finditer(r'\b([0-9a-f]{7,40})\b', _notice):
    _h = _m.group(1)
    if not _base_sha.startswith(_h) and not _h.startswith(_base_sha[:len(_h)]):
        fails.append(f'NOTICE 里出现的提交 {_h} 不是 patch/BASELINE 的基线（{_base_sha[:12]}…）')
if 'jhaizhou-ops/S2Arm' not in _notice:
    fails.append('NOTICE 没写上游出处（jhaizhou-ops/S2Arm）')

# 7) README 里凡是声称「用 xcrun/metal 真编译过」的 ✅ 行，必须点明编的是哪个 .metal 文件。
#    踩过的实例：状态表里「MSL 语法 ✅ CI hdr-check 用 xcrun … 真编译」与下一行
#    「MSL 编译 ⚠️ 只编过两处…HDR fragment 从未过 xcrun metal」在同一张表里互相打脸，
#    读者只会记住那个 ✅。要么点名文件，要么别打 ✅。
for _i, _line in enumerate(readme.split('\n'), 1):
    if 'xcrun' in _line and '✅' in _line:
        # 只认「xcrun 之后点名的 .metal」：行首写 `hdr_hist.metal` 表格列名不算，
        # 因为那正是原来那行的样子 —— 列名写着 MSL 语法、内容说的是一次编译。
        if '.metal' not in _line.split('xcrun', 1)[1]:
            fails.append(f'README.md:{_i} 用 ✅ 声称 xcrun 真编译，但没点明编的是哪个 .metal 文件')

# 8) 文档里凡写「N 项自测 / N 项合成日志自测 / --selftest N 项」，必须等于判读脚本现算的用例数。
#    踩过的实例：`--selftest` 从 8 项长到 15 项（加过 16F 正例、缺跟随行、显式 headroom 三个用例），
#    而 docs/PR.md 与 report_review.py 里各写着一处「8 项」，生成物 REVIEW.md 也跟着错 ——
#    两处手写的数字，没人改。数字要么现算，要么不写。
#    「现算」= 真去跑一次 --selftest 并读它自报的项数：数夹具条目会数错（脚本里还内联了
#    「logcat 前缀」那条不进 SELFTEST_CASES 的用例），也不该让文档守卫复制一份计数逻辑。
_out = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'check_device_log.py'), '--selftest'],
                      capture_output=True, text=True)
_m = re.search(r'自测 (\d+) 项', _out.stdout)
if not _m:
    fails.append('scripts/check_device_log.py --selftest 没跑出「自测 N 项」（判读脚本坏了？）')
    _self_cases = -1
else:
    _self_cases = int(_m.group(1))
for _md in [ROOT / 'README.md'] + sorted((ROOT / 'docs').glob('*.md')):
    for _i, _line in enumerate(_md.read_text(encoding='utf-8').split('\n'), 1):
        # 只在「说的是 check_device_log」的行上认这个数字：别的脚本也有 --selftest（判据项数不同），
        # 一律按 device_log 的项数去要求它，会逼着人写模糊话。
        if 'check_device_log' not in _line:
            continue
        for _m in re.finditer(r'(\d+)\s*项(?:合成日志)?自测|--selftest[^0-9\n]{0,16}(\d+)\s*项', _line):
            _n = int(_m.group(1) or _m.group(2))
            if _n != _self_cases:
                fails.append(f'{_md.name}:{_i} 写 {_n} 项自测，而 check_device_log.py 现有 '
                             f'{_self_cases} 个用例 —— 数字要么现算，要么别写死')

# 9) 文档让人敲的 `./scripts/x.py` / `./tools/x.py`，必须真的有可执行位。
#    踩过的实例：`check_hdr_hooks.py` 建的时候漏了 `chmod +x` —— README 里写着
#    `./scripts/check_hdr_hooks.py`，用户照着敲得到 `权限不够`，而 run_all 里写的是
#    `python3 scripts/…`，所以闸门一路绿。文档里的命令必须能原样跑。
_exec_refs = {}
for _md in [ROOT / 'README.md'] + sorted((ROOT / 'docs').glob('*.md')):
    for _m in re.finditer(r'\./(scripts|tools)/([A-Za-z0-9_.\-]+)', _md.read_text(encoding='utf-8')):
        _f = ROOT / _m.group(1) / _m.group(2)
        if _f.exists() and not _f.stem == '':
            _exec_refs.setdefault(str(_f.relative_to(ROOT)), _md.name)
for _rel, _mdname in sorted(_exec_refs.items()):
    if not os.access(ROOT / _rel, os.X_OK):
        fails.append(f'{_mdname} 让你敲 ./{_rel}，但它没有可执行位（chmod +x {_rel}）')

# 10) 文档里写「文件:行号 符号()」时，那个符号必须真在那一行附近。
#     踩过的实例：docs/PR.md 写 `nvn_mtl.m:16311 hdr_head_tick()`，而 `hdr_head_tick()`
#     在两个版本里挪到了 17631 —— 行号是当初写文档时抄的，之后代码长了 1300 行没人回头改。
#     审阅者点过去看到的是无关代码，而这类引用恰恰是「文档里最像证据的东西」。
#     容差 ±10 行（插入/删除几行注释不该判红）；找不到该文件时打印跳过，不猜。
_MISS = 10
for _md in [ROOT / 'README.md'] + sorted((ROOT / 'docs').glob('*.md')):
    _text = _md.read_text(encoding='utf-8')
    for _i, _line in enumerate(_text.split('\n'), 1):
        for _m in re.finditer(r'([A-Za-z0-9_./\-]+\.(?:m|h|c|inc|metal|py|sh)):(\d+)\s+([A-Za-z_]\w*)\(\)', _line):
            _rel, _ln, _sym = _m.group(1), int(_m.group(2)), _m.group(3)
            _cand = [ROOT / _rel] + [p for p in ROOT.rglob(_rel.split('/')[-1]) if p.is_file()]
            _hit = next((p for p in _cand if p.is_file()), None)
            if _hit is None:
                print(f'  · {_md.name}:{_i} 引用了 {_rel}，本树里没有（投影形态只有补丁）⇒ 跳过行号核对')
                continue
            _lines = _hit.read_text(encoding='utf-8', errors='replace').split('\n')
            _lo, _hi = max(1, _ln - _MISS), min(len(_lines), _ln + _MISS)
            if not any(re.search(r'\b%s\s*\(' % re.escape(_sym), _lines[k - 1]) for k in range(_lo, _hi + 1)):
                _real = [k for k, t in enumerate(_lines, 1) if re.search(r'\b%s\s*\(' % re.escape(_sym), t)]
                _tip = f'；它现在在 {_rel}:{_real[0]}' if _real else '；整份文件里也没找到这个符号'
                fails.append(f'{_md.name}:{_i} 写 {_rel}:{_ln} {_sym}()，但那附近没有它'
                             f'（文件共 {len(_lines)} 行）{_tip}')

# 11) 文档里写「N 条不变量」时，N 必须等于 check_hdr_hooks.py 现算的条数（它自己在收尾行报）。
#     踩过的实例：H7 加上之后脚本报 7 条，而 docs/DESIGN.md 还写着「`0w20/5` 的 6 条不变量」。
#     与第 8 条（自测项数）同一类：手写的「几条」迟早会和代码分家。
_hk = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'check_hdr_hooks.py')],
                     capture_output=True, text=True)
_hm = re.search(r'（(\d+) 条）', _hk.stdout)
if _hm:
    _hook_n = int(_hm.group(1))
    for _md in [ROOT / 'README.md'] + sorted((ROOT / 'docs').glob('*.md')):
        for _i, _line in enumerate(_md.read_text(encoding='utf-8').split('\n'), 1):
            for _m in re.finditer(r'(\d+)\s*条(?:结构)?不变量', _line):
                if int(_m.group(1)) != _hook_n:
                    fails.append(f'{_md.name}:{_i} 写 {_m.group(1)} 条不变量，而 check_hdr_hooks.py 现在 '
                                 f'报 {_hook_n} 条 —— 数字要么现算，要么别写死')

if fails:
    for f in fails:
        print(f'::error::{f}')
    print(f'✗ README 与仓库不一致：{len(fails)} 处')
    sys.exit(1)
print(f'✓ README 与仓库一致（引用 {len(refs)} 个路径、{len(ENTRY)} 个入口脚本全在）')
