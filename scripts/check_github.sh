#!/bin/sh
# GitHub 那一侧的声称，也要能被核。
#
# 到目前为止所有闸门都只核「本地这棵树」：补丁能不能打、单测过不过、投影里该有的文件在不在。
# 而 README 与 docs/GITHUB-COMPLIANCE.md 还有一批只有 GitHub 才知道的声称：
#
#   - 许可是 Apache-2.0（GitHub 自己识别出来的那个 spdx_id，不是我嘴上说的）
#   - 默认分支是 main（推送命令到处写 `HEAD:main`）
#   - 公开、未归档、不是 fork（「公开投影」的定义）
#   - 不跑定期 CI：远端不能有任何自动触发的 workflow
#   - 远端 main 的 sha 就是本地投影的 HEAD（不然「已推送」只是本地信念）
#   - 社区档案（GitHub 自己算的 health_percentage）认得我们的 CONTRIBUTING / SECURITY
#     —— 「仓库里有这个文件」和「GitHub 把它当成社区档案」是两件事：文件名放错目录
#     （比如放 docs/ 下）文件在、GitHub 不认，仓库页照样提示「添加贡献指南」。
#     SECURITY.md 还有个第二层：社区档案里的 security_policy 只有在
#     **私密漏洞报告开关**打开时才非空（`private-vulnerability-reporting` 的 enabled），
#     而那个开关要 `Administration:write` —— 本机 PAT 没有（PUT 返回 403），只能网页面板点。
#     所以判据分两层：文件在远端 main 上（我们能保证）＝硬要求；
#     GitHub 认不认（开关在我们手上之外）＝提示。
#
# 只读：本脚本不发任何修改请求。需要网络，所以不进 run_all.sh（那样每跑一次闸门都要出门）。
#
# 用法：scripts/check_github.sh [owner/repo]
#   TOKEN 环境变量优先，否则读 ~/.config/gh-new/token；
#   HTTP 代理默认 http://127.0.0.1:7890（本机唯一的境外出口），可用 PROXY= 覆盖成空。
set -u
REPO=${1:-fwordnvidia/IosAutoHDR}
TOKEN=${TOKEN:-$(cat "$HOME/.config/gh-new/token" 2>/dev/null)}
[ -n "$TOKEN" ] || { echo "✗ 没有 token（TOKEN=… 或 ~/.config/gh-new/token）"; exit 1; }
PROXY=${PROXY-http://127.0.0.1:7890}
HERE=$(cd "$(dirname "$0")/.." && pwd)

api() {  # api <path> [jq-ish python 表达式]
    if [ -n "$PROXY" ]; then
        curl -sS -m 20 -x "$PROXY" -H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json" \
            "https://api.github.com$1"
    else
        curl -sS -m 20 -H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json" \
            "https://api.github.com$1"
    fi
}

fail=0
bad() { echo "  ✗ $*"; fail=1; }
ok()  { echo "  ✓ $*"; }

JSON=$(api "/repos/$REPO")
case "$JSON" in
    *'"message"'*) echo "✗ 取不到仓库信息：$(printf '%s' "$JSON" | head -c 200)"; exit 1 ;;
esac

echo "=== $REPO 的远端状态 ==="
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
printf '%s' "$JSON" > "$TMP/repo.json"
python3 - "$TMP/repo.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
lic = (d.get("license") or {}).get("spdx_id")
rows = [
    ("许可（GitHub 识别）", lic, "Apache-2.0"),
    ("默认分支", d.get("default_branch"), "main"),
    ("可见性", d.get("visibility"), "public"),
    ("归档", d.get("archived"), False),
    ("fork", d.get("fork"), False),
]
for name, got, want in rows:
    if got == want:
        shown = ("是" if got else "否") if isinstance(got, bool) else got
        print(f"  ✓ {name}：{shown}")
    else:
        print(f"  ✗ {name}：实际 {got}，README/合规文档声称 {want}")
        sys.exit(3)
PY
[ $? -eq 3 ] && fail=1

echo "=== 远端有没有自动触发的 workflow ==="
WL=$(api "/repos/$REPO/actions/workflows")
printf '%s' "$WL" > "$TMP/wf.json"
python3 - "$TMP/wf.json" "$REPO" "$PROXY" "$TOKEN" <<'PY' || fail=1
import json, re, subprocess, sys
repo, proxy, token = sys.argv[2], sys.argv[3], sys.argv[4]
d = json.load(open(sys.argv[1], encoding="utf-8"))
ws = d.get("workflows") or []
if not ws:
    print("  ✓ 远端 0 个 workflow（投影默认不带 CI 文件——推它要 PAT 的 workflow scope）")
    raise SystemExit(0)
bad = 0
for w in ws:
    path, name = w.get("path", "?"), w.get("name", "?")
    cmd = ["curl", "-sS", "-m", "20", "-H", f"Authorization: token {token}",
           "-H", "Accept: application/vnd.github.raw",
           f"https://api.github.com/repos/{repo}/contents/{path}"]
    if proxy:
        cmd[1:1] = ["-x", proxy]
    txt = subprocess.run(cmd, capture_output=True, text=True).stdout
    trig = re.findall(r"^\s{0,4}(push|pull_request|pull_request_target|schedule|workflow_run|workflow_dispatch)\s*:", txt, re.M)
    auto = [t for t in trig if t != "workflow_dispatch"]
    if auto:
        print(f"  ✗ {path}（{name}）有自动触发：{sorted(set(auto))} —— 合规文档写的是「不跑定期 CI」")
        bad = 1
    elif "workflow_dispatch" in trig:
        print(f"  ✓ {path}（{name}）只有 workflow_dispatch")
    else:
        print(f"  ✗ {path}（{name}）连 workflow_dispatch 都没有（那是怎么跑的？）")
        bad = 1
