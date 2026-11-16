#!/bin/sh
# 公开投影自检：这个仓库**对外宣称的性质**，逐条在本地可验证。
#
# 为什么放在仓库里而不是只在维护者机器上：本仓库的主张是「一个补丁交付，不含上游整文件、
# 不含 CI、不含构建产物」。任何人都能 clone 下来跑这条命令核对，不必相信 README。
# 用法：./scripts/check_projection.sh          （在**公开投影的根目录**跑）
#   注意：它检查的是「投影这个仓库声称的性质」。在开发工作树里跑会正确地失败——工作树里有上游整文件
#   （nvn_mtl.m / host_adapt.m / nvn_present_fi.inc），投影里按设计没有。想核对投影，就 clone 投影再跑。
set -u
cd "$(dirname "$0")/.." || exit 1
fail=0
say() { printf '%s\n' "$*"; }
bad() { printf '  ✗ %s\n' "$*"; fail=$((fail+1)); }
ok()  { printf '  ✓ %s\n' "$*"; }

say "1) 不含任何 GitHub Actions 配置（物理上不可能起 runner）"
if [ -e .github ]; then bad ".github/ 存在"; else ok "无 .github/"; fi
n=$(find . -path ./.git -prune -o -name '*.yml' -print -o -name '*.yaml' -print 2>/dev/null | wc -l)
[ "$n" -eq 0 ] && ok "无任何 .yml/.yaml" || bad "有 $n 个 yml/yaml，请确认不是 workflow"

say "2) 不含上游整文件（只交付补丁与自写代码）"
before=$fail
for f in nvn_mtl.m host_adapt.m nvn_present_fi.inc galaxy_ng.m; do
    hit=$(find . -path ./.git -prune -o -name "$f" -print 2>/dev/null | head -1)
    [ -z "$hit" ] || bad "发现上游整文件 $hit"
done
[ "$fail" -eq "$before" ] && ok "无上游整文件（patch/ 里只有 diff 文本，属预期）"

say "3) 不含构建产物与二进制"
n=$(find . -path ./.git -prune -o \( -name build -o -name __pycache__ \) -print 2>/dev/null | wc -l)
[ "$n" -eq 0 ] && ok "无 build/ 与 __pycache__/" || bad "有 $n 个产物目录"
n=$(find . -path ./.git -prune -o -type f \( -name '*.tar.gz' -o -name '*.bundle' -o -name '*.pyc' -o -name '*.so' -o -name '*.dylib' -o -name '*.o' -o -name '*.a' \) -print 2>/dev/null | wc -l)
[ "$n" -eq 0 ] && ok "无归档/二进制" || bad "有 $n 个归档或二进制"

say "4) 无 >1 MB 的单文件（大文件走 Release，不进 git）"
big=$(find . -path ./.git -prune -o -type f -size +1M -print 2>/dev/null | head -5)
[ -z "$big" ] && ok "最大文件 < 1 MB" || { for b in $big; do bad "$(du -h "$b" | cut -f1) $b"; done; }

say "5) 无凭据与内网地址"
leak=$(grep -rIn --exclude-dir=.git --exclude=check_projection.sh -E 'github_pat_|ghp_[A-Za-z0-9]{20,}|LBFDKWNAR45|66\.42\.108\.173|192\.168\.71\.|qwertyuiop9456|ssh-rsa ' . 2>/dev/null | head -5)
[ -z "$leak" ] && ok "无 token / 私网 IP / 口令 / 公钥" || { say "$leak"; bad "发现疑似凭据或内网信息（见上）"; }
if git remote -v 2>/dev/null | grep -q '@'; then bad "remote URL 里带凭据"; else ok "remote URL 无凭据"; fi

say "6) 许可与出处"
[ -f LICENSE ] && grep -q 'Apache License' LICENSE && ok "LICENSE 是 Apache-2.0 原文" || bad "LICENSE 缺失或不是 Apache-2.0"
[ -f NOTICE ] && ok "NOTICE 存在（署名上游）" || bad "NOTICE 缺失"
grep -q 'jhaizhou-ops/S2Arm' README.md 2>/dev/null && ok "README 写明上游出处" || bad "README 未写明上游出处"

say "7) 基线与文档一致"
if python3 scripts/check_baseline.py >/dev/null 2>&1; then ok "patch/BASELINE 与 README/PR 对齐"; else bad "基线不一致（跑 python3 scripts/check_baseline.py 看细节）"; fi

say ""
if [ "$fail" -eq 0 ]; then say "✓ 公开投影自检通过：仓库性质与 README 声称一致"; exit 0; fi
say "✗ $fail 项不通过"
exit 1
