#!/usr/bin/env python3
"""校验 .github/ISSUE_TEMPLATE/*.yml（issue form）的结构。

为什么需要这条：**表单 YAML 写错时 GitHub 静默忽略**。仓库页上那份表单根本不出现，
社区档案也不会把它算进 health —— 但本地 `git status` 干净、CI 绿、README 也照常写着
「用 issue 表单报」；没有任何东西会红。2026-11-18 第一次推上去就撞上这个盲区：
health 从 85% 跳到 100% 才敢确认「GitHub 其实认了」，而从 85→100 之间差的就是
「表单有没有被解析成功」这件事本身。

这里只查 GitHub issue form schema 里**写错会整份忽略**的那几条：
- 顶层必须有 name / description / body（title 可选）；
- body 每项必须有 type，且 type 在已知集合里；除 markdown 外必须有 id；
- id 只允许 [A-Za-z0-9_-]，且整份唯一；
- 每种 type 的 attributes 只允许 schema 里的键（多写键会整份忽略）；
- input/textarea/dropdown/checkboxes 必须有 label；dropdown 与 checkboxes 必须有非空 options；
- markdown 不允许 validations（有则整份忽略）；
- validations 只允许 required 一个键，且只对 input/textarea/dropdown/checkboxes 有效。

不做的事：不校验 label 是否在仓库里存在（要网络）、不校验渲染效果。
"""
import os
import sys

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDIR = os.path.join(ROOT, ".github", "ISSUE_TEMPLATE")

ATTRS = {
    "markdown": {"value"},
    "input": {"label", "description", "placeholder", "value", "render"},
    "textarea": {"label", "description", "placeholder", "value", "render"},
    "dropdown": {"label", "description", "multiple", "options", "default"},
    "checkboxes": {"label", "description", "options"},
}
NEEDS_LABEL = {"input", "textarea", "dropdown", "checkboxes"}
NEEDS_OPTIONS = {"dropdown", "checkboxes"}
TOPLIMIT = 10          # GitHub：body 最多 10 个元素
DESC_LIMIT = 200       # description 上限


def check_one(path):
    rel = os.path.relpath(path, ROOT)
    errs = []
    try:
        doc = yaml.safe_load(open(path, encoding="utf-8"))
    except Exception as exc:                      # noqa: BLE001 - 要把 YAML 错误原样带出来
        return [f"{rel}:2：YAML 解析失败（{exc.__class__.__name__}: {exc}）—— GitHub 会静默忽略整份表单"]

    if not isinstance(doc, dict):
        return [f"{rel}: 顶层必须是映射（name/description/body），实际是 {type(doc).__name__}"]
    for k in ("name", "description", "body"):
        if not doc.get(k):
            errs.append(f"{rel}: 缺 {k}（GitHub 要求 name/description/body 三项都有）")
    if len(str(doc.get("description", ""))) > DESC_LIMIT:
        errs.append(f"{rel}: description 超过 {DESC_LIMIT} 字符（现在是 {len(str(doc['description']))}）")
    if doc.get("assignees") or doc.get("projects"):
        pass  # 允许，不判
    body = doc.get("body") or []
    if not isinstance(body, list):
        errs.append(f"{rel}: body 必须是列表")
        body = []
    if len(body) > TOPLIMIT:
        errs.append(f"{rel}: body 有 {len(body)} 个元素，GitHub 上限 {TOPLIMIT}")
    seen = set()
    for i, item in enumerate(body, 1):
        where = f"{rel}: body 第 {i} 项"
        if not isinstance(item, dict):
            errs.append(f"{where} 必须是映射")
            continue
        t = item.get("type")
        if t not in ATTRS:
            errs.append(f"{where} 的 type={t!r} 不在 {sorted(ATTRS)} 里 —— 整份表单会被忽略")
            continue
        attrs = item.get("attributes") or {}
        if not isinstance(attrs, dict):
            errs.append(f"{where} 的 attributes 必须是映射")
            continue
        extra = set(attrs) - ATTRS[t]
        if extra:
            errs.append(f"{where}（{t}）的 attributes 里有 schema 不认的键：{sorted(extra)}")
        if t in NEEDS_LABEL and not attrs.get("label"):
            errs.append(f"{where}（{t}）缺 attributes.label")
        if t in NEEDS_OPTIONS:
            opts = attrs.get("options")
            if not isinstance(opts, list) or not opts:
                errs.append(f"{where}（{t}）的 options 必须是非空列表")
        if t == "markdown":
            if "validations" in item:
                errs.append(f"{where}（markdown）不允许 validations —— 有就整份忽略")
            continue
        if "id" not in item:
            errs.append(f"{where}（{t}）缺 id")
            continue
        ident = str(item["id"])
        if not ident.replace("-", "").replace("_", "").isalnum():
            errs.append(f"{where} 的 id={ident!r} 只能含 [A-Za-z0-9_-]")
        if ident in seen:
            errs.append(f"{where} 的 id={ident!r} 与前面重复")
        seen.add(ident)
        val = item.get("validations")
        if val is not None:
            if not isinstance(val, dict):
                errs.append(f"{where} 的 validations 必须是映射")
            else:
                unk = set(val) - {"required"}
                if unk:
                    errs.append(f"{where} 的 validations 只允许 required，多了 {sorted(unk)}")
    return errs


def main():
    if not os.path.isdir(TDIR):
        print("· 没有 .github/ISSUE_TEMPLATE/（投影形态可能不带？开发树应当有）⇒ 跳过")
        return 0
    files = sorted(f for f in os.listdir(TDIR) if f.endswith((".yml", ".yaml")))
    if not files:
        print("✗ .github/ISSUE_TEMPLATE/ 里没有任何 .yml —— 仓库页上不会出现表单入口")
        return 1
    errs = []
    for f in files:
        errs.extend(check_one(os.path.join(TDIR, f)))
    if errs:
        for e in errs:
            print("::error::" + e)
        print(f"✗ issue 表单结构：{len(errs)} 处（GitHub 会静默忽略有问题的整份表单）")
        return 1
    print(f"✓ issue 表单结构：{len(files)} 个文件通过（type/id/attributes/validations 都对）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
