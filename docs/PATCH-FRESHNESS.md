# 补丁的新鲜度：它还能打到上游最新代码上吗

本文是一次**实测快照**，不是永久保证。上游一动，重跑 `./scripts/check_upstream.sh`
（只读，不推送）拿到新结论；要连"打上之后还能不能过闸门"一起验，加 `--full`。

## 2026-11-17 实测

| 项 | 值 |
| --- | --- |
| 补丁基线（`patch/BASELINE`） | `d5cbf7ff53f75b6132b17ce3a442d431bbbc8726`（2026-11-17） |
| 上游默认分支 | `main` = `b66dc5644a6f`（第一次采样）→ `b49de8179a00`（十分钟后再采，又前进） |
| 上游最新 tag | `v0.5.4` = `5c3fedbdeb6c` |
| `git apply --check` 到 `main` | **通过**（无 hunk 被拒） |
| 实打上后 `./run_all.sh <该树>` | **rc=0，149 项 ✓**（两次采样都是 149 项，`--full` 用时 2m26s） |
| 上游 `graphics/runtime/tools/compat` 变动 | 81 个文件，+1073 −145 |
| 与我们补丁**重叠**的文件 | 仅 `graphics/nvn/gfx/nvn/nvn_mtl.m`（上游 +75 −7，改的是 PSO 首遇等待 `fu_cls_report`/`fu_list_class`，与我们的 HDR 直方图/上屏路径不同区） |
| 上游 HDR/EDR 意图标记 | **0 个文件**（`S2A_HDR`/`ExtendedDynamicRange`/`EDRHeadroom`/`potentialEDR` 在 `graphics`、`runtime` 零命中）⇒ 本补丁仍是纯增量 |
| 提到 `RGBA16Float` 的文件 | 22 个，属通用浮点格式处理，不构成 HDR 实现 |

> 上游 `main` 在**不停地动**：本文第一次采样是 `b66dc564`，约十分钟后再跑就是 `b49de817`。
> 两次都 apply 干净、都 149 项全过。所以本文记的 sha 必然很快过期——**判断以自己跑出来的
> 结果为准**，`./scripts/check_upstream.sh --full` 两分半就能给一份新的。

## 这个结论为什么不是平凡的

「补丁能打上」在两种情况下没有意义：上游根本没动它碰的文件，或者只动了无关目录。
这次不是：上游在同一批目录里改了 81 个文件，**并且改了我们的主战场 `nvn_mtl.m`**（+75 行），
只是改在别的函数里，hunk 上下文才没有冲突。所以「apply 通过 + 149 项全过」是真的验证，
不是「文件没变所以当然通过」。

## 怎么自己复核

```bash
./scripts/check_upstream.sh          # 两分钟：默认分支/tag 在哪、能否 apply、是否纯增量
./scripts/check_upstream.sh --full   # 再加上「打上后跑完整闸门」（慢，需要编译）
```

脚本里不写死基线：`BASE` 从 `patch/BASELINE` 第一列读。曾经写死过 `dcd05e5`（v0.4.0），
而 `patch/BASELINE` 早已是 `d5cbf7ff`——两个值不一致的结果是「默认分支就停在基线提交上」
那条判断永远不成立、README 的基线说明与脚本各说一套。唯一真相源只留一个。

## 基线比上游旧的时候怎么办

不要硬 apply。按 README「打补丁」一节：

```bash
./scripts/rebase_patch.sh --head <上游树的路径或 sha>    # 成功给候选补丁 + 往返验证
```

失败会出 `docs/BASELINE-MIGRATION.md` 工单（每个被拒 hunk 一节，给出我们要插入什么、
上游现在是什么），而不是留下一棵「上游 + 半套补丁」的树——那棵树曾被误当成上游代码。
