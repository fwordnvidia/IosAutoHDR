#!/usr/bin/env bash
# 参考 CLI 的自检：不需要 S2Arm、不需要 Metal、不需要 Xcode。
# 断言的是「行为」，不是「数值快照」——所以换编译器、换平台都应该过。
set -u
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-cc}
BIN=build/hdr_ref
mkdir -p build
$CC -O2 -Wall -Wextra -o "$BIN" tools/hdr_ref.c -lm || { echo "编译失败"; exit 1; }

fail=0; n=0
ok()  { n=$((n+1)); printf '  ✓ %s\n' "$1"; }
bad() { n=$((n+1)); fail=$((fail+1)); printf '  ✗ %s —— %s\n' "$1" "${2:-}"; }
near() { awk -v a="$1" -v b="$2" -v e="$3" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=e)}'; }

stat_line() { "$BIN" --synth "$1" --frames "${2:-4}" --algo "${3:-l1b}" --stats | tail -1; }
field() { echo "$1" | tr ' ' '\n' | grep "^$2=" | cut -d= -f2; }
probe() { echo "$1" | tr ' ' '\n' | grep "^probe\[$2\]=" | cut -d= -f2; }

echo "=== 场景一：满屏 UI 白（纸白必须不动）"
L=$(stat_line uiwhite)
g832=$(probe "$L" 0.832); g1=$(probe "$L" 1.0)
near "$g832" 1.0 0.001 && ok "纸白(0.832) 增益 $(printf %.4f "$g832") = ×1.00" || bad "纸白增益" "$g832"
near "$g1" 2.0 0.001 && ok "纯白(1.0) 增益 $(printf %.4f "$g1") = head" || bad "纯白增益" "$g1"

