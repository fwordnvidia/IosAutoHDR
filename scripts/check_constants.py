#!/usr/bin/env python3
# 常数漂移闸门：nvn_hdr.h（CPU 参考）与上屏 MSL 字符串（GPU 实现）必须用同一组常数。
# 这两个文件本来就是「改一个必须同步改另一个」，靠人眼守不住（MSL 无 double、矩阵归一化都是踩过的坑）。
#
# 用法：check_constants.py <nvn_hdr.h> <src_msl_hdr.metal>
import re
import sys

REQUIRED = [
    # (名字, 说明)
    ('0.2126', 'Rec.709 亮度 R 权重'),
    ('0.7152', 'Rec.709 亮度 G 权重'),
    ('0.0722', 'Rec.709 亮度 B 权重'),
    ('0.04045', 'sRGB 解码分段点'),
    ('12.92', 'sRGB 解码低段斜率'),
    ('0.055', 'sRGB 解码偏移'),
    ('1.055', 'sRGB 解码增益'),
    ('2.4', 'sRGB 解码指数'),
]


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    hdr, msl = argv[1], argv[2]
    h = open(hdr, encoding='utf-8').read()
    m = open(msl, encoding='utf-8').read()
    bad = []

    # P3 矩阵：从 #define S2A_HDR_P3_Rn a, b, c 取值，逐个要求出现在 MSL 里
    rows = re.findall(r'#define\s+S2A_HDR_P3_R(\d)\s+([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f', h)
    if len(rows) != 3:
        bad.append('nvn_hdr.h 里没找到 3 行 S2A_HDR_P3_Rn（实得 %d）' % len(rows))
    for _, a, b, c in rows:
        for v in (a, b, c):
            if v not in m:
                bad.append('P3 常数 %sf 在 MSL 里缺失（CPU 侧有、GPU 侧没有 → 两端偏色）' % v)

    for lit, why in REQUIRED:
        if lit not in h:
            bad.append('nvn_hdr.h 缺 %s（%s）' % (lit, why))
        if lit not in m:
            bad.append('MSL 缺 %s（%s）' % (lit, why))

    # 曲线形状：gain = 1 + (head-1)*smoothstep，smoothstep 用 u*u*(3-2u)
    for pat, why in [(r'1\.0f\s*\+\s*\(c\.knee\.z\s*-\s*1\.0f\)', 'gain 曲线起点'),
                     (r'u\s*\*\s*u\s*\*\s*\(3\.0f\s*-\s*2\.0f\s*\*\s*u\)', 'smoothstep')]:
        if not re.search(pat, m):
            bad.append('MSL 缺 %s' % why)
    # 直方图侧常数：CPU 的 nvn_hdr_auto_l1.h 与 hdr_hist.metal 的同名量必须一致。
    # 重心门限那次分叉（CPU -4.7120 vs GPU -1.2049）就是没人盯这两个文件。
    pairs = [('S2A_HDR_AVG_CUT', r'kAvgCut\s*=\s*(-?[0-9.]+)f', '重心黑场门限'),
             ('S2A_HDR_BLACK_CUT', r'kBlackCut\s*=\s*(-?[0-9.]+)f', '白点黑场门限'),
             ('S2A_HDR_WHITE_FRAC', r'kWhiteFrac\s*=\s*([0-9.]+)f', '白点分位'),
             ('S2A_HDR_KNEE_TARGET', r'kKneeTarget\s*=\s*([0-9.]+)f', 'knee 目标'),
             ('S2A_HDR_SCENE_LOW', r'kSceneLow\s*=\s*([0-9.]+)f', '切镜下阈'),
             ('S2A_HDR_SCENE_HIGH', r'kSceneHigh\s*=\s*([0-9.]+)f', '切镜上阈')]
    import os
    d = os.path.dirname(hdr)
    l1 = open(hdr.replace('nvn_hdr.h', 'nvn_hdr_auto_l1.h'), encoding='utf-8').read()
    l0 = open(hdr.replace('nvn_hdr.h', 'nvn_hdr_auto.h'), encoding='utf-8').read()
    # 直方图常数在 hdr_hist.metal 里（不是上屏着色器），按同目录找
    # 直方图常数在 hdr_hist.metal（不是上屏着色器）。树里位置是 shaders/，打包目录里是 src/，
    # 也允许调用方直接把路径作为第三个参数传进来。
    cands = [os.path.join(d, 'hdr_hist.metal'),
             os.path.join(d, '..', '..', 'shaders', 'hdr_hist.metal'),
             os.path.join(d, '..', 'src', 'hdr_hist.metal')]
    if len(argv) > 3:
        cands.insert(0, argv[3])
    met = ''
    for c in cands:
        if os.path.exists(c):
            met = open(c, encoding='utf-8').read()
            break
    if not met and any(mac in ('S2A_HDR_AVG_CUT',) for mac, _, _ in pairs):
        bad.append('找不到 hdr_hist.metal（试过：%s），无法校验直方图侧常数' % ', '.join(cands))
    for mac, pat, why in pairs:
        mv = re.search(r'#define\s+' + mac + r'\s+\(?(-?[0-9.]+)f', l1) or \
             re.search(r'#define\s+' + mac + r'\s+\(?(-?[0-9.]+)f', l0)
        gv = re.search(pat, m) or (re.search(pat, met) if met else None)
        if not mv:
            bad.append('nvn_hdr_auto_l1.h 缺 %s（%s）' % (mac, why))
        elif not gv:
            bad.append('MSL 缺 %s（%s）' % (mac, why))
        elif abs(float(mv.group(1)) - float(gv.group(1))) > 1e-6:
            bad.append('%s 两端不一致：CPU %s vs MSL %s（%s）' % (mac, mv.group(1), gv.group(1), why))

    if 's2a_hdr_itm_gain' in h and 's2a_hdr_smoothstep' not in h:
        bad.append('nvn_hdr.h 缺 s2a_hdr_smoothstep')

    if bad:
        for b in bad:
            print('  ✗ ' + b)
        print('::error::常数漂移 %d 处' % len(bad))
        return 1
    print('✓ 常数一致：P3 三行 9 个数 + 亮度三权重 + sRGB 解码 5 常数 + gain/smoothstep 形状')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
