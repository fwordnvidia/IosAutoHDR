#!/bin/sh
# 把补丁重放到**任意上游提交**上，并证明重放无损。这不是「追 head」的自动化，而是「不必追」的自动化：
# 使用者拿到的是适配他自己那棵树的补丁，而不是「你必须 checkout 我验证过的那个 sha」。
#
# 用法：
#   ./scripts/rebase_patch.sh <sha>            重放到指定提交，输出 patch/hdr-auto-l1.<sha12>.patch
#   ./scripts/rebase_patch.sh --head           重放到上游当前头
#   ./scripts/rebase_patch.sh <sha> --verify   重放成功后顺手在那棵树上跑 run_all.sh
#   ./scripts/rebase_patch.sh --check <树>     离线自检：对一棵已 checkout 的树做往返，不联网
#
# 无损怎么证明（这是整个脚本的要点，别删）：
#   正向 patch → 检查无 .rej → **反向 patch** → `git status --porcelain` 必须为空。
#   为空说明「正向改了 N 处、反向一字不差地撤回」，即我们的改动没有被模糊匹配吃掉，
#   上游的新增内容也没有被覆盖。`patch --dry-run` 只说「上下文对得上」，证明不了这两件事。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 1
PATCH=patch/hdr-auto-l1.patch
UP=${S2A_UPSTREAM:-https://github.com/jhaizhou-ops/S2Arm.git}
# 补丁触及的顶层目录。历史上需要 `ios`（ios/play_reflection.m 曾补过一行 EDR 开关）；上游 11-17
# 删除倒影功能后补丁已全部落在 graphics/ 内。这里保留 ios 只为「万一将来再触及」时不会表现为
# 「找不到文件」而不是「冲突」——sparse 多拉一个目录的代价可以忽略。
SCOPE="graphics runtime tools compat ios"
VERIFY=0

arg=${1:-}
[ -n "$arg" ] || { sed -n '2,16p' "$0"; exit 1; }
case "$arg" in
  --check)
      TREE=${2:?用法: rebase_patch.sh --check <已 checkout 的树>}
      MODE=check ;;
  --head)
      MODE=tip; sha=$(git ls-remote "$UP" | awk '$2=="HEAD"{print $1}') ;;
  *)
      MODE=sha; sha=$arg ;;
esac
[ "${3:-}" = "--verify" ] && VERIFY=1
[ "${2:-}" = "--verify" ] && VERIFY=1

workdir=""
if [ "$MODE" != check ]; then
    [ -n "$sha" ] || { echo "✗ 取不到提交号"; exit 1; }
    workdir=$(mktemp -d)
    echo "· 取上游 $sha 的工作树"
    git clone -q --filter=blob:none --no-checkout "$UP" "$workdir/t" || { echo "✗ clone 失败"; exit 1; }
    git -C "$workdir/t" sparse-checkout init --cone >/dev/null 2>&1 || true
    git -C "$workdir/t" sparse-checkout set $SCOPE >/dev/null 2>&1 || true
    git -C "$workdir/t" fetch -q --filter=blob:none origin "$sha" || { echo "✗ fetch 失败"; exit 1; }
    git -C "$workdir/t" checkout -q FETCH_HEAD || { echo "✗ checkout 失败"; exit 1; }
    TREE=$workdir/t
fi
if [ "${S2A_KEEP_TREE:-}" = 1 ]; then
    echo "· 保留工作树（S2A_KEEP_TREE=1）：$TREE"
else
    trap '[ -n "$workdir" ] && rm -rf "$workdir"' EXIT INT TERM
fi

echo "· 目标树：$TREE  (HEAD $(git -C "$TREE" rev-parse --short HEAD 2>/dev/null || echo '?')）"
[ -d "$TREE/graphics/nvn" ] || { echo "✗ 这棵树里没有 graphics/nvn（sparse 范围不对？）"; exit 1; }
[ -f "$TREE/graphics/nvn/gfx/nvn/nvn_mtl.m" ] || { echo "✗ 缺 nvn_mtl.m，不是有效目标树"; exit 1; }
# 目标树必须是干净的：已经打过补丁的树再正向应用必然冲突，那不是「上游冲突」，别让人误会。
dirty0=$(git -C "$TREE" status --porcelain -- $SCOPE 2>/dev/null | head -3)
if [ -n "$dirty0" ]; then
    echo "✗ 这棵树有未提交改动（可能已经打过补丁）："
    echo "$dirty0" | sed 's/^/    /'
    echo "  换一棵干净树：git clone --filter=blob:none <上游> /tmp/clean && git -C /tmp/clean checkout <sha>"
    exit 1
fi

