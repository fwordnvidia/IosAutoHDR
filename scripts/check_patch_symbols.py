#!/usr/bin/env python3
"""check_patch_symbols.py：补丁改动过的文件里，每个自定义前缀符号都必须真有定义。

为什么需要：Linux 侧**从不编译** ObjC 与 MSL。`nvn_mtl.m` 我们手工合并了四处
（`#include "nvn_hdr_hist.inc"`、`hdr_hist_tick(cb, src);`、`hdr_dst` 块、`if (!dr)`
降级/回头路块），`nvn_hdr_hist.inc` 是整块设备专属代码 —— 这两处**任何拼写错误**
在 `./run_all.sh`、`consumer_test.sh`、`check_msl.py`、C 单测里都不会现形：
它们只在 macOS 的 `clang -fsyntax-only`（CI）、或真机运行时才炸。

能在这里做的最近似检查：把「用到的名字」与「有定义的名字」对账。
  - 用到的名字：补丁触及的 41 个 C 系文件里，代码部分（剥掉注释与字符串）出现的
    `s2a_*` / `nvn_hdr_*` / `hdr_*` 标识符。注释与字符串必须剥掉：`hdr_hist.metal`
    这种**文件名**写在注释里，不剥就会当成符号。
  - 有定义的名字：全树（`graphics runtime tools compat src`）里出现在**定义位置**的名字
    —— 行首类型 + `NAME(`、`static … NAME;`、`#define NAME`、`} NAME;`、`typedef … NAME;`
    —— 加上被改文件在 `HEAD` 版本里的所有声明（上游部分）。
  - 判据：用到而既无定义也无上游声明 ⇒ 红。典型就是「调用点多打一个字母」或
    「定义改了名但调用点没跟」。

为什么不能只看「树里出现过 `NAME(`」：那样只要我们自己调了一次，就自动算作已声明，
守卫永远绿。定义位置（行首类型）才是判据。

用法：check_patch_symbols.py <打补丁后的树根>
"""
import collections
import os
import re
import subprocess
import sys

CEXT = (".c", ".h", ".m", ".mm", ".inc", ".metal", ".cpp", ".hpp")
IDENT = re.compile(r"\b(s2a_[A-Za-z0-9_]+|nvn_hdr_[A-Za-z0-9_]+|hdr_[A-Za-z0-9_]+)\b")
NOT_STMT = r"(?!return\b|if\b|for\b|while\b|switch\b|case\b|else\b|do\b|\})"
FUNC_DEF = re.compile(r"^\s*" + NOT_STMT + r"[A-Za-z_][\w:<>,\s\*]*?\b"
                      r"(s2a_\w+|nvn_hdr_\w+|hdr_\w+)\s*\(")
VAR_DEF = re.compile(r"^\s*(?:static\s+|extern\s+|const\s+)*[A-Za-z_][\w:<>,\s\*]*?\b"
                     r"(s2a_\w+|nvn_hdr_\w+|hdr_\w+)\s*(?:=|;|\[)")
PREFIXES = (r"(s2a_\w+|nvn_hdr_\w+|hdr_\w+)")
ANY_DECL = [
    re.compile(r"#\s*define\s+" + PREFIXES),
    re.compile(r"\}\s*" + PREFIXES + r"\s*;"),
    re.compile(r"typedef[^;]*?\b" + PREFIXES + r"\s*;"),
    re.compile(r"\b(?:struct|union|enum)\s+" + PREFIXES + r"\s*[{;]"),
    re.compile(r"(?<![.>\w])" + PREFIXES + r"\s*\("),
    re.compile(r"\b(?:static|extern|const|unsigned|signed)\b[^;(){}]*?\b"
               + PREFIXES + r"\b\s*(?:=|;|\[|,)"),
]


