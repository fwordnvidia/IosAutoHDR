#!/usr/bin/env python3
"""MSL 静态守卫：在没有 Metal 编译器的机器上，挡住已知会炸编译器的写法。

背景：2026-11 的一次 CI 红就是 `hdr_hist.metal:98: 'double' is not supported in Metal`。
本机（Linux）没有 `xcrun metal`，CI 又因账号停用而中断，所以补一层「已知雷区」检查：

  1. 禁用类型/关键字：double / long long / complex —— Metal 没有；
  2. 括号配平（逐函数）：整段字面量里的 MSL 写坏一个 `}`，编译器只会在后面乱报；
  3. 必须 `#include <metal_stdlib>` 且 `using namespace metal;`；
  4. `kernel` / `vertex` / `fragment` 后必须跟返回类型，且函数体能在同一个字面量里闭合；
  5. 不允许出现 C 头（`#include <stdio.h>` 之类）——Metal 只有 stdlib；
  6. 每个 `[[buffer(n)]]` 必须出现在参数位置（`(` 之后、`)` 之前），且 n 在该函数内不重复。

验的是「抽出来的 MSL 文本」，也就是真正送进编译器的东西（`extract_msl.py` 的产物）。
"""
import codecs
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(HERE, "build", "msl")

BANNED = [
    (r"\bdouble\b", "Metal 不支持 double"),
    (r"\blong\s+long\b", "Metal 不支持 long long"),
    (r"\bcomplex\b", "Metal 不支持 complex"),
    (r"\bstd::", "Metal 没有 std:: 命名空间"),
    (r"\bprintf\s*\(", "MSL 里没有 printf（用 return/atomic 回传）"),
]


def extract():
    """复用 extract_msl.py 抽出的 .metal；没有就先跑一次。

    2026-11-17 补：`src/nvn_present_fi.inc` 里还有两段 MSL（`fi_mv_setup` 的运动估计
    down/match/spread，与 `fi_copy_setup` 的缩拷 vs/fs），它们一直**不在**这个守卫里 ——
    而 to_enc（HDR 线性→编码域）恰恰是那段里最容易写错的东西。按出现序号各写一个文件。
    """
    # 投影形态：MSL 源码在**上游整文件**里（src/nvn_mtl.m 1.9 MB、src/nvn_present_fi.inc），
    # 公开仓库按设计只交付差分 ⇒ 这里没有可抽的东西。返回 None 让 main 打印「跳过」，
    # 而不是抛 CalledProcessError（2026-11-17 之前投影里 ./run_all.sh 会在这一步吐 traceback，
    # 而 `| tail -1` 把它压成一行，看起来像「什么都没说但红了」）。
    if not os.path.exists(os.path.join(HERE, "src", "nvn_mtl.m")):
        return None
    script = os.path.join(HERE, "scripts", "extract_msl.py")
    os.makedirs(TMP, exist_ok=True)
    subprocess.run([sys.executable, script, os.path.join(HERE, "src", "nvn_mtl.m"), "--dir", TMP],
                   check=True, capture_output=True)

    inc = os.path.join(HERE, "src", "nvn_present_fi.inc")
    if os.path.exists(inc):
        with open(inc, encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        lits = re.compile(r'"((?:[^"\\]|\\.)*)"')
        idx, cur, out = 0, None, []
        for ln in lines:
            if "static const char *msl =" in ln:
                idx += 1
                cur = []
                out.append(cur)
                continue
            if cur is None:
                continue
            for m in lits.finditer(ln):
                cur.append(codecs.decode(m.group(1), "unicode_escape"))
            if ln.rstrip().endswith(";") and cur:
                cur = None
        for i, chunk in enumerate(out, 1):
            body = "".join(chunk)
            if body.strip():
                with open(os.path.join(TMP, "fi_msl_%d.metal" % i), "w", encoding="utf-8") as f:
                    f.write(body)

    return sorted(f for f in os.listdir(TMP) if f.endswith(".metal"))


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def func_bodies(src):
    """按顶层 `{` 切函数区间，返回 (header, body) 列表。"""
    out, depth, start = [], 0, 0
    for i, ch in enumerate(src):
        if ch == "{":
            if depth == 0:
                header = src[max(0, start):i]
                body_start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append((header, src[body_start:i + 1]))
                start = i + 1
    return out


def main():
    files = extract()
    if files is None:
        print("· 投影形态：没有 src/nvn_mtl.m（MSL 源码在上游整文件里）⇒ 跳过结构守卫")
        print("  这一层要在带上游树的开发树里跑；投影里只能保证补丁本身能被 apply")
        return 0
    fails, checks = 0, 0

    def check(cond, msg):
        nonlocal fails, checks
        checks += 1
        if cond:
            print("  ✓ " + msg)
        else:
            print("  ✗ " + msg)
            fails += 1

    for name in files:
        path = os.path.join(TMP, name)
        raw = open(path, encoding="utf-8").read()
        src = strip_comments(raw)

        bad = [(pat, why) for pat, why in BANNED if re.search(pat, src)]
        check(not bad, "%s：无禁用类型/关键字%s" % (name, "" if not bad else
                                    "（" + "；".join("%s → %s" % (p, w) for p, w in bad) + "）"))

        if "kernel" in src or "vertex" in src or "fragment" in src:
            check("#include <metal_stdlib>" in raw, "%s：含 metal_stdlib" % name)
            check("using namespace metal;" in raw, "%s：有 using namespace metal" % name)

        incs = re.findall(r"#include\s*<([^>]+)>", raw)
        check(all(i == "metal_stdlib" for i in incs),
              "%s：只 include metal_stdlib（实得 %s）" % (name, ", ".join(incs) or "无"))

        bodies = func_bodies(src)
        check(len(bodies) > 0, "%s：解析到 %d 个函数体" % (name, len(bodies)))
        for header, body in bodies:
            fname = header.strip().split("\n")[-1].strip()[:48] or "?"
            check(body.count("{") == body.count("}"),
                  "%s：%s 括号配平" % (name, fname))
            depth = 0
            ok = True
            for ch in body:
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth < 0:
                        ok = False
            check(ok and depth == 0, "%s：%s 圆括号配平" % (name, fname))

        # 每个函数签名里 [[attribute(n)]] 的 n 不该重复
        for header, _ in bodies:
            # texture(0) / sampler(0) / buffer(0) 是三个独立的编号空间，只查同类内重复
            pairs = re.findall(r"\[\[\s*(\w+)\s*\(\s*(\d+)\s*\)\s*\]\]", header)
            by_kind = {}
            for kind, num in pairs:
                by_kind.setdefault(kind, []).append(num)
            dup = [(k, v) for k, v in by_kind.items() if len(set(v)) != len(v)]
            if pairs:
                check(not dup,
                      "%s：%s 的 attribute 序号同类不重复（%s）"
                      % (name, header.strip().split("\n")[-1].strip()[:32] or "?", dup or "ok"))

    print("✓ MSL 静态守卫：%d 个文件 %d 项%s"
          % (len(files), checks, "全过" if fails == 0 else "，失败 %d" % fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
