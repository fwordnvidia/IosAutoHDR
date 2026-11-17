#!/usr/bin/env python3
"""判读真机日志里的 HDR 行（S2A_HDR_LOG=1）。

为什么需要它：HDR 生效与否在画面上看不出来，真机三格矩阵 T1/T2/T3 的唯一证据就是日志。
手工读日志容易读漏（尤其格式/ITM/head 三个字段互相矛盾的组合），所以把判据写成脚本：
输入一份 logcat 文本，输出「这一格验到了什么 / 哪一格没验到 / 是否有黑屏迹象」。

用法：
    ./scripts/check_device_log.py --log device.log --expect t1
    ./scripts/check_device_log.py --log device.log              # 只做通用一致性检查
    ./scripts/check_device_log.py --selftest                    # 用合成日志自测判据

三格矩阵（与 docs/PR.md「已知未验证」一致）：
    T1  S2A_HDR_FMT=1               只看格式：期望 fmt=16F，但 head 应停在 1.00（没声明 EDR，系统不给余量）
    T2  S2A_HDR_OUTPUT=1            期望 fmt=16F 且 head>1.0（XDR 屏才有 >1；SDR 屏 head=1.0 属正常）
                                    加 FMT=1 也等价（它只换画布，不再关掉 EDR），见 t_hdr_env
    T3  S2A_HDR_FMT=0 OUTPUT=1      期望 fmt=8B、itm=0（声明了 EDR 但仍是 8 位字节）
"""
import argparse
import re
import sys

STAT_RE = re.compile(
    r"HDR stat: fmt=(?P<fmt>16F|8B) head=(?P<head>[-0-9.]+) "
    r"knee=(?P<lo>[-0-9.]+)\+(?P<de>[-0-9.]+) white=(?P<white>[0-9.]+|n/a) "
    r"avg=(?P<avg>[-0-9.]+|n/a) scene=(?P<scene>[-0-9.]+|n/a) "
    r"frozen=(?P<frozen>[01]) itm=(?P<itm>[01])(?: map=(?P<map>[01]))?")
# map 字段是后加的：旧日志没有它，按 0（拐点档）理解，不能因为「没这个字段」就判整行无效。
FALLBACK_RE = re.compile(r"16F 画布连续 \d+ 帧取不到 drawable")
REARM_RE = re.compile(r"重新试探 16F 画布")
# 真机上 MSL 是运行时从源码编译的（newLibraryWithSource:），所以「xcrun metal 能抓的错」
# 在设备上表现为 program_source 报错行。没有这条判据时，这类失败只会表现为
# 「HDR stat 一行都没有」，被误读成「开关没开」。
MSL_ERR_RE = re.compile(r"program_source:\d+:\d+: (?:fatal )?error:|"
                        r"(?:metal_stdlib|metal_texture) (?:file )?not found|"
                        r"Compilation failed: .*\.metal")


def parse(text):
    st = []
    for ln in text.splitlines():
        m = STAT_RE.search(ln)
        if m:
            st.append({
                "fmt": m.group("fmt"),
                "head": float(m.group("head")),
                "lo": float(m.group("lo")),
                "de": float(m.group("de")),
                "white": m.group("white"),
                "avg": m.group("avg"),
                "scene": m.group("scene"),
                "frozen": m.group("frozen") == "1",
                "itm": m.group("itm") == "1",
                "map": (m.group("map") == "1") if m.group("map") else False,
                "has_map": m.group("map") is not None,
            })
    return {
        "stats": st,
        "fallback": len(FALLBACK_RE.findall(text)),
        "rearm": len(REARM_RE.findall(text)),
        "msl_err": len(MSL_ERR_RE.findall(text)),
    }


