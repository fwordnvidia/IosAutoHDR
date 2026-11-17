#!/usr/bin/env python3
"""check_msl_symbols.py：MSL 内部符号对账 —— 3555 B HDR fragment 从未过 `xcrun metal`。

为什么需要：`src_msl_hdr` 那 3555 字节的 MSL 从写下到现在**一次都没被编译器看过**
（GitHub 账号停用后 CI 不可触发；本地无 iPhoneOS SDK）。`check_msl.py` 核的是结构
（kernel 签名、纹理/缓冲绑定、常量值），核不到「这个函数名拼错了」：
`to_enc` 写成 `to_encd`、`s2a_lin` 写成 `s2a_ln` —— 结构全对，`xcrun metal` 会报
`use of undeclared identifier`，而 Linux 侧的所有闸门都不会红。

判据（与 check_patch_symbols.py 同一个思路，只是换成 MSL 语言）：
  - 用到的名字：MSL 源码里的 `s2a_*` / `nvn_hdr_*` / `hdr_*` / `to_*` 标识符；
  - 有定义的名字：MSL 源码里出现在定义位置的名字 —— 行首（可带 static/inline 与返回类型）
    后跟 `NAME(`、`struct NAME`、`#define NAME`；
  - 判据：用到而没有定义 ⇒ 红。

MSL 源从哪儿来（全部来自**树**，不是补丁文本）：
  1. `graphics/nvn/gfx/nvn/nvn_mtl.m` 里的 `src_msl`（8 位直通）与 `src_msl_hdr`（16F ITM）
     —— 用 `scripts/extract_msl.py` 抽，与 CI 真编用的是同一个抽取器；
  2. `graphics/nvn/gfx/nvn/nvn_present_fi.inc` 里的全部 `static const char *msl =` 块
     （补帧的 down/match/spread 与缩拷 vs/fs）；
  3. `graphics/nvn/shaders/hdr_hist.metal`（真 `.metal` 文件）。

用法：check_msl_symbols.py <打补丁后的树根>
"""
import os
import re
import subprocess
import sys
import tempfile

PREFIXES = r"(s2a_\w+|nvn_hdr_\w+|hdr_\w+|to_\w+)"
IDENT = re.compile(r"\b" + PREFIXES + r"\b")
NOT_STMT = r"(?!return\b|if\b|for\b|while\b|switch\b|case\b|else\b|do\b|\})"
DEF_LINE = re.compile(r"^\s*" + NOT_STMT + r"[A-Za-z_][\w:<>,\s\*&]*?\b" + PREFIXES + r"\s*\(")
OTHER_DEF = [
    re.compile(r"\bstruct\s+" + PREFIXES),
    re.compile(r"#\s*define\s+" + PREFIXES),
    re.compile(r"\bconstant\s+[\w \*&]*?\b" + PREFIXES + r"\s*(?:=|;|\[)"),
]
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def c_string_blocks(path, marker='static const char *msl ='):
    """把 .inc/.m 里每个 `static const char *msl = "…";` 块拼成一个字符串。"""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    blocks, cur = [], None
    for ln in lines:
        if marker in ln:
            if cur is not None:
                blocks.append("".join(cur))
            cur = []
        if cur is not None:
            cur.extend(m.group(1) for m in LIT.finditer(ln))
            if ln.rstrip().endswith(";"):
                blocks.append("".join(cur))
                cur = None
    if cur is not None:
        blocks.append("".join(cur))
    return blocks


def unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(nxt, nxt))
            i += 2
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def defined_names(src):
    out = set()
    for line in src.splitlines():
        m = DEF_LINE.match(line)
        if m:
            out.add(m.group(1))
            continue
        for rx in OTHER_DEF:
            m = rx.search(line)
            if m:
                out.add(m.group(1))
    return out


def tree_sources(root):
    """返回 [(名字, MSL 源码)]；缺文件就跳过（投影形态）。"""
    out = []
    nvn = os.path.join(root, "graphics", "nvn")
    mtl_m = os.path.join(nvn, "gfx", "nvn", "nvn_mtl.m")
    here = os.path.dirname(os.path.abspath(__file__))
    if os.path.exists(mtl_m):
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, os.path.join(here, "extract_msl.py"),
                                mtl_m, "--dir", tmp],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print("::error::extract_msl.py 失败：%s" % (r.stderr.strip().splitlines() or ["?"])[-1])
                return None
            for f in sorted(os.listdir(tmp)):
                if f.endswith(".metal"):
                    with open(os.path.join(tmp, f), encoding="utf-8") as fh:
                        out.append(("nvn_mtl.m:" + f, unescape(fh.read())))
    inc = os.path.join(nvn, "gfx", "nvn", "nvn_present_fi.inc")
    if os.path.exists(inc):
        for i, blk in enumerate(c_string_blocks(inc), 1):
            out.append(("nvn_present_fi.inc:#%d" % i, unescape(blk)))
    hist = os.path.join(nvn, "shaders", "hdr_hist.metal")
    if os.path.exists(hist):
        with open(hist, encoding="utf-8", errors="replace") as fh:
            out.append(("shaders/hdr_hist.metal", fh.read()))
    return out


def main():
    if len(sys.argv) < 2:
        print("用法：check_msl_symbols.py <打补丁后的树根>")
        return 1
    root = sys.argv[1]
    sources = tree_sources(root)
    if sources is None:
        return 1
    if not sources:
        print("· 投影形态：树里没有上游整文件（MSL 源码在 nvn_mtl.m / nvn_present_fi.inc 里）"
              "⇒ 跳过 MSL 符号对账")
        return 0

    defined, used, where = set(), set(), {}
    for name, src in sources:
        code = strip_comments(src)
        defined |= defined_names(code)
        for line_no, line in enumerate(code.splitlines(), 1):
            for ident in IDENT.findall(line):
                used.add(ident)
                where.setdefault(ident, "%s:%d" % (name, line_no))

    missing = sorted(n for n in used if n not in defined)
    print("MSL 源 %d 段（%s）；用到 %d 种前缀符号；其中定义 %d 个"
          % (len(sources), "、".join(n for n, _ in sources), len(used), len(defined)))
    if missing:
        for n in missing:
            print("  ✗ %s —— 用到但 MSL 里没有定义（首见 %s）" % (n, where.get(n, "?")))
        print("::error::MSL 里 %d 个前缀符号没有定义：%s —— 3555 B HDR fragment 从未过 "
              "`xcrun metal`，这类拼写错误只有 CI 真编或真机才会报"
              % (len(missing), ", ".join(missing[:5])))
        return 1
    dead = sorted(defined - used)
    if dead:
        print("  · 定义了但没用到：%s（不算错，只是别是改名改漏了）" % ", ".join(dead[:6]))
    print("✓ %d 种前缀符号在 MSL 里全部有定义（覆盖从未真编的 HDR fragment / 补帧着色器）"
          % len(used))
    return 0


if __name__ == "__main__":
    sys.exit(main())