raise SystemExit(1 if bad else 0)
PY

echo "=== 远端 main 与本地投影的 HEAD 是否同一个提交 ==="
if [ -d "$HERE/.git" ]; then
    local_head=$(git -C "$HERE" rev-parse HEAD 2>/dev/null)
    remote_head=$(git -C "$HERE" ls-remote origin main 2>/dev/null | cut -f1)
    if [ -n "$local_head" ] && [ "$local_head" = "$remote_head" ]; then
        ok "远端 main = 本地 HEAD = $(echo "$local_head" | cut -c1-12)"
    else
        bad "远端 main（${remote_head:-取不到}）≠ 本地 HEAD（${local_head:-?}）—— 「已推送」只是本地信念"
    fi
else
    echo "  · 当前目录不是 git 仓库，跳过 HEAD 比对（这条只在投影根有意义）"
fi

echo "=== 社区档案：文件在不在远端 main、GitHub 认不认 ==="
# 第一层：文件是否真的在远端 main 上（这一层我们能保证，缺了就是没推）
python3 - "$REPO" "$PROXY" "$TOKEN" <<'PY'
import json, subprocess, sys
repo, proxy, token = sys.argv[1], sys.argv[2], sys.argv[3]
bad = 0
for f in ("CONTRIBUTING.md", "SECURITY.md"):
    cmd = ["curl", "-sS", "-m", "20", "-H", f"Authorization: token {token}",
           "-H", "Accept: application/vnd.github+json",
           f"https://api.github.com/repos/{repo}/contents/{f}?ref=main"]
    if proxy:
        cmd[1:1] = ["-x", proxy]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    try:
        d = json.loads(out)
    except Exception:
        d = {}
    if d.get("path"):
        print(f"  ✓ {f} 在远端 main 上（{d.get('size')} B，sha {str(d.get('sha'))[:12]}）")
    else:
        print(f"  ✗ 远端 main 上没有 {f} —— 本地有不算，得推上去")
        bad = 1
raise SystemExit(1 if bad else 0)
PY
[ $? -eq 0 ] || fail=1

# 第二层：GitHub 的社区档案认不认（CONTRIBUTING 归我们管；SECURITY 还受开关限制）
CP=$(api "/repos/$REPO/community/profile")
printf '%s' "$CP" > "$TMP/comm.json"
PVR=$(api "/repos/$REPO/private-vulnerability-reporting")
printf '%s' "$PVR" > "$TMP/pvr.json"
python3 - "$TMP/comm.json" "$TMP/pvr.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
pvr = json.load(open(sys.argv[2], encoding="utf-8"))
if "health_percentage" not in d:
    print(f"  ✗ 取不到社区档案：{str(d)[:160]}")
    raise SystemExit(1)
hp = d["health_percentage"]
files = d.get("files") or {}
bad = 0
c = files.get("contributing")
if c and c.get("html_url"):
    print(f"  ✓ GitHub 认到 CONTRIBUTING：{c.get('path') or c.get('html_url')}")
else:
    print("  ✗ GitHub 没认到 CONTRIBUTING —— 文件名要对（CONTRIBUTING.md），位置只认根级或 .github/")
    bad = 1
sp = files.get("security_policy")
pvr_on = bool(pvr.get("enabled"))
if sp and sp.get("html_url"):
    print(f"  ✓ GitHub 认到 SECURITY：{sp.get('path') or sp.get('html_url')}")
elif pvr_on:
    print("  ✗ 私密漏洞报告已开，但社区档案仍没认到 SECURITY —— 文件内容/位置有问题")
    bad = 1
else:
    print("  · SECURITY.md 在远端，但社区档案里 security_policy 为空 —— 因为「私密漏洞报告」"
          "开关是关的（private-vulnerability-reporting.enabled = false）")
    print("    这个开关要 Administration:write，本机 PAT 没有（PUT 返回 403）⇒ 网页面板 "
          "Settings → Code security → Private vulnerability reporting 点开即可")
print(f"  · health_percentage：{hp}%")
if hp < 50:
    print(f"  ✗ 社区档案只有 {hp}%（缺的项见上）—— 仓库页会一直提示「添加贡献指南」")
    bad = 1
raise SystemExit(1 if bad else 0)
PY
[ $? -eq 0 ] || fail=1

echo "=== 打开仓库能看到的元数据（不影响判定，只提示） ==="
python3 - "$TMP/repo.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
desc = (d.get("description") or "").strip()
topics = d.get("topics") or []
print(f"  · 描述：{desc or '（空）'}")
print(f"  · topics：{', '.join(topics) if topics else '（空——GitHub 搜索里几乎找不到）'}")
print(f"  · wiki：{'开' if d.get('has_wiki') else '关'}｜issues：{'开' if d.get('has_issues') else '关'}")
if not topics or not desc:
    print("  · 提示：这两项要用带 Administration:write 的 token 才能改（本机 fine-grained PAT 没有），"
          "网页面板 Settings 里手工填")
PY

if [ "$fail" -ne 0 ]; then
    echo "✗ 远端状态与仓库里的声称不一致"
    exit 1
fi
echo "✓ 远端状态与仓库里的声称一致"
