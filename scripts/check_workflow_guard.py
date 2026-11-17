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
import shutil
import subprocess
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

    # 4) 不许引入额外凭据面：这个 workflow 只用 checkout 的默认 token
    if re.search(r"\$\{\{\s*secrets\.", raw):
        problems.append("出现 secrets.*：这个 workflow 不需要任何密钥，引入它只是多一个泄露面")

    # 5) 不许 build 整包
    for word in ("xcodebuild", "^\\s*make\\b"):
        if re.search(word, raw, re.M):
            problems.append("出现 %r —— 这个 workflow 只编 shader 与语法检查，不 build 整包" % word)

    # 6) 不许用 matrix：它是「一次起多台虚机」的另一种写法
    for job_id, job in (doc.get("jobs") or {}).items():
        if isinstance(job, dict) and job.get("strategy", {}).get("matrix"):
            problems.append("job %s 用了 strategy.matrix：一次 push/点按就能起 N 台虚机" % job_id)

    # 7) uses: 必须钉版本，不许追 main/master（上游一改行为就变，且不可复核）
    for m in re.finditer(r"uses:\s*([^\s#]+)", raw):
        ref = m.group(1)
        if "@" not in ref:
            problems.append("uses: %s 没钉版本" % ref)
        else:
            tag = ref.split("@", 1)[1]
            if tag in ("main", "master", "HEAD"):
                problems.append("uses: %s 追分支而不是版本" % ref)

    # 8) 每个 run: 块的 shell 语法都要在本地先过一遍 ——
    #    这个 workflow 只有在 macOS 上才跑得起来，语法错一次就白烧一段 runner 时长。
    steps = []
    for job_id, job in (doc.get("jobs") or {}).items():
        if isinstance(job, dict):
            for i, step in enumerate(job.get("steps") or []):
                if isinstance(step, dict) and step.get("run"):
                    steps.append((job_id, i, step.get("name", ""), step["run"]))
    for job_id, i, name, body in steps:
        r = subprocess.run(["bash", "-n"], input=body, text=True, capture_output=True)
        if r.returncode != 0:
            first = (r.stderr.strip().splitlines() or ["?"])[0]
            problems.append("job %s 第 %d 步「%s」的 run 脚本 bash -n 失败：%s" % (job_id, i + 1, name, first))

    # 9) run 块里引用的树内文件必须真的存在（引用错路径 = 第一次手动跑就红）
    for job_id, i, name, body in steps:
        for ref in re.finditer(r"(?<![\w/.-])((?:src|scripts|tools|testdata|patch|docs)/[\w./-]+\.(?:sh|py|c|m|metal|inc|h|patch|ppm))", body):
            rel = ref.group(1)
            if not any(os.path.exists(os.path.join(base, rel)) for base in (ROOT, os.path.dirname(ROOT))):
                problems.append("job %s 第 %d 步引用了不存在的文件：%s" % (job_id, i + 1, rel))

    # 10) 引用到的上游路径必须落在 sparse 集里 —— 否则 clone 出来的树里根本没有它
    #     （2026-11-17 真实踩到：-I/tmp/s2a/build/nvn/gen 指向仓库里不存在的目录）
    sparse_lines = re.findall(r"git sparse-checkout (?:set|add) ([^\n\\]+)", raw)
    dirs = set()
    for line in sparse_lines:
        dirs.update(t for t in line.split() if not t.startswith("#"))
    for m in re.finditer(r"/tmp/s2a/([A-Za-z0-9_.-]+)", raw):
        seg = m.group(1)
        if dirs and seg not in dirs and seg != "s2a":
            problems.append("引用了 /tmp/s2a/%s，但它不在 sparse 集（%s）里 ⇒ 干净树里不存在"
                            % (seg, " ".join(sorted(dirs))))

    # 11) 写死的基线必须与 patch/BASELINE 一致
    if not os.path.isfile(BASELINE):
        problems.append("找不到 patch/BASELINE")
    else:
        base = read(BASELINE).split()[0]
        shas = set(re.findall(r"\b[0-9a-f]{40}\b", raw))
        if base not in shas:
            problems.append("workflow 里没有出现 patch/BASELINE 的基线 %s（clone 会停在旧提交）" % base[:12])
        for s in sorted(shas - {base}):
            problems.append("workflow 里出现与 BASELINE 不一致的 sha %s" % s[:12])

    # 12 条：有 actionlint 就再跑一遍。手写断言查不到「表达式上下文名拼错」「runner 标签不存在」
    # 「needs 指向不存在的 job」这类问题，actionlint 专治这些（1.7.9 实测：macos-99 / needs: nonexistent /
    # github.no_such_context / timeout "abc" 四处全报）。它是可选依赖，没装时明确说跳过，不静默。
    al = shutil.which("actionlint")
    wf = os.path.join(ROOT, ".github", "workflows", "verify.yml")
    if al and os.path.exists(wf):
        r = subprocess.run([al, "-shellcheck=", "-oneline", str(wf)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            for line in (r.stdout or r.stderr).strip().split("\n")[:6]:
                msg = line.split(": ", 1)[-1]
                if len(msg) > 180:   # runner-label 那条会把整张标签表倒出来，截断留诊断要点
                    msg = msg[:180] + " …（标签表已截断）"
                problems.append("actionlint 报：%s" % msg)
        else:
            print("· actionlint %s：无告警" % subprocess.run([al, "--version"], capture_output=True,
                                                             text=True).stdout.split()[0])
    else:
        print("· 未装 actionlint —— 这一层跳过（brew install actionlint，或下 release 的静态二进制放 PATH）")

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
