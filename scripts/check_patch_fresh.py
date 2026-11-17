#!/usr/bin/env python3
"""check_patch_fresh.py：src/ 与「打包出去的那个补丁」是不是同一份东西。

为什么要单独一条：`src/` 是唯一真相源，补丁是从它重建出来的**产物**。两份东西一旦不同步，
出问题的方向很恶心：

  - `run_all.sh` 的 4/5 是把 `patch/hdr-auto-l1.patch` 打到干净树上、再跑树上那批单测
    ⇒ 它测的是**补丁里的旧内容**；
  - `check_counts.py` / `check_msl.py` / `check_constants.py` 读的是 `src/`
    ⇒ 它们核的是**src 里的新内容**。

于是「一半闸门测旧、一半测新」，全绿，而 dist 里发出去的还是旧的。2026-11-16 有过一次
同源的静默回退（改动只做在树上，没回写 src/，下一次重建把树里的新版本覆盖掉）。那次之后
`make_patch.sh` 会在生成前比「树 vs src」，但它**只负责重建**，不是一个只读闸门：
没人跑它的时候，没人知道补丁已经过期。

本脚本只读，不写任何文件：

  1. 遍历 `src/*`，按 `make_patch.sh` 的同一套映射算出它在树里的路径，逐字节比较；
  2. 反向：补丁里每个 `diff --git` 的文件都必须有 `src/` 来源（否则下次重建会丢）；
  3. 两个方向任一不符即红，并打印「谁更新、谁更旧」。

用法：check_patch_fresh.py <打补丁后的树根>
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "scripts"))


def map_path(name):
    """与 scripts/make_patch.sh 的 map_path() 逐条对应（改一处就要改另一处）。"""
    if name == "hdr_hist.metal":
        return "graphics/nvn/shaders/" + name
    if name.startswith("t_") and name.endswith(".c"):
        return "graphics/nvn/tests/" + name
    if name in ("host_adapt.m", "Makefile"):
        return "graphics/nvn/" + name
    return "graphics/nvn/gfx/nvn/" + name


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def patch_files():
    p = os.path.join(HERE, "patch", "hdr-auto-l1.patch")
    if not os.path.exists(p):
        return None
    out = []
    with open(p, encoding="utf-8") as f:
        for line in f:
            if line.startswith("diff --git "):
                out.append(line.rstrip("\n").split(" b/")[-1])
    return out or None


def main():
    if len(sys.argv) < 2:
        print("用法：check_patch_fresh.py <打补丁后的树根>")
        return 1
    tree = sys.argv[1]
    files = patch_files()
    if files is None:
        print("✗ 找不到 patch/hdr-auto-l1.patch（或里面没有 diff --git 行）")
        return 1

    src_dir = os.path.join(HERE, "src")
    src_names = sorted(os.listdir(src_dir))
    src_names = [n for n in src_names if os.path.isfile(os.path.join(src_dir, n))]

    stale, missing, extra = [], [], []
    for name in src_names:
        rel = map_path(name)
        dst = os.path.join(tree, rel)
        if not os.path.exists(dst):
            missing.append(rel)
            continue
        a = sha(os.path.join(src_dir, name))
        b = sha(dst)
        if a != b:
            stale.append((name, rel, a, b))

    mapped = {map_path(n) for n in src_names}
    for rel in files:
        if rel not in mapped:
            extra.append(rel)

    print("src/ %d 个文件 ↔ 树里 %d 个路径；补丁 %d 个文件" % (len(src_names), len(mapped), len(files)))

    if missing:
        for rel in missing:
            print("  ✗ 树里没有 %s —— 补丁没打到这个文件？（或 map_path 映射变了）" % rel)
    for name, rel, a, b in stale:
        print("  ✗ %s 与树里 %s 不一致（src %s… vs 树 %s…）—— 补丁是从旧 src 生成的"
              % (name, rel, a[:12], b[:12]))
    for rel in extra:
        print("  ✗ 补丁里有 %s，但 src/ 里没有来源 —— 下次重建时它会被悄悄丢掉" % rel)

    if missing or stale or extra:
        print("::error::src/ 与补丁不同步（%d 个不一致、%d 个缺、%d 个无来源）—— "
              "补丁是发货内容，src/ 是真相源，两者必须逐字节相同。"
              "修法：./scripts/make_patch.sh <树>（它会先比树与 src，再重建补丁）"
              % (len(stale), len(missing), len(extra)))
        return 1

    print("✓ src/ 的 %d 个文件与补丁打出来的树逐字节一致，补丁里没有无来源的文件" % len(src_names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
