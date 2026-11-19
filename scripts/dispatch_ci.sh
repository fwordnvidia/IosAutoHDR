#!/bin/sh
# 一条命令把「推 workflow → 触发 → 等 → 判读」串起来，并判它到底真编过没有。
#
# 为什么要有它：`.github/workflows/verify.yml` 在本机跑不了，只能靠 GitHub 的 macOS runner。
# 而手工走这一趟要十来步（SYNC_CI 同步、提交、推送、dispatch、轮询 run、下载日志、判读），
# 中间任何一步「以为成功」都会让结论变假。这里把顺序固定，并把**判读**交给
# scripts/check_ci_result.py（它认 R1–R7，包括「macOS 那步被 skipped 也是绿」）。
#
# 默认 dry-run：只打印将要执行的命令，不改远端、不触发。
#   ./scripts/dispatch_ci.sh                     # 看会做什么（含前置条件自检）
#   ./scripts/dispatch_ci.sh --yes               # 真的推 + 触发 + 等 + 判读
#   ./scripts/dispatch_ci.sh --yes --timeout 1800
#
# 前置条件（dry-run 也会查，缺哪条就直说，不硬闯）：
#   1) 投影目录存在且是 git 仓库（默认 /home/sechs/hdr-auto-release，可用 S2A_PROJ= 覆盖）
#   2) token 有 Workflows: Read and write —— 否则推送 / dispatch 都会 403
#      （细粒度 PAT 的这个权限只能网页勾；推送 .github/workflows/* 时就要求它）
#   3) workflow 自己先过一遍结构守卫（只许 workflow_dispatch、pin 版本、无 matrix…）
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
PROJ=${S2A_PROJ:-/home/sechs/hdr-auto-release}
REPO=${S2A_REPO:-fwordnvidia/IosAutoHDR}
REF=${S2A_REF:-main}
PROXY=${PROXY-http://127.0.0.1:7890}
TOKEN=${TOKEN:-$(cat "$HOME/.config/gh-new/token" 2>/dev/null)}
TIMEOUT=${S2A_CI_TIMEOUT:-1500}
YES=0
while [ $# -gt 0 ]; do
    case $1 in
        --yes) YES=1 ;;
        --timeout) shift; TIMEOUT=${1:-1500} ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "✗ 不认识的参数：$1（-h 看用法）"; exit 2 ;;
    esac
    shift
done

say() { echo "· $*"; }
die() { echo "✗ $*"; exit 1; }
# 所有 API 调用都带代理：本机唯一的境外出口（直连 api.github.com 会被墙）
api() {
    if [ -n "$PROXY" ]; then
        curl -sS -m 30 -x "$PROXY" -H "Authorization: token $TOKEN" \
            -H "Accept: application/vnd.github+json" "https://api.github.com$1"
    else
        curl -sS -m 30 -H "Authorization: token $TOKEN" \
            -H "Accept: application/vnd.github+json" "https://api.github.com$1"
    fi
}

echo "=== 前置条件 ==="
[ -n "$TOKEN" ] || die "没有 token（TOKEN=… 或 ~/.config/gh-new/token）"
[ -d "$PROJ/.git" ] || die "$PROJ 不是 git 仓库（S2A_PROJ= 指到公开投影的 clone 上）"
[ -f "$HERE/.github/workflows/verify.yml" ] || die "源码仓里没有 .github/workflows/verify.yml"
python3 "$HERE/scripts/check_workflow_guard.py" --require >/dev/null \
    || die "workflow 自己就过不了结构守卫（先跑 scripts/check_workflow_guard.py --require 看哪条）"
say "workflow 结构守卫：通过"

# token 的 scopes 头是唯一能在触发前就知道「会不会 403」的地方
HDRS=$(curl -sS -m 20 ${PROXY:+-x "$PROXY"} -D - -o /dev/null \
    -H "Authorization: token $TOKEN" "https://api.github.com/user" 2>/dev/null)
SCOPES=$(printf '%s' "$HDRS" | tr -d '\r' | sed -n 's/^[Xx]-[Oo]auth-[Ss]copes: //p')
if [ -n "$SCOPES" ]; then
    say "token scopes：$SCOPES"
    case $SCOPES in
        *workflow*) say "  含 workflow —— 可以推 .github/workflows/ 并 dispatch" ;;
        *) echo "  ✗ 不含 workflow：推送 workflow 文件与 dispatch 都会 403。" \
                "细粒度 PAT 要到 Settings → Developer settings 里勾 Workflows: Read and write（网页操作）" ;;
    esac
else
    say "token 是细粒度 PAT（没有 X-OAuth-Scopes 头）：workflow 权限只能试了才知道"
fi

echo "=== 计划 ==="
say "1) SYNC_CI=1 ./scripts/sync_release.sh $PROJ"
say "2) cd $PROJ && git add -A && git commit（有改动才提交）&& push origin HEAD:$REF"
say "3) POST /repos/$REPO/actions/workflows/verify.yml/dispatches {\"ref\":\"$REF\"}"
say "4) 轮询 run 直到 completed（上限 ${TIMEOUT}s）"
say "5) gh run view <id> --log > ci.log；jobs → run.json"
say "6) ./scripts/check_ci_result.py --run-file run.json --log-file ci.log"

if [ "$YES" != "1" ]; then
    echo "=== dry-run：以上命令一条都没执行（要真做加 --yes）==="
    exit 0
fi

echo "=== 1) 同步（带 CI）==="
SYNC_CI=1 "$HERE/scripts/sync_release.sh" "$PROJ" || die "同步失败"

