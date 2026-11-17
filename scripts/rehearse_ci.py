#!/usr/bin/env python3
"""在 Linux 上预演 verify.yml：把 macOS 专属行挖掉，其余**整块**真跑。

## 为什么需要它

`.github/workflows/verify.yml` 是唯一能做「xcrun metal 真编译 MSL + clang -fsyntax-only
ObjC」的路径，但它需要 macOS runner，而本机是 Ubuntu，且推 workflow 需要 PAT 带
`workflow` scope（没收）。结果是：那个 workflow **从写下来就没被执行过一次**。

不执行的代价已经出现过两次：第一次靠对着干净树人工核，发现 3 处必红错（`host_adapt.m`
路径错、`-I/tmp/s2a/build/nvn/gen` 这个目录上游根本不存在、`! grep` 在 set -e 下被豁免）；
第二次靠 shellcheck 发现第 4 处（SC2251）。这些都是读出来的，不是跑出来的。

这个脚本把「真跑一次」的范围扩到 Linux 能做到的部分：YAML 里每个 `run:` 块原样取出，
只把含 `xcrun` / `xcodebuild` / `-isysroot` 的行替换成 `:`（注释里保留原文），其余逐行执行。
块结构（`set -e`、`for`、`if`、变量）全部原样保留，所以不是「另写一份流程」——没有双源。

## 它证明什么、不证明什么

证明：路径写对了、参数对得上、步骤顺序能走通、`run_all` 与 apply 的先后关系正确、
`check_msl.py` 在这条链里真能产出 4 个 metal 文件、`check_constants.py` 的入参存在。
**不证明**：Metal 能否编过、ObjC 语法是否正确——那两条只有 macOS runner 能给。

用法：
    ./scripts/rehearse_ci.py             # 挖掉 macOS 行（默认）
    ./scripts/rehearse_ci.py --with-macos  # 不挖（在 macOS 上跑，等价于 CI）
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WF = os.path.join(ROOT, ".github", "workflows", "verify.yml")
MACOS = re.compile(r"\bxcrun\b|\bxcodebuild\b|-isysroot")


def blocks():
    import yaml

    with open(WF, encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    # YAML 1.1 把裸 `on` 解析成 True；这里只取 jobs，不受影响
    for st in doc["jobs"]["verify"]["steps"]:
        if st.get("run"):
            yield st.get("name") or "(unnamed)", st["run"]


def main():
    if not os.path.exists(WF):
        print("· 没有 .github/workflows/verify.yml（投影默认不带 CI 文件）⇒ 跳过预演")
        return 0
    keep_macos = "--with-macos" in sys.argv
    bad = 0
    total_skipped = 0
    for name, body in blocks():
        lines, skipped = [], []
        src = body.split("\n")
        i = 0
        while i < len(src):
            ln = src[i]
            if not keep_macos and MACOS.search(ln) and not ln.lstrip().startswith("#"):
                skipped.append(ln.strip())
                # 续行必须一起吃掉：clang 那条语句是 `-isysroot "$SDK" \` 换行接
                # `-mios-version-min=18.0 ...`，只挖首行会把续行留成一条独立命令
                # （实跑报 `sh: 24: -mios-version-min=18.0: not found`，然后
                # `tee` 拿到空日志、grep 无匹配 ⇒ 打出「· 语法通过」的假绿）。
                while ln.rstrip().endswith("\\") and i + 1 < len(src):
                    i += 1
                    ln = src[i]
                    skipped.append(ln.strip())
                lines.append(':  # SKIP(macOS)')
            else:
                lines.append(ln)
            i += 1
        # 被挖掉的步骤不能留下「成功文案」：workflow 里的 `echo "· 语法通过：$src"` /
        # `echo "· 编译通过：..."` 在 clang/xcrun 被挖掉后依然会执行，日志读起来像通过了。
        # 只在预演里改写这两句文案（不改逻辑、CI 侧原文不动）。
        if skipped:
            lines = [ln.replace('· 语法通过：', '· [预演] clang 被挖掉，语法未验证：')
                       .replace('· 编译通过：', '· [预演] xcrun 被挖掉，未编译：')
                     for ln in lines]
        total_skipped += len(skipped)
        print("=== %s（%d 行，挖掉 %d 行 macOS 专属）" % (name, len(body.split("\n")), len(skipped)))
        r = subprocess.run(["sh", "-c", "\n".join(lines)], cwd=ROOT)
        if r.returncode != 0:
            print("✗ 该步骤在 Linux 上预演失败（rc=%d）" % r.returncode)
            bad += 1
            break
    if bad:
        print("✗ CI 预演：有步骤在 Linux 上就走不通 —— 就算 PAT 补上 workflow scope，"
              "那 10 分钟也只会拿到红")
        return 1
    print("✓ CI 预演：Linux 能做的部分全部走通")
    print("· 共挖掉 %d 行 macOS 专属命令（xcrun / xcodebuild / -isysroot 及其续行）"
          % total_skipped)
    print("· 因此 **Metal 能否编过、ObjC 语法是否正确，这次预演没有验证** —— 那两条只有")
    print("  macOS runner 能给。被挖掉的步骤里若出现「[预演] … 未验证」，就是这两项。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
