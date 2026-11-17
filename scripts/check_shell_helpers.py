#!/usr/bin/env python3
"""调用不存在辅助函数的检查：`ok: not found` 这类错误只写 stderr，不会让脚本判红。

踩过的实例：run_all.sh 的 0w16/5 第一版照抄了 scripts/check_repo_hygiene.sh 的 `ok "…"` /
`bad "…"`，但 run_all.sh 只定义了 `step()`。结果跑起来是

    ./run_all.sh: 159: ok: not found

而 `fail` 从未被置 1 —— 整体照样打印「✓ 全部通过」。shellcheck 也看不见它（它不跨文件
推断函数是否存在，且 `ok`/`bad` 是合法命令名）。

规则：只查公认的一组辅助名（ok/bad/step/die/warn/note/info）。文件里用了但没定义、
也没 source 别的文件的，判红。系统里不存在叫这些名字的命令，所以「未定义即 bug」成立。
"""
import os
import re
import sys

HELPERS = ("ok", "bad", "step", "die", "warn", "note", "info")
DEF_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\)", re.M)
CALL_RE = re.compile(r"^\s*(%s)\s+[\"'$A-Za-z0-9_-]" % "|".join(HELPERS), re.M)
SRC_RE = re.compile(r"^\s*(?:\.|source)\s+\S", re.M)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def files():
    out = []
    for base, _, names in os.walk(ROOT):
        if "/." in base or base.endswith("/build") or "/node_modules" in base:
            continue
        for n in names:
            if n.endswith(".sh"):
                out.append(os.path.join(base, n))
    return sorted(out)


def strip_comments(text):
    # 只去掉行首 # 注释行；正文里的 # 可能是字符串内容，不碰
    return "\n".join(l for l in text.split("\n") if not l.lstrip().startswith("#"))


def main():
    bad = 0
    for path in files():
        text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        defined = set(DEF_RE.findall(text))
        if SRC_RE.search(text):
            continue  # 可能从别处 source 进辅助函数，不在这条规则的射程内
        rel = os.path.relpath(path, ROOT)
        for m in CALL_RE.finditer(text):
            name = m.group(1)
            if name in defined:
                continue
            line = text[: m.start()].count("\n") + 1
            print("::error::%s:%d：调用了 %s \"…\"，但这个文件没定义 %s()" % (rel, line, name, name))
            print("  ⇒ 运行时会打「%s: not found」到 stderr，而 fail 不会被置 1 —— 假绿" % name)
            bad += 1
    # 第二条：提示字符串里不许出现反引号。
    # 在 sh 双引号内 `` `…` `` 是**命令替换**，不是排版。踩过两次：
    #   1) `ok ".gitattributes 有 \`* -text\`（…）"` → shell 去执行 `*`（glob 成 LICENSE 等）→
    #      `LICENSE: Permission denied`，提示空白，shellcheck 报 SC2211 才看出来；
    #   2) `say "11) … \`./run_all.sh\`（不带树根）必须绿"` → 真的把 run_all 跑了一遍，
    #      它的「✓ 全部通过」被插进标题里，而 shellcheck **不报**（这是合法的命令替换）。
    # 第 2 次说明：只靠 shellcheck 挡不住，要一条自己的规则。
    BT_RE = re.compile(r'\b(?:say|ok|bad|step|note|warn)\b.*`')
    for path in files():
        for i, ln in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
            t = ln.strip()
            if t.startswith("#"):
                continue   # 注释里的反引号不执行（这条规则是给提示串的）
            if BT_RE.search(ln):
                print("::error::%s:%d：提示字符串里有反引号 —— 在 sh 双引号里它是命令替换，会被执行"
                      % (os.path.relpath(path, ROOT), i))
                bad += 1
    if bad:
        print("✗ shell 辅助函数检查：%d 处" % bad)
        return 1
    print("✓ shell 辅助函数检查：%d 个脚本里调用的 ok/bad/step/… 都有定义，提示串里没有反引号"
          % len(files()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
