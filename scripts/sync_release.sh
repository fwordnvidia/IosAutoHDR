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