def common_checks(parsed):
    """与具体哪一格无关的一致性检查：返回 (失败项, 备注列表)。"""
    bad, notes = [], []
    st = parsed["stats"]
    if parsed["msl_err"]:
        bad.append("真机日志里有 %d 行 Metal 运行时编译错误（program_source 报错）"
                   "——这说明 MSL 在设备上编不过；这类错本可以在 macOS 上先用 xcrun metal 抓掉" % parsed["msl_err"])
    if not st:
        # 一行都没有：要么没开 S2A_HDR_LOG，要么 16F 从没跑起来（后者由 expect 判）
        notes.append("日志里没有 HDR stat 行")
        return bad, notes
    for i, s in enumerate(st):
        if s["fmt"] == "8B" and s["itm"]:
            bad.append("第 %d 行：fmt=8B 但 itm=1（8 位画布不该走 ITM，管线没重建？）" % (i + 1))
        if s["map"] and s["fmt"] == "8B":
            bad.append("第 %d 行：map=1 但 fmt=8B（内容映射只挂在 HDR fragment 上，8 位画布根本走不到）" % (i + 1))
        if s["fmt"] == "16F" and not s["itm"]:
            bad.append("第 %d 行：fmt=16F 但 itm=0（画布换了、shader 没换）" % (i + 1))
        if not (0.99 <= s["head"] <= 4.01):
            bad.append("第 %d 行：head=%.2f 超出 [1,4]" % (i + 1, s["head"]))
        if s["lo"] < 0.0 or s["lo"] > 1.0001:
            bad.append("第 %d 行：knee_lo=%.4f 不在 [0,1]" % (i + 1, s["lo"]))
        if s["lo"] + s["de"] > 1.0001:
            bad.append("第 %d 行：knee_hi=%.4f 超过 SDR 裁切点 1.0" % (i + 1, s["lo"] + s["de"]))
    drift = 0.0
    for a, b in zip(st, st[1:], strict=False):
        if not a["frozen"]:            # 冻结帧之间比跳变没有意义（本来就是刻意保持）
            drift = max(drift, abs(b["lo"] - a["lo"]))
    notes.append("%d 行 HDR stat；fmt=%s；head %.2f–%.2f；未冻结帧最大 knee 跳变 %.4f" % (
        len(st), "/".join(sorted({s["fmt"] for s in st})),
        min(s["head"] for s in st), max(s["head"] for s in st), drift))
    if parsed["fallback"]:
        notes.append("出现过 %d 次 16F 降级（%d 次重试探）" % (parsed["fallback"], parsed["rearm"]))
    return bad, notes


def expect_checks(parsed, which):
    """三格矩阵的单格判据。返回 (失败项, 结论)。"""
    st = parsed["stats"]
    if not st:
        if parsed["fallback"]:
            return [], "16F 拿不到 drawable ⇒ 已降级到 8 位（**黑屏假设成立**）；HDR 从没真正上屏"
        return ["没有任何 HDR stat 行（S2A_HDR_LOG 没开，或 HDR 链路没跑起来）"], ""
    fmt16 = any(s["fmt"] == "16F" for s in st)
    maxhead = max(s["head"] for s in st)
    bad = []
    if which == "t1":
        if not fmt16:
            bad.append("T1 期望 fmt=16F，实际只有 8B（S2A_HDR_FMT=1 没生效，或已降级）")
        if maxhead > 1.01:
            bad.append("T1 只换格式、没声明 EDR，head 应停在 1.00，实际 %.2f —— "
                       "说明 EDR 也被打开了（那就不是 T1）" % maxhead)
        concl = "T1 验到：16F 画布可用、上屏走 ITM；head=1.00 表示没有 EDR 余量（符合「只换格式」）"
    elif which == "t2":
        if not fmt16:
            bad.append("T2 期望 fmt=16F，实际只有 8B（要么已降级，要么 OUTPUT 没生效）")
        if maxhead <= 1.01:
            bad.append("T2 期望 head>1.0（XDR 屏才有余量）；实测 %.2f —— "
                       "若这台是 SDR 屏，这一格本来就测不到，需换 XDR 设备" % maxhead)
        concl = "T2 验到：16F + EDR 同时生效，head=%.2f（扩张上限跟着屏幕走）" % maxhead
    elif which == "t3":
        if fmt16:
            bad.append("T3 期望 fmt=8B（FMT=0），实际出现过 16F —— 这条链不该切格式")
        if any(s["itm"] for s in st):
            bad.append("T3 期望 itm=0（8 位不上 ITM）")
        concl = "T3 验到：只声明 EDR、画布仍是 8 位（itm=0），画面应与 SDR 逐字节相同"
    else:
        raise SystemExit("未知格：%s（可选 t1/t2/t3）" % which)
    return bad, concl


