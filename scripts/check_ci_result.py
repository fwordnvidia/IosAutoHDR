#!/usr/bin/env python3
"""判读一次 GitHub Actions 运行：它到底是「真编译过」还是「绿了个寂寞」。

为什么需要它：`.github/workflows/verify.yml` 是本机唯一跑不了的东西（要 macOS + Xcode）。
于是它有两种病都没人管：
  1. 它跑绿了，但 macOS 那一步被跳过 / 什么都没编 —— 结论照样是绿的；
  2. 它其实没跑，我拿 `scripts/rehearse_ci.py` 的预演日志当成 CI 结果来说事。
两种都属于「返回 0 没做事」。所以让它产出**可判读的**日志，判据写在这里。

判据（任意一条不满足就红）：
  R1 本次运行 completed + success；
  R2 没有任何一步 failure/cancelled；名字里带「MSL 真编译」的那一步必须是 success（skipped 也算红——
     它被跳过时 workflow 仍然是绿的，这正是要抓的）；
  R3 日志里每个 MSL 产物都有 `· 编译通过：<文件名> <N> B`（workflow 自己 echo 的那行）；
  R4 日志里两个 ObjC 文件都有 `· 语法通过：<路径>`；
  R5 日志里没有 `[预演]` —— 出现就说明这是 rehearse_ci.py 的输出，不是 CI；
  R6 日志里出现本地 `patch/hdr-auto-l1.patch` 的 sha256 前缀 —— 否则 CI 跑的不是这份补丁；
  R7 日志里有 `✓ 全部通过`（run_all 的结论行）。

用法：
    gh run view <id> --json status,conclusion,jobs > /tmp/run.json
    gh run view <id> --log > /tmp/ci.log
    ./scripts/check_ci_result.py --run-file /tmp/run.json --log-file /tmp/ci.log
    ./scripts/check_ci_result.py --selftest          # 用合成样本自测判据（不需要网络）
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
MSL_FILES = ["src_msl.metal", "src_msl_hdr.metal", "fi_msl_1.metal", "fi_msl_2.metal", "hdr_hist.metal"]
OBJC = ["graphics/nvn/gfx/nvn/nvn_mtl.m", "graphics/nvn/host_adapt.m"]
MAC_STEP = "MSL 真编译"


def patch_sha():
    p = HERE / "patch" / "hdr-auto-l1.patch"
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else ""


def judge(run, log, sha):
    bad = []
    status = run.get("status")
    concl = run.get("conclusion")
    if status != "completed" or concl != "success":
        bad.append("R1 本次运行 status=%r conclusion=%r（要 completed/success）" % (status, concl))
    steps = []
    for job in run.get("jobs") or []:
        for st in job.get("steps") or []:
            steps.append(st)
            c = st.get("conclusion")
            if c in ("failure", "cancelled", "timed_out"):
                bad.append("R2 步骤「%s」结论是 %s" % (st.get("name"), c))
    mac = [s for s in steps if MAC_STEP in (s.get("name") or "")]
    if not mac:
        bad.append("R2 运行里没有「%s」这一步 —— workflow 变了，或者拿错了 run" % MAC_STEP)
    else:
        for s in mac:
            if s.get("conclusion") != "success":
                bad.append("R2 macOS 那一步「%s」结论是 %r —— 它被跳过时整个运行仍然是绿的，"
                           "这才是最容易骗人的绿色" % (s.get("name"), s.get("conclusion")))
    for f in MSL_FILES:
        if not re.search(r"· 编译通过：%s \d+ B" % re.escape(f), log):
            bad.append("R3 日志里没有「· 编译通过：%s <字节数> B」—— 这一个 MSL 没真编" % f)
    for src in OBJC:
        if "· 语法通过：%s" % src not in log:
            bad.append("R4 日志里没有「· 语法通过：%s」" % src)
    if "[预演]" in log:
        bad.append("R5 日志里有「[预演]」—— 这是 scripts/rehearse_ci.py 的输出，不是 CI 的结果")
    if sha and sha[:12] not in log:
        bad.append("R6 日志里找不到本地补丁的 sha256 前缀 %s —— CI 跑的不是这份补丁" % sha[:12])
    if "✓ 全部通过" not in log:
        bad.append("R7 日志里没有 run_all 的「✓ 全部通过」")
    return bad


# ---- 自测样本：一份真的（形状照 workflow 的 echo），四份假的 ----
def _good_run():
    return {"status": "completed", "conclusion": "success", "jobs": [{"steps": [
        {"name": "取上游干净树（sparse: graphics runtime tools compat ios）", "conclusion": "success"},
        {"name": "本地全部闸门（run_all 在干净树上打补丁跑单测；结束后树是打过补丁的）", "conclusion": "success"},
        {"name": "断言补丁确实在树上（MSL/ObjC 检查需要打完补丁的源码）", "conclusion": "success"},
        {"name": "MSL 真编译 + 常数对账 + ObjC 语法（Apple 工具链）", "conclusion": "success"},
    ]}]}


def _good_log(sha):
    out = ["· 补丁 42 文件 +6947 -27", "sha256 %s" % sha]
    out += ["· 编译通过：%s %d B" % (f, 1000 + i) for i, f in enumerate(MSL_FILES)]
    out += ["· 语法通过：%s" % s for s in OBJC]
    out.append("✓ 全部通过")
    return "\n".join(out) + "\n"


SELFTEST = [
    ("good", lambda s: (_good_run(), _good_log(s)), True),
    ("skipped_mac", lambda s: ({"status": "completed", "conclusion": "success", "jobs": [{"steps": [
        {"name": "MSL 真编译 + 常数对账 + ObjC 语法（Apple 工具链）", "conclusion": "skipped"}]}]},
        "✓ 全部通过\n"), False),
    ("no_metal", lambda s: (_good_run(), _good_log(s).replace("· 编译通过：src_msl_hdr.metal", "· 跳过：src_msl_hdr.metal")), False),
    ("rehearsal", lambda s: (_good_run(), _good_log(s) + "· [预演] xcrun 被挖掉，未编译：hdr_hist.metal\n"), False),
    ("stale_patch", lambda s: (_good_run(), _good_log("deadbee" + s[7:])), False),
    ("failed", lambda s: ({"status": "completed", "conclusion": "failure", "jobs": [{"steps": [
        {"name": "MSL 真编译 + 常数对账 + ObjC 语法（Apple 工具链）", "conclusion": "failure"}]}]},
        _good_log(s)), False),
]


def selftest():
    sha = "42f86d35fcc42857b10d1ada30b3371f123973059bbd2181fd9679cb7b233033"
    fails = 0
    for name, make, expect_ok in SELFTEST:
        run, log = make(sha)
        bad = judge(run, log, sha)
        got = not bad
        if got != expect_ok:
            print("  ✗ 自测 %s：期望 %s，实际 %s（%s）" % (name, expect_ok, got, "; ".join(bad) or "无失败项"))
            fails += 1
    n = len(SELFTEST)
    print("%s check_ci_result 自测 %d 项 → %s" % ("✓" if not fails else "✗", n, "全过" if not fails else "%d 项失败" % fails))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-file")
    ap.add_argument("--log-file")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return 1 if selftest() else 0
    if not (a.run_file and a.log_file):
        ap.error("要么 --selftest，要么 --run-file + --log-file")
    run = json.loads(Path(a.run_file).read_text(encoding="utf-8"))
    log = Path(a.log_file).read_text(encoding="utf-8", errors="replace")
    bad = judge(run, log, patch_sha())
    for b in bad:
        print("::error::" + b)
    if bad:
        print("✗ CI 结果判读：%d 处不成立 —— 这次运行不能当作「MSL 真编过」的证据" % len(bad))
        return 1
    print("✓ CI 结果：5 个 MSL 真编、2 个 ObjC 语法通过、补丁指纹一致、run_all 绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
