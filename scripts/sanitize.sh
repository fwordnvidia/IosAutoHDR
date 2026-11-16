#!/bin/sh
# 30 套单测在 ASan + UBSan 下再跑一遍（内存错误、越界、未定义行为）。
#
# 为什么要单独一遍：run_all.sh 用 -O2 跑，编译器会假设"没有 UB"来优化——一旦真有 UB，
# 它可能表现为"在某些机器上算错"，而不是崩溃。Sanitizer 把这类问题变成明确的失败。
#
# 两个必须写死的选项：
#   ASAN_OPTIONS=detect_leaks=1      泄漏也算失败（这些测试都是纯计算，不该有泄漏）
#   UBSAN_OPTIONS=halt_on_error=1    **默认只打印不中止**，不写这个就是假绿（踩过）
#
# 在仓库根跑（t_hdr_oracle 要读 build/real/*.ppm）。
# 用法：./scripts/sanitize.sh [--keep]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 1
OUT=${SAN_DIR:-/tmp/s2a-san}
mkdir -p "$OUT"

# 探测工具链（无 libasan 的机器上优雅跳过，不算失败）
if ! printf 'int main(void){return 0;}' > "$OUT/probe.c" || \
   ! cc -fsanitize=address,undefined "$OUT/probe.c" -o "$OUT/probe" 2>/dev/null; then
    echo "· 本机 cc 不支持 -fsanitize=address,undefined，跳过（不是失败）"
    exit 0
fi

fail=0; n=0; t0=$(date +%s)
for f in src/t_hdr*.c; do
    b=$(basename "$f" .c); n=$((n+1))
    if ! cc -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer \
            -I src "$f" -lm -o "$OUT/$b" 2>"$OUT/$b.cc.log"; then
        printf '  ✗ %-24s 编译失败\n' "$b"; head -3 "$OUT/$b.cc.log"; fail=$((fail+1)); continue
    fi
    if ! ( cd "$ROOT" && ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
             UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
             "$OUT/$b" >"$OUT/$b.out" 2>&1 ); then
        printf '  ✗ %-24s 运行失败\n' "$b"
        grep -E 'runtime error|ERROR: (Address|Leak)Sanitizer|SUMMARY' "$OUT/$b.out" | head -4
        fail=$((fail+1)); continue
    fi
    # 即使退出码 0 也查一遍（halt_on_error 生效前的报告、ASan 的 "not freed"）
    if grep -qE 'runtime error|Sanitizer' "$OUT/$b.out"; then
        printf '  ✗ %-24s 有 sanitizer 报告（退出码 0）\n' "$b"
        grep -E 'runtime error|Sanitizer' "$OUT/$b.out" | head -3; fail=$((fail+1))
    fi
done
printf '· %d 套 × ASan+UBSan（halt_on_error=1, detect_leaks=1），%ds\n' "$n" "$(( $(date +%s)-t0 ))"
[ "$fail" -eq 0 ] || { echo "✗ $fail 套有问题，日志在 $OUT/"; exit 1; }
echo "✓ 无内存错误、无未定义行为、无泄漏（$n 套）"
