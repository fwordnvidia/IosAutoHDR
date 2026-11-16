#!/usr/bin/env python3
"""断言「文档里写的基线」== patch/BASELINE（唯一真相源）。

为什么需要：基线迁移是手工动作（重放 3 个文件的改动），最容易漏的就是文档里某处还写着旧 sha。
写成断言后，改 patch/BASELINE 而没改文档（或反之）都会在 0w7/5 红。
无网络、无编译，纯文本。
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEX40 = re.compile(r'\b[0-9a-f]{40}\b')
fails = []

def read(p):
    f = ROOT / p
    return f.read_text(encoding='utf-8') if f.exists() else None

base_raw = read('patch/BASELINE')
if base_raw is None:
    print('::error::patch/BASELINE 不存在'); sys.exit(1)
lines = [l for l in base_raw.splitlines() if l.strip() and not l.startswith('#')]
if not lines:
    print('::error::patch/BASELINE 里没有 sha'); sys.exit(1)
parts = lines[0].split()
BASE = parts[0]
DATE = parts[1] if len(parts) > 1 else ''
if not HEX40.fullmatch(BASE):
    print(f'::error::patch/BASELINE 第一行不是 40 位 sha：{BASE}'); sys.exit(1)
if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', DATE):
    print(f'::error::patch/BASELINE 缺少 YYYY-MM-DD 日期（跟在 sha 后面）'); sys.exit(1)

# 1) README 的「目标基线」行
txt = read('README.md')
if txt is None:
    fails.append('README.md 不存在')
else:
    hit = [l for l in txt.splitlines() if '目标基线' in l]
    if not hit:
        fails.append('README.md 里找不到「目标基线」一行')
    else:
        for l in hit:
            if BASE not in l:
                fails.append(f'README.md 的「目标基线」行没写 {BASE[:12]}：{l.strip()[:70]}')

# 2) docs/PR.md 的「基于 main 的 <sha>」
txt = read('docs/PR.md')
if txt is None:
    fails.append('docs/PR.md 不存在')
else:
    hit = [l for l in txt.splitlines() if '基于 `main` 的' in l]
    if not hit:
        fails.append('docs/PR.md 里找不到「基于 `main` 的 <sha>」一行')
    else:
        for l in hit:
            if BASE not in l:
                fails.append(f'docs/PR.md 的基线行没写 {BASE[:12]}：{l.strip()[:70]}')

# 3) 任何提到「基线」的行里的 40 位 sha 都必须等于 BASE（历史叙述行豁免）
EXEMPT = ('历史', '实测通过', '更早', '此前', '曾经')
for p in ['README.md', 'docs/PR.md', 'docs/VERIFICATION.md', 'docs/DESIGN.md']:
    txt = read(p)
    if txt is None:
        continue
    for i, l in enumerate(txt.splitlines(), 1):
        if '基线' not in l or any(k in l for k in EXEMPT):
            continue
        for sha in HEX40.findall(l):
            if sha != BASE:
                fails.append(f'{p}:{i} 提到基线却写旧 sha {sha[:12]}（应为 {BASE[:12]}）')

# 4) 本地补丁的树必须就是基线：src.manifest 存在即说明 make_patch 在某个树上跑过，
#    这里只断言补丁文件存在且非空（基线是否匹配由 verify_upstream.sh 真正验）
p = ROOT / 'patch/hdr-auto-l1.patch'
if not p.exists() or p.stat().st_size < 10000:
    fails.append('patch/hdr-auto-l1.patch 缺失或异常小')


# 5) patch/ 下只允许三件东西：主补丁、BASELINE、src.manifest。
#    候选补丁（hdr-auto-l1.<sha>.patch）是 rebase_patch.sh 的临时产物，绝不能留在仓库里被当成主补丁。
allowed = {'hdr-auto-l1.patch', 'BASELINE', 'src.manifest'}
extra = sorted(p.name for p in (ROOT / 'patch').iterdir() if p.name not in allowed)
if extra:
    fails.append('patch/ 下有非预期文件（候选补丁用完要删）：' + '、'.join(extra))

if fails:
    for f in fails:
        print(f'::error::{f}')
    print(f'✗ 基线一致性：{len(fails)} 处不一致')
    sys.exit(1)
print(f'✓ 基线一致：{BASE[:12]}（{DATE}），README/PR 与 patch/BASELINE 对齐')
