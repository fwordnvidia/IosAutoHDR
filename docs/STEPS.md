# 闸门步骤索引

由 `scripts/report_steps.py` 从 `run_all.sh` 的 `step` 行生成，**不要手改**：
`run_all.sh` 的 `0w22/5` 会跑 `--check`，手改或忘记重生都会红。

| 步骤 | 标题 | 叙述在 |
| --- | --- | --- |
| `0/5` | 量化报告（docs/QUALITY.md 必须与实测一致） | VERIFICATION.md、README.md |
| `0b/5` | 真实内容验证（docs/REAL-CONTENT.md 必须与实测一致） | VERIFICATION.md |
| `0c/5` | 时间行为（真实照片 60 帧序列，判缓慢运动不抖、纸白不动） | VERIFICATION.md |
| `0d/5` | 使用者命令 hdr_convert（值域 / 纸白不动 / 色域真生效 / 抖动只动最低位） | VERIFICATION.md |
| `0e/5` | 色相误差（docs/COLOR.md：Lab 缩放 vs 线性锚定） | VERIFICATION.md |
| `0f/5` | MSL 静态守卫（没有 Metal 编译器时挡已知雷区） | REVIEW.md、VERIFICATION.md |
| `0g/5` | 消融报告（四档策略对照） | VERIFICATION.md |
| `0h/5` | 转场序列（300 帧：硬切 / 交叉淡入 / 淡到黑 / 黑场 / 淡入） | VERIFICATION.md、README.md |
| `0i/5` | 每帧 CPU 开销（控制器一次调用的实测） | VERIFICATION.md、README.md |
| `0q/5` | 直方图回读控制流（节拍 / 双槽 / 代号作废） | VERIFICATION.md |
| `0p/5` | 运行期 headroom（EDR 上界变化、纸白不跳） | VERIFICATION.md |
| `0o/5` | 上屏 shader 对账（src_msl_hdr 逐像素翻成 C） | VERIFICATION.md |
| `0n/5` | L0 分位对账（MSL hdr_hist_knee 逐行翻成 C） | VERIFICATION.md |
| `0m/5` | GPU/CPU 分箱对账（MSL 算式逐行翻成 C） | VERIFICATION.md、README.md |
| `0l/5` | 画幅与黑边（信箱 / 脏黑边 / 画幅切换） | VERIFICATION.md、README.md |
| `0k/5` | BT.2408 display-referred 放置方式（与 knee 并列的第二条路） | VERIFICATION.md |
| `0j/5` | 对照图（真实照片：SDR vs 自动 HDR；--check 连图内容一起核） | VERIFICATION.md、README.md |
| `0r/5` | 与方法 A 的定量对照（逆函数自检 + 逐码值表） | README.md |
| `0s/5` | 整幅图像上屏/CPU 对账（3 张真实照片 × 3 档旋钮） | VERIFICATION.md |
| `0z/5` | 保真（真实照片：纸白以下必须逐位不动） | VERIFICATION.md |
| `0y/5` | review 地图（docs/REVIEW.md 必须与补丁文件清单/行数一致） | VERIFICATION.md |
| `0w2/5` | 可调项（docs/OPTIONS.md 必须与源码里的常数/注释一致） | VERIFICATION.md |
| `0w4/5` | 随机直方图不变量（t_hdr_fuzz：12 条性质 × 20 万次判定） | VERIFICATION.md |
| `0w5/5` | 白点/重心 与独立参考实现对账（t_hdr_oracle：4000 组随机直方图 + 真实照片） | VERIFICATION.md |
| `0w6/5` | 全关态逐位一致（t_hdr_identity：穷举 256 个码值 × 5 条路径） | VERIFICATION.md |
| `0e7/5` | 内存与未定义行为（30 套 × ASan+UBSan，halt_on_error=1 / detect_leaks=1） | VERIFICATION.md |
| `0w7/5` | 基线一致（patch/BASELINE 是唯一真相源，README/PR 必须与它对齐） | VERIFICATION.md |
| `0w8/5` | README 与仓库一致（引用的路径存在、入口脚本都被写进 README） | VERIFICATION.md |
| `0w9/5` | 调用点实参个数（上游改签名时 patch 只报定义、不报调用点） | VERIFICATION.md |
| `0w10/5` | hdr_hist_l1（MSL）与 CPU 参考实现逐行对账 | VERIFICATION.md |
| `0w11/5` | 设备侧代码登记完整（每个只在 Metal 上跑的 kernel 都必须有对账或写明理由） | DEVICE-ONLY.md、VERIFICATION.md |
| `0w12/5` | 补帧的运动估计与 HDR 在同一个域（tools/fi_domain + docs/FI-DOMAIN.md） | VERIFICATION.md |
| `0w14/5` | 补丁统计三处一致（文件数 / ±行数 / sha256 必须与补丁本体现算一致） | VERIFICATION.md |
| `0w15/5` | CI workflow 只手动跑（无 push/PR 触发、有 concurrency、timeout ≤20 分钟、基线不漂移） | GITHUB-COMPLIANCE.md、VERIFICATION.md、README.md |
| `0w16/5` | 仓库自己的 shell（shellcheck -S warning + 调用的辅助函数必须有定义） | GITHUB-COMPLIANCE.md、VERIFICATION.md |
| `0w17/5` | C 警告审计（46 个带 main 的源文件用 -Wall -Wextra 重编一遍） | GITHUB-COMPLIANCE.md、VERIFICATION.md |
| `0w18/5` | Python 脚本 lint（34 个脚本：E9 运行时错 + F pyflakes + B905 zip 显式 strict） | GITHUB-COMPLIANCE.md、VERIFICATION.md |
| `0w19/5` | 社区文件结构（issue 表单写错时 GitHub 会静默忽略整份） | VERIFICATION.md |
| `0w20/5` | HDR 钩子不变量（图层每帧记 / 两处 tick / 显式 headroom 让位 / 降级与回头路成对） | DESIGN.md、VERIFICATION.md |
| `0w21/5` | CI 结果判读（CI 绿了但 macOS 那步被跳过，也是绿：判据要认得出） | VERIFICATION.md |
| `0w22/5` | 步骤索引（docs/STEPS.md 必须与 run_all.sh 的 step 行逐条一致） | VERIFICATION.md |
| `0w3/5` | 文档计数（套数 / 断言总数 / 上屏 fragment 字节数 必须与实测一致） | VERIFICATION.md |
| `0x/5` | 设备日志判读（三格矩阵 T1/T2/T3 的判据） | FI-DOMAIN.md、REVIEW.md、VERIFICATION.md |
| `0w/5` | 真实照片颜色保真（逐像素 CIELAB） | VERIFICATION.md、README.md |
| `0v/5` | 图像级用例（夜景 / HUD / 字幕 / 淡出） | VERIFICATION.md |
| `0u/5` | knee 原子发布（撕裂读写模型） | VERIFICATION.md |
| `0t/5` | 设备链路整幅对账（分箱逐格 → L1 白点/重心 → 上屏画面） | REVIEW.md、VERIFICATION.md |
| `1/5` | 参考 CLI 自检（40 项） | VERIFICATION.md |
| `2/5` | HUD 剔除常数平台区 | VERIFICATION.md |
| `3/5` | 曲线图 | VERIFICATION.md |
| `4/5` | 补丁自洽（在干净树上打补丁 + 全部单测） | REVIEW.md、VERIFICATION.md |
| `5/5` | 结论 | （仅索引） |

共 52 个步骤。
