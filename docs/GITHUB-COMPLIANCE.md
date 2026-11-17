# 本仓库的合规约定（可验证，不是承诺）

这个仓库只做一件事：**交付一个补丁**。它不构建 App、不分发二进制、不跑定期 CI。
下面每一条都能用 `./scripts/check_projection.sh` 在本地核验（一条命令，9 组检查）。

```sh
git clone https://github.com/fwordnvidia/IosAutoHDR
cd IosAutoHDR && ./scripts/check_projection.sh
# → ✓ 公开投影自检通过：仓库性质与 README 声称一致
```

## 一、这个仓库**不做**什么

| 不做 | 怎么核验 |
| --- | --- |
| 不跑定期 CI | 源码树里的 `workflows/verify.yml` **只接受 `workflow_dispatch`**，出现 push / pull_request / schedule 即失败（`0w15/5`）。投影默认不带它——推它需要 PAT 的 `workflow` scope，不为一个 CI 文件去扩权 |
| 不并发起虚机 | 该 workflow 有 `concurrency`（同 ref 只跑一个）与 `timeout-minutes: 20`；本机 `check_workflow_guard.py` 逐条核（投影里带上时由第 9 组再核一次） |
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
 - 设备端行为没有任何承诺：MSL 在本机只做静态核对（42 项守卫），真编译要靠上面那个手动
  workflow 跑一次（约 10 分钟，需 token 有 `workflow` scope 才能推上去）；ObjC 在 macOS 上做
  `-fsyntax-only`，**没有链接、也没有在真机跑过**。
