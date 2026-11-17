# 安全策略

## 范围

这个仓库交付的是打在 [S2Arm](https://github.com/jhaizhou-ops/S2Arm) 上的一个补丁，
以及独立的 CPU 参考实现与验证脚本。**在范围内的**：

- `src/nvn_mtl.m` 里新增的 Metal 路径（HDR 上屏 fragment、直方图 compute、补帧参数化）
- `src/host_adapt.m` 里新增的输出格式 / EDR 决策（`s2a_hdr_out_plan`）
- `src/*.c`（参数层与单测）、`tools/*.c`、`scripts/*.py`
- 环境变量解析：`S2A_HDR_*` 等全部从环境读的旋钮

**不在范围内的**：上游 S2Arm 本体与 Nintendo Switch 运行时的任何问题——请报给上游。
本仓库不分发二进制、不含上游源码，也拿不到任何设备凭据。

## 已知风险面（我们自己盯着的）

| 面 | 现状 |
|---|---|
| 环境变量解析 | `atof`/`atoi` 后一律 clamp 到合法区间（`S2A_HDR_SMOOTH` 0–1、`S2A_OUT_SCALE` 0.5–1.0 等），非法值回落缺省 |
| GPU 直方图 buffer | 下标由 bin 数决定，`clear→add→l1` 三步都在同一个 buffer；CPU 侧读前校验长度 |
| `uint32` 直方图溢出 | 已测「拉满」输入（`t_hdr_robust`） |
| NaN / Inf 传播 | 20 万次 fuzz，无 NaN 外泄、无越界；入口三处守卫是 fuzz 抓出来补的 |
| 补丁本身 | 只改 42 个文件的 diff，可 `git apply --check` 复核；`4f/5` 还核 `git apply` 与 GNU `patch` 结果逐字节一致 |

## 怎么报

1. **优先用 GitHub 的私密渠道**：仓库页 → Security → *Report a vulnerability*
   （若该入口未开启，退而用下面的方式）。
2. 其次：开一个 issue，**只写现象与影响范围**（哪个开关组合、哪个函数、期望 vs 实际），
   不要贴可直接利用的 PoC，我们会回复一个私下交换细节的渠道。
3. 请在报告里带上：`uname -a` / `Xcode 或 clang 版本`、`S2A_*` 环境变量组合、
   以及 `S2A_HDR_LOG=1` 下的 `HDR stat:` 行（如果问题是运行期）。

## 我们承诺的

- 收到后尽快确认（单人维护，按周看）。
- 修复会带一条**能复现的断言**进 `run_all.sh`，而不只是改代码——这个仓库的历史是
  「每个 bug 都换一条守卫」，安全类问题按同一规矩办。
- 不要求报告者放弃披露时间；若两周内没有回应，可以直接公开。
