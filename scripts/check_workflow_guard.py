#!/usr/bin/env python3
"""守住 .github/workflows/verify.yml 的「只手动跑」形态。

为什么需要这个脚本（真事）：
  2026-11-17 之前用的 GitHub 号 Kippythisway 因为 workflow 带 push 触发、并发又没上限，
  一天之内起了 114 台 macOS 虚机（每台按分钟计费），账号被停用。这次换号重建之后，
  「只手动触发」不能是 README 里的一句承诺 —— 它必须是被闸门锁住的事实：
    · 触发方式只有 workflow_dispatch，出现 push / pull_request / schedule 就算失败
    · 必须有 concurrency（连点两下不会起两台）
    · timeout-minutes 必须存在且 <= 20（防止一个卡死的 job 烧满 6 小时上限）
    · 不许 continue-on-error: true（失败被吞掉比失败更贵：你会重跑一次）
    · 不许出现 xcodebuild / make（这个 workflow 只编 shader + 语法检查，不 build 整包）
    · 里面写死的那条基线 sha 必须与 patch/BASELINE 第一列一致（否则 clone 到旧提交，
      打补丁直接失败，而失败原因是「基线漂移」，不是「代码坏了」）

用法：python3 scripts/check_workflow_guard.py [--check]
      不带参数也校验；--check 只是让 run_all 的调用方式统一。
"""
import os
import re
import sys

try:
    import yaml
except ImportError:
    sys.exit("✗ 需要 pyyaml：pip3 install pyyaml")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WF = os.path.join(ROOT, ".github", "workflows", "verify.yml")
BASELINE = os.path.join(ROOT, "patch", "BASELINE")

FORBIDDEN_TRIGGERS = ("push", "pull_request", "pull_request_target", "schedule", "workflow_run")


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def main():
    problems = []
    if not os.path.isfile(WF):
        print("✗ 找不到 %s" % os.path.relpath(WF, ROOT))
        return 1
    raw = read(WF)
    try:
        doc = yaml.safe_load(raw)
    except Exception as e:
        print("✗ workflow 不是合法 YAML：%s" % e)
        return 1
    if not isinstance(doc, dict):
        print("✗ workflow 顶层不是映射")
        return 1

    # 1) 触发方式：只允许 workflow_dispatch
    on = doc.get(True) if True in doc else doc.get("on")   # YAML 1.1 会把裸 on 解析成 True
    if isinstance(on, str):
        triggers = [on]
    elif isinstance(on, dict):
        triggers = list(on.keys())
    elif isinstance(on, list):
        triggers = list(on)
    else:
        triggers = []
    bad = [t for t in triggers if str(t) in FORBIDDEN_TRIGGERS]
    if bad:
        problems.append("出现自动触发：%s（会让每个 push 都起一台 macOS 虚机）" % ", ".join(map(str, bad)))
    if "workflow_dispatch" not in [str(t) for t in triggers]:
        problems.append("没有 workflow_dispatch —— 那这个 workflow 就只能靠自动触发跑")

    # 2) concurrency
    if not doc.get("concurrency"):
        problems.append("没有 concurrency（连点两下会起两台虚机）")

    # 3) 每个 job 的 timeout 与 continue-on-error
    jobs = doc.get("jobs") or {}
    if not jobs:
        problems.append("没有 jobs")
    for name, job in jobs.items():
        if not isinstance(job, dict):
            continue
        tm = job.get("timeout-minutes")
        if tm is None:
            problems.append("job %s 没有 timeout-minutes" % name)
        elif not isinstance(tm, int) or tm > 20:
            problems.append("job %s 的 timeout-minutes=%r 超过 20 分钟上限" % (name, tm))
        if job.get("continue-on-error") is True:
            problems.append("job %s 写了 continue-on-error: true（失败会被吞掉）" % name)
        runs_on = str(job.get("runs-on", ""))
        if not runs_on.startswith("macos-"):
            problems.append("job %s 的 runs-on=%r 不是 macos-*" % (name, runs_on))

    # 4) 不许 build 整包
    for word in ("xcodebuild", "^\\s*make\\b"):
        if re.search(word, raw, re.M):
            problems.append("出现 %r —— 这个 workflow 只编 shader 与语法检查，不 build 整包" % word)

    # 5) 写死的基线必须与 patch/BASELINE 一致
    if not os.path.isfile(BASELINE):
        problems.append("找不到 patch/BASELINE")
    else:
        base = read(BASELINE).split()[0]
        shas = set(re.findall(r"\b[0-9a-f]{40}\b", raw))
        if base not in shas:
            problems.append("workflow 里没有出现 patch/BASELINE 的基线 %s（clone 会停在旧提交）" % base[:12])
        for s in sorted(shas - {base}):
            problems.append("workflow 里出现与 BASELINE 不一致的 sha %s" % s[:12])

    if problems:
        for p in problems:
            print("::error::workflow 守卫：%s" % p)
        print("✗ workflow 守卫失败：%d 条" % len(problems))
        return 1

    print("✓ workflow 守卫通过：仅 workflow_dispatch 触发、有 concurrency、"
          "timeout %d 分钟、基线 %s 与 patch/BASELINE 一致"
          % (jobs["verify"]["timeout-minutes"], read(BASELINE).split()[0][:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