def strip_code(text):
    """去掉注释与字符串/字符字面量，只留代码（保留换行，行号才对得上）。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n:
            d = text[i + 1]
            if d == "/":
                j = text.find("\n", i)
                i = n if j < 0 else j
                continue
            if d == "*":
                j = text.find("*/", i + 2)
                i = n if j < 0 else j + 2
                out.append("\n" * text.count("\n", i, i + 1))
                continue
        if c in "\"'":
            q = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            out.append(" ")
            continue
        out.append(c)
        i += 1
    return "".join(out)


def any_decls(code):
    out = set()
    for rx in ANY_DECL:
        out.update(rx.findall(code))
    return out


def tree_defs(code):
    """只在定义位置出现的名字。调用点不算 —— 否则守卫是自指的。"""
    out = set()
    for line in code.splitlines():
        s = line.strip()
        if not s:
            continue
        for rx in (r"#\s*define\s+" + PREFIXES,
                   r"\}\s*" + PREFIXES + r"\s*;",
                   r"typedef[^;]*?\b" + PREFIXES + r"\s*;"):
            m = re.match(rx, s)
            if m:
                out.add(m.group(1))
                break
        else:
            m = FUNC_DEF.match(line) or VAR_DEF.match(line)
            if m:
                out.add(m.group(1))
    return out


def walk(root):
    for d in ("graphics", "runtime", "tools", "compat", "src"):
        base = os.path.join(root, d)
        for r, _, fs in os.walk(base):
            for f in fs:
                if f.endswith(CEXT):
                    yield os.path.join(r, f)


def touched_files(root):
    patch = os.path.join(root, "..", "patch", "hdr-auto-l1.patch")
    if not os.path.exists(patch):
        patch = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "patch", "hdr-auto-l1.patch")
    if not os.path.exists(patch):
        return None
    names = [l.rstrip("\n").split(" b/")[-1]
             for l in open(patch, encoding="utf-8") if l.startswith("diff --git ")]
    hit = [n for n in names if n.endswith(CEXT)]
    return hit or None      # 一个都没有 = 补丁格式变了，别当成「没文件要查」


def main():
    if len(sys.argv) < 2:
        print("用法：check_patch_symbols.py <打补丁后的树根>")
        return 1
    root = sys.argv[1]
    touched = touched_files(root)
    if touched is None:
        print("✗ 找不到 patch/hdr-auto-l1.patch，或里面一个 C 系文件都没有（补丁头格式变了？）")
        return 1
    if not os.path.exists(os.path.join(root, touched[0])):
        print("✗ 树里没有 %s（补丁没打上？）" % touched[0])
        return 1

    tset = set(touched)
    upstream = set()
    defined = set()
    for fp in walk(root):
        rel = os.path.relpath(fp, root)
        try:
            code = strip_code(open(fp, encoding="utf-8", errors="ignore").read())
        except OSError:
            continue
        defined |= tree_defs(code)
        if rel not in tset:
            upstream |= any_decls(code)
    for rel in touched:
        r = subprocess.run(["git", "-C", root, "show", "HEAD:%s" % rel],
                           capture_output=True, text=True)
        if r.returncode == 0:
            upstream |= any_decls(strip_code(r.stdout))

    used = collections.Counter()
    where = {}
    for rel in touched:
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            continue
        code = strip_code(open(p, encoding="utf-8", errors="ignore").read())
        used.update(IDENT.findall(code))
        for line_no, line in enumerate(code.splitlines(), 1):
            for name in IDENT.findall(line):
                where.setdefault(name, "%s:%d" % (rel, line_no))

    known = upstream | defined
    missing = [(n, c) for n, c in used.most_common() if n not in known]
    print("补丁触及 %d 个 C 系文件；用到 %d 种前缀符号；树内定义 %d 个、上游声明 %d 个"
          % (len(touched), len(used), len(defined), len(upstream)))
    if missing:
        for name, count in missing:
            print("  ✗ %s（出现 %d 次，首见 %s）—— 树里找不到它的定义，也没在上游声明过"
                  % (name, count, where.get(name, "?")))
        print("::error::%d 个前缀符号没有定义：%s —— ObjC/MSL 在 Linux 侧从不编译，"
              "这类拼写错误只会在 macOS CI 或真机上炸"
              % (len(missing), ", ".join(n for n, _ in missing[:5])))
        return 1
    print("✓ %d 种前缀符号全部有定义或上游声明（覆盖从不编译的 ObjC / MSL 路径）" % len(used))
    return 0


if __name__ == "__main__":
    sys.exit(main())
