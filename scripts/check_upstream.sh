#!/bin/sh
# 只读地检查「这份补丁还能不能打到上游当前代码上」。不推送、不改任何远端。
#
# 为什么要同时看两条线：PR 的目标是**默认分支**（main），不是某个 tag。历史上只看最新 tag
# 会给出「上游没有前进，补丁按原样可用」这种让人放心的错结论——2026-11-16 实测 tag 仍是
# v0.4.0，而 main 已经前进 353 个提交。所以两条线都要报，并且以默认分支的打补丁结果为准。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=dcd05e5fa84c8c5c68b6ba66e3b1c6de016636ae      # 补丁的制作基线（v0.4.0）
UP=https://github.com/jhaizhou-ops/S2Arm.git

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT INT TERM

# 1) 默认分支名与它当前的 sha（一次 ls-remote 拿全，不克隆）
ref=$(git ls-remote --symref "$UP" HEAD | sed -n 's/^ref: refs\/heads\/\([^\t]*\).*/\1/p')
[ -n "$ref" ] || ref=main
head=$(git ls-remote "$UP" "refs/heads/$ref" | cut -f1)
# 2) 最新 tag 及其 sha（对 tag 排序取最大版本号）
tags=$(git ls-remote --tags --refs "$UP")
newest=$(printf '%s\n' "$tags" | sed 's#.*refs/tags/##' | grep '^v[0-9]' | sort -V | tail -1)
tagsha=$(printf '%s\n' "$tags" | awk -v t="refs/tags/$newest" '$2==t {print $1}')

echo "· 补丁制作基线：v0.4.0 ($BASE)"
echo "· 上游默认分支：$ref ($(echo "$head" | cut -c1-12))"
echo "· 上游最新 tag：$newest ($(echo "$tagsha" | cut -c1-12))"

[ "$head" = "$BASE" ] && echo "  （默认分支就停在基线提交上）"

# 3) 在默认分支的树上试打（这才是 PR 真正落地的地方）
git clone -q --filter=blob:none --no-checkout "$UP" "$dir"
git -C "$dir" sparse-checkout init --cone >/dev/null 2>&1 || true
git -C "$dir" sparse-checkout set graphics runtime tools compat >/dev/null 2>&1 || true
git -C "$dir" fetch -q --depth 1 origin "$head"
git -C "$dir" checkout -q FETCH_HEAD

# 4) 上游有没有自己搞 HDR/EDR。有的话就不是「纯增量」，要人工对齐而不是叠一层。
# 只用「意图」标记，不用 RGBA16Float：后者在格式分类器/测试枚举里到处都有，会把噪音当信号。
hits=$(git -C "$dir" grep -Il -e S2A_HDR -e ExtendedDynamicRange -e EDRHeadroom -e potentialEDR -- graphics runtime 2>/dev/null | wc -l | tr -d ' ')
if [ "$hits" = "0" ]; then
    echo "· 上游 graphics/runtime 里 HDR/EDR 意图标记：0（本补丁是纯增量）"
    n16=$(git -C "$dir" grep -Il -e RGBA16Float -- graphics runtime 2>/dev/null | wc -l | tr -d ' ')
    [ "$n16" = "0" ] || echo "  （另有 $n16 个文件提到 RGBA16Float，属通用浮点格式处理，不构成 HDR 实现）"
else
    echo "! 上游已有 $hits 个文件含 HDR/EDR 意图，本补丁不再是纯增量，合入前需人工对齐："
    git -C "$dir" grep -Il -e S2A_HDR -e ExtendedDynamicRange -e EDRHeadroom -e potentialEDR -- graphics runtime | sed 's/^/    /'
fi

echo
if git -C "$dir" apply --check "$ROOT/patch/hdr-auto-l1.patch" 2>/dev/null; then
    echo "✓ 补丁能干净地打到 $ref ($(echo "$head" | cut -c1-12))"
    if [ "$head" != "$BASE" ]; then
        echo "  注意：默认分支已比基线前进，打上不等于行为未变。"
        echo "  下一步：./run_all.sh <该树>（会就地跑二十四套，并核对 MSL/常数）"
    fi
else
    echo "✗ 补丁打不上 $ref —— 上游动过它碰的文件"
    echo "  重做办法：把这棵树当新基线，按 src/ 覆盖 + ./scripts/make_patch.sh <新树> --force"
    git -C "$dir" apply --check "$ROOT/patch/hdr-auto-l1.patch" 2>&1 | head -8 || true
    exit 1
fi
