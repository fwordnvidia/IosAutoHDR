// 内容映射（S2A_HDR_MAP=1 → BT.2408）的单元测试。
//
// 为什么要有这一档：默认的 knee 方案「纸白以下一律不动」在游戏内容上几乎看不出 HDR——
//   docs/FIDELITY.md 实测码值有变化的像素只有 0.02%–0.41%。RTX HDR / BT.2408 的思路相反：
//   整幅按 L_out = head * L^1.15 抬起来（黑仍 0、白正好落 head），观感上才是「换了个显示器件」。
//   两种都对，问题是以前设备上只有 knee 一种，用户没得选。
//
// 这个套件守三件事：
//   1. 缺省档不能变（map=0 必须与 s2a_hdr_grade_rgb 逐位相同）——否则所有历史结论作废；
//   2. map=1 的数学性质（黑→0、白→head、中调被抬起、增益随亮度单调、不超 head）；
//   3. C 侧与上屏 MSL 的**逐行转写**在同一网格上逐位相同（这是最容易漂的地方）。
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "nvn_hdr.h"
#include "nvn_hdr_msl.h"

static int g_fail, g_total;

static void ok(int cond, const char *what) {
    g_total++;
    if (!cond) { printf("  ✗ %s\n", what); g_fail++; }
}
static void close_to(float a, float b, float tol, const char *what) {
    g_total++;
    if (!(fabsf(a - b) <= tol)) { printf("  ✗ %s（%.9f vs %.9f）\n", what, a, b); g_fail++; }
}
static const float *grid(int i) {
    static float v[3];
    v[0] = (float)((i * 7) % 11) / 10.0f;
    v[1] = (float)((i * 3) % 13) / 12.0f;
    v[2] = (float)((i * 5) % 17) / 16.0f;
    return v;
}