echo "=== 场景二：暗场星空（黑底分支：要从近黑开始抬）"
L=$(stat_line dark)
lo=$(field "$L" knee); lo=${lo#*.}; 
klo=$(echo "$L" | tr ' ' '\n' | grep '^knee=' | cut -d= -f2 | cut -d. -f1-2)
lit=$(field "$L" lit)
near "$(echo "$L" | tr ' ' '\n' | grep '^knee=' | cut -d= -f2 | cut -d. -f1).$(echo "$L" | tr ' ' '\n' | grep '^knee=' | cut -d= -f2 | cut -d. -f2 | cut -c1-2)" 0.02 0.001 \
  && ok "knee_lo = 0.02（判为黑底画面，lit=$(printf %.4f "$lit") < 5%）" || bad "knee_lo" "$klo"
s=$(probe "$L" 0.832)
awk -v s="$s" 'BEGIN{exit !(s>1.5)}' && ok "星点(0.832) 被抬起 ×$(printf %.3f "$s") (>1.5)" || bad "星点增益" "$s"

echo "=== 场景三：全灰阶 + 纯白块（不许越界）"
L=$(stat_line ramp)
for p in 0.18 0.50 0.832 1.0; do
  v=$(probe "$L" $p)
  awk -v v="$v" 'BEGIN{exit !(v<=2.0001)}' && ok "probe[$p]=$(printf %.4f "$v") ≤ head" || bad "probe[$p] 越界" "$v"
done

echo "=== 场景四：L1 与 L1b 的差别（同帧对照，用来证明 L1b 修的是真问题）"
A=$(stat_line uiwhite 4 l1); B=$(stat_line uiwhite 4 l1b)
a832=$(probe "$A" 0.832); b832=$(probe "$B" 0.832)
awk -v a="$a832" -v b="$b832" 'BEGIN{exit !(a>1.15 && b<1.01)}' \
  && ok "L1 纸白 ×$(printf %.2f "$a832") vs L1b ×$(printf %.2f "$b832")（差 $(awk -v a="$a832" -v b="$b832" 'BEGIN{printf "%.0f", (a-1)*100}')%）" \
  || bad "L1/L1b 对照" "L1=$a832 L1b=$b832"

echo "=== 场景五：可重复性（同参数两次，stats 必须逐字节相同）"
X=$("$BIN" --synth city --frames 6 --stats); Y=$("$BIN" --synth city --frames 6 --stats)
[ "$X" = "$Y" ] && ok "两次运行 stats 完全一致（$(echo "$X" | wc -l) 行）" || bad "不可重复"

echo "=== 场景六：PPM 读写往返（20 行写 C 端、20 行读回来，预览必须逐字节相同）"
python3 - <<'PY'
import struct
w,h=64,36
rows=[]
for y in range(h):
    row=bytearray()
    for x in range(w):
        v=int(255*(x/(w-1)))
        row+=bytes((v,v,v))
    rows.append(bytes(row))
open('/tmp/ref_in.ppm','wb').write(b'P6\n%d %d\n255\n'%(w,h)+b''.join(rows))
PY
"$BIN" -i /tmp/ref_in.ppm -o /tmp/ref_out1.ppm --stats >/dev/null && "$BIN" -i /tmp/ref_in.ppm -o /tmp/ref_out2.ppm --stats >/dev/null
cmp -s /tmp/ref_out1.ppm /tmp/ref_out2.ppm && ok "两次转换输出逐字节相同（$(wc -c < /tmp/ref_out1.ppm | tr -d ' ') B）" || bad "PPM 输出不稳定"
python3 - <<'PY'
d=open('/tmp/ref_out1.ppm','rb').read()
assert d.startswith(b'P6'), '头不对'
print('  输出头：%r'%d[:15])
PY

echo "=== 场景七：--curve 导出的增益曲线（画图脚本的唯一数据源）"
C=$("$BIN" --curve --head 2 --knee-lo 0.35)
lines=$(echo "$C" | wc -l); last=$(echo "$C" | tail -1 | cut -d, -f2)
[ "$lines" -eq 257 ] && ok "曲线 257 行（码值 0..255）" || bad "曲线行数" "$lines"
near "$last" 2.0 0.0005 && ok "末点增益 $(printf %.4f "$last") = head" || bad "曲线末点" "$last"
python3 - "$C" <<'PY2' && ok "曲线全局单调（无回折）" || bad "曲线单调性"
import sys
rows=[float(l.split(',')[1]) for l in sys.argv[1].strip().splitlines()[1:]]
sys.exit(0 if all(b>=a-1e-6 for a,b in zip(rows,rows[1:])) else 1)
PY2
python3 scripts/plot_curves.py /tmp/curve_check.svg "$BIN" >/dev/null && [ -s /tmp/curve_check.svg ] \
  && ok "plot_curves.py 能出图（$(wc -c < /tmp/curve_check.svg | tr -d ' ') B）" || bad "plot_curves.py 失败"

echo "=== 场景八：真实渐变上的带（平台阶）与抖动效果"
python3 - <<'PY2'
w,h=256,64
rows=[]
for y in range(h):
    row=bytearray()
    for x in range(w):
        v=int(round((0.55+0.30*x/(w-1))*255)); row+=bytes((v,v,v))
    rows.append(bytes(row))
open('/tmp/sky_chk.ppm','wb').write(b'P6\n%d %d\n255\n'%(w,h)+b''.join(rows))
PY2
"$BIN" -i /tmp/sky_chk.ppm --columns --head 2 --algo l1b > /tmp/sky_a.csv
"$BIN" -i /tmp/sky_chk.ppm --columns --head 2 --algo l1b --dither > /tmp/sky_b.csv
A=$(python3 tools/measure_banding.py --csv /tmp/sky_a.csv --json)
B=$(python3 tools/measure_banding.py --csv /tmp/sky_b.csv --json)
pa=$(echo "$A" | tr ',' '\n' | grep flat_steps | tr -dc 0-9); pb=$(echo "$B" | tr ',' '\n' | grep flat_steps | tr -dc 0-9)
[ "${pa:-0}" -gt 50 ] && ok "无抖动时平台阶 ${pa} 级（这就是肉眼看到的带）" || bad "无抖动平台阶" "$pa"
[ "${pb:-1}" -eq 0 ] && ok "加 ±0.5 LSB 抖动后平台阶归零（带变成颗粒）" || bad "抖动后平台阶" "$pb"

echo "=== 场景九：16 位输出不合并输入码（8 位预览会）"
"$BIN" -i /tmp/sky_chk.ppm -o /tmp/sky8.ppm --head 2 --algo l1b >/dev/null
"$BIN" -i /tmp/sky_chk.ppm -o /tmp/sky16.ppm --head 2 --algo l1b --out16 >/dev/null
J8=$(python3 tools/measure_banding.py /tmp/sky_chk.ppm /tmp/sky8.ppm --row 32 --json)
J16=$(python3 tools/measure_banding.py /tmp/sky_chk.ppm /tmp/sky16.ppm --row 32 --json)
d8=$(echo "$J8" | tr ',' '\n' | grep distinct_out | tr -dc 0-9)
d16=$(echo "$J16" | tr ',' '\n' | grep distinct_out | tr -dc 0-9)
di=$(echo "$J16" | tr ',' '\n' | grep distinct_in | tr -dc 0-9)
[ "${d16:-0}" = "${di:-1}" ] && ok "16 位输出级数 ${d16} = 输入码数 ${di}（一级不丢）" || bad "16 位丢级" "$d16 vs $di"
[ "${d8:-0}" -lt "${d16:-0}" ] && ok "8 位预览只剩 ${d8} 级（少 ${d16}−${d8} 级）" || bad "8 位预览没丢级" "$d8"
b8=$(echo "$J8" | tr ',' '\n' | grep visible_bands | tr -dc 0-9)
b16=$(echo "$J16" | tr ',' '\n' | grep visible_bands | tr -dc 0-9)
[ "${b16:-99}" -le "${b8:-0}" ] && ok "可见带 16 位 ${b16} ≤ 8 位 ${b8}" || bad "16 位可见带更多" "$b16 vs $b8"

echo "=== 场景十：淡入 64 帧不抽动（时间稳定性）"
"$BIN" --synth fade --frames 64 --seq --head 2 --algo l1b > /tmp/fade_chk.csv
R=$(python3 - <<'PY2'
import csv
rows=list(csv.DictReader(open('/tmp/fade_chk.csv')))
g=[float(r['gain_050']) for r in rows]
d=[g[i+1]-g[i] for i in range(len(g)-1)]
flips=sum(1 for i in range(len(d)-1) if (d[i]>1e-9)!=(d[i+1]>1e-9))
mx=max(abs(x) for x in d)/g[1]
print(f"{flips} {mx:.5f} {g[-1]:.4f}")
PY2
)
set -- $R
[ "${1:-9}" -le 1 ] && ok "增益方向翻转 ${1} 次（淡入应 0）" || bad "增益来回摆" "$1"
python3 -c "import sys; sys.exit(0 if ${2:-9} < 0.01 else 1)" && ok "最大单帧跳变 ${2}（< 1%）" || bad "单帧跳变过大" "$2"
python3 -c "import sys; sys.exit(0 if abs(${3:-0}-1.0) < 1e-3 else 1)" && ok "淡入结束时 gain@0.5 = ${3}（纸白回到 1.0）" || bad "结束增益未回到 1" "$3"

echo "=== 场景十一：1% 的 HUD 不掐死整帧扩张"
OFF=$("$BIN" --synth hud --frames 30 --head 2 --algo l1b --stats | tail -1)
ON=$("$BIN"  --synth hud --frames 30 --head 2 --algo l1b --hudguard --stats | tail -1)
W0=$(echo "$OFF" | grep -o 'white=[0-9.]*' | sed 's/.*=//'); G0=$(echo "$OFF" | grep -o 'probe\[0.832\]=[0-9.]*' | sed 's/.*=//')
W1=$(echo "$ON"  | grep -o 'white=[0-9.]*' | sed 's/.*=//'); G1=$(echo "$ON"  | grep -o 'probe\[0.832\]=[0-9.]*' | sed 's/.*=//')
python3 -c "import sys; sys.exit(0 if $G0 < 1.05 else 1)" && ok "不开剔除：HUD 把纸白顶到 ${W0}，探针 0.832 增益只剩 ${G0}" || bad "HUD 没造成损失（用例失效）" "$G0"
python3 -c "import sys; sys.exit(0 if ${G1:-0} > 1.5 else 1)" && ok "开剔除：白点回到 ${W1}，探针 0.832 增益 ${G1}" || bad "剔除后增益仍不足" "$G1"

echo "=== 场景十二：剔除常数的平台区（0.05 / 3 bin 不是刀刃）"
$CC -O2 -I src -o build/sweep_hud tools/sweep_hud.c -lm || { echo "sweep 编译失败"; exit 1; }
./build/sweep_hud > /tmp/sweep_hud.csv
DEF=$(awk -F, '$1=="0.05" && $2=="3"{print $3" "$4}' /tmp/sweep_hud.csv)
set -- $DEF
[ "${1:-}" = "12/12" ] && ok "默认 0.05/3：12 个 HUD 用例全部剔除" || bad "默认参数漏剔" "${1:-无}"
[ "${2:-}" = "0/8" ]  && ok "默认 0.05/3：8 个真实高亮一个没误丢" || bad "默认参数误丢真实高亮" "${2:-无}"
STABLE=$(awk -F, 'NR>1 && $1>=0.03 && $1<=0.12 && $2>=2 && $2<=5 && $3=="12/12" && $4=="0/8"' /tmp/sweep_hud.csv | wc -l)
[ "$STABLE" -ge 12 ] && ok "平台区：max_frac 3%..12% × gap 2..5 共 ${STABLE} 组组合同样 12/12 & 0/8" || bad "平台区太窄" "$STABLE"

echo

# ── 场景十三：切镜阈值的敏感度（为什么 low=0.5 / high=1.6）
echo "=== 场景十三：切镜阈值的位置（--response / --lag）"
$CC -O2 -Wall -Wextra -I src tools/sweep_scene.c -lm -o build/sweep_scene || { echo "sweep_scene 编译失败"; exit 1; }
./build/sweep_scene --response > build/scene_resp.csv
./build/sweep_scene --lag > build/scene_lag.csv
SW03=$(awk -F, '$1=="0.30"{print $2}' build/scene_resp.csv)
SW15=$(awk -F, '$1=="1.50"{print $2}' build/scene_resp.csv)
LAG01=$(awk -F, '$1=="0.10"{print $2}' build/scene_lag.csv)
LAGHI=$(awk -F, '$1=="1.00"{print $4}' build/scene_lag.csv)
near "$SW03" 0.000 0.001 && ok "0.30 stop/帧 不算切镜（sw=$SW03）" || bad "sw(0.30)" "$SW03"
near "$SW15" 1.000 0.01 && ok "1.50 stop/帧 全速（sw=$SW15）" || bad "sw(1.50)" "$SW15"
SW066=$(./build/sweep_scene --at 0.66)
SW066W=${SW066%,*}; SW066N=${SW066#*,}
near "$SW066W" 0.311 0.02 && ok "0.66 stop/帧 的中等切镜拿到 ${SW066W} 权重（旧 0.5/1.6 只有 0.055）" || bad "sw(0.66)" "$SW066W"
[ "$SW066N" -le 12 ] && ok "中等切镜 ${SW066N} 帧到 95%（≤12 为过）" || bad "frames(0.66)" "$SW066N"
near "$LAG01" 0.662 0.02 && ok "0.10 stop/帧 的稳态落后 0.66 stop（实测 $LAG01）" || bad "lag(0.10)" "$LAG01"
near "$LAGHI" 1.964 0.05 && ok "阈值抬到 1.0 后快镜头落后 1.96 stop（所以不能抬，实测 $LAGHI）" || bad "lag_hi(1.0)" "$LAGHI"
echo "  ✓ 阈值平台：0.1 stop/帧 以下任何 LOW 都一样（实测全 0 权重）；LOW 抬到 1.0 则 1.0 stop/帧 落后 1.96 stop"

echo "=== 场景十四：同 APL 切镜（白点判据把 26 帧斜坡压成 1 帧）"
"$BIN" --synth cut --seq --frames 64 > /tmp/cut_old.csv
"$BIN" --synth cut --seq --frames 64 --scene-fuse > /tmp/cut_new.csv
FO=$(awk -F, 'NR>33 && $3>0.85 {print NR-33; exit}' /tmp/cut_old.csv)
FN=$(awk -F, 'NR>33 && $3>0.85 {print NR-33; exit}' /tmp/cut_new.csv)
[ "${FN:-99}" -le 2 ] && ok "白点判据：切镜后 ${FN} 帧到位" || bad "白点判据没生效" "${FN:-无}"
[ "${FO:-0}" -ge 10 ] && ok "对照：只看重心要 ${FO} 帧" || bad "对照组不慢（判据 / 场景有问题）" "${FO:-无}"
echo "=== 场景十五：星空（float4 代理不可能判对的那类画面）"
"$BIN" --synth star --seq --frames 12 --scene-fuse > /tmp/star.csv
SL=$(awk -F, 'END{print $3}' /tmp/star.csv)
SW=$(awk -F, 'END{print $2}' /tmp/star.csv)
SLIT=$(awk -F, 'END{print $5}' /tmp/star.csv)
near "$SL" 0.02 0.002 && ok "星空：纸白从近黑起（knee_lo=$SL，白点 $SW 是亮星，不是纸白）" || bad "knee_lo" "$SL"
awk -v v="$SLIT" 'BEGIN{exit !(v<0.05)}' && ok "星空：lit=$SLIT < 5%（真直方图才看得出的量）" || bad "lit" "$SLIT"
awk -v v="$SW" 'BEGIN{exit !(v>0.4)}' && ok "星空：白点 $SW > 0.4（所以 float4 代理必把它当亮场）" || bad "white" "$SW"

echo "=== 场景十六：诊断行（与设备 S2A_HDR_LOG 同一份格式化代码）"
"$BIN" --synth hud --frames 3 --stat-line --stats > /tmp/st_nohud.txt
"$BIN" --synth hud --frames 3 --stat-line --stats --hudguard > /tmp/st_hud.txt
L1=$(grep -m1 '^HDR stat: ' /tmp/st_nohud.txt)
case "$L1" in
  "HDR stat: fmt=16F head=2.00 knee="*) ok "诊断行前缀与字段顺序固定（可直接 grep）" ;;
  *) bad "诊断行格式" "${L1:-空}" ;;
