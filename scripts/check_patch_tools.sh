#!/bin/sh
# 两种打补丁的工具必须得到同一份结果：`git apply`（我们的闸门用的）与 GNU `patch -p1`
#（使用者手上很可能是这个）。README 里让人 `git apply`，但真实世界里有人只有 `patch`，
# 也有人习惯 `patch -p1 < x.patch`。
#
# 这条为什么值得测：`git apply` 是**严格**的（上下文必须逐字节对上、新文件按 patch 头里的
# mode 建、末行没有换行要写 `\ No newline at end of file`），而 GNU `patch` 有 fuzz 容忍度，
# 上下文差一两行也能贴上、还会留 `.orig`/`.rej`。两者"都成功"不代表"结果一样"，
# 而它们不一样时，用户跑出来的就是我们没测过的那份代码。
#
# 做法（全部在临时目录里，不动传入的树）：
#   1. 把干净树 copy 两份；
#   2. A 用 `git apply`，B 用 `patch -p1 --forward --no-backup-if-mismatch`；
#   3. 两边都必须 rc=0、都不许出现 `.rej`/`.orig`；
#   4. `diff -r` 两边的 `graphics ios runtime tools compat`：内容必须逐字节相同
#      （权限位不比：git apply 按 umask 建文件、GNU patch 按 patch 头建，二者本来就会差）；
#   5. 再比一遍两边相对 `HEAD` 的改动文件清单，必须一致（防止一边多贴了几个文件）。
#
# 用法：check_patch_tools.sh <干净树根>
set -u
TREE=${1:?用法: scripts/check_patch_tools.sh <干净树根>}
HERE=$(cd "$(dirname "$0")/.." && pwd)
PATCH=$HERE/patch/hdr-auto-l1.patch
[ -f "$PATCH" ] || { echo "✗ 找不到 $PATCH"; exit 1; }
command -v patch >/dev/null 2>&1 || { echo "· 没有 GNU patch，跳过（只验 git apply 那条路）"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
for d in a b; do
    cp -a "$TREE" "$TMP/$d" || exit 1
    ( cd "$TMP/$d" && git checkout -q -- . && git clean -qfd graphics ios runtime tools compat ) || exit 1
done

if ! ( cd "$TMP/a" && git apply --whitespace=nowarn "$PATCH" ); then
    echo "::error::git apply 失败（这条路径是 4/5 用的，先看 verify.sh）"
    exit 1
fi
if ! ( cd "$TMP/b" && patch -p1 --forward --no-backup-if-mismatch <"$PATCH" >"$TMP/b.log" 2>&1 ); then
    echo "::error::GNU patch -p1 失败 —— README 说「用 patch 也行」就不成立"
    grep -iE 'fail|reject|fuzz' "$TMP/b.log" | head -5
    exit 1
fi

# fuzz 容忍是 GNU patch 的隐藏行为：只报 "with fuzz N" 时它认为自己成功了，但贴的位置可能偏。
if grep -qi 'with fuzz' "$TMP/b.log"; then
    echo "::error::GNU patch 用了 fuzz（补丁与上游上下文差了几行）—— 结果不可信"
    grep -i 'with fuzz' "$TMP/b.log" | head -3
    exit 1
fi
rej=$(find "$TMP/a" "$TMP/b" -name '*.rej' -o -name '*.orig' | head -5)
[ -z "$rej" ] || { echo "::error::打补丁留下了 reject/orig 文件：$rej"; exit 1; }

if ! diff -r "$TMP/a/graphics" "$TMP/b/graphics" >"$TMP/diff.log" 2>&1; then
    echo "::error::两种工具打出来的 graphics/ 不一致："
    head -8 "$TMP/diff.log"
    exit 1
fi

la=$( cd "$TMP/a" && git status --porcelain | sort )
lb=$( cd "$TMP/b" && git status --porcelain | sort )
if [ "$la" != "$lb" ]; then
    echo "::error::两种工具改动的文件清单不一致："
    printf '%s\n' "$la" > "$TMP/la.txt"; printf '%s\n' "$lb" > "$TMP/lb.txt"
    diff "$TMP/la.txt" "$TMP/lb.txt" | head -8
    exit 1
fi

n=$(printf '%s\n' "$la" | grep -c .)
echo "✓ git apply 与 GNU patch -p1 结果一致：$n 个文件的改动内容逐字节相同、无 fuzz、无 .rej"
