#!/bin/sh
# 在**上游当前默认分支**上从零验一遍：取最新提交 → sparse 克隆 → 打补丁 → 21 套单测 + MSL/常数 + 落点自检。
#
# 为什么单独一个脚本：上游一天能走几百个提交（2026-11-16 实测 tag 还停在 v0.4.0，main 已 353+，
# 且当天又从 2b5aca92 → ddfef415 → 1c31fdc0）。「已适配 main」这句话半小时就可能过期，
# 所以把验证做成一条命令：谁都能自己跑，结论带上提交号。
#
# 用法：./scripts/verify_upstream.sh [提交号]     不给提交号就用默认分支当前头
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UP=${S2A_UPSTREAM:-https://github.com/jhaizhou-ops/S2Arm.git}

sha=${1:-}
if [ -z "$sha" ]; then
    sha=$(git ls-remote "$UP" HEAD | cut -f1)
fi
[ -n "$sha" ] || { echo "✗ 取不到上游 HEAD"; exit 1; }

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT INT TERM
echo "· 上游提交：$sha"
git clone -q --filter=blob:none --no-checkout "$UP" "$dir/t"
git -C "$dir/t" sparse-checkout init --cone >/dev/null 2>&1 || true
git -C "$dir/t" sparse-checkout set graphics runtime tools compat >/dev/null 2>&1 || true
git -C "$dir/t" fetch -q --depth 1 origin "$sha"
git -C "$dir/t" checkout -q FETCH_HEAD

echo "· 跑 verify.sh（打补丁 + 21 套 + MSL/常数 + 落点自检）"
echo
"$ROOT/verify.sh" "$dir/t"
echo
echo "✓ 上游 $(echo "$sha" | cut -c1-12) 上全部通过"
echo "  把这个提交号写进 PR / 文档时，同时写上日期——上游在动，结论只对这一个提交成立。"
