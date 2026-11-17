# 怎么往这里提东西

这个仓库只交付**一个补丁**（`patch/hdr-auto-l1.patch`）与它的验证手段。它不构建 App、
不分发二进制、不跑定期 CI。所以「贡献」在这里的范围很窄，规则也就很硬。

## 一、先跑这两条，再开 PR

```sh
./run_all.sh /path/to/S2Arm          # 31 套 904 项 + 落点 + MSL 静态 + 报告对账 + ASan/UBSan
./scripts/check_projection.sh        # 11 组：仓库性质、无整文件、无产物、无凭据、换行、可用性
```

第一条需要一棵干净的 S2Arm 树（`sparse-checkout set graphics runtime tools compat ios`）。
两条都必须绿；PR 描述里贴最后几行输出就行，不需要贴全量日志。

## 二、几条会被自动拦下的做法

| 你的改动 | 谁拦 | 怎么改 |
|---|---|---|
| 改了 `src/` 却没重建补丁 | `4e/5`（`check_patch_fresh.py`） | `./scripts/make_patch.sh <树>` |
| 补丁里加了 `src/` 没有的文件 | 同上（反向那一半） | 要么放进 `src/`，要么别放补丁 |
| 改了 `run_all.sh` 的步骤号却没改文档 | `check_docs.py` 第 4 条 | 文档里的 `0j/5` 这类必须真实存在 |
| 文档里写了投影自检的组数但数不对 | `check_docs.py` 第 5 条 | 组数以 `check_projection.sh` 的 `say "N) …"` 为准 |
| 把上游整文件（`nvn_mtl.m` 等）拷进来 | `check_projection.sh` 第 2 组 | 只交付 diff；补丁打在上游树上 |
| 提交 `build/`、`*.tar.gz`、`.bundle` | 第 3 组 | 大件放 Release 或私有裸仓库 |
| 给 workflow 加 `push` / `schedule` / `matrix` | `0w15/5`（12 条） | 只 `workflow_dispatch`、20 分钟、不并发 |

## 三、迁移基线（上游前进之后）

```sh
./scripts/rebase_patch.sh --head        # 打→查 .rej→反打→比 status，无损才算过
./scripts/check_upstream.sh --full      # 顺带把补丁打到上游当前 main 上跑一遍全部闸门
```

改完基线要同步三处：`patch/BASELINE`、`NOTICE` 里的补丁适用版本、`docs/BASELINE-MIGRATION.md`
（工单式记录每个冲突怎么合的）。`NOTICE` 里那个提交号由 `check_docs.py` 第 6 条核对，
写错会红。

## 四、不要为了这个仓库去扩大 token 权限

`.github/workflows/verify.yml` 只做三件事（`xcrun metal` 编 shader、常数对账、
`clang -fsyntax-only`），且只接受手动触发。推它需要 PAT 的 `workflow` scope；**不为一个
CI 文件扩权**是这个仓库的既定选择，投影默认不带它（`SYNC_CI=1` 才拷）。

## 五、回复预期

单人维护、业余时间做，按周看 issue / PR，不承诺时限。若你提的是设备实测数据
（`S2A_HDR_LOG=1` 的 `HDR stat:` 行，或三格矩阵 T1/T2/T3 的截图 + 日志），会优先看，
那是这个补丁目前最缺的证据。

## 六、行为准则与安全

讨论以技术为主，基本的相处规则写在 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

安全相关见下一节——漏洞不要开公开 issue。

## 七、安全相关

见 [`SECURITY.md`](SECURITY.md)。请不要在公开 issue 里贴可直接利用的 PoC。
