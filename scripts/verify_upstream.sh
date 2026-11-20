#!/bin/sh
# 在有补丁的树上从零验一遍：取提交 → sparse 克隆 → 打补丁 → 30 套单测 + MSL/常数 + 落点自检。
#
# 三种用法：
#   ./scripts/verify_upstream.sh            验 patch/BASELINE 里写的基线（默认，可复现）
#   ./scripts/verify_upstream.sh <sha>      验指定提交
#   ./scripts/verify_upstream.sh --head     追上游当前头；打不上时打印「差多少、哪些文件变了」��不硬失败
#
# 为什么默认验基线而不是 head：上游一天能走几百个提交，head 只能说明「此刻」，而 patch/BASELINE
# 是「任何人 clone 这个 sha 都能复现」的承诺。追 head 是需要人工重放的工程动作，不该混进日常验证。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
UP=${S2A_UPSTREAM:-https://github.com/jhaizhou-ops/S2Arm.git}
BASE=$(grep -v '^#' patch/BASELINE | awk '{print $1}' | head -1)
[ -n "$BASE" ] || { echo "✗ patch/BASELINE 里没有 sha"; exit 1; }

MODE=baseline
case "${1:-}" in
  "")        sha=$BASE ;;
  --head)    MODE=tip; sha=$(git ls-remote "$UP" HEAD | cut -f1) ;;
  -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
  *)         MODE=explicit; sha=$1 ;;
esac
[ -n "$sha" ] || { echo "✗ 取不到提交号"; exit 1; }

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT INT TERM
echo "· 模式：$MODE    提交：$sha"
[ "$sha" = "$BASE" ] || echo "· 基线是 ${BASE}（本补丁就是对它验证的）"

git clone -q --filter=blob:none --no-checkout "$UP" "$dir/t"
git -C "$dir/t" sparse-checkout init --cone >/dev/null 2>&1 || true
git -C "$dir/t" sparse-checkout set graphics runtime tools compat >/dev/null 2>&1 || true
git -C "$dir/t" fetch -q --depth 1 origin "$sha"
git -C "$dir/t" checkout -q FETCH_HEAD

echo
if "$ROOT/verify.sh" "$dir/t"; then
    echo
    echo "✓ 上游 $(echo "$sha" | cut -c1-12) 上全部通过"
    echo "  把这个提交号写进 PR / 文档时，同时写上日期——上游在动，结论只对这一个提交成立。"
    exit 0
fi

echo
echo "✗ 这个提交上打不上或验不过。"
if [ "$MODE" = tip ]; then
    echo "  上游 head 已离开基线，属于正常。要看差多少："
    echo "    ./scripts/baseline_report.sh"
    echo "  要跟上去：按 docs/PR.md「基线迁移」把改动重放到上游新版文件上，再改 patch/BASELINE。"
    exit 2
fi
echo "  $MODE 模式下这是真失败：要么补丁坏了，要么这个提交确实不兼容。"
exit 1
