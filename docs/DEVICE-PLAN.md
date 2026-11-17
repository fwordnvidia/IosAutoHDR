# 真机三格矩阵：现在卡在哪、每条路线的成本

这份文件记录 2026-11-17 实测的设备与工具链现状，以及为什么「真机验黑屏」这件事仍然做不成。
不是计划，是盘点——每一步都有对应的命令与输出。

## 一、已经通的部分（都能一条命令复现）

| 环节 | 现状 | 证据 |
| --- | --- | --- |
| 设备 | iPhone **14,6**（SE 3）、iOS **27.3**（`24C5049d`）、arm64e | `ideviceinfo \| grep Product` |
| UDID | `00008110-000129610E08401E` | `idevice_id -l` |
| USB 通道 | 通（usbmuxd 在本机，设备已配对） | 同上 |
| **日志** | **通** | `idevicesyslog` 3 秒抽样抓到了 `kernel(Sandbox)[0] <Error>:` 行 |
| 安装 | 通（go-ios 1.3.2，`ios install --path=<ipa>`） | `ios help install` |
| 签名 | 通道在：`ios-altserver --apple-id … --ipa …` 现场向 Apple 申请免费证书（7 天） | `~/.local/ios-kit/bin/ios-altserver` 头注释 |
| 截图 | 不通，需要先挂 Developer Disk Image | `idevicescreenshot` → `Could not start screenshotr service!` |

也就是说：**装、跑、抓日志这三件都不缺工具**，`scripts/check_device_log.py`（13 项自测）随时可以判一份真机日志。

## 二、唯一缺的是「编出 ipa」

本机是 Ubuntu，`clang 18.1.3` 能 `-target arm64-apple-ios`，但缺三样：

| 缺 | 说明 | 获取方式 |
| --- | --- | --- |
| iOS SDK | 本机 `find / -name 'iPhoneOS*.sdk'` **0 命中**，也没有 theos | theos 的 `sdks` 仓库（约 200 MB）或从 Xcode 提取 |
| 链接器 | 无 `ld64` / `cctools`，`ldid` 也没有 | `apt install cctools-port`（需 PPA）或自编 |
| 框架头 | S2Arm 用 UIKit / Metal / MetalFX / QuartzCore / IOSurface 一大片私有头 | SDK 自带大部分，私有头需补 |

## 三、四条路线，按「每小时换来多少结论」排

| 路线 | 代价 | 能关掉什么 | 建议 |
| --- | --- | --- | --- |
| **A. macOS 上一次手动 `xcrun metal` + `clang -fsyntax-only`** | 一个 PAT 权限开关 + 10 分钟 runner | MSL 真编译、ObjC 语法（已在 workflow 里写好，本地 11 条守卫盯着） | **先做这个** |
| B. macOS 上完整 `xcodebuild` 出 ipa | 每次 350–450 分钟 runner 分钟；且现在 workflow 明令禁止 `xcodebuild` | 真机三格矩阵 | 只有确定要上机时才值得，且必须单独一次、单独记录 |
| C. Linux 上 theos 交叉编译 | SDK + cctools + 私有头；S2Arm 规模下预计数十小时，且每一处私有头缺失都要手补 | 同上，但不烧 runner 分钟 | 不划算：投入远超「验证一段 MSL 能不能编」 |
| D. 只做静态守卫（现状） | 0 | 结构错（72 项）、调用点、格式假设、fi 运动域 | 已经在做 |

结论：真机矩阵的**最短路径是 B**，而 B 的前置是「先有能过 A 的编译证据」——两者都在同一个 macOS runner 里，A 是 B 的真子集。所以顺序是 A 先跑通，再决定要不要为一张 ipa 花一次 350–450 分钟。

## 四、这张表更新时该改什么

- 设备换了（UDID 变了）：改第一节的 UDID 与型号，并同步 `scripts/check_device_log.py` 头部注释里的设备号。
- 装了 app 之后：把 `T1/T2/T3` 三次日志存进 `evidence/`，用 `./scripts/check_device_log.py --log <文件> --expect t1` 判，结论写进 `docs/PR.md`「已知未验证」表。
- 若真机上出现 `program_source:NN:NN: error:`，那是 MSL 编译失败（iOS 的 Metal 是运行时从源码编的），判据已把它单独判成「MSL 编不过」而不是「开关没开」。