finish() {  # $1=说明
    echo
    if [ "$VERIFY" -eq 1 ]; then
        echo "· 在这棵树上跑 run_all.sh（30 套 885 项 + 全部闸门）"
        if "$ROOT/run_all.sh" "$TREE"; then echo "✓ 重放后的补丁在这棵树上全过"; else echo "✗ 重放成功但验证不过——不要采用这份补丁"; return 1; fi
    fi
    echo "· 下一步：把这棵树当作新基线"
    echo "    patch/BASELINE ← $(git -C "$TREE" rev-parse HEAD 2>/dev/null) $(date +%F)"
    echo "    然后 ./scripts/make_patch.sh <树> --force 覆盖主补丁，或直接采用上面那份候选补丁"
}

# ---- 正向 ----
cd "$TREE" || exit 1
before=$(git rev-parse HEAD)
# 入口时树脏不脏，决定失败后能不能替用户回退（脏树 reset --hard 会毁掉别人的未提交改动）
pre_dirty=$(git status --porcelain | head -1)
if ! patch -p1 --forward --no-backup-if-mismatch -i "$ROOT/$PATCH" >/tmp/rebase_fwd.log 2>&1; then
    echo "✗ 正向应用有失败："
    grep -E '^patching file|FAILED|Hunk #[0-9]+ (FAILED|succeeded)' /tmp/rebase_fwd.log | grep -E 'FAILED' | head -20
    rej=$(find . -name '*.rej' -not -path './.git/*' | head -20)
    [ -n "$rej" ] && { echo "· .rej 文件："; echo "$rej" | sed 's/^/    /'; }
    if [ -n "$rej" ]; then
        echo
        echo "· 生成迁移工单：docs/BASELINE-MIGRATION.md（每个失败 hunk：我们要插什么 + 上游现在这段是什么）"
        # shellcheck disable=SC2086
        python3 "$ROOT/scripts/report_migration.py" "$TREE" $rej >/dev/null 2>&1 && echo "  ✓ 已写出（在仓库根）" || echo "  ⚠ 工单生成失败，手动看 .rej"
    fi
    echo
    echo "  含义：上游在这些位置改到与我们重叠，需要按工单逐 hunk 人工合并（不要 --fuzz）。"
    # 关键：失败路径必须把树恢复原状。曾经不回退，留下「上游 + 半套补丁」的混合树，
    # 下一次 `--check` 或人肉 grep 会把半套补丁当成上游代码（本轮就误判过 present_blit 已有 hdr_head_tick）。
    if [ -z "$pre_dirty" ]; then
        git reset -q --hard "$before" && git clean -qfd $SCOPE
        echo "  · 已回退：树复原到 $before（无 .rej、无半套补丁）"
    else
        echo "  · 树在本次运行前就有未提交改动，未自动回退；请手动 git checkout -- <冲突文件> 后再查"
    fi
    exit 2
fi
offsets=$(grep -cE 'offset [0-9]+ lines' /tmp/rebase_fwd.log || true)
files=$(grep -cE '^patching file' /tmp/rebase_fwd.log || true)
echo "· 正向成功：$files 个文件，其中 $offsets 个 hunk 带偏移（偏移只说明上下文位置变了）"
[ -n "$(find . -name '*.rej' -not -path './.git/*' | head -1)" ] && { echo "✗ 有 .rej 残留"; exit 2; }

# ---- 反向守卫 ----
if ! patch -R -p1 --forward --no-backup-if-mismatch -i "$ROOT/$PATCH" >/tmp/rebase_rev.log 2>&1; then
    echo "✗ 反向撤销失败——说明正向应用不是可逆的（有 hunk 被模糊匹配吃掉）"
    grep -E 'FAILED' /tmp/rebase_rev.log | head -10
    exit 3
fi
dirty=$(git status --porcelain -- $SCOPE 2>/dev/null | head -20)
if [ -n "$dirty" ]; then
    echo "✗ 反向撤销后树不是原样，以下路径仍有改动（重放丢了内容或引入了多余改动）："
    echo "$dirty" | sed 's/^/    /'
    exit 3
fi
echo "· 双向守卫通过：正向 $files 个文件、反向撤销后 \`git status --porcelain\` 为空 ⇒ 重放无损"

# ---- 再来一次正向，留作产物 ----
patch -p1 --forward --no-backup-if-mismatch -i "$ROOT/$PATCH" >/tmp/rebase_fwd2.log 2>&1 || { echo "✗ 第二次正向失败"; exit 2; }
out="patch/hdr-auto-l1.$(git rev-parse --short HEAD).patch"
git add -A $SCOPE 2>/dev/null
git diff --cached > "$ROOT/$out"
git reset -q
sha256=$(sha256sum "$ROOT/$out" | cut -d' ' -f1)
echo "· 候选补丁：$out  $(grep -c '^diff --git' "$ROOT/$out") 文件 / $(wc -c <"$ROOT/$out") B / sha256 ${sha256%????????????????????????????????????????????????}…"
finish
