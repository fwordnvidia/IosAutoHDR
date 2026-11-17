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
    rm -rf "$DST/.github"
fi

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