SELFTEST_CASES = {
    "t1": ("HDR stat: fmt=16F head=1.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1", "t1", True),
    "t2": ("HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1", "t2", True),
    "t2_sdr": ("HDR stat: fmt=16F head=1.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1", "t2", False),
    "t3": ("HDR stat: fmt=8B head=1.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=0", "t3", True),
    "t3_bad": ("HDR stat: fmt=8B head=1.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1", "t3", False),
    "t1_bad": ("HDR stat: fmt=8B head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=0", "t1", False),
    "black": ("I/nvn: HDR: 16F 画布连续 60 帧取不到 drawable，已回退 8 位上屏（HDR 关闭）", None, None),
    # map 字段（后加）：旧日志没有它 → 按 0 理解，必须仍然判过
    "old_no_map": ("HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1", "t2", True),
    # 内容映射档：map=1、fmt=16F、itm=1 是合法组合（knee 仍在算，只是没用于上屏）
    "map1_ok": ("HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1 map=1", "t2", True),
    # 内容映射落在 8 位画布上：走不到 HDR fragment，说明开关接了但管线没换
    # MSL 在真机上编不过：只有 program_source 报错、没有 stat 行 —— 必须判红，不能当成「没开日志」
    "msl_fail": ("E nvn: program_source:41:9: error: use of undeclared identifier 'to_enc'\n"
                 "E nvn: Compilation failed: /tmp/src_msl_hdr.metal", None, False),
    "msl_fail_with_stat": ("program_source:41:9: error: no matching function for call to 'select'\n"
                           "HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1", "t2", False),
    "map1_8b": ("HDR stat: fmt=8B head=1.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=0 map=1", "t3", False),
}


def selftest():
    fails = 0
    for name, (line, want, expect_ok) in SELFTEST_CASES.items():
        parsed = parse(line)
        bad, _ = common_checks(parsed)
        if want:
            b2, _ = expect_checks(parsed, want)
            bad += b2
        got_ok = not bad
        if expect_ok is not None and got_ok != expect_ok:
            print("  ✗ 自测 %s：期望 %s，实际 %s（%s）" % (name, expect_ok, got_ok, "; ".join(bad) or "无失败项"))
            fails += 1
        elif name == "black":
            _, concl = expect_checks(parsed, "t1")
            if "黑屏假设成立" not in concl:
                print("  ✗ 自测 black：降级日志没有判成黑屏")
                fails += 1
    # 解析器必须能忽略 logcat 前缀与时间戳
    noisy = "11-16 02:00:01.123  1234  1234 I nvn: HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1"
    if len(parse(noisy)["stats"]) != 1:
        print("  ✗ 自测 noisy：没能从 logcat 行里解析出来")
        fails += 1
    n = len(SELFTEST_CASES) + 1
    print("%s check_device_log 自测 %d 项 → %s" % ("✓" if not fails else "✗", n, "全过" if not fails else "%d 项失败" % fails))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log")
    ap.add_argument("--expect", choices=["t1", "t2", "t3"])
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return 1 if selftest() else 0
    if not a.log:
        ap.error("要么 --log，要么 --selftest")
    with open(a.log, encoding="utf-8", errors="replace") as f:
        parsed = parse(f.read())
    bad, notes = common_checks(parsed)
    for n in notes:
        print("· " + n)
    concl = ""
    if a.expect:
        b2, concl = expect_checks(parsed, a.expect)
        bad += b2
    for b in bad:
        print("  ✗ " + b)
    if concl:
        print("→ " + concl)
    print("✓ %s 判读通过" % (a.expect or "通用检查") if not bad else "✗ %s 判读失败（%d 项）" % (a.expect or "通用检查", len(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
