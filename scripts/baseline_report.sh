#!/bin/sh
# 诊断「补丁基线与上游 head 的距离」：差多少提交、我们改过的文件里哪些被上游动过、动了多少行。
#
# 为什么单独一个脚本：上游一天能走几百个提交。日常验证固定验 patch/BASELINE（可复现），
# 但"要不要追"必须有个便宜的答案——本脚本只读两个 sha 的树，不出补丁、不编译。
# 用法：./scripts/baseline_report.sh [--json]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
UP=${S2A_UPSTREAM:-https://github.com/jhaizhou-ops/S2Arm.git}
BASE=$(grep -v '^#' patch/BASELINE | awk '{print $1}' | head -1)
HEAD_SHA=$(git ls-remote "$UP" HEAD | cut -f1)
echo "基线: $BASE"
echo "上游: $HEAD_SHA"
[ "$BASE" = "$HEAD_SHA" ] && { echo "✓ 基线就是上游 head，无需追。"; exit 0; }

dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT INT TERM
git clone -q --filter=blob:none --no-checkout "$UP" "$dir/t"
cd "$dir/t"
git sparse-checkout init --cone >/dev/null 2>&1 || true
git sparse-checkout set graphics runtime tools compat >/dev/null 2>&1 || true
git fetch -q --depth 1 origin "$BASE"; git fetch -q --depth 1 origin "$HEAD_SHA"
N=$(git rev-list --count "$BASE".."$HEAD_SHA" 2>/dev/null || echo "?")
echo "距离: $N 个提交"
echo
echo "我们改过、上游也会改的文件（这些要重放）："
# 补丁里涉及、且上游树里本来就存在的文件
for rel in $(grep '^diff --git' "$ROOT/patch/hdr-auto-l1.patch" | sed 's|.* b/||'); do
    git cat-file -e "$BASE:$rel" 2>/dev/null || continue    # 我们新增的文件，不冲突
    a=$(git show "$BASE:$rel" | sha256sum | cut -c1-8)
    b=$(git show "$HEAD_SHA:$rel" | sha256sum | cut -c1-8)
    [ "$a" = "$b" ] && continue
    # 上游在这两个提交之间对该文件改了多少行
    git diff --numstat "$BASE" "$HEAD_SHA" -- "$rel" | awk -v r="$rel" '{printf "  ⚠ %-62s +%s −%s\n", r, $1, $2}'
done
echo
echo "重放步骤：见 docs/PR.md「基线迁移」；改完 patch/BASELINE 再 ./scripts/verify_upstream.sh"
