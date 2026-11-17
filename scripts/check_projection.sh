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

say "1) CI 配置只有一个只手动触发的 workflow（没有任何自动触发入口）"
# 2026-11-17 改口径：workflow 现在保留在源码树里（只接受 workflow_dispatch），
# 但**默认不进投影** —— 推它需要 PAT 有 `workflow` scope，不为一个 CI 文件去扩权。
# 所以这里两种形态都合法：没有 .github（默认）；有则必须只有那一个手动 workflow。
if [ -e .github ]; then
    # 允许的只有「手动 workflow」+「社区文件」（issue 表单 / PR 模板）。社区文件推上去
    # 不需要 workflow scope，因此它们与 CI 走的不是同一条开关，不能一起否定。
    extra=$(find .github -type f \
        ! -path '.github/workflows/verify.yml' \
        ! -path '.github/pull_request_template.md' \
        ! -path '.github/ISSUE_TEMPLATE/*' | head -5)
    if [ -z "$extra" ]; then ok ".github/ 只有手动 workflow 与社区文件（issue 表单 / PR 模板）"
    else bad ".github/ 里有多余文件：$(echo $extra | tr '\n' ' ')"; fi
else
    ok "投影未带 .github/（默认形态：不为推 CI 文件去扩 token 权限）"
fi
# 只数 CI 用的 yml：issue 表单（.github/ISSUE_TEMPLATE/*.yml）是给 GitHub 渲染表单的，
# 不是 workflow，混在一起数会让「带了 issue 表单」看起来像「多了一个 CI」。
n=$(find . -path ./.git -prune -o -path ./.github/ISSUE_TEMPLATE -prune -o \
        \( -name '*.yml' -o -name '*.yaml' \) -print 2>/dev/null | wc -l)
if [ "$n" -eq 0 ]; then ok "无 workflow yml（投影默认不带 CI）"
elif [ "$n" -eq 1 ]; then ok "只有 1 个 yml（那个手动 workflow）"
else bad "有 $n 个非社区 yml/yaml，预期 0 或 1"; fi

say "2) 不含上游整文件（只交付补丁与自写代码）"
before=$fail
for f in nvn_mtl.m host_adapt.m nvn_present_fi.inc galaxy_ng.m; do
    hit=$(find . -path ./.git -prune -o -name "$f" -print 2>/dev/null | head -1)
    [ -z "$hit" ] || bad "发现上游整文件 $hit"
done
[ "$fail" -eq "$before" ] && ok "无上游整文件（patch/ 里只有 diff 文本，属预期）"

say "3) 不含构建产物与二进制"
# 判「仓库里有没有」而不是「工作区里有没有」：`./run_all.sh` 会在工作区留下 build/ 与
# __pycache__/，上一轮自检若在中途失败就没机会清 —— 用 find 判会把「自己的残留」当成
# 「仓库里有产物」，下一次同步就报「有 1 个产物目录」，看起来像刚提交了文件。
if [ -d .git ]; then
    n=$(git ls-files | grep -cE '(^|/)(build|__pycache__)/' || true)
else
    n=$(find . -path ./.git -prune -o \( -name build -o -name __pycache__ \) -print 2>/dev/null | wc -l)
fi
[ "$n" -eq 0 ] && ok "仓库里没有 build/ 与 __pycache__/（工作区残留不算，见末尾清理）" || bad "仓库里有 $n 个产物目录"
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
# 许可文件必须与源码树逐字节相同：它们没进白名单很久（手工拷过一次就再没刷新），
# 投影里那份 NOTICE 甚至是上游的三方清单（列着 GPL bash、Ryujinx，指向投影里不存在的
# THIRD_PARTY_LICENSES.md）。文件在、内容错，比缺文件更难发现。
_SRC=${SRC:-/home/sechs/ios-build/hdr-auto}
for f in LICENSE NOTICE; do
    if [ -f "$f" ] && [ -f "$_SRC/$f" ] && cmp -s "$f" "$_SRC/$f"; then
        ok "$f 与源码树逐字节相同"
    else
        bad "$f 与 $_SRC/$f 不一致（同步漏了这一步？）"
    fi
done
# 社区档案：GitHub 只在「根级」或「.github/」找 CONTRIBUTING/SECURITY，而投影不带 .github/
# ⇒ 这两个文件必须在根级存在，否则仓库页永远提示「添加贡献指南」，health 停在 42%。
for f in CONTRIBUTING.md SECURITY.md; do
    [ -f "$f" ] && ok "$f 在根级（GitHub 只认根级或 .github/，投影不带后者）" || bad "缺 $f —— 仓库页会一直提示「添加贡献指南」"
done
# 两份文件不是摆设：CONTRIBUTING 必须写出「PR 前跑哪两条」，SECURITY 必须写出报告渠道。
grep -q 'run_all.sh' CONTRIBUTING.md 2>/dev/null && grep -q 'check_projection.sh' CONTRIBUTING.md 2>/dev/null \
    && ok "CONTRIBUTING.md 写明了 PR 前必跑的两条命令" || bad "CONTRIBUTING.md 没写 PR 前要跑 ./run_all.sh 与 ./scripts/check_projection.sh"
grep -q 'Report a vulnerability' SECURITY.md 2>/dev/null \
    && ok "SECURITY.md 写明了私密报告渠道" || bad "SECURITY.md 没有私密报告渠道（GitHub Security Advisories）"

say "7) 基线与文档一致"
if python3 scripts/check_baseline.py >/dev/null 2>&1; then ok "patch/BASELINE 与 README/PR 对齐"; else bad "基线不一致（跑 python3 scripts/check_baseline.py 看细节）"; fi

