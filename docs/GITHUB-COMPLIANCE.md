# 本仓库的合规约定（可验证，不是承诺）

这个仓库只做一件事：**交付一个补丁**。它不构建 App、不分发二进制、不跑定期 CI。
下面每一条都能用 `./scripts/check_projection.sh` 在本地核验（一条命令，11 组检查——组数由 `check_docs.py` 第 5 条核，不手抄）。

```sh
git clone https://github.com/fwordnvidia/IosAutoHDR
cd IosAutoHDR && ./scripts/check_projection.sh
# → ✓ 公开投影自检通过：仓库性质与 README 声称一致
```

这份文件里写的组数不是手抄的：`scripts/check_docs.py` 第 5 条数 `check_projection.sh` 里
的 `say "N) …"` 标题，与本文出现的「N 组检查」逐字比对——2026-11-18 之前这里写「9 组」，
而脚本已经加到 11 组，合规文档自己报的数不准，读者按它核对时会以为少跑了两组。

## 一、这个仓库**不做**什么

| 不做 | 怎么核验 |
| --- | --- |
| 不跑定期 CI | 源码树里的 `workflows/verify.yml` **只接受 `workflow_dispatch`**，出现 push / pull_request / schedule 即失败（`0w15/5`）。投影默认不带它——推它需要 PAT 的 `workflow` scope，不为一个 CI 文件去扩权 |
| 不并发起虚机 | 该 workflow 有 `concurrency`（同 ref 只跑一个）与 `timeout-minutes: 20`，且**不许用 `matrix`**（那是「一次点按起 N 台」的另一种写法）；本机 `check_workflow_guard.py` 逐条核（投影里带上时由 `./scripts/check_projection.sh` 第 9 组再核一次） |
| 不用 runner 构建 App | workflow 里禁止出现 `xcodebuild` 与 `make`；它只做三件事：`xcrun metal` 编 shader、常数对账、`clang -fsyntax-only` |
| 不含上游整文件 | 无 `nvn_mtl.m` / `host_adapt.m` / `nvn_present_fi.inc`；`patch/` 里只有 diff 文本 |
| 不含构建产物 | 无 `build/`、`__pycache__/`、`*.tar.gz`、`*.bundle`、`*.o`、`*.so` |
| 不吞大文件 | 最大单文件 < 1 MB（补丁 395 KB）；大交付件走 Release，不进 git |
| 不含凭据或内网信息 | 按 token 前缀模式、私网 IP 段、口令、公钥扫全树 = 0 命中；`git remote -v` 无 `@` |

大交付件（完整工作树 bundle、打包 tar.gz）放在别处的一个**私有**裸仓库，不做镜像、不公开。

## 二、为什么会有这份文件

维护者此前的另一个账号（`Kippythisway`）因**托管 runner 的用量与收益不成比例**被停用。
事后按条款逐条自查，命中的是 GitHub Actions 附加条款这两句：

> Any activity that places a burden on our servers, where that burden is disproportionate to the
> benefits provided to users.

> If using GitHub-hosted runners, any other activity unrelated to the production, testing,
> deployment, or publication of the software project associated with the repository.

具体情形：一个 `on: push` + `paths: ['hdr-auto/**']` + `runs-on: macos-26` 的 workflow，
在单个分支上被**约 100 次提交**触发；加上 14 次整包 iOS 构建（每次冷构建 350–450 macOS 分钟，
而免费额度是每月 2000 分钟）。合计约 **114 台 macOS 虚机**。当时还把一个仓库从 private 改成
public 以绕开账单门限——这一点在提交信息里留下了原文，是最难看的一条。

`runs-on: macos-*` 那一行本身并不违规：违规的是**自动触发**和**并发**。所以现在的做法不是
「从此不碰 Actions」，而是把触发方式收成手动、把并发收成一个、把时长收在 20 分钟内，
并且**只用它做本地做不到的那三件事**（Metal 编译、常数对账、ObjC 语法）——这三件在
Ubuntu 上无论跑多少次都跑不出来。跑一次约 10 分钟，公开仓库不消耗额度。

### 为什么敢说「第一次手动跑就能过」

