# 本仓库的合规约定（可验证，不是承诺）

这个仓库只做一件事：**交付一个补丁**。它不跑 CI、不构建、不分发二进制。
下面每一条都能用 `./scripts/check_projection.sh` 在本地核验（一条命令，7 组检查）。

```sh
git clone https://github.com/fwordnvidia/IosAutoHDR
cd IosAutoHDR && ./scripts/check_projection.sh
# → ✓ 公开投影自检通过：仓库性质与 README 声称一致
```

## 一、这个仓库**不做**什么

| 不做 | 怎么核验 |
| --- | --- |
| 不含任何 Actions 配置 | 无 `.github/`，全树 `.yml`/`.yaml` 数量为 0 |
| 不触发托管 runner | 上一条的直接后果：没有 workflow 就没有 job |
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

所以这个仓库**从设计上就拿掉了那个能力**：没有 `.github/` 目录，谁也没法在这里起 runner。

## 三、维护者此后遵守的九条

1. 不用托管 runner 做重复构建。本仓库无 CI；需要验证时在本地跑 `./run_all.sh`。
2. 不为「换免费额度」把仓库改成 public。仓库可见性与额度无关。
3. 上游代码用 fork 或补丁交付，不整包重新上传。本仓库即补丁交付。
4. API 调用手动、串行：`gh api` 之间 ≥1 s；遇 403/429 立刻停 15 分钟，不轮询 run 状态。
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
- 没有任何设备端行为被承诺：MSL 只做过静态核对，ObjC 只做过语法检查，未在真机上运行。
