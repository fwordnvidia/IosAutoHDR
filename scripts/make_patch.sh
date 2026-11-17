#!/bin/sh
# 从 src/ 重新生成补丁。**src/ 是唯一真相源**：树里的文件不许手改。
#
# 为什么要有这个脚本：2026-11-16 出过一次静默回退——改动直接在「打过补丁的树」里做，
# 没回写 src/，下一次重建补丁时 src 的旧版本覆盖了树里的新版本，补丁悄悄退回上一代，
# 而所有测试都还在跑旧代码。这里的守卫就是防这件事：同步前先比较树与 src，
# 不一致就拒绝执行（说明有人改了树却没回写），--force 才继续。
#
# 用法：./scripts/make_patch.sh <干净树或已打补丁的树> [--force]
set -eu
TREE=${1:?用法: scripts/make_patch.sh <树根> [--force]}
FORCE=${2:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"
PATCH=patch/hdr-auto-l1.patch

[ -d "$TREE/.git" ] || { echo "✗ $TREE 不是 git 树"; exit 1; }

# 把 src 里每个文件映射到树里的路径
# 注意三个非 gfx/nvn 的位置：着色器、测试、宿主层（host_adapt.m）与 Makefile。
# 2026-11-16 的教训：host_adapt.m 与 Makefile 原先只存在于树里、没进 src/，
# 重建补丁时它们被静默丢掉（20 文件 vs 22 文件），验证全绿却少了两个文件的改动。
map_path() {
    case "$1" in
        hdr_hist.metal)  echo "graphics/nvn/shaders/$1" ;;
        t_*.c)           echo "graphics/nvn/tests/$1" ;;
        host_adapt.m)    echo "graphics/nvn/$1" ;;
        Makefile)        echo "graphics/nvn/$1" ;;
        *)               echo "graphics/nvn/gfx/nvn/$1" ;;
    esac
}

# 反向守卫：补丁里出现过的文件，必须在 src/ 里有对应来源。否则下一次重建就会丢。
check_coverage() {
    miss=0
    for rel in $(grep '^diff --git' "$PATCH" | sed 's|.* b/||'); do
        case "$rel" in
            graphics/nvn/shaders/*) base=$(basename "$rel") ;;
            graphics/nvn/tests/*)   base=$(basename "$rel") ;;
            graphics/nvn/host_adapt.m|graphics/nvn/Makefile) base=$(basename "$rel") ;;
            graphics/nvn/gfx/nvn/*) base=$(basename "$rel") ;;
            *) echo "⚠ 补丁含未映射路径：$rel"; miss=1; continue ;;
        esac
        [ -f "src/$base" ] || { echo "⚠ 补丁里的 $rel 在 src/ 里没有来源（下一次重建会丢掉它）"; miss=1; }
    done
    return $miss
}

# 守卫：区分两种情况——
#   a) src 自上次生成以来没变，树里却和 src 不同 ⇒ 有人在树里手改没回写（危险，拒绝）
#   b) src 变了 ⇒ 正常演进，照常重建（只是提示一句）
# 用 patch/src.manifest 记录「上次生成时 src 的指纹」。
MANIFEST=patch/src.manifest
drift=0
advanced=0
: >/tmp/src_now.txt
for f in src/*; do
    b=$(basename "$f")
    now=$(sha256sum "$f" | cut -d' ' -f1)
    echo "$b $now" >>/tmp/src_now.txt
    was=$(grep -E "^$b " "$MANIFEST" 2>/dev/null | cut -d' ' -f2 | head -1)
    dst="$TREE/$(map_path "$b")"
    if [ -n "$was" ] && [ "$was" = "$now" ]; then
        if [ -f "$dst" ] && ! cmp -s "$f" "$dst"; then
            # 树里和 src 不同有两种完全合法的情形，不能一律当「手改」：
            #   a) 树是上游原版（file == git HEAD 版本）⇒ 我们正要往上打补丁，正常
            #   b) 树里已经是本补丁的产物但 src 又没变 ⇒ 内容应与当前 patch 一致
            # 只有「既不是 HEAD 版本、也不是 src 版本」才真的是手改没回写。
            rel=${dst#"$TREE"/}
            if ( cd "$TREE" && git cat-file -e "HEAD:$rel" 2>/dev/null ); then
                if ( cd "$TREE" && git show "HEAD:$rel" ) | cmp -s - "$dst"; then
                    continue            # a) 上游原版
                fi
            fi
            echo "⚠ $b：src 未变，树里既不是上游版本也不是 src 版本（树 $(wc -c <"$dst") B vs src $(wc -c <"$f") B）⇒ 树里被手改过"
            drift=1
        fi
    elif [ -n "$was" ]; then
        advanced=1
    fi
done
[ "$advanced" -eq 1 ] && echo "· src/ 有更新，按 src 重建补丁"
if [ "$drift" -ne 0 ] && [ "$FORCE" != "--force" ]; then
    echo "✗ 拒绝生成：树里有人手改过却没回写 src/。"
    echo "  正确做法：把树里那份改动合并进 src/，再重跑本脚本。"
    echo "  确认要丢弃树里的改动：$0 $TREE --force"
    exit 1
fi

# 同步 src → 树，然后出补丁
for f in src/*; do
    b=$(basename "$f")
    dst="$TREE/$(map_path "$b")"
    mkdir -p "$(dirname "$dst")"
    cp "$f" "$dst"
done

( cd "$TREE" && git add -A graphics ios && git diff --cached > "$HERE/$PATCH" )

# 行数口径：`grep -c '^+'` 会把每个文件的 `+++ b/…` 头也算一行（42 个文件就多报 42 行，
# 曾把 6870 报成 6912）。这里显式排除 +++/--- 头，并把删除行数一起报出来。
ADD=$(awk '/^\+/ && !/^\+\+\+/{n++} END{print n+0}' "$PATCH")
DEL=$(awk '/^-/ && !/^---/{n++} END{print n+0}' "$PATCH")
echo "✓ 补丁已重建：$(grep -c '^diff --git' "$PATCH") 文件，+$ADD -$DEL 行，sha256 $(sha256sum "$PATCH" | cut -d' ' -f1)"

# 立即自检：MSL 抽取 + 常数一致（这两步正是上次回退时唯一会报警的地方）
python3 scripts/extract_msl.py src/nvn_mtl.m --dir build/msl_check >/dev/null
SIZE=$(wc -c <build/msl_check/src_msl_hdr.metal)
python3 scripts/check_constants.py src/nvn_hdr.h build/msl_check/src_msl_hdr.metal >/dev/null
if [ "$SIZE" -lt 1200 ]; then
    echo "✗ 16F 上屏着色器只有 $SIZE B —— 低于 1200 B 通常意味着色域/饱和度那段丢了"
    exit 1
fi
cp /tmp/src_now.txt "$MANIFEST"
if ! check_coverage; then
    echo "✗ 覆盖检查失败：补丁里有文件没有 src/ 来源（见上）"
    exit 1
fi
echo "✓ 自检通过：src_msl_hdr.metal $SIZE B，常数一致（指纹已写入 $MANIFEST）"
