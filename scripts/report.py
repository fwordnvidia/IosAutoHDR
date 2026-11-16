#!/usr/bin/env python3
# 从 hdr_ref 自动生成 docs/QUALITY.md —— 文档里的数字必须能一条命令复现，不能手抄。
# 用法：report.py [hdr_ref 路径] [输出路径]
#   缺省：build/hdr_ref → docs/QUALITY.md
# 校验模式（供 CI / run_all 用）：report.py --check   → 现存的 QUALITY.md 与重新生成的内容不一致就 rc=1
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(binary, *args):
    out = subprocess.run([binary, *args], capture_output=True, text=True, check=True).stdout
    return out.strip()


def parse_stats(line):
    d = {}
    for k, v in re.findall(r'\b([a-z_0-9]+)=([-0-9.]+)(?![0-9.])', line):
        try:
            d[k] = float(v)
        except ValueError:
            pass
    m = {}
    for k, v in re.findall(r'probe\[([0-9.]+)\]=([-0-9.]+)', line):
        m[k] = float(v)
    d['probe'] = m
    kr = re.search(r'knee=([0-9.]+)\.\.([0-9.]+)', line)
    if kr:
        d['knee_lo'], d['knee_hi'] = float(kr.group(1)), float(kr.group(2))
    return d


SCENES = [
    ('uiwhite', [], '满屏 UI 白（纸白场景）'),
    ('dark', [], '暗场 + 星点'),
    ('city', [], '夜景（低 APL 但有高光）'),
    ('ramp', [], '灰阶渐变'),
    ('hud', [], 'HUD 压在画面上（不开剔除）'),
    ('hud', ['--hudguard'], '同上，开 HUD 剔除'),
]


