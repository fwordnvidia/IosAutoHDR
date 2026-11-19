#!/usr/bin/env bash
# 从真机拉 app 容器里的 host.log（HDR stat 那几行就在这里）。
#
#   ./scripts/fetch_device_log.sh <bundle-id> [输出文件=/tmp/host.log]
#
# 为什么要专门一个脚本：真机上的诊断行走 s2a_log_line2 → write(fd 2)，而 runtime 的
# logfd_redirect() 会把 fd 2 dup2 到 <Documents>/host.log，所以 `idevicesyslog` 抓不到它
# ——那是 syslog，不是文件。取证必须读文件。
#
# 两条路径（2026-11-17 在 iPhone SE 3 / iOS 27.3 上各实测过一次）：
#   A  ifuse --documents <bundle> <mnt>
#      要求 app 的 Info.plist 里 UIFileSharingEnabled=true（iOS 8.3 起强制）。不需要 root、
#      不需要隧道。实测：Stardew Valley 挂载成功；scrcpy 报 InstallationLookupFailed（没这个键）。
#   B  ios tunnel start --userspace（要 sudo）+ ios file pull --app=… --remote=/Documents/host.log
#      App Store 装的包会被 ContainerLookupErrorDomain:3 挡（安全边界）；我们自己开发签名
#      的包（带 get-task-allow）才读得到。实测：--temp 容器可列（Total 0 files），第三方 app 不可。
#
# 两条都不通时的判据：这不是脚本坏了，是 app 没有 UIFileSharingEnabled 又不是开发签名包。
set -u
BUNDLE=${1:-}
OUT=${2:-/tmp/host.log}
IOSKIT=${IOSKIT:-$HOME/.local/ios-kit/bin}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/s2a-fetch.XXXXXX") || exit 1
MNT=${MNT:-$WORK/mnt}
trap 'fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true; rm -rf "$WORK"' EXIT

if [ -z "$BUNDLE" ]; then
    echo "用法：$0 <bundle-id> [输出文件=/tmp/host.log]"
    echo "  bundle-id 例：com.example.s2arm（自己要验的那个包）"
    exit 2
fi
if [ ! -d "$IOSKIT" ]; then
    echo "✗ 找不到 $IOSKIT（ios-kit 没装？这里应该有 ifuse / ios）"
    exit 1
fi

# ── 路径 A：house_arrest，只要 UIFileSharingEnabled ──────────────────────────
if [ -x "$IOSKIT/ifuse" ]; then
    mkdir -p "$MNT"
    if "$IOSKIT/ifuse" --documents "$BUNDLE" "$MNT" 2>"$WORK/ifuse.err"; then
        if [ -f "$MNT/host.log" ]; then
            cp "$MNT/host.log" "$OUT" || { echo "✗ 复制失败：$MNT/host.log → $OUT"; fusermount -u "$MNT" 2>/dev/null; exit 1; }
            fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
            echo "✓ 已用 ifuse 拉到 $OUT（$(wc -c < "$OUT") B，$(grep -c 'HDR stat' "$OUT" 2>/dev/null || echo 0) 行 HDR stat）"
            echo "  下一步：./scripts/check_device_log.py --log $OUT --expect t2"
            exit 0
        fi
        echo "· ifuse 挂上了，但 $MNT/host.log 不在（这个包还没跑出日志，或日志在别的目录）"
        ls "$MNT" 2>/dev/null | head -5 | sed 's/^/    /'
        fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
    else
        echo "· 路径 A 不可用：$(head -2 "$WORK/ifuse.err" 2>/dev/null | tr '\n' ' ')"
    fi
fi

# ── 路径 B：userspace 隧道 + file pull（要 sudo，仅开发签名包可读）───────────
if [ -x "$IOSKIT/ios" ]; then
    echo "· 试路径 B（userspace 隧道 + ios file pull，需要 sudo）"
    if sudo -n true 2>/dev/null; then
        pkill -f '[t]unnel start' 2>/dev/null
        # shellcheck disable=SC2024  # 私有 mktemp 目录本用户可写，重定向打开的 fd 由 root 继承
        sudo -n env ENABLE_GO_IOS_AGENT=user nohup "$IOSKIT/ios" tunnel start --userspace \
            --tunnel-info-port=28100 >"$WORK/tun.log" 2>&1 &
        sleep 10
        if grep -q 'userspace tunnel negotiated' "$WORK/tun.log" 2>/dev/null; then
            if "$IOSKIT/ios" file pull --app="$BUNDLE" --remote=/Documents/host.log --local="$OUT" \
                    --nojson --tunnel-info-port=28100 2>"$WORK/pull.err"; then
                pkill -f '[t]unnel start' 2>/dev/null
                echo "✓ 已用 file pull 拉到 $OUT（$(wc -c < "$OUT") B）"
                echo "  下一步：./scripts/check_device_log.py --log $OUT --expect t2"
                exit 0
            fi
            echo "· file pull 失败：$(grep -oE 'ErrorDomain:[A-Za-z]+|ErrorCode:[0-9]+' "$WORK/pull.err" 2>/dev/null | tr '\n' ' ')"
        else
            echo "· 隧道没起来，日志前两行：$(head -2 "$WORK/tun.log" 2>/dev/null | tr '\n' ' ')"
        fi
        pkill -f '[t]unnel start' 2>/dev/null
    else
        echo "· 路径 B 要 sudo，本脚本不替你输密码。手动两条命令："
        echo "    sudo ENABLE_GO_IOS_AGENT=user $IOSKIT/ios tunnel start --userspace --tunnel-info-port=28100 &"
        echo "    $IOSKIT/ios file pull --app=$BUNDLE --remote=/Documents/host.log --local=$OUT --tunnel-info-port=28100"
    fi
fi

echo "✗ 两条路径都没拿到 host.log。"
echo "  判据：app 的 Info.plist 没有 UIFileSharingEnabled=true（路径 A 的前提），且不是开发签名包"
echo "  （路径 B 的前提，App Store 包一律 ContainerLookupErrorDomain:3）。"
echo "  这不是脚本坏了 —— 给自己的开发包加上 UIFileSharingEnabled 再试。"
exit 1
