#!/bin/sh
# Python 脚本的 lint：脚本是闸门自己，写错了闸门就骗人。
# 选 E9（语法/运行时错）、F（pyflakes：未定义名、未使用 import/变量、f-string 缺占位符）、
# B905（zip 没有显式 strict=）。
#
# 首次跑 34 个脚本：7 条 E9/F 全是真残留（两处 f-string 少了 {}、check_msl 里一个算了不用的
# fn、三个多余 import、plot_curves 里算出来没人用的 nmax），8 处 zip 没有一个写 strict=。
# 其中 4 处本该等长（逐像素比较、CSV 行配表头、两批结果对账）——zip 静默截断等于「比了一半
# 就宣布通过」，所以改成 strict=True；另 4 处是相邻对（steps vs steps[1:]），天生差一个元素，
# 显式写 strict=False。
#
# 注意：zip(strict=) 需要 Python ≥ 3.10，README 已写明。ruff 是可选依赖，没装就明确跳过。
set -u
cd "$(dirname "$0")/.." || exit 1

if [ -z "${RUFF:-$(command -v ruff 2>/dev/null)}" ]; then
    echo "· 未装 ruff —— 跳过 Python lint（pipx install ruff，或下 release 的静态二进制）"
    exit 0
fi
RUFF=${RUFF:-ruff}

if out=$("$RUFF" check --select E9,F,B905 --no-cache --output-format concise scripts/*.py 2>&1); then
    echo "✓ Python lint：$(ls scripts/*.py | wc -l | tr -d ' ') 个脚本 E9/F/B905 全过"
    exit 0
fi
printf '%s\n' "$out" | head -20
echo "✗ Python lint：上面这些是 E9（运行时错）/F（pyflakes）/B905（zip 未显式 strict）"
exit 1