say "8) 根级入口脚本存在（README 的接收步骤要真能照做）"
# 起因（2026-11-17）：白名单同步只拷了 README 与 src/docs/patch/tools/scripts，投影里从来没有
# run_all.sh / verify.sh，而 README 让使用者第一步就敲 `./run_all.sh <树>`。tar 包里有，
# 只 clone 投影的人第一步就 command not found —— 而此前 7 组自检全绿。
for f in run_all.sh verify.sh; do
    if [ -f "$f" ] && [ -x "$f" ]; then ok "$f 存在且可执行"; else bad "缺 $f（README 让使用者敲它，投影里却没有）"; fi
done

say "9) 投影里的 CI 只手动跑（在 github.com 上按一下就是一台 macOS 虚机）"
# 起因：旧号 Kippythisway 的 workflow 带 push 触发 + 并发无上限，一天 114 台 macOS 虚机被停用。
# 这里直接复用仓库内的守卫脚本，投影要是被改回自动触发，这组就红。
if [ -f .github/workflows/verify.yml ]; then
    if python3 scripts/check_workflow_guard.py >/tmp/proj_wf.log 2>&1; then
        ok "workflow 仅 workflow_dispatch 触发（timeout / concurrency / 基线由同一守卫核对）"
    else
        bad "workflow 不再是「只手动跑」：$(grep -m1 '::error::' /tmp/proj_wf.log | sed 's/::error::workflow 守卫：//')"
    fi
else
    ok "投影未带 workflow（SYNC_CI=1 同步后本组就会校验它只手动跑）"
fi

say '10) 换行不被 Git 改写（.gitattributes 有 * -text，补丁 sha256 才不会变）'
# 这一组是给 Windows 读者的。Git for Windows 默认 core.autocrlf=true，克隆时会把文本文件的
# 换行改成 CRLF —— 然后 patch/hdr-auto-l1.patch 的 sha256 与 SHA256SUMS 里那个值对不上，
# consumer_test.sh 打补丁报 "corrupt patch"。`* -text` 让 Git 完全不做转换（比 `* text=auto
# eol=lf` 更强：字节原样进出，不经过任何规范化）。源码仓 2026-11-17 之前就有这一行，投影仓没有。
if [ ! -f .gitattributes ]; then
    bad "缺 .gitattributes：Windows 上 core.autocrlf=true 的 clone 会把补丁改成 CRLF，sha256 对不上"
else
    if grep -qE '^\*[[:space:]]+-text[[:space:]]*$' .gitattributes; then
        ok '.gitattributes 有 * -text（Git 不做任何换行转换）'
    else
        bad ".gitattributes 没有 * -text（现有：$(head -2 .gitattributes | tr '\n' ' ')）"
    fi
    # 投影里的补丁必须与源码仓逐字节相同：既抓「投影忘了同步」，也抓「换行被改过」。
    SRC=${SRC:-/home/sechs/ios-build/hdr-auto}
    if [ -f "$SRC/patch/hdr-auto-l1.patch" ]; then
        a=$(sha256sum patch/hdr-auto-l1.patch | cut -d' ' -f1)
        b=$(sha256sum "$SRC/patch/hdr-auto-l1.patch" | cut -d' ' -f1)
        if [ "$a" = "$b" ]; then ok "补丁与源码仓逐字节一致（sha256 ${a%??????????????????????????????????????????????????????}…）"
        else bad "投影里的补丁与源码仓不同（投影 ${a}… / 源码 ${b}…）—— 忘了 sync 还是换行被改？"; fi
    else
        ok "本机没有源码仓（$SRC）—— 跳过逐字节比对"
    fi
fi

say "11) clone 下来就能跑：./run_all.sh（不带树根）必须绿"
# 2026-11-17 才发现的问题：投影里跑 ./run_all.sh 会红 4 处，因为好几个检查假定「有上游整文件」
# （src/nvn_mtl.m 1.9 MB、src/nvn_present_fi.inc、src/host_adapt.m）与「带 .github」，
# 而投影按设计两者都没有 ⇒ 使用者第一步就看到 ::error::，会以为补丁坏了。
# 现在这些步骤在缺上游文件时明确打印「投影形态：跳过」。这一组把「还是不是绿的」钉住。
if [ ! -x ./run_all.sh ]; then
    bad "没有可执行的 run_all.sh（第 7 组应该已经抓到）"
elif ./run_all.sh >/tmp/proj_runall.log 2>&1; then
    ok "投影里 ./run_all.sh 通过（$(grep -c '^  ✓' /tmp/proj_runall.log) 个 ✓；$(grep -c '投影形态' /tmp/proj_runall.log) 步按设计跳过）"
    rm -rf ./build   # run_all 会生成 build/，别留在工作树里
else
    bad "投影里 ./run_all.sh 失败：$(grep -m1 '✗' /tmp/proj_runall.log | sed 's/^ *//')"
fi

say ""
# 自检会在工作区跑 run_all（要 build/），中途失败时上面那条 rm 执行不到。
# 无论成败都清一次，否则下一次「组 3」会先看到自己的残留。
rm -rf ./build ./__pycache__ ./src/__pycache__ ./scripts/__pycache__ ./tools/__pycache__ 2>/dev/null
if [ "$fail" -eq 0 ]; then say "✓ 公开投影自检通过：仓库性质与 README 声称一致"; exit 0; fi
say "✗ $fail 项不通过"
exit 1