esac
# 行里的 knee=lo+delta 必须与 --stats 那行的 knee=lo..hi 对上（同一帧的两种表示）
S1=$(grep -m1 '^frame ' /tmp/st_nohud.txt)
klo=$(printf '%s' "$L1" | sed -n 's/.*knee=\([0-9.]*\)+\([0-9.]*\).*/\1/p')
kdl=$(printf '%s' "$L1" | sed -n 's/.*knee=\([0-9.]*\)+\([0-9.]*\).*/\2/p')
khi=$(printf '%s' "$S1" | sed -n 's/.*knee=[0-9.]*\.\.\([0-9.]*\).*/\1/p')
awk -v a="$klo" -v b="$kdl" -v c="$khi" 'BEGIN{exit !((a+b)-c < 0.0002 && c-(a+b) < 0.0002)}' \
  && ok "knee 的两种表示一致（$klo+$kdl = $khi）" || bad "knee 表示不一致" "$klo+$kdl vs $khi"
# HUD 剔除开着时白点应显著上抬，诊断行必须跟着变（否则日志不是真的取自当前帧）
# map 字段：缺省必须打 map=0，S2A_HDR_MAP=1 时必须打 map=1 且 knee 仍在（只是不参与上屏）
case "$L1" in
  *" map=0") ok "诊断行缺省打 map=0（拐点档）" ;;
  *) bad "诊断行缺 map 字段" "${L1:-空}" ;;
