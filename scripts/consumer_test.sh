#!/bin/sh
# 消费者测试：把打好的 tar.gz 解到一个空目录，从零跑一遍全部闸门。
#
# 为什么单独做：开发目录里 build/ 有编译好的二进制、patch/src.manifest 有指纹，
# 「在我这儿能跑」不代表「别人解包后能跑」。这个脚本只依赖包里的东西 + 系统里的
# cc / python3 / ImageMagick（可选）+ 一个上游 S2Arm 树（可选）。
#
# 用法：scripts/consumer_test.sh [上游干净树根]
#   不给树根：只验证包自检部分（报告 / ref_check / 平台区 / 曲线图）。
#   给树根：额外把包里的补丁打进那棵树并跑十三套单测（树必须是干净 v0.4.0 sparse graphics）。
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TARBALL=${TARBALL:-$(ls -t "$HERE"/../hdr-auto-delivery/hdr-auto-*.tar.gz 2>/dev/null | head -1)}
[ -n "$TARBALL" ] && [ -f "$TARBALL" ] || { echo "✗ 找不到交付包（用 TARBALL=... 指定）"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "=== 解包 $TARBALL → $TMP ==="
tar xzf "$TARBALL" -C "$TMP" || exit 1
if [ -e "$TMP/build" ]; then
    echo "✗ 包里不该有 build/（那是本机产物）"; exit 1
fi

echo "=== 包里跑全部闸门 ==="
if [ $# -ge 1 ]; then
    "$TMP/run_all.sh" "$1" || exit 1
else
    "$TMP/run_all.sh" || exit 1
fi

echo
echo "✓ 消费者测试通过：解包即用（$TARBALL）"
