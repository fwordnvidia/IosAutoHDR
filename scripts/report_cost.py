#!/usr/bin/env python3
"""report_cost.py：把 tools/hdr_cost 的实测写成 docs/COST.md（--check 校验一致）。

为什么需要这份报告：补丁只在设备上多花「每帧一次控制器调用」的 CPU 时间，
但评论者看到的是一堆 float 运算，容易以为它会掉帧。这里给的是实测数字与上限判据。
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'COST.md')
LIMIT_US = 5.0          # 一次控制器调用的上限：5 µs ≈ 120 Hz 帧预算的 0.06%


def measure():
    exe = os.path.join(ROOT, 'build', 'hdr_cost')
    if not os.path.exists(exe):
        subprocess.check_call(['cc', '-O2', '-w', '-I', os.path.join(ROOT, 'src'),
                               os.path.join(ROOT, 'tools', 'hdr_cost.c'), '-lm', '-o', exe])
    out = subprocess.check_output([exe], text=True)
    rows = []
    for line in out.strip().split('\n')[1:]:
        c = line.split(',')
        rows.append((c[0], c[1], c[2], c[3], c[4]))
    return rows


def build(rows):
    worst = max(float(r[1]) for r in rows)
    L = []
    L.append('# 每帧 CPU 开销（实测）')
    L.append('')
    L.append('由 `scripts/report_cost.py` 生成，数据来自 `tools/hdr_cost`（本机 x86 实测，'
             '取 7 批 × 20 万次的中位数，先热身 2000 次）。单位：µs/次调用。')
    L.append('')
    L.append('| 用例 | µs/次 | 30 Hz 预算占比 | 60 Hz | 120 Hz |')
    L.append('|---|---|---|---|---|')
    for c, us, p30, p60, p120 in rows:
        L.append('| `%s` | %s | %s%% | %s%% | %s%% |' % (c, us, p30, p60, p120))
    L.append('')
    L.append('判据：最慢一次调用 **%.4f µs**，上限 %.1f µs（≈120 Hz 帧预算 8.33 ms 的 %.2f%%）。'
             % (worst, LIMIT_US, 100.0 * LIMIT_US / 8333.0))
    L.append('')
    L.append('说明：')
    L.append('')
    L.append('- 这套补丁在设备上每帧的 CPU 工作只有两件：1 KB 直方图的 blit 回读（driver 决定）'
             '与一次 `s2a_hdr_auto_l1b_step_fused_hz`。上表就是后者的实测。')
    L.append('- 逐像素函数（`s2a_hdr_itm_rgb`、`s2a_hdr_grade_rgb`、`s2a_hdr_sat_lab_rgb`）跑在 GPU 上，'
             'C 侧只被 `hdr_convert` 这类离线工具调用，把它们和帧预算比没有意义，故不列。')
    L.append('- 表里是快照，计时随机器/负载浮动（生成器只要求重测结果与表同一量级）；'
             '重新生成用 `python3 scripts/report_cost.py`。')
    L.append('- 本机是 x86 Ubuntu，不是 A 系列 CPU；所以结论按「数量级」读：亚微秒级，'
             '即使设备慢 30 倍也只占 120 Hz 帧预算的 0.2%。真机耗时未测（无设备通道）。')
    L.append('')
    return '\n'.join(L)


def main():
    rows = measure()
    text = build(rows)
    check = '--check' in sys.argv
    if check:
        # 计时天生带抖动（同机两次也能差 20%），所以不比对逐位数字，只校验：
        #   1) 用例集合没变（漏了新用例 = 报告过期）；
        #   2) 现有表里的数字与本次实测同一数量级（比值 <2 倍）；
        #   3) 本次实测没超过上限。
        if not os.path.exists(OUT):
            print('✗ docs/COST.md 不存在（重跑不带 --check 生成）')
            return 1
        old_rows = []
        for line in open(OUT, encoding='utf-8'):
            if line.startswith('| `'):
                c = [x.strip() for x in line.strip().strip('|').split('|')]
                old_rows.append((c[0].strip('`'), float(c[1])))
        if [r[0] for r in old_rows] != [r[0] for r in rows]:
            print('✗ docs/COST.md 的用例集合与工具输出不一致（重跑不带 --check 生成）')
            return 1
        for (name, old_us), r in zip(old_rows, rows):
            new_us = float(r[1])
            if old_us <= 0 or not (0.5 <= new_us / old_us <= 2.0):
                print('✗ %s 的数字与实测差太远（表里 %.4f，本次 %.4f）' % (name, old_us, new_us))
                return 1
        worst = max(float(r[1]) for r in rows)
        if worst > LIMIT_US:
            print('✗ 控制器调用 %.4f µs 超过上限 %.1f µs' % (worst, LIMIT_US))
            return 1
        print('✓ docs/COST.md 与实测同量级（%d 用例）；最慢 %.4f µs（上限 %.1f µs）'
              % (len(rows), worst, LIMIT_US))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(text)
    print('✓ 写入 docs/COST.md（%d 行）' % len(text.split('\n')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