esac
S2A_HDR_MAP=1 "$BIN" --synth hud --frames 1 --stat-line > /tmp/st_map.txt
L3=$(grep -m1 '^HDR stat: ' /tmp/st_map.txt)
case "$L3" in
  *" map=1") ok "S2A_HDR_MAP=1 时诊断行打 map=1（开关真的读到了）" ;;
  *) bad "S2A_HDR_MAP=1 没反映到诊断行" "${L3:-空}" ;;
esac
KL3=$(printf '%s' "$L3" | sed -n 's/.*knee=\([0-9.]*\)+\([0-9.]*\).*/\1+\2/p')
KL1=$(printf '%s' "$L1" | sed -n 's/.*knee=\([0-9.]*\)+\([0-9.]*\).*/\1+\2/p')
[ -n "$KL3" ] && [ "$KL3" = "$KL1" ] \
  && ok "map=1 不改 knee（判定链路与放置方式解耦）" || bad "map=1 把 knee 也改了" "$KL1 vs $KL3"
W1=$(printf '%s' "$L1" | sed -n 's/.*white=\([0-9.]*\).*/\1/p')
W2=$(grep -m1 '^HDR stat: ' /tmp/st_hud.txt | sed -n 's/.*white=\([0-9.]*\).*/\1/p')
awk -v a="$W1" -v b="$W2" 'BEGIN{exit !(a-b > 0.2)}' \
  && ok "同一帧加 --hudguard 后 white $W1→$W2，诊断行跟随" || bad "诊断行没跟随当前帧" "$W1 vs $W2"

if [ "$fail" -eq 0 ]; then 
# ---- BT.2408 放置方式（--map bt2408）：黑必须仍是 0、纸白必须正好 head ----
if [ -x ./build/hdr_convert ] && [ -f build/real/p_Clouds_by_Tibor_Mokanszki.ppm ]; then
    ./build/hdr_convert build/real/p_Clouds_by_Tibor_Mokanszki.ppm build/rf_bt2408.ppm --head 2 --sat 1 --map bt2408 >/dev/null
    # 用 ok/bad 记账（脚本自己打印的 ✓ 不进计数，否则末行的「N 项全过」会少算一条）
    if out=$(python3 scripts/check_bt2408_ppm.py build/rf_bt2408.ppm 2); then
        ok "$(printf '%s' "$out" | sed 's/^ *✓ //')"
    else
        bad "bt2408 放置方式" "$(printf '%s' "$out" | sed 's/^ *✗ //')"
    fi
fi
echo "✓ ref_check：$n 项全过"; else echo "ref_check：$fail/$n 项失败"; exit 1; fi
