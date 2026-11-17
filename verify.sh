#!/bin/sh
# 在干净的目标树上打补丁并跑全部 HDR 单测。用法：./verify.sh <树根> [补丁路径]
set -e
ROOT=${1:?用法: ./verify.sh <树根> [补丁]}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH=${2:-$HERE/patch/hdr-auto-l1.patch}
case "$PATCH" in /*) ;; *) PATCH=$(cd "$(dirname "$PATCH")" && pwd)/$(basename "$PATCH") ;; esac
cd "$ROOT"
# 真实照片素材在交付目录的 testdata/ 下、不在目标树里；t_hdr_oracle 靠这个变量找。
# 不设的话它读到 0 张、判失败——而这只在「在树根跑单测」时才暴露（开发目录因为 cwd 有 build/real 一直绿）。
S2A_REAL_DIR=${S2A_REAL_DIR:-$HERE/testdata}
export S2A_REAL_DIR
git apply --check -v "$PATCH"
git apply "$PATCH"
INC=graphics/nvn/gfx/nvn
OUT=$(mktemp -d)
# 补丁里的 Makefile 规则要能真的把 .metal 嵌成头文件（CI 里由 xcrun metal 单独验语法）
if [ -f graphics/metal/tools/embed_msl.py ] && [ -f graphics/nvn/shaders/hdr_hist.metal ]; then
    python3 graphics/metal/tools/embed_msl.py graphics/nvn/shaders/hdr_hist.metal k_hdr_hist "$OUT/nvn_hdr_hist_msl.h" >/dev/null
    echo "✓ embed_msl：hdr_hist.metal → nvn_hdr_hist_msl.h（$(wc -c <"$OUT/nvn_hdr_hist_msl.h") B）"
fi
# 不要再手写套件清单：它漂过一次（新加 8 套没进来，verify.sh 静默只跑 13 套）。
# 断言条数由 run_all.sh 的 `-ge 30` 兜底，这里只负责「补丁打上后每套都能编能过」。
n=0
for t in graphics/nvn/tests/t_hdr*.c; do
    name=$(basename "$t" .c)
    cc -O2 -Wall -Wextra -I "$INC" -o "$OUT/$name" "$t" -lm
    "$OUT/$name" | tail -1
    n=$((n + 1))
done
[ "$n" -ge 30 ] || { echo "✗ 只跑了 $n 套 HDR 单测（应 >= 30）"; exit 1; }
# 上屏着色器是 nvn_mtl.m 里的 C 字符串：本地没有 xcrun，只能抽出来确认能抽出、体积合理，
# 真正的 MSL 编译交给 CI 的第 8 步。
if [ -f "$HERE/scripts/extract_msl.py" ]; then
    mkdir -p "$OUT/msl"
    python3 "$HERE/scripts/extract_msl.py" graphics/nvn/gfx/nvn/nvn_mtl.m --dir "$OUT/msl"
    grep -q HdrCrop "$OUT/msl/src_msl_hdr.metal"
    python3 "$HERE/scripts/check_constants.py" "$INC/nvn_hdr.h" "$OUT/msl/src_msl_hdr.metal"
    echo "✓ 上屏着色器已抽出（8 位直通 + 16F ITM/色域/饱和度），MSL 语法由 CI 编"
fi
# 能打上 != 插对了地方：逐处确认注入还落在预期函数体内（上游前进后最容易坏的就是这个）
if [ -f "$HERE/scripts/check_landing.py" ]; then
    python3 "$HERE/scripts/check_landing.py" "$ROOT"
fi
rm -rf "$OUT"
