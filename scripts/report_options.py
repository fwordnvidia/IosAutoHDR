#!/usr/bin/env python3
"""生成 / 校验 docs/OPTIONS.md：补丁引入的每一个旋钮。

这套补丁有 40 多个 `S2A_HDR_*` 编译期常数，外加 9 个 `S2A_HDR_*` 环境变量。它们散在
四个头文件里、只有就地注释——PR 里最常被问的就是「有哪些可调项、缺省值多少、能不能运行期改」。

这里不手抄：**说明直接取宏上方那段 `//` 注释**，缺省取 `#define` 的值，`读取处` 用 grep 数出来。
于是「新增一个常数却忘了写注释」或「注释删了」都会让 `--check` 变红，文档不可能比代码旧。

两种旋钮性质不同，必须分开说：
  - 编译期常数（`#ifndef` + `#define`）：可以用 `-D` 覆盖，但覆盖后要重编；
  - 环境变量（`getenv` + `static` 缓存）：运行期生效，但**首次读取后不再变**（进程内缓存）。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(HERE, "docs", "OPTIONS.md")

DEF_RE = re.compile(r"^#define\s+(S2A_HDR_[A-Z0-9_]+)\s*(.*)$")
IFNDEF_RE = re.compile(r"^#ifndef\s+(S2A_HDR_[A-Z0-9_]+)\s*$")


def scan(path):
    """返回 [{name, value, comment, file, line, guarded, inherited}]"""
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    last_comment, last_line = "", -9
    for i, ln in enumerate(lines):
        m = DEF_RE.match(ln)
        if not m:
            continue
        name, value = m.group(1), m.group(2).strip()
        # 注释通常在 `#ifndef` 上方（guarded 常数），也可能直接贴在 `#define` 上方。
        # 两种都往上看，取最近的一段连续 `//`；回看最多 6 行。
        comment = []
        start = i - 1
        while start >= 0 and start >= i - 3 and (
                IFNDEF_RE.match(lines[start]) or not lines[start].strip()):
            start -= 1
        j = start
        while j >= 0 and lines[j].lstrip().startswith("//") and len(comment) < 8:
            comment.insert(0, lines[j].lstrip()[2:].strip())
            j -= 1
        if not comment and j >= 0 and lines[j].strip().endswith("*/"):
            # `/* … */` 块注释（多行）：往上找到开头的 `/*`
            block = []
            k = j
            while k >= 0 and len(block) < 10:
                raw = lines[k].strip()
                block.insert(0, raw)
                if "/*" in raw:
                    break
                k -= 1
            text_block = " ".join(block)
            text_block = text_block.replace("/*", "").replace("*/", "")
            text_block = " ".join(x.strip().lstrip("*").strip() for x in block)
            text_block = text_block.replace("/*", "").replace("*/", "")
            comment = [" ".join(text_block.split())]
        # 行尾注释也算（很多短常数写在右边）
        tail = ""
        if "//" in value:
            value, tail = value.split("//", 1)
            value, tail = value.strip(), tail.strip()
        # 行尾注释最具体（就写在这个常数后面），有它就用它，不再拼上方那段
        text = tail if tail else " ".join(comment).strip()
        inherited = False
        if not text and last_comment and i - last_line <= 3:
            # 一组同类常数（如 P3 的三行矩阵）只有一段注释时，后续行沿用并标注
            text, inherited = last_comment, True
        guarded = any(IFNDEF_RE.match(lines[k]) for k in range(max(0, j - 2), i))
        if text:
            last_comment, last_line = text, i
        out.append({
            "name": name, "value": value or "（无值，作开关）",
            "comment": text, "file": os.path.basename(path), "line": i + 1,
            "guarded": guarded, "inherited": inherited,
        })
    return out


def readers(name):
    """谁引用这个宏（按文件计，排除定义它的头本身）"""
    hits = set()
    for root, _, files in os.walk(os.path.join(HERE, "src")):
        for fn in files:
            if not fn.endswith((".h", ".c", ".m", ".inc", ".metal")):
                continue
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                body = f.read()
            if re.search(r"\b%s\b" % re.escape(name), body):
                hits.add(fn)
    return sorted(hits)


def one_line(text, limit=132):
    """表格里只要一行：取第一句（句号/分号前），过长再截断。

    完整论证在头文件的注释里，表格的职责是让人一眼知道「这是什么旋钮」。
    """
    t = text.strip()
    for sep in ("。", "；"):
        i = t.find(sep)
        if 8 <= i <= limit:
            return t[:i]
    return t if len(t) <= limit else t[:limit] + "…"


def env_vars():
    """环境变量：名字 → 解析函数名（`getenv("X")` 所在函数）"""
    out = {}
    for root, _, files in os.walk(os.path.join(HERE, "src")):
        for fn in files:
            if not fn.endswith((".h", ".inc", ".c", ".m")):
                continue
            path = os.path.join(root, fn)
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
            for i, ln in enumerate(lines):
                for m in re.finditer(r'getenv\("(S2A_HDR_[A-Z0-9_]+)"\)', ln):
                    fn_name = os.path.splitext(fn)[0]
                    for k in range(i, max(-1, i - 40), -1):
                        mm = re.search(r"static\s+inline\s+[\w ]+\s+(\w+)\s*\(", lines[k])
                        if mm:
                            fn_name = mm.group(1)
                            break
                    out.setdefault(m.group(1), (fn_name, fn))
    return out


def build():
    macros = []
    for fn in sorted(os.listdir(os.path.join(HERE, "src"))):
        if fn.endswith((".h", ".inc")):
            macros += scan(os.path.join(HERE, "src", fn))
    seen, rows = set(), []
    for m in macros:
        if m["name"] in seen:
            continue
        seen.add(m["name"])
        rows.append(m)

    undocumented = [r for r in rows if len(r["comment"]) < 4]
    if undocumented:
        print("::error::以下常数没有就地注释（本脚本从注释生成说明，缺注释就没法写进 OPTIONS.md）：")
        for r in undocumented:
            print("  %s:%d %s" % (r["file"], r["line"], r["name"]))
        return None, None

    envs = env_vars()
    L = []
    L.append("# 可调项一览\n")
    L.append("补丁引入的全部旋钮，由 `scripts/report_options.py` 从源码生成（`--check` 挡漂移）。"
             "说明直接取宏上方的注释，所以**文档不会比代码新**。\n")
    L.append("## 环境变量（运行期生效，进程内首次读取后缓存）\n")
    L.append("| 变量 | 读取处 | 说明 |")
    L.append("|---|---|---|")
    notes = {
        "S2A_HDR_AUTO": "1 = 打开自动判定（直方图 → 纸白 → 拐点）；缺省关，关时整条 HDR 链路逐字节同以前",
        "S2A_HDR_OUTPUT": "1 = 总开关：16F 画布 + 声明 EDR（三格矩阵的 T2，head 才会 >1）",
        "S2A_HDR_FMT": "1 = 只换 16F 画布、不声明 EDR（T1，分辨黑屏归因）；0 = 只声明 EDR、画布留 8 位（T3）",
        "S2A_HDR_HEADROOM": "覆盖 EDR 上界（钳 [1,4]）；不设则每帧读屏幕 EDR 上界（iOS：UIScreen.currentEDRHeadroom；macOS：NSScreen.maximumExtendedDynamicRangeColorComponentValue）",
        "S2A_HDR_MAP": "0/缺省 = 拐点档；1/bt2408/content = BT.2408 内容映射（整幅抬起，knee 只作日志）",
        "S2A_HDR_LOG": "1 = 每 600 帧打一行 `HDR stat:`（真机判读与三格矩阵全靠它）",
        "S2A_HDR_SAT": "饱和度倍数，钳 [0.5,2]，1 = 不动（CIELAB 里缩放 a*/b*，保色相）",
        "S2A_HDR_SAT_SPACE": "1 = 在 CIELAB 里缩放色度（缺省）；0 = 旧的线性锚定（会偏色相）",
        "S2A_HDR_P3": "Display P3 混合比例 [0,1]，0 = 不动（BT.2087 矩阵按行归一化，灰不变）",
    }
    for name in sorted(envs):
        fn, where = envs[name]
        L.append("| `%s` | `%s`（%s） | %s |" % (
            name, fn, where, notes.get(name, "（见源码注释）")))
    L.append("")
    L.append("## 编译期常数（可用 `-D` 覆盖，覆盖后需重编）\n")
    L.append("| 常数 | 缺省 | 说明 | 读取处 |")
    L.append("|---|---|---|---|")
    for r in sorted(rows, key=lambda x: x["name"]):
        readers_ = [x for x in readers(r["name"]) if x != r["file"]]
        note = one_line(r["comment"])
        if r.get("inherited"):
            note = "（同上一组）" + note
        where = ("、".join(readers_[:4]) + ("…" if len(readers_) > 4 else "")) if readers_ else "（仅本文件）"
        L.append("| `%s` | `%s` | %s | %s |" % (r["name"], r["value"], note, where))
    L.append("")
    L.append("## 不在这里的东西\n")
    L.append("- 上游自己的 `DM_*` 调试开关（一百多个）与本补丁无关，不去罗列。\n")
    L.append("- 三格矩阵（T1/T2/T3）用到的只是 `S2A_HDR_FMT` 与 `S2A_HDR_OUTPUT` 的组合，"
             "判据见 `scripts/check_device_log.py`。\n")
    return "\n".join(L) + "\n", (rows, envs)


def main():
    text, info = build()
    if text is None:
        return 1
    rows, envs = info
    # 投影形态：这份文档是在**开发树**里扫 src/ 生成的，而公开仓库按设计不含上游整文件
    # （src/nvn_mtl.m 1.9 MB 等）。投影里重算必然得到不同结果——不是文档旧了，是没有对象可扫。
    # 2026-11-17 之前在 clone 下来的投影里 ./run_all.sh 会红在这一步。
    if "--check" in sys.argv and not os.path.exists(os.path.join(HERE, "src/nvn_mtl.m")):
        print("· 投影形态：没有上游整文件 ⇒ 跳过这份快照的漂移校验")
        print("  文档本身仍随投影发布（给人读）；一致性由开发树的 0w 段保证")
        return 0
    if "--check" in sys.argv:
        if not os.path.exists(DOC) or open(DOC, encoding="utf-8").read() != text:
            print("::error::docs/OPTIONS.md 与源码不一致（加了常数/改了注释？跑一次不带 --check）")
            return 1
        print("✓ docs/OPTIONS.md 与源码一致（%d 个常数、%d 个环境变量、%d 行）"
              % (len(rows), len(envs), text.count("\n")))
        return 0
    open(DOC, "w", encoding="utf-8").write(text)
    print("写出 docs/OPTIONS.md（%d 个常数、%d 个环境变量、%d 行）"
          % (len(rows), len(envs), text.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
