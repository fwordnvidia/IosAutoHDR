#!/bin/sh
# 把工作树同步到公开投影目录。**白名单**，不是通配。
#
# 为什么改成白名单（真事）：用 `cp -r src/*.inc` 同步时，把上游整文件 `nvn_present_fi.inc`
# 一起拷进了公开仓库——那个文件是上游的，投影的规矩是「只交付补丁与自写代码」。
# `check_projection.sh` 抓住了它，但那是同步之后。白名单让「根本拷不进去」成为默认。
#
# 用法：./scripts/sync_release.sh <投影目录> [--commit "说明"]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DST=${1:?用法: sync_release.sh <投影目录> [--commit 说明]}
MSG=""; [ "${2:-}" = "--commit" ] && MSG=${3:-sync}
[ -d "$DST/.git" ] || { echo "✗ $DST 不是 git 仓库"; exit 1; }

# 自写源码（唯一真相源里的、属于我们的那部分）
SRC_OK="nvn_hdr.h nvn_hdr_auto.h nvn_hdr_auto_l1.h nvn_hdr_hist.inc"
for f in $SRC_OK; do cp "$ROOT/src/$f" "$DST/src/$f" || exit 1; done
for f in "$ROOT"/src/t_hdr*.c; do cp "$f" "$DST/src/"; done
# 测试用真实照片（三张 480x270 PPM，来源与授权见 docs/SOURCES.md）。
# 不放进来，公开仓库的 t_hdr_oracle 就永远是「真实照片 0 张」——那一步的对账等于没跑。
mkdir -p "$DST/testdata"
cp "$ROOT"/testdata/*.ppm "$DST/testdata/" 2>/dev/null
# 文档、补丁、工具
cp "$ROOT"/docs/*.md "$DST/docs/" 2>/dev/null
cp "$ROOT"/patch/hdr-auto-l1.patch "$ROOT"/patch/BASELINE "$ROOT"/patch/src.manifest "$DST/patch/" || exit 1
cp "$ROOT"/tools/*.c "$DST/tools/" 2>/dev/null
for f in "$ROOT"/scripts/*; do
    case $(basename "$f") in
        publish.sh) continue ;;   # 含旧仓库地址，不进投影
        *) cp "$f" "$DST/scripts/" ;;
    esac
done
cp "$ROOT/README.md" "$DST/README.md"
# 根级入口脚本：投影的「接收步骤」写的就是 `./run_all.sh <树>` / `./verify.sh <树>`，
# 但白名单此前只拷了 README 与 src/docs/patch/tools/scripts —— 于是公开仓库里根本没有这两个文件，
# 照文档做的人第一步就 command not found。2026-11-17 消费端测试发现（tar 包里有，投影里没有）。
cp "$ROOT/run_all.sh" "$ROOT/verify.sh" "$DST/" || exit 1
# 根级社区档案：GitHub 只认根级或 .github/ 下的这几个文件名，而投影按设计不带 .github/
# （见上面 CI 那条），所以只能放根级。缺了它们仓库页会一直提示「添加贡献指南」，
# 社区档案 health 停在 42%。
cp "$ROOT/CONTRIBUTING.md" "$ROOT/SECURITY.md" "$DST/" || exit 1
# LICENSE 与 NOTICE 一直没进白名单 —— 它们是投影建仓那一次手工拷进去的，之后再没被刷新。
# 后果有两条：① 我们改了 NOTICE（补丁基线迁移时必然要改），投影里那个还是旧的；
# ② 投影里的 NOTICE 其实是**上游**那份，列的是上游打进去的三方件（GPL bash、Ryujinx…），
#    还指向 THIRD_PARTY_LICENSES.md 与 docs/第三方依赖.md —— 这两个文件投影里根本没有。
# 不分发上游代码却挂上游的三方清单，等于对外声称一件不存在的事。
cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$ROOT/CODE_OF_CONDUCT.md" "$DST/" || exit 1

# .github 里的**社区文件**（issue 表单、PR 模板）与 CI 是两件事：推它们不需要 PAT 的
# `workflow` scope（只有 `.github/workflows/*.yml` 才是 workflow 文件），所以默认同步。
# 它们的用处不是门面：issue 表单把我们最缺的证据（基线、树、S2A_* 开关、HDR stat 行、
# 三格矩阵哪一格）变成必填项，PR 模板把「新行为必须带一条能红的断言」放在提交前。
# `.github/workflows/verify.yml` 仍然只由 SYNC_CI=1 决定（见下）。
mkdir -p "$DST/.github/ISSUE_TEMPLATE"
cp "$ROOT"/.github/ISSUE_TEMPLATE/*.yml "$DST/.github/ISSUE_TEMPLATE/" 2>/dev/null
cp "$ROOT/.github/pull_request_template.md" "$DST/.github/" 2>/dev/null

# .gitattributes：**这一行不是排版洁癖**。公开仓库要给人 clone，Windows 的 Git 默认
# core.autocrlf=true，会把文本文件的换行改成 CRLF —— 然后 patch/hdr-auto-l1.patch 的
# sha256 就变了，SHA256SUMS 校验失败、consumer_test 打补丁报 corrupt patch。
# `* -text` 让 Git 完全不做换行转换（比 `* text=auto eol=lf` 更强：字节原样进出）。
# 源码仓 /home/sechs/ios-build 早就有这一行，投影仓一直到 2026-11-17 都没有。
printf '* -text\n' > "$DST/.gitattributes"

# CI 配置默认**不**进投影。原因不在合规（那个 workflow 只接受 workflow_dispatch），
# 而在凭据：GitHub 拒绝用没有 `workflow` scope 的 PAT 创建/改写 workflow 文件，
# 报 “refusing to allow a Personal Access Token to create or update workflow … without
# `workflow` scope”。为了不为了一个 CI 文件去扩权，默认路径保持「投影里没有 workflow」；
# 真要在 GitHub 上点 Run 时，先给 token 加上 `Workflows: Read and write`，再：
#   SYNC_CI=1 ./scripts/sync_release.sh <投影目录>
if [ "${SYNC_CI:-0}" = "1" ]; then
    mkdir -p "$DST/.github/workflows"
    cp "$ROOT/.github/workflows/verify.yml" "$DST/.github/workflows/verify.yml" || exit 1
    echo "· 已同步 workflow（SYNC_CI=1）"
else
    # 只清 CI 文件，不动社区文件：直接 `rm -rf $DST/.github` 会把上面刚拷进去的
    # issue 表单和 PR 模板一起删掉（2026-11-18 真的发生了：同步 rc=0、自检也绿，
    # 因为自检那时还没要求 .github 里有东西 —— 于是「文件同步了」变成了假话）。
    rm -rf "$DST/.github/workflows"
fi

# 同步自己也别「rc=0 但没做事」：上面每一步都用 `2>/dev/null` 兜过可选文件，
# 于是「拷贝失败」与「本来就没有」长得一模一样。这里按名字点一遍必须存在的产物 ——
# 2026-11-18 出过：社区文件拷进去了，随后 `rm -rf $DST/.github` 又删掉，rc 仍是 0。
missing=""
for f in README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md LICENSE NOTICE run_all.sh verify.sh \
         .gitattributes .github/pull_request_template.md \
         .github/ISSUE_TEMPLATE/bug_report.yml \
         patch/hdr-auto-l1.patch patch/BASELINE src docs scripts tools; do
    [ -e "$DST/$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
    echo "✗ 同步后投影里缺：$missing"
    exit 1
fi
echo "· 必需产物齐全（README/CONTRIBUTING/SECURITY/LICENSE/NOTICE/入口脚本/.github 社区文件/补丁/src/docs/scripts/tools）"

# 清掉「曾经误拷进去的上游整文件」，否则自检红
for bad in nvn_mtl.m host_adapt.m nvn_present_fi.inc galaxy_ng.m; do rm -f "$DST/src/$bad"; done

cd "$DST" || exit 1
if ./scripts/check_projection.sh >/tmp/proj_check.log 2>&1; then
    echo "✓ 投影自检通过"
else
    echo "✗ 投影自检失败："; grep '✗' /tmp/proj_check.log | head -8; exit 1
fi
git add -A
if [ -n "$MSG" ]; then
    git commit -q -m "$MSG" && echo "✓ 已提交：$(git log --oneline -1)"
else
    echo "· 已暂存（未提交）："; git status --porcelain | head -20
fi