def build(binary):
    lines = []
    lines.append('# 量化结果（自动生成，勿手改）\n')
    lines.append('本文件由 `scripts/report.py` 跑 `tools/hdr_ref` 生成；改算法后重新生成即可。\n')
    lines.append('约定：**SDR 白 = 203 nit**（BT.2408 的 graphics white），`--head 2` 即峰值 406 nit；')
    lines.append('`mean_nit` 是整帧平均亮度，`probe[x]` 是输入 `x`（线性）处的亮度增益。\n')

    lines.append('\n## 场景表（algo=l1b，head=2）\n')
    lines.append('| 场景 | 白点 | 重心 avg | 亮像素比 | knee 区间 | 平均亮度 | probe 0.18 | 0.50 | 0.832 | 1.0 |')
    lines.append('|---|---|---|---|---|---|---|---|---|---|')
    for syn, extra, name in SCENES:
        d = parse_stats(run(binary, '--synth', syn, '--stats', *extra).splitlines()[0])
        lines.append('| %s | %.4f | %+.4f | %.4f | %.3f..%.3f | %.1f nit | %.4f | %.4f | %.4f | %.4f |' % (
            name, d['white'], d['avg'], d['lit'], d['knee_lo'], d['knee_hi'], d['mean_nit'],
            d['probe']['0.18'], d['probe']['0.50'], d['probe']['0.832'], d['probe']['1.0']))

    # L1 vs L1b：纸白那条线到底动没动
    lines.append('\n## L1 与 L1b 的差别（同一帧，只换 alg）\n')
    lines.append('| 场景 | 算法 | 白点 | knee 区间 | probe 0.832 | probe 1.0 |')
    lines.append('|---|---|---|---|---|---|')
    for syn, name in [('uiwhite', '满屏 UI 白'), ('dark', '暗场'), ('city', '夜景')]:
        for algo in ('l1', 'l1b'):
            d = parse_stats(run(binary, '--synth', syn, '--stats', '--algo', algo).splitlines()[0])
            lines.append('| %s | %s | %.4f | %.3f..%.3f | %.4f | %.4f |' % (
                name, algo, d['white'], d['knee_lo'], d['knee_hi'], d['probe']['0.832'], d['probe']['1.0']))

    # head 扫描：纸白是否随 head 漂
    lines.append('\n## head 扫描（场景 uiwhite）：纸白不能随峰值漂\n')
    lines.append('| head | 峰值 nit | knee 区间 | 纸白增益 | 纯白增益 |')
    lines.append('|---|---|---|---|---|')
    for h in ('1', '1.5', '2', '4'):
        d = parse_stats(run(binary, '--synth', 'uiwhite', '--stats', '--head', h).splitlines()[0])
        lines.append('| %s | %.0f | %.3f..%.3f | %.4f | %.4f |' % (
            h, float(h) * 203.0, d['knee_lo'], d['knee_hi'], d['probe']['0.832'], d['probe']['1.0']))

    # 淡入：逐帧稳定性
    seq = run(binary, '--synth', 'fade', '--seq', '--frames', '64')
    rows = [r.split(',') for r in seq.splitlines()[1:]]
    g50 = [float(r[5]) for r in rows]
    g100 = [float(r[6]) for r in rows]
    jump = max(abs(g50[i + 1] - g50[i]) / max(g50[i], 1e-6) for i in range(len(g50) - 1))
    lines.append('\n## 淡入 64 帧（整帧亮度 0.18 → 1.0）\n')
    lines.append('- `gain@0.5`：%.4f → %.4f' % (g50[0], g50[-1]))
    lines.append('- `gain@1.0`：%.4f → %.4f（恒等于 head，与 APL 无关）' % (g100[0], g100[-1]))
    lines.append('- 单帧最大相对跳变：**%.3f%%**' % (jump * 100.0))

    # 同 APL 切镜：白点判据把 26 帧的斜坡压成 1 帧
    def frames_to_target(extra, tgt):
        out = run(binary, '--synth', 'cut', '--seq', '--frames', '64', *extra).splitlines()[1:]
        rows = [r.split(',') for r in out]
        for i, r in enumerate(rows[32:], start=32):
            if abs(float(r[2]) - tgt) < 0.05 * tgt:
                return i - 32
        return None

    a = frames_to_target([], 0.89735)
    b = frames_to_target(['--scene-fuse'], 0.89735)
    lines.append('\n## 同 APL 切镜（`--synth cut`：底色一样、只有白点从 0.55 跳到 0.95）\n')
    lines.append('| 场景判据 | 85% 到位所需帧数 |')
    lines.append('|---|---|')
    lines.append('| 只看重心（旧） | %s |' % (a if a is not None else '>32'))
    lines.append('| 重心 + 白点变化（现在上屏走的） | %s |' % (b if b is not None else '>32'))
    lines.append('\n只看重心时两段画面的 avg 只差约 0.05 stop，判不出切镜，knee 沿斜坡爬 20 多帧；')
    lines.append('白点差 0.79 stop 越过 `S2A_HDR_SCENE_HIGH`，融合后一帧到位。')

    # 平台阶 / 抖动
    lines.append('\n## 8 位输出里的平台阶（sky 渐变，第 32 行）\n')
    lines.append('输入是 256 列、0.55..0.85 的窄带灰阶渐变（`--row 32`，单行量，不跨行平均）。')
    lines.append('口径：「相邻列输出完全相同」= 平台阶 = 肉眼看到的带；「最大台阶」只统计 0.5 nit 以上的亮度。\n')
    lines.append('| 输出 | 输出级数 | 平台阶 | 最大台阶 | 可见带 |')
    lines.append('|---|---|---|---|---|')
    try:
        meas = os.path.join(ROOT, 'tools', 'measure_banding.py')
        t = os.path.join(ROOT, 'build')
        # 输入必须是一条真正的窄带平滑渐变（0.55..0.85 铺满 256 列）：
        # 用 --synth sky 那种带纹理的图去数「平台阶」会把纹理本身的列差也算进去，数字没有意义。
        src = os.path.join(t, 'rep_grad.ppm')
        w, h = 256, 64
        rows = []
        for _ in range(h):
            row = bytearray()
            for x in range(w):
                v = int(round((0.55 + 0.30 * x / (w - 1)) * 255))
                row += bytes((v, v, v))
            rows.append(bytes(row))
        with open(src, 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (w, h) + b''.join(rows))
        for depth, extra in (('8 位预览（限幅）', []),
                             ('8 位预览 + 抖动', ['--dither']),
                             ('16 位（上屏等价）', ['--out16'])):
            dst = os.path.join(t, 'rep_out.ppm')
            subprocess.run([binary, '-i', src, '-o', dst, '--head', '2', '--algo', 'l1b'] + extra,
                           capture_output=True, text=True, check=True)
            out = subprocess.run([sys.executable, meas, src, dst, '--row', '32'],
                                 capture_output=True, text=True, check=True).stdout
            lv = re.search(r'不同输出亮度\s*(\d+)', out)
            pl = re.search(r'平台阶\s*(\d+)', out)
            st = re.search(r'最大台阶\s*([0-9.]+)%', out)
            vb = re.search(r'可见带\s*(\d+)', out)
            lines.append('| %s | %s | %s | %s%% | %s |' % (
                depth, lv.group(1) if lv else '?', pl.group(1) if pl else '?',
                st.group(1) if st else '?', vb.group(1) if vb else '?'))
    except Exception as e:                      # 测量脚本缺失不该让整份报告失败
        lines.append('| （measure_banding.py 未跑通：%s） | | | | |' % e)

    # 线性输出（不做 8 位量化）= 上屏 RGBA16Float 的语义：这里才看得出抖动到底在做什么
    lines.append('\n线性输出（未量化，等价于 16F 上屏）：\n')
    lines.append('| 输出 | 输出级数 | 平台阶 | 最大台阶 | 可见带 |')
    lines.append('|---|---|---|---|---|')
    try:
        meas = os.path.join(ROOT, 'tools', 'measure_banding.py')
        t = os.path.join(ROOT, 'build')
        src = os.path.join(t, 'rep_grad.ppm')
        for name, extra in (('不抖动', []), ('±0.5 LSB 抖动', ['--dither'])):
            csvf = os.path.join(t, 'rep_cols.csv')
            out = run(binary, '-i', src, '--columns', '--head', '2', '--algo', 'l1b', *extra)
            with open(csvf, 'w', encoding='utf-8') as f:
                f.write(out + '\n')
            res = subprocess.run([sys.executable, meas, '--csv', csvf],
                                 capture_output=True, text=True, check=True).stdout
            lv = re.search(r'不同输出亮度\s*(\d+)', res)
            pl = re.search(r'平台阶\s*(\d+)', res)
            st = re.search(r'最大台阶\s*([0-9.]+)%', res)
            vb = re.search(r'可见带\s*(\d+)', res)
            lines.append('| %s | %s | %s | %s%% | %s |' % (
                name, lv.group(1) if lv else '?', pl.group(1) if pl else '?',
                st.group(1) if st else '?', vb.group(1) if vb else '?'))
    except Exception as e:
        lines.append('| （CSV 路径未跑通：%s） | | | | |' % e)

    lines.append('\n## 复现\n')
    lines.append('```bash')
    n_suites = sum(1 for f in os.listdir(os.path.join(HERE, "..", "src")) if re.match(r"t_hdr.*\.c$", f))
    lines.append('./run_all.sh            # 全部闸门（%d 套单测 + ref_check + 本报告 + 平台区扫描）' % n_suites)
    lines.append('python3 scripts/report.py   # 只重生成这份表')
    lines.append('```')
    return '\n'.join(lines) + '\n'


def main(argv):
    binary = os.path.join(ROOT, 'build', 'hdr_ref')
    out = os.path.join(ROOT, 'docs', 'QUALITY.md')
    check = '--check' in argv
    rest = [a for a in argv[1:] if a != '--check']
    if rest:
        binary = rest[0]
    if len(rest) > 1:
        out = rest[1]
    if not os.path.exists(binary):
        print('找不到 hdr_ref：%s（先跑 ./run_all.sh 或 make -C tools）' % binary)
        return 2
    text = build(binary)
    if check:
        old = open(out, encoding='utf-8').read() if os.path.exists(out) else ''
        if old != text:
            print('::error::docs/QUALITY.md 与实测不一致（改过算法没重新生成报告？）')
            return 1
        print('✓ docs/QUALITY.md 与实测一致（%d 行）' % text.count('\n'))
        return 0
    with open(out, 'w', encoding='utf-8') as f:
        f.write(text)
    print('✓ 已生成 %s（%d 行）' % (out, text.count('\n')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
