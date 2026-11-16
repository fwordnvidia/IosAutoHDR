#!/usr/bin/env python3
# 从 nvn_mtl.m 里把上屏着色器的 MSL 字符串抽出来，写成真正的 .metal 文件，
# 好让 CI 用 xcrun metal 真编译一遍（ObjC 语法闸门看不到字符串里的 MSL）。
#
# 用法：extract_msl.py <nvn_mtl.m> [--dir 输出目录] [变量名 ...]
#   缺省抽 src_msl（8 位直通）与 src_msl_hdr（16F ITM + 色域 + 饱和度）。
#   给 --dir 时每个变量写一个独立 .metal（两个着色器都有 vs/fs，合成一个文件会重定义）；
#   不给就全部拼到 stdout，只供人看。
# 只做「找 C 字符串字面量 → 反转义」两件事；字符串两边的 // 注释会被忽略。
import re
import sys
import codecs

LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def extract(path, var):
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    head = 'static const char *%s =' % var
    start = None
    for i, ln in enumerate(lines):
        if head in ln:
            start = i
            break
    if start is None:
        raise SystemExit('找不到 %s' % head)
    out = []
    for ln in lines[start:]:
        for m in LIT.finditer(ln):
            out.append(codecs.decode(m.group(1), 'unicode_escape'))
        if ln.rstrip().endswith(';') and out:
            break
    return ''.join(out)


def main(argv):
    args = argv[1:]
    outdir = None
    if '--dir' in args:
        i = args.index('--dir')
        outdir = args[i + 1]
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    names = args[1:] or ['src_msl', 'src_msl_hdr']
    for n in names:
        text = extract(path, n)
        if outdir:
            import os
            os.makedirs(outdir, exist_ok=True)
            dst = os.path.join(outdir, n + '.metal')
            with open(dst, 'w', encoding='utf-8') as f:
                f.write(text if text.endswith('\n') else text + '\n')
            sys.stdout.write('%s: %d B\n' % (dst, len(text)))
        else:
            sys.stdout.write('// ===== %s (%d B) =====\n' % (n, len(text)))
            sys.stdout.write(text if text.endswith('\n') else text + '\n')


if __name__ == '__main__':
    main(sys.argv)
