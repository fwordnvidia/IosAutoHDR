#!/usr/bin/env python3
"""补丁的「文件数 / 增删行数 / sha256」在文档里的说法必须与现算一致。

为什么单独一个守卫：这三个数字至少漂过两次——`README.md` 写「23 文件 +3650 行」时补丁已经是
42 文件 +6870 −26 行（差 19 个文件、3220 行），而 `check_counts.py` 只管套数 / 断言数 /
fragment 字节数，管不到补丁本体；`make_patch.sh` 自己的行数口径也曾把每个文件的 `+++` 头算成
一行（6870 报成 6912）。数字是 review 的人用来核对下载物的，写错等于给假指纹。

只校验三处**当前补丁**的锚点，不全文扫（历史叙述里出现旧指纹是正常的，全文扫必然误报）：
  1. `README.md` 以「补丁本体：」开头的行；
  2. `docs/VERIFICATION.md` 以「| 补丁文件指纹 |」开头的表行；
  3. `docs/PR.md` 含「结果：补丁 **」的行。

锚点行找不到也算失败——删掉说明不是「不用校验了」，而是守卫静默失效。
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(HERE, "patch", "hdr-auto-l1.patch")
DOC = os.path.join(HERE, "docs", "PATCH-STATS.md")
CHECK = "--check" in sys.argv


def measure():
    raw = open(PATCH, "rb").read()
    lines = raw.decode("utf-8", "replace").splitlines()
    return {
        "sha256": hashlib.sha256(raw).hexdigest(),
        "bytes": len(raw),
        "files": sum(1 for l in lines if l.startswith("diff --git ")),
        # 口径：排除 `+++ b/…` / `--- a/…` 头，否则每个文件多算一行
        "add": sum(1 for l in lines if l.startswith("+") and not l.startswith("+++")),
        "del": sum(1 for l in lines if l.startswith("-") and not l.startswith("---")),
    }


def line_of(rel, pred, what):
    path = os.path.join(HERE, rel)
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if pred(line):
            return n, line.rstrip("\n")
    raise SystemExit(f"::error::{rel} 里找不到锚点行（{what}）——守卫会静默失效，先补回说明")


def need(cond, msg):
    if not cond:
        print(f"::error::{msg}")
        return 1
    return 0


def main():
    m = measure()
    fails = 0

    # 1) README
    n, line = line_of("README.md", lambda l: l.startswith("补丁本体："), "补丁本体：")
    mo = re.search(r"\*\*(\d+) 文件 \+(\d+) 行\*\*（−(\d+)）", line)
    fails += need(mo is not None, f"README.md:{n} 认不出「NN 文件 +NNNN 行（−NN）」")
    if mo:
        f, a, d = (int(x) for x in mo.groups())
        fails += need((f, a, d) == (m["files"], m["add"], m["del"]),
                      f"README.md:{n} 写 {f} 文件 +{a} −{d}，实测 {m['files']} 文件 +{m['add']} −{m['del']}")
    ms = re.search(r"sha256 ([0-9a-f]{64})", line)
    fails += need(ms is not None and ms.group(1) == m["sha256"],
                  f"README.md:{n} 的 sha256 不是当前补丁的（实测 {m['sha256']}）")

    # 2) VERIFICATION
    n, line = line_of("docs/VERIFICATION.md", lambda l: l.startswith("| 补丁文件指纹 |"), "补丁文件指纹")
    mo = re.search(r"`([0-9a-f]{8})…`（(\d+) 文件，\+(\d+) −(\d+) 行）", line)
    fails += need(mo is not None, f"docs/VERIFICATION.md:{n} 认不出「`XXXXXXXX…`（NN 文件，+NN −NN 行）」")
    if mo:
        pre, f, a, d = mo.group(1), int(mo.group(2)), int(mo.group(3)), int(mo.group(4))
        fails += need(pre == m["sha256"][:8],
                      f"docs/VERIFICATION.md:{n} 指纹前 8 位写 {pre}，实测 {m['sha256'][:8]}")
        fails += need((f, a, d) == (m["files"], m["add"], m["del"]),
                      f"docs/VERIFICATION.md:{n} 写 {f} 文件 +{a} −{d}，实测 {m['files']} +{m['add']} −{m['del']}")

    # 3) PR
    n, line = line_of("docs/PR.md", lambda l: "结果：补丁 **" in l, "结果：补丁 **")
    mo = re.search(r"\*\*(\d+) 文件 / \+(\d+) −(\d+) 行 / `sha256 ([0-9a-f]{8,16})…`\*\*", line)
    fails += need(mo is not None, f"docs/PR.md:{n} 认不出「**NN 文件 / +NN −NN 行 / `sha256 XXXX…`**」")
    if mo:
        f, a, d, pre = int(mo.group(1)), int(mo.group(2)), int(mo.group(3)), mo.group(4)
        fails += need((f, a, d) == (m["files"], m["add"], m["del"]),
                      f"docs/PR.md:{n} 写 {f} 文件 +{a} −{d}，实测 {m['files']} +{m['add']} −{m['del']}")
        fails += need(m["sha256"].startswith(pre),
                      f"docs/PR.md:{n} 指纹写 {pre}…，实测以 {m['sha256'][:len(pre)]} 开头")

    text = (
        "# 补丁本体的三个数字（脚本生成，`--check` 挡漂移）\n\n"
        "| 项 | 值 | 怎么算 |\n| --- | --- | --- |\n"
        f"| 文件数 | **{m['files']}** | `diff --git ` 行数 |\n"
        f"| 增加行 | **+{m['add']}** | `^+` 且非 `+++`（否则每个文件多算一行，曾把 6870 报成 6912） |\n"
        f"| 删除行 | **−{m['del']}** | `^-` 且非 `---` |\n"
        f"| 字节 | {m['bytes']:,} | `len(bytes)` |\n"
        f"| sha256 | `{m['sha256']}` | `sha256sum patch/hdr-auto-l1.patch` |\n\n"
        "三处文档引用必须与上表一致（`README.md`「补丁本体：」行、`docs/VERIFICATION.md`\n"
        "「补丁文件指纹」行、`docs/PR.md`「结果：补丁 **」行）。历史叙述里出现旧指纹不算错，\n"
        "所以本守卫只认这三个锚点、不全文扫。\n"
    )
    old = open(DOC, encoding="utf-8").read() if os.path.exists(DOC) else None
    if CHECK:
        if old != text:
            print("::error::docs/PATCH-STATS.md 与补丁不符（跑 python3 scripts/check_patch_stats.py 重生）")
            fails += 1
    else:
        open(DOC, "w", encoding="utf-8").write(text)
        print(f"写出 docs/PATCH-STATS.md（{m['files']} 文件 +{m['add']} −{m['del']}）")

    if fails:
        print(f"✗ {fails} 项不一致")
        return 1
    print(f"✓ 补丁统计三处一致：{m['files']} 文件 +{m['add']} −{m['del']}，sha256 {m['sha256'][:8]}…")
    return 0


sys.exit(main())