int main(void) {

    // ── 一、环境变量解析：拼错就回缺省，不猜 --------------------------------
    ok(s2a_hdr_map_parse(NULL) == 0, "未设置 → knee");
    ok(s2a_hdr_map_parse("") == 0, "空串 → knee");
    ok(s2a_hdr_map_parse("0") == 0, "\"0\" → knee");
    ok(s2a_hdr_map_parse("knee") == 0, "\"knee\" → knee");
    ok(s2a_hdr_map_parse("1") == 1, "\"1\" → bt2408");
    ok(s2a_hdr_map_parse("bt2408") == 1, "\"bt2408\" → bt2408");
    ok(s2a_hdr_map_parse("content") == 1, "\"content\" → bt2408");
    ok(s2a_hdr_map_parse("bt") == 0, "半截 \"bt\" 不算（不做前缀猜测）");
    ok(s2a_hdr_map_parse("yes") == 0, "不认识的值回缺省");
    ok(s2a_hdr_map() == 0, "本进程没设 S2A_HDR_MAP → 缺省 knee");

    // ── 二、缺省档逐位不变 --------------------------------------------------
    {
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.4f; p.knee_hi = 0.9f;
        int diff = 0;
        for (int i = 0; i < 500; i++) {
            const float *c = grid(i);
            float lin[3], a[3], b[3];
            for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(c[k]);
            s2a_hdr_grade_rgb(&p, 1.0f, 0.0f, lin, a);
            s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 0, lin, b);
            if (memcmp(a, b, sizeof a) != 0) diff++;
        }
        ok(diff == 0, "map=0 与 s2a_hdr_grade_rgb 逐位相同（500 组）");
        int diff2 = 0;
        for (int i = 0; i < 200; i++) {
            const float *c = grid(i + 3);
            float lin[3], a[3], b[3];
            for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(c[k]);
            s2a_hdr_grade_rgb(&p, 1.25f, 0.5f, lin, a);
            s2a_hdr_grade_rgb_map(&p, 1.25f, 0.5f, 0, lin, b);
            if (memcmp(a, b, sizeof a) != 0) diff2++;
        }
        ok(diff2 == 0, "map=0 在 sat/p3 也打开时同样逐位相同（200 组）");
    }

    // ── 三、BT.2408 的数学性质 ----------------------------------------------
    {
        s2a_hdr_itm p; p.head = 2.03f; p.knee_lo = 0.5f; p.knee_hi = 1.0f;
        float black[3] = {0.0f, 0.0f, 0.0f}, out[3];
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, black, out);
        ok(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "黑仍映射到 0（不是抬灰）");

        float white[3] = {1.0f, 1.0f, 1.0f};
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, white, out);
        close_to(out[0], 2.03f, 1e-6f, "线性 1.0 正好落在 head（BT.2408 参考白 → 203 nit）");

        // 与 s2a_hdr_bt2408_rgb_lin 同源：sat=1、p3=0 时必须逐位一致
        int diff = 0;
        for (int i = 0; i < 300; i++) {
            const float *c = grid(i + 2);
            float lin[3], a[3], b[3];
            for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(c[k]);
            s2a_hdr_bt2408_rgb_lin(lin, p.head, a);
            s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, lin, b);
            if (memcmp(a, b, sizeof a) != 0) diff++;
        }
        ok(diff == 0, "sat/p3 关闭时与 s2a_hdr_bt2408_rgb_lin 逐位相同（300 组）");

        // 中调：线性 0.18（18% 灰）在 knee 下不动，在 bt2408 下被抬起约 ×1.57
        float mid[3] = {0.18f, 0.18f, 0.18f}, mk[3], mb[3];
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 0, mid, mk);
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, mid, mb);
        close_to(mk[0] / mid[0], 1.0f, 1e-6f, "knee：线性 0.18 完全不动");
        ok(mb[0] / mid[0] > 1.55f && mb[0] / mid[0] < 1.60f, "bt2408：线性 0.18 抬到 ×1.55–1.60");
        ok(mb[0] > mk[0], "同一中调上 bt2408 比 knee 亮（这就是「看得出 HDR」的来源）");

        // 增益随亮度单调（否则会出现反相的对比度压缩）
        float ys[5] = {0.02f, 0.05f, 0.18f, 0.5f, 1.0f};
        float prev = 0.0f; int mono = 1;
        for (int i = 0; i < 5; i++) {
            float in[3] = {ys[i], ys[i], ys[i]}, o[3];
            s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, in, o);
            float g = o[0] / ys[i];
            if (g < prev - 1e-6f) mono = 0;
            prev = g;
        }
        ok(mono, "增益随亮度单调不减（0.02→1.0 五点）");

        // 不超 head：极端饱和度 + bt2408 也一样
        int over = 0;
        for (int i = 0; i < 200; i++) {
            const float *c = grid(i + 5);
            float lin[3], o[3];
            for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(c[k]);
            s2a_hdr_grade_rgb_map(&p, 3.0f, 1.0f, 1, lin, o);
            for (int k = 0; k < 3; k++) if (!(o[k] <= p.head + 1e-5f)) over++;
        }
        ok(over == 0, "sat=3.0 / p3=1.0 下也没有通道超 head（200 组）");
    }

    // ── 四、C 与上屏 MSL 转写逐位一致（这一档也会上屏，必须对账）--------------
    {
        float heads[3] = {1.0f, 2.0f, 2.03f};
        float sats[3] = {1.0f, 1.0f, 1.35f};
        float p3s[2] = {0.0f, 0.6f};
        int bad = 0, worst_i = -1;
        float worst = 0.0f;
        for (int hh = 0; hh < 3; hh++)
            for (int si = 0; si < 3; si++)
                for (int pi = 0; pi < 2; pi++)
                    for (int i = 0; i < 60; i++) {
                        const float *c = grid(i * 3 + hh);
                        s2a_hdr_itm p;
                        p.head = heads[hh]; p.knee_lo = 0.35f; p.knee_hi = 0.95f;
                        float lin[3], a[3], b[3];
                        for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(c[k]);
                        s2a_hdr_grade_rgb_map(&p, sats[si], p3s[pi], 1, lin, a);
                        msl_fs_map(c, p.knee_lo, p.knee_hi, p.head, sats[si], p3s[pi], 1, b);
                        for (int k = 0; k < 3; k++) {
                            float d = fabsf(a[k] - b[k]);
                            if (d > worst) { worst = d; worst_i = i; }
                            if (d > 2e-6f) bad++;
                        }
                    }
        ok(bad == 0, "C 与上屏 MSL 转写在 map=1 上逐通道一致（3×3×2×60 组）");
        printf("    最大通道差 %.9f（组 %d）\n", worst, worst_i);

        // msl_fs（map 缺省）必须等于 msl_fs_map(...,0)：保证老调用点语义没被改动
        int same = 0;
        for (int i = 0; i < 100; i++) {
            const float *c = grid(i + 1);
            float a[3], b[3];
            msl_fs(c, 0.4f, 0.9f, 2.0f, 1.2f, 0.3f, a);
            msl_fs_map(c, 0.4f, 0.9f, 2.0f, 1.2f, 0.3f, 0, b);
            if (memcmp(a, b, sizeof a) == 0) same++;
        }
        ok(same == 100, "msl_fs 等价于 msl_fs_map(map=0)（100 组）");
    }

    // ── 五、退化输入 ---------------------------------------------------------
    {
        s2a_hdr_itm p; p.head = 0.0f; p.knee_lo = 0.5f; p.knee_hi = 1.0f;
        float in[3] = {0.5f, 0.5f, 0.5f}, out[3];
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, in, out);
        ok(out[0] > 0.0f && isfinite(out[0]), "head=0 时回落到 BT2408_HEAD 而不是输出 0 或 NaN");
        s2a_hdr_grade_rgb_map(NULL, 1.0f, 0.0f, 1, in, out);
        ok(isfinite(out[0]) && out[0] > 0.0f, "p=NULL 安全");
        float zero[3] = {0.0f, 0.0f, 0.0f}, o2[3];
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0.0f, 1, zero, o2);
        ok(o2[0] == 0.0f && o2[1] == 0.0f && o2[2] == 0.0f, "y=0 走 0.0f 分支（不产生 pow(0)·NaN）");
        int nanfree = 1;
        for (int i = 0; i < 300; i++) {   // 含 0/1 端点与极端组合
            const float *c = grid(i);
            float lin[3], o[3];
            for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(c[k]);
            s2a_hdr_grade_rgb_map(&p, 3.0f, 1.0f, 1, lin, o);
            for (int k = 0; k < 3; k++) if (!isfinite(o[k])) nanfree = 0;
        }
        ok(nanfree, "300 组极端输入无 NaN/Inf");
    }

    printf("t_hdr_map：内容映射（BT.2408）%d 项（失败 %d）\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
