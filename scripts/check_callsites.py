#!/usr/bin/env python3
"""对 ObjC/C 源码做「定义与调用点的实参个数」对账。

为什么需要（真事）：上游给 `present_blit` / `present_blit_m4` 加了第 6 个参数 `int screen`，
`patch` 只把冲突报在**定义**附近；调用点一个都没报，但漏一个就编译不过。这里没有 macOS、
没有 Xcode，`clang -fsyntax-only` 也跑不了（缺 Metal 头），所以只能自己数。

能抓到的：参数个数不符、函数改名后残留旧调用。
抓不到的：类型不符、重载/宏展开、函数指针赋值（这些会显式跳过，不猜）。

用法：scripts/check_callsites.py <file.m|.c> [...]   （默认检查 src/nvn_mtl.m + src/nvn_present_fi.inc）
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT = ['src/nvn_mtl.m', 'src/nvn_present_fi.inc', 'src/host_adapt.m']

def strip_comments(s):
    """去注释但保留字符串与字符字面量（字符串里的 // 不是注释）。"""
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            q = c; out.append(c); i += 1
            while i < n:
                if s[i] == '\\': out.append(s[i:i+2]); i += 2; continue
                out.append(s[i]); i += 1
                if s[i-1] == q: break
            continue
        if c == '/' and i+1 < n and s[i+1] == '/':
            while i < n and s[i] != '\n': i += 1
            continue
        if c == '/' and i+1 < n and s[i+1] == '*':
            j = s.find('*/', i+2); i = n if j < 0 else j+2
            out.append(' ')
            continue
        out.append(c); i += 1
    return ''.join(out)

def match_close(s, i, open_ch, close_ch):
    """s[i] 是 open_ch，返回配对 close_ch 的下标。"""
    depth = 0
    while i < len(s):
        c = s[i]
        if c == '"' or c == "'":
            q = c; i += 1
            while i < len(s):
                if s[i] == '\\': i += 2; continue
                if s[i] == q: break
                i += 1
        elif c == open_ch: depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0: return i
        i += 1
    return -1

def split_args(argstr):
    """按顶层逗号切实参；^块 也算一层括号。"""
    if not argstr.strip(): return []
    args, depth, cur, i = [], 0, [], 0
    while i < len(argstr):
        c = argstr[i]
        if c == '"' or c == "'":
            q = c; cur.append(c); i += 1
            while i < len(argstr):
                cur.append(argstr[i])
                if argstr[i] == '\\': cur.append(argstr[i+1]); i += 2; continue
                if argstr[i] == q: i += 1; break
                i += 1
            continue
        if c in '([': depth += 1
        elif c in ')]': depth -= 1
        elif c == '{': depth += 1
        elif c == '}': depth -= 1
        elif c == ',' and depth == 0:
            args.append(''.join(cur)); cur = []; i += 1; continue
        cur.append(c); i += 1
    args.append(''.join(cur))
    return [a.strip() for a in args if a.strip()]

DEF_RE = re.compile(r'(?m)^(?:(?:static|API_AVAILABLE\([^)]*\)|extern|inline)\s+)*[\w\s\*<>,\[\]]*?\b(\w+)\s*\(')

def collect_defs(src, only=None):
    """跨行找定义：name(args) 之后紧跟 { 的才是定义（不是声明/调用）。

    `only` 是白名单：**只检查被点名的函数**。全文件扫描会把 `ms(` / `draw(` 这类短名
    的同名冲突全算进来（实测 156 处误报，全是从别的库/局部变量来的），所以默认走白名单，
    白名单装的是上屏链路与 HDR 那几个「改签名就会漏参」的函数。
    """
    defs = {}
    for m in re.finditer(r'(?m)^(?:static|API_AVAILABLE\([^)]*\))\s', src):
        start = m.start()
        lp = src.find('(', start)
        if lp < 0: continue
        rp = match_close(src, lp, '(', ')')
        if rp < 0: continue
        head = src[start:lp]
        nm = re.search(r'(\w+)\s*$', head)
        if not nm: continue
        name = nm.group(1)
        # 定义 = 参数表之后（跳过 __attribute__ 之类）出现 '{'
        tail = src[rp+1:rp+200]
        brace = tail.find('{')
        semi = tail.find(';')
        if brace < 0 or (semi >= 0 and semi < brace): continue
        params = split_args(src[lp+1:rp])
        if params == ['void']: params = []
        if only and name not in only: continue
        defs[name] = len(params)
    return defs

