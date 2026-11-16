#!/usr/bin/env python3
"""docs 里的「计数类」说法必须与实测一致。

为什么单独拿出来：这轮的 T2 事故是「文档写的一套、代码做的一套」，而更早几轮反复出现的是
另一种——**文档里的数字落后代码**（「22 套」「796 项」「3406 B」在四处文档里各留一份拷贝，
每加一套单测就要手改四个地方，漏一处就变成说谎）。

这里只查能机器判定的三类，全部与实测对齐：

  1. 单测套数（`NN 套`）——数 `src/t_hdr*.c`；
  2. 断言总数（`NN 条断言` / `NN 项` 里紧跟「单测」语义的那种）——把每套编译跑一遍再求和
     （和人工统计时的口径完全一致：取每套末行里的「N 项」）；
  3. 上屏 HDR fragment 字节数（`NNNN B`）——把 `src/nvn_mtl.m` 里的 `src_msl_hdr` 抽出来量。

不查的东西：`docs/*.md` 里由脚本生成的报告（QUALITY/REVIEW/OPTIONS/… 各自带 `--check`），
以及散文里的历史数字（比如「曾硬编码 13 个套件名」——那是叙述，不是现状）。
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = ["README.md"] + ["docs/" + f for f in ("PR.md", "VERIFICATION.md", "DESIGN.md",
                                            "REVIEW.md", "QUALITY.md", "SOURCES.md")]
# 叙述性历史数字，不参与校验（连同行号说明写在这里，便于以后有人问「为什么不查」）
EXEMPT = ("13 个套件名", "曾硬编码", "少跑 8 套")


def suites():
    return sorted(f for f in os.listdir(os.path.join(HERE, "src")) if re.match(r"t_hdr.*\.c$", f))


def count_items():
    """逐套编译运行并求和（与人工统计同口径：每套末行的「N 项」）。"""
    total = 0
    bad = []
    for f in suites():
        exe = os.path.join(tempfile.gettempdir(), "cnt_" + f[:-2])
        r = subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(HERE, "src"), "-o", exe,
                            os.path.join(HERE, "src", f), "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            bad.append(f + "（编译失败）")
            continue
        out = subprocess.run([exe], capture_output=True, text=True).stdout
        m = None
        for line in out.strip().split("\n")[::-1]:
            m = re.search(r"(\d+) (项|条)", line)
            if m:
                break
        if not m:
            bad.append(f + "（没有「N 项/条」）")
            continue
        total += int(m.group(1))
    return total, bad


def msl_bytes():
    """上屏 HDR fragment 的字节数——直接用 scripts/extract_msl.py 抽出来量。

    不要在这里另写一个解析器：`src_msl_hdr` 是跨多行、中间还夹着 `//` 注释的一串字面量，
    手写正则会在第一个引号处停下（曾经量出 47 B）。抽 MSL 只有一份实现，就有只有一个口径。
    """
    out = os.path.join(tempfile.gettempdir(), "msl_counts")
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(HERE, "scripts", "extract_msl.py"),
                        os.path.join(HERE, "src", "nvn_mtl.m"), "--dir", out, "src_msl_hdr"],
                       capture_output=True, text=True)
    if r.returncode:
        return None
    path = os.path.join(out, "src_msl_hdr.metal")
    return os.path.getsize(path) if os.path.exists(path) else None


def main():
    n_suites = len(suites())
    n_items, bad = count_items()
    if bad:
        print("::error::统计断言总数时出问题：" + "、".join(bad))
        return 1
    size = msl_bytes()
    problems = []
    for doc in DOCS:
        path = os.path.join(HERE, doc)
        if not os.path.exists(path):
            continue
        for i, line in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
            if any(e in line for e in EXEMPT):
                continue
            # 去掉 Markdown 的粗体标记再扫：`**885** 条断言` 里的 `**` 会把 `(\d+) 条断言` 拦掉，
            # 而「参数层」那一行恰好就是粗体（2026-11-16 的负向实测就栽在这上面）。
            line = line.replace("**", "")
            # 1) 「NN 套」：叙述里提到别的数目（少跑 8 套之类）由 EXEMPT 挡；其余一律按总套数查
            for m in re.finditer(r"(\d+) 套", line):
                if int(m.group(1)) != n_suites:
                    problems.append("%s:%d 写了「%s 套」，实测 %d 套单测" % (doc, i, m.group(1), n_suites))
            # 2) 「NN 条断言 / NN 项」：总数语境 = ① 前有「共/全部/总计」；② 紧跟在本文件的「NN 套」之后
            #    （`30 套单测，873 项`、`**873** 条断言（30 套）` 都属这一类）。
            #
            #    为什么加 ②：2026-11-16 漏掉过一处——`README.md:105` 原先写「**873** 条断言（30 套）」，
            #    句子里没有「共/全部/总计」，于是 873 这个过时数字连续两轮没被抓住，而它正好出现在
            #    「参数层」那一行（reviewer 最先看的位置）。凡与套数同句的条数，一定是总数。
            total_ctx = bool(re.search(r"(共|全部|总计)[^。]{0,16}条断言", line))
            if total_ctx:
                for m in re.finditer(r"(\d+) 条断言", line):
                    if int(m.group(1)) != n_items:
                        problems.append("%s:%d 写了「%s 条断言」（总数语境），实测 %d 项" % (doc, i, m.group(1), n_items))
            # 与套数**贴在一起**的条数一定是总数：`30 套 885 项`、`885 条断言（30 套）`。
            # 注意不能退化成「本行出现过 N 套 ⇒ 本行所有 NN 项 都是总数」——`30 套 885 项 + 落点自检 46 项`
            # 里那个 46 是另一件事（实测 46 项落点断言），第一版这么写就误报了三处。
            for m in re.finditer(r"(\d+) 套[^。；、]{0,16}?(\d+) 项", line):
                if re.search(r"落点|自检|ref_check|MSL", m.group(0)):
                    continue      # 同句里的另一件事（`30 套全过、落点自检 46 项`）
                if int(m.group(2)) != n_items:
                    problems.append("%s:%d 写了「%s 套 %s 项」，实测 %d 项" % (doc, i, m.group(1), m.group(2), n_items))
            for m in re.finditer(r"(\d+) 条断言[^。]{0,16}?(\d+) 套", line):
                if int(m.group(1)) != n_items:
                    problems.append("%s:%d 写了「%s 条断言（%s 套）」，实测 %d 项" % (doc, i, m.group(1), m.group(2), n_items))
            # 3) 上屏 HDR fragment 的字节数：只认明确说 fragment 的行；`7,756 B` 这类千分位不算
            if size:
                for m in re.finditer(r"HDR fragment\D{0,4}(\d+) B", line):
                    if int(m.group(1)) != size:
                        problems.append("%s:%d 写了「HDR fragment %s B」，实测 %d B" % (doc, i, m.group(1), size))
    if problems:
        print("::error::文档里的计数与实测不一致：")
        for p in problems:
            print("  " + p)
        print("  （实测：%d 套 / %d 项 / %s B）" % (n_suites, n_items, size))
        return 1
    print("✓ 文档计数与实测一致（%d 套 / %d 项 / %s B）" % (n_suites, n_items, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