这个 workflow 只能在 macOS 上跑，语法或路径写错一次就白烧一段 runner 时长。所以本地
`check_workflow_guard.py` 把它能在 Linux 上核的部分全核了（共 12 条；第 12 条是「本机装了
`actionlint` 就再交给它跑一遍」——表达式上下文名、runner 标签、`needs` 引用这些手写断言查不到的
东西由它兜底，1.7.9 实测四类错全报；没装时明确打印跳过而不是静默。再装了 `shellcheck`
（0.10.0）时 actionlint 会连每个 `run:` 块的 shell 一起审——本轮靠这一层抓到第四处真错：
`! grep -E ": (fatal )?error:"` 在 `set -e` 下被 bash 豁免（SC2251），clang 真有 error 时
grep 命中、`!` 返回 1，脚本却继续打印「· 语法通过」⇒ 一次手动运行烧掉 10 分钟给出绿色假象；
已改成显式 `if grep …; then exit 1; fi`，最小实验证明旧写法 rc=0、新写法 rc=1）。
本轮靠前 11 条抓到三处真错：

| 抓到的错 | 为什么本地就能发现 |
| --- | --- |
| `clang … host_adapt.m` 假设它和 `nvn_mtl.m` 同目录 | 实际在 `graphics/nvn/host_adapt.m`；对照上游树就知道 |
| `-I/tmp/s2a/build/nvn/gen` | 上游仓库里**没有** `build/` 目录；而且它不在 sparse 集里，clone 出来永远不存在 |
| 每个 `run:` 块没做语法校验 | 现在逐个跑 `bash -n`；`for`/`if` 写错一次就是一次白跑 |
| `! grep -E ": error:"` 判 clang 失败 | `!` 前缀命令的失败在 `set -e` 下被 bash 豁免（SC2251），有 error 也打印「语法通过」⇒ 改成 `if grep …; then exit 1; fi` |

## 三、维护者此后遵守的九条

1. 不用托管 runner 做**重复**构建。唯一允许的 runner 用途是「手动、单次、只编 shader 与语法」；
   其余验证在本地跑 `./run_all.sh`。
2. 不为「换免费额度」把仓库改成 public。仓库可见性与额度无关。
3. 上游代码用 fork 或补丁交付，不整包重新上传。本仓库即补丁交付。
4. API 调用手动、串行：`gh api` 之间 ≥1 s；遇 403/429 立刻停 15 分钟。等一次手动 Run 的结论时
   间隔 ≥90 s、最多查 4 次，不做定时轮询。
5. 构建产物永不入库（`.gitignore` + 卫生检查双重挡）。
6. 单账号，不建第二身份。
7. 私有不构成 mirroring：完整工作树只存于私有裸仓库，不对外提供下载。
8. issue/PR 手动、少量、有具体内容；不做批量互动。
9. 二进制与大文件走 Release，且只在确实需要分发时。

## 四、给使用者的边界

- 这是**个人研究产生的补丁**，不是上游官方分支，也不代表上游立场。
- 上游 `jhaizhou-ops/S2Arm` 是 Apache-2.0；本仓库保留其 `LICENSE` 原文与 `NOTICE` 署名。
- 补丁对**一个特定上游提交**验证过（见 `patch/BASELINE`）。上游前进后需要重放，流程见
  `docs/PR.md` 的「基线迁移」。
 - 设备端行为没有任何承诺：MSL 在本机只做静态核对（72 项 / 4 个 MSL 文件），真编译要靠上面那个手动
  workflow 跑一次（约 10 分钟，需 token 有 `workflow` scope 才能推上去）；ObjC 在 macOS 上做
  `-fsyntax-only`，**没有链接、也没有在真机跑过**。

## 仓库自己的 shell 也过一遍（0w16/5）

给 workflow 的 `run:` 块接了 shellcheck 之后才想明白一件事：仓库里 13 个 `.sh` 从没被审过。
一开就 4 条 warning，其中两条是真错：

| 告警 | 真问题 |
| --- | --- |
| `run_all.sh:9 SC2164` `cd "$(dirname "$0")"` | 不检查失败：进不去时后面每条命令都在调用者 cwd 跑，`mkdir -p build` 会建在错的地方 |
| `fetch_device_log.sh:60 SC2024` | 误报（`/tmp` 本用户可写，重定向 fd 由 root 继承），已用 `# shellcheck disable` + 理由标注 |
| `rebase_patch.sh:32` / `verify_upstream.sh:21` `SC2209` | `MODE=head` 里的 `head` 是命令名，shellcheck 以为你想 `$(head …)`；改名 `MODE=tip` |