echo "=== 2) 提交并推送 ==="
WORK=$(mktemp -d "${TMPDIR:-/tmp}/s2a-ci.XXXXXX") || die "建临时目录失败"
trap 'rm -rf "$WORK"' EXIT
# 用私有临时 ASKPASS 传凭据，不把 token 拼进 `git -c` 命令行（避免 ps 暴露与元字符注入）
ASKPASS="$WORK/askpass.sh"
TOKEN_FILE="$WORK/token"
(umask 077 && printf '%s' "$TOKEN" > "$TOKEN_FILE")
cat <<'EOF' > "$ASKPASS"
#!/bin/sh
case "$1" in
    *Username*) printf '%s\n' "${S2A_GH_USER:-fwordnvidia}" ;;
    *) cat "$S2A_GH_TOKEN_FILE" ;;
esac
EOF
chmod 700 "$ASKPASS"
cd "$PROJ" || die "进不去 $PROJ"
if [ -n "$(git status --porcelain)" ]; then
    git add -A
    git -c user.name=YG -c user.email=kippyiloveyou@gmail.com \
        commit -q -m "ci: 同步 verify.yml（dispatch_ci.sh）" || die "提交失败"
    say "已提交 $(git log --oneline -1)"
else
    say "投影无改动，直接推"
fi
HEAD_SHA=$(git rev-parse HEAD)
if [ -n "$PROXY" ]; then
    GIT_TERMINAL_PROMPT=0 GIT_ASKPASS="$ASKPASS" S2A_GH_USER="${REPO%%/*}" S2A_GH_TOKEN_FILE="$TOKEN_FILE" \
        git -c http.proxy="$PROXY" -c credential.helper= \
        push origin "HEAD:$REF" || die "推送失败（workflow 文件被拒 = token 缺 Workflows: Read and write）"
else
    GIT_TERMINAL_PROMPT=0 GIT_ASKPASS="$ASKPASS" S2A_GH_USER="${REPO%%/*}" S2A_GH_TOKEN_FILE="$TOKEN_FILE" \
        git -c credential.helper= \
        push origin "HEAD:$REF" || die "推送失败"
fi
say "推送完成：$(git rev-parse --short HEAD)"

echo "=== 3) 触发 ==="
BODY='{"ref":"'"$REF"'","inputs":{"reason":"dispatch_ci.sh：真编译 MSL 与 ObjC 语法"}}'
if [ -n "$PROXY" ]; then
    CODE=$(curl -sS -m 30 -o "$WORK/dispatch_resp.json" -w '%{http_code}' -x "$PROXY" \
        -X POST -H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$REPO/actions/workflows/verify.yml/dispatches" -d "$BODY")
else
    CODE=$(curl -sS -m 30 -o "$WORK/dispatch_resp.json" -w '%{http_code}' \
        -X POST -H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$REPO/actions/workflows/verify.yml/dispatches" -d "$BODY")
fi
[ "$CODE" = "204" ] || die "dispatch 返回 $CODE（403 = token 缺 Workflows: Read and write；响应：$(head -c 200 "$WORK/dispatch_resp.json")）"
say "已触发（204）"

echo "=== 4) 等待运行 ==="
RUN=""
i=0
while [ "$i" -lt 20 ]; do
    i=$((i + 1))
    RUN=$(api "/repos/$REPO/actions/runs?event=workflow_dispatch&per_page=5" \
        | python3 -c 'import json,sys; sha=sys.argv[1]; d=json.load(sys.stdin); hits=[r for r in (d.get("workflow_runs") or []) if r.get("head_sha")==sha]; print(hits[0].get("id","") if hits else "")' "$HEAD_SHA" 2>/dev/null)
    [ -n "$RUN" ] && break
    sleep 5
done
[ -n "$RUN" ] || die "20 次轮询都没看到 head_sha=$HEAD_SHA 的 workflow_dispatch 运行（文件真在 $REF 上吗？）"
say "run id：$RUN"

POLL=${S2A_CI_POLL:-90}
waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
    ST=$(api "/repos/$REPO/actions/runs/$RUN" \
        | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("status",""), d.get("conclusion",""))')
    case $ST in
        "completed "*) say "运行结束：$ST"; break ;;
        *) say "…运行中（已等 ${waited}s）"; sleep "$POLL"; waited=$((waited + POLL)) ;;
    esac
done
[ "$waited" -lt "$TIMEOUT" ] || die "等了 ${TIMEOUT}s 还没结束；去 Actions 页看，或用 run id $RUN 手工取日志"

echo "=== 5) 取日志与结论 ==="
api "/repos/$REPO/actions/runs/$RUN" > "$WORK/ci_run.json" || die "取 run 失败"
api "/repos/$REPO/actions/runs/$RUN/jobs" > "$WORK/ci_jobs.json" || die "取 jobs 失败"
python3 - "$WORK/ci_run.json" "$WORK/ci_jobs.json" "$WORK/ci_run_full.json" <<'PY' || die "拼 run.json 失败"
import json, sys
run = json.load(open(sys.argv[1]))
jobs = json.load(open(sys.argv[2]))
run['jobs'] = jobs.get('jobs') or []
json.dump(run, open(sys.argv[3], 'w'))
PY
# 日志走 gh（它知道怎么拉 zip 并展开）：没有 gh 就退化成提示
if command -v gh >/dev/null 2>&1; then
    GH_HOST=github.com HTTPS_PROXY=${PROXY:-} gh run view "$RUN" -R "$REPO" --log > "$WORK/ci.log" 2>/dev/null \
        || say "gh 取日志失败（可能没登录）；日志文件为空会让判读报 R3/R4"
else
    say "没有 gh：请手工把日志存成 ci.log（判读会被 R3/R4 判红）"
    : > "$WORK/ci.log"
fi

echo "=== 6) 判读 ==="
python3 "$HERE/scripts/check_ci_result.py" --run-file "$WORK/ci_run_full.json" --log-file "$WORK/ci.log"
