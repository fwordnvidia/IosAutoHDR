#!/bin/sh
# C 警告审计：正式构建一律带 -w（关闭全部警告），等于把编译器这个最基础的检查器关掉。
# 这里用 -Wall -Wextra 把每个带 main 的源文件重编一遍（-o /dev/null，只编译不运行），
# 有任何 warning 判红。46 个文件里抓到的第一个是 tools/sweep_darkproxy.c 的 proxy_gap()——
# 定义了从没被调用（候选 3 的公式早就内联进 main 了），-Wall 之前没人知道。
#
# 只审带 main 的文件（src/t_*.c 单测、tools/*.c 工具）。src/hdr_*.c 是纯库，编成可执行会
# 因缺 main 失败，那是预期，不是问题。
#
# cc 不可用就明确跳过（不静默）。
set -u
cd "$(dirname "$0")/.." || exit 1

if ! command -v cc >/dev/null 2>&1; then
    echo "· 没有 cc —— 跳过 C 警告审计"
    exit 0
fi

CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -I src"
n=0; bad=0
for f in src/t_*.c tools/*.c; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    out=$(cc $CFLAGS "$f" -lm -o /dev/null 2>&1)
    if [ $? -ne 0 ]; then
        echo "::error::${f}：编译失败"
        printf '%s\n' "$out" | grep -E 'error:' | head -3
        bad=$((bad + 1))
        continue
    fi
    w=$(printf '%s' "$out" | grep -c 'warning:')
    if [ "$w" -gt 0 ]; then
        echo "::error::${f}：$w 条 warning"
        printf '%s\n' "$out" | grep 'warning:' | head -3
        bad=$((bad + 1))
    fi
done

if [ "$bad" = 0 ]; then
    echo "✓ C 警告审计：$n 个文件在 -Wall -Wextra 下零 warning"
    exit 0
fi
echo "✗ C 警告审计：$bad / $n 个文件有 warning（正式构建用 -w 把它们全遮住了）"
exit 1