还顺带抓到一个**更严重的**：`0w16/5` 第一版照抄了 `check_repo_hygiene.sh` 的 `ok "…"` / `bad "…"`，
可 `run_all.sh` 只定义了 `step()`。运行时是 `./run_all.sh: 159: ok: not found`，而 `fail` 从未被置 1
—— 整体照样「✓ 全部通过」。shellcheck 也看不见（它不跨文件推断函数存在，`ok`/`bad` 又是合法命令名）。

所以补了 `scripts/check_shell_helpers.py`：扫所有 `.sh`，把 `ok/bad/step/die/warn/note/info` 的调用
与文件内定义比对，缺定义即判红（系统里没有叫这些名字的命令，「未定义即 bug」成立）。

## C 侧：正式构建的 `-w` 把编译器关掉了（0w17/5）

`run_all.sh` 里 21 处编译全是 `cc -O2 -w`。`-w` 关闭**全部**警告——编译器是最基础、最便宜的
检查器，等于一直没接上。改成「正式构建继续用 `-w`（不动行为），另加一步用 `-Wall -Wextra`
重编一遍只判警告」：

- 覆盖 46 个带 `main` 的源文件（`src/t_*.c` 31 个单测 + `tools/*.c` 15 个工具）；`src/hdr_*.c`
  是纯库，编成可执行会因缺 `main` 失败，不在射程内。
- 首次审计 46 个里**只有 1 个有问题**：`tools/sweep_darkproxy.c:40` 的 `proxy_gap()` 定义了
  从没被调用——候选 3 的公式早就内联进 `main`（`(gap > gaps[g]) != cpu`）。已删，留下注释说明
  公式去哪了。
- 负向验证：往 `src/t_hdr_head.c` 末尾塞一个没人调用的静态函数 ⇒
  `::error::src/t_hdr_head.c：1 条 warning` + `‘unused_probe’ defined but not used [-Wunused-function]`，
  审计红；删掉即绿。

## Python 侧：脚本就是闸门本身（0w18/5）

34 个 `.py` 从没 lint 过。`ruff check --select E9,F` 一开 7 条，全是真残留：

| 位置 | 问题 |
| --- | --- |
| `check_baseline.py:31`、`check_patch_stats.py:108` | `f"…"` 里一个占位符都没有（本该插值的地方可能曾经有） |
| `check_msl.py:128` | `fn = re.search(…)` 算了不用——注释还写着「从右往左找最后一个名字」，实际函数名改用 `fname` 取了 |
| `plot_curves.py:38` | `nmax` 算出来没人用（图例用的是 `rows[-1][3]` 而不是全局最大） |
| `make_sequence.py:23` | `import struct` 多余 |
| `report_color_image.py:3`、`report_review.py:16` | `os` / `subprocess` 多余（`report_review` 改成读补丁头后 `subprocess` 就废了） |

另加 `B905`：8 处 `zip()` 没写 `strict=`。逐条看语义后分两类——

- **4 处本该等长**（逐像素比较 `zip(px, px3)`、CSV 行配表头 `zip(head, r.split(","))`、两批结果对账 `zip(old_rows, rows)`）⇒ `strict=True`。`zip` 的默认行为是**按最短的截断**，等于「比了一半就宣布通过」，这正是本项目最忌讳的假绿。
- **4 处是相邻对**（`zip(steps, steps[1:])`、`zip(steps, lo[1:])`）⇒ 天生差一个元素，显式写 `strict=False`。

代价：`zip(strict=)` 需要 Python ≥ 3.10，README 已把它写成**显式依赖**（原来只写「依赖 `python3`」，
现在写「`python3` 要 ≥ 3.10，因为用了 `zip(strict=)`」）。改完 5 个受影响脚本复跑全绿
（`report_ablate` 57 行、`report_cost` 最慢 0.6383 µs、`report_video` 25 行、`check_convert` 15 项、
`check_device_log --selftest` 13 项）。

负向验证：往 `check_baseline.py` 末尾加 `import json` ⇒
`scripts/check_baseline.py:92:8: F401 [*] `json` imported but unused`，脚本红；删掉即绿。
