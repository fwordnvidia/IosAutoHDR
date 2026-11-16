#!/bin/sh
# 把补丁变成一条能提 PR 的分支：fork 上游 → 打补丁 → commit → push 分支 → 开 PR。
#
# 默认 **dry-run**：只打印将要执行的命令，不连网、不改远端。
#   ./scripts/publish.sh                # 看一眼会做什么
#   ./scripts/publish.sh --yes          # 真的做（需要 gh 已登录且有 fork 权限）
#
# 为什么要有它：补丁包本身只能「打上」，走到 PR 还差十来步容易漏（分支名、基线、body、
# 不要碰 main）。这里把顺序固定下来，并且拒绝三种危险操作：push 到 upstream、push 到 main、
# force push。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UP=${S2A_UPSTREAM:-jhaizhou-ops/S2Arm}
BRANCH=${S2A_BRANCH:-hdr-auto/l1b}
BASE=${S2A_BASE:-main}
PATCH="$ROOT/patch/hdr-auto-l1.patch"
BODY="$ROOT/docs/PR.md"

YES=0
PRE=0
[ "${1:-}" = "--yes" ] && YES=1
[ "${1:-}" = "--dry-run" ] && YES=0
[ "${1:-}" = "--preflight" ] && PRE=1

say() { printf '%s\n' "$*"; }
# 打印时给含空格的参数补上引号（否则 dry-run 看起来像会把标题拆成多个参数）；
# 执行时用 "$@"，与打印无关，不会真的拆。
q() {
    for a in "$@"; do
        case "$a" in
            *[!A-Za-z0-9_./:=@%+,^-]*) printf "'%s' " "$a" ;;
            *) printf '%s ' "$a" ;;
        esac
    done
}
run() {
    printf '  $ '
    q "$@"
    printf '\n'
    if [ "$YES" -eq 1 ]; then "$@"; fi
}

say "· 上游：$UP"
say "· 分支：$BRANCH  →  基线：$BASE"
say "· 补丁：$PATCH（$(wc -c <"$PATCH") B）"
say "· PR 正文：$BODY（$(wc -l <"$BODY") 行）"
[ -f "$BODY" ] || { say "✗ 缺 PR 正文 $BODY"; exit 1; }

case "$BRANCH" in
    hdr-auto/*) ;;
    *) say "✗ 分支名必须以 hdr-auto/ 开头（避免误推 main 或别人的分支）"; exit 1 ;;
esac
case "$BASE" in
    main|master) ;;
    *) say "! 基线不是 main/master：$BASE（确认这是你想要的合入目标）" ;;
esac

if [ "$PRE" -eq 1 ]; then
    # 只读核对：不 fork、不 push、不开 PR。回答「这条 PR 现在提还成立吗」。
    # 存在的理由：账号被停用期间上游仍在前进，PR 正文里那句「已实测能干净打到 main（<sha>）」
    # 会随时间变成假话。每次准备推送前先跑一遍，把 stale 的 sha 抓出来。
    say "— preflight（只读，不改远端）—"
    head=$(git ls-remote "https://github.com/$UP.git" "refs/heads/$BASE" 2>/dev/null | awk '{print $1}')
    if [ -z "$head" ]; then
        say "✗ 取不到上游 $BASE 的头（网络或代理问题）"
        exit 1
    fi
    say "· 上游 $BASE 当前：$head"
    claim=$(grep -o '`main`（`[0-9a-f]\{7,40\}`' "$BODY" 2>/dev/null | grep -o '[0-9a-f]\{7,40\}' | head -1)
    if [ -z "$claim" ]; then
        say "✗ PR 正文里找不到「已实测的 main sha」——请先补一句，否则 reviewer 无从判断"
        exit 1
    fi
    say "· PR 正文声称实测于：$claim"
    if [ "$claim" != "$head" ]; then
        case "$head" in
            "$claim"*) say "✓ 上游只是把该 sha 往后延伸（前缀一致）" ;;
            *) say "! 上游已前进到 $head（正文写的是 $claim）——推送前先跑 ./scripts/verify_upstream.sh $head 并更新正文" ;;
        esac
    else
        say "✓ 正文 sha 与上游头一致"
    fi
    if command -v gh >/dev/null 2>&1; then
        who=$(gh api user --jq '.login' 2>/dev/null | tr -d '\r\n ')
        case "$who" in
            ''|*[!A-Za-z0-9-]*) say "✗ gh 账号不可用（停用/未登录）⇒ 还不能 push" ;;
            *) say "✓ gh 账号 $who 可用 ⇒ 可以 ./scripts/publish.sh --yes" ;;
        esac
    else
        say "✗ 没有 gh"
    fi
    say "— preflight 结束（未改动任何远端）—"
    exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
    say "✗ 没有 gh（GitHub CLI）。装法与登录：https://cli.github.com/"
    exit 1
fi

me=$(gh api user --jq '.login' 2>/dev/null | tr -d '\r\n ')
# 必须是合法 login（被停用/未登录时 gh 会把 JSON 错误体打在 stdout 上）
case "$me" in
    ''|*[!A-Za-z0-9-]*) me="" ;;
esac
if [ -z "$me" ]; then
    say "✗ gh 未登录，或账号不可用。先跑：gh auth login"
    say "  （2026-11-16 实测本机账号被停用：gh api user → \"Sorry. Your account was suspended\"）"
    exit 1
fi
say "· 账号：$me"

TMP=$(mktemp -d)
say ""
say "将执行（临时目录 $TMP）："
run gh repo fork "$UP" --clone=false --remote=false
run git clone --filter=blob:none --no-checkout "https://github.com/$me/${UP#*/}.git" "$TMP/t"
run git -C "$TMP/t" sparse-checkout set graphics runtime tools compat
run git -C "$TMP/t" checkout "$BASE"
run git -C "$TMP/t" checkout -b "$BRANCH"
run git -C "$TMP/t" apply "$PATCH"
run git -C "$TMP/t" add -A
run git -C "$TMP/t" -c user.name="$me" -c user.email="$me@users.noreply.github.com" \
    commit -m "graphics/nvn: 自动 SDR→HDR（直方图纸白/拐点 + 16F 上屏）"
run git -C "$TMP/t" push -u origin "$BRANCH"
run gh pr create --repo "$UP" --base "$BASE" --head "$me:$BRANCH" \
    --title "graphics/nvn: 自动 SDR→HDR（类 RTX HDR）——直方图驱动纸白/拐点 + 16F 上屏" \
    --body-file "$BODY"

say ""
if [ "$YES" -eq 1 ]; then
    say "✓ 完成。若 gh 提示已存在 PR，用 gh pr view --repo $UP 查看。"
else
    say "（dry-run：以上命令一条都没执行。确认无误后加 --yes）"
fi