def scan_calls(path, defs):
    raw = path.read_text(encoding='utf-8', errors='replace')
    src = strip_comments(raw)
    fails = []
    checked = 0
    for name, want in defs.items():
        for m in re.finditer(r'(?<![\w.>])' + re.escape(name) + r'\s*\(', src):
            i = m.start()
            line = src.count('\n', 0, i) + 1
            # 跳过定义本身
            lp = src.find('(', m.start())
            rp = match_close(src, lp, '(', ')')
            if rp < 0: continue
            tail = src[rp+1:rp+200]
            b, s2 = tail.find('{'), tail.find(';')
            if b >= 0 and (s2 < 0 or b < s2) and src[m.start():lp].strip().startswith(('static', 'API_AVAILABLE')):
                continue
            # 取实参
            args = split_args(src[lp+1:rp])
            if len(args) == 1 and args[0] in ('void',): args = []
            # 函数指针赋值 / 取地址：跳过
            before = src[max(0, i-40):i]
            if re.search(r'=\s*&?\s*$', before) or '&' + name in before:
                continue
            checked += 1
            if len(args) != want:
                fails.append(f'{path.name}:{line} 调用 {name}(…) 给了 {len(args)} 个实参，定义要 {want} 个')
    return fails, checked

WHITELIST = {
    'present_blit', 'present_blit_m4', 'present_encode_slot', 'present_encode_m4', 'present_encode',
    'present_pipeline', 'hdr_head_tick', 'hdr_hist_tick', 'present_fx_encode', 'present_aa_encode',
    'present_aa_prepare', 'present_raw_src', 'present_options', 'present_fx_for', 'rs_aa_crop',
    'fi_pass', 'fi_commit', 'fi_get', 'fi_tex', 'fi_ticket_done', 'fi_inflight_add', 'fi_may_take_drawable',
}
argv = [a for a in sys.argv[1:]]
only = set(WHITELIST)
if '--all' in argv: only = None; argv.remove('--all')
paths = [ROOT / p for p in (argv or DEFAULT)]
present = [p for p in paths if p.exists()]
# 定义表必须**跨文件合并**：`present_blit_m4` 定义在 nvn_mtl.m，但 nvn_present_fi.inc 里也在调用。
# 第一版按文件各查各的，结果 FI 里少传 screen 完全查不出来（负向实测抓到）。
defs = {}
for p in present:
    d = collect_defs(strip_comments(p.read_text(encoding='utf-8', errors='replace')), only)
    for k, v in d.items():
        if k in defs and defs[k] != v:
            print(f'::error::{k} 在两处定义里参数个数不同（{defs[k]} vs {v}）')
        defs[k] = v
total_defs = len(defs); total_calls = 0
allfails = []
for p in present:
    fails, checked = scan_calls(p, defs)
    total_calls += checked
    allfails += fails

for f in allfails:
    print(f'::error::{f}')
print(f'· 定义 {total_defs} 个、调用点 {total_calls} 个')
# 定点回归：这几个调用点在 75eb4302 合并时全靠人工补第 6 个实参（patch 不报调用点）。
# 数对了还不够——必须确实是 screen=1（画进画布）那条分支，否则画幅/中间图会错。
SPOT = [
    ('src/nvn_present_fi.inc', 'present_blit_m4(cb, mid, dm.texture, NULL, f->uv[2], f->tab[2], flip, 1)', 1),
    ('src/nvn_present_fi.inc', 'present_blit_m4(cb, src, dr.texture, crop, f->uv[3], f->tab[3], flip, 1)', 1),
    ('src/nvn_mtl.m', 'present_blit(cb, src, dst, crop, flip, 1)', 1),
    ('src/nvn_mtl.m', 'present_blit(cb, src, mirror, crop, flip, 1)', 1),
    ('src/nvn_mtl.m', 'present_blit(cb, src, s->input, crop, flip, 0)', 1),
    ('src/nvn_mtl.m', 'present_blit_m4(cb, src, dst, crop, g_present_uv[slot], g_present_args, flip, 1)', 1),
    ('src/nvn_mtl.m', 'present_blit_m4(cb, src, s->input, crop, s->cropUV, s->cropTable, flip, 0)', 1),
    # ios/play_reflection.m：镜像/反射层跟着游戏层的格式与色彩空间走（上游 11-13），EDR 开关不跟 ⇒
    #   HDR 下这块层按 SDR 合成、镜像里 >1 的高光被钳。这一行是我们补的，必须还在。
    ('src/play_reflection.m', 'if (m.wantsExtendedDynamicRangeContent!=game.wantsExtendedDynamicRangeContent)', 1),
]
for rel, needle, want in SPOT:
    f = ROOT / rel
    if not f.exists():
        print(f'::error::定点回归缺文件 {rel}'); allfails.append(rel); continue
    n = f.read_text(encoding='utf-8', errors='replace').count(needle)
    if n != want:
        print(f'::error::{rel} 里 `{needle[:52]}…` 出现 {n} 次，应为 {want} 次（screen 实参或分支被人改/丢了）')
        allfails.append(f'{rel}:{needle[:24]}')

if allfails:
    print(f'✗ 实参个数不符：{len(allfails)} 处'); sys.exit(1)
print('✓ 所有调用点的实参个数与定义一致')
