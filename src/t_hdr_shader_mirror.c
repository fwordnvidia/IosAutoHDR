// 上屏 fragment shader 与 CPU 参考实现的逐像素对账
//
// 为什么需要：`src/nvn_mtl.m` 里内联的 `src_msl_hdr`（1460→3159 B）是**每像素**执行的，
// 它决定最终画面上是什么颜色；而 `docs/GALLERY.md`、`docs/REAL-CONTENT.md`、`run_all.sh`
// 的所有数字都来自 CPU 侧的 `s2a_hdr_grade_rgb`。两边只要有一处算式/顺序不同，
// 「测试通过」就不代表「设备上看到的是这个」。
//
// 抓到的真分歧：MSL 末尾有 `if (!(hi > lo + 1e-5f)) return a * c.knee.z;`（整帧直接吃满 head），
//   CPU 的 `s2a_hdr_itm_gain` 原来没有这条 ⇒ knee_lo == knee_hi 时 smoothstep 的 (x-e0)/0
//   落进「<= 0」分支给 gain 1，两侧差整整一个 head（实测 0.214041 vs 0.428082）。
//   已在 s2a_hdr_itm_gain 补同一分支。
#include <stdio.h>
#include <math.h>
#include "nvn_hdr.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(...) do { checks++; printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

#include "nvn_hdr_msl.h"

static float diff3(const float a[3], const float b[3]) {
    float m = 0.0f;
    for (int i = 0; i < 3; i++) { float d = fabsf(a[i] - b[i]); if (d > m) m = d; }
    return m;
}

int main(void) {
    printf("上屏 fragment shader 与 CPU 参考实现对账\n");

    // 场景一：3^9 编码值 × 拐点 × head × 饱和度 × 色域
    const float codes[9] = {0.0f, 0.02f, 0.05f, 0.18f, 0.35f, 0.5f, 0.74f, 0.9f, 1.0f};
    const float heads[3] = {2.0f, 2.03f, 4.0f};
    const float los[3] = {0.55f, 0.35f, 0.6579f};
    const float his[3] = {1.0f, 0.74f, 0.9f};
    const float sats[3] = {1.0f, 1.15f, 2.0f};
    const float p3s[3] = {0.0f, 0.5f, 1.0f};
    int n = 0, bad = 0;
    float worst = 0.0f;
    for (int ci = 0; ci < 9; ci++) for (int cj = 0; cj < 9; cj++) for (int ck = 0; ck < 9; ck++)
    for (int li = 0; li < 3; li++) for (int hi_ = 0; hi_ < 3; hi_++) for (int hh = 0; hh < 3; hh++)
    for (int si = 0; si < 3; si++) for (int pi = 0; pi < 3; pi++) {
        float code[3] = {codes[ci], codes[cj], codes[ck]}, lin[3], a[3], b[3];
        for (int i = 0; i < 3; i++) lin[i] = s2a_hdr_to_linear(code[i]);
        msl_fs(code, los[li], his[hi_], heads[hh], sats[si], p3s[pi], a);
        s2a_hdr_itm p;
        p.head = heads[hh]; p.knee_lo = los[li]; p.knee_hi = his[hi_];
        s2a_hdr_grade_rgb(&p, sats[si], p3s[pi], lin, b);
        float d = diff3(a, b);
        n++;
        if (d > worst) worst = d;
        if (d > 2e-5f) bad++;
    }
    CHECK(bad == 0, "%d / %d 组超差（>2e-5），最大 %.7f", bad, n, (double)worst);
    OK("%d 组组合（编码值³ × 拐点 × head × 饱和度 × 色域）最大通道差 %.7f", n, (double)worst);

    // 场景一之后：两侧都不得把输出推到 head 之上（超了系统会硬钳 ⇒ 切色块/偏色）。
    // 这是本轮修掉的真缺陷：sat=3.0 时蓝通道曾被推到 head+2.05。
    {
        int over_msl = 0, over_cpu = 0; float worst_over = 0.0f;
        const float probes[4][3] = {{1.0f, 0.24f, 1.0f}, {1.0f, 0.0f, 0.6f}, {0.2f, 0.9f, 1.0f}, {1.0f, 1.0f, 0.1f}};
        for (int i = 0; i < 4; i++) {
            for (int s = 0; s < 3; s++) {
                float sat = s == 0 ? 1.6f : (s == 1 ? 2.0f : 3.0f);
                float a[3], b[3], lin[3];
                for (int c = 0; c < 3; c++) lin[c] = s2a_hdr_to_linear(probes[i][c]);
                msl_fs(probes[i], 0.55f, 1.0f, 2.0f, sat, 0.0f, a);
                s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.55f; p.knee_hi = 1.0f;
                s2a_hdr_grade_rgb(&p, sat, 0.0f, lin, b);
                for (int c = 0; c < 3; c++) {
                    if (a[c] > 2.0f + 1e-6f) over_msl++;
                    if (b[c] > 2.0f + 1e-6f) over_cpu++;
                    float ov = a[c] - 2.0f; if (ov > worst_over) worst_over = ov;
                }
            }
        }
        CHECK(over_msl == 0, "上屏没有超 head 的通道（%d 处，最大超 %.6f）", over_msl, (double)worst_over);
        CHECK(over_cpu == 0, "CPU 侧没有超 head 的通道（%d 处）", over_cpu);
    }

    // 场景二：退化拐点 lo == hi（这里就是修掉的那条分歧）
    {
        float code[3] = {0.5f, 0.5f, 0.5f}, lin[3], a[3], b[3];
        for (int i = 0; i < 3; i++) lin[i] = s2a_hdr_to_linear(code[i]);
        msl_fs(code, 0.5f, 0.5f, 2.0f, 1.0f, 0.0f, a);
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.5f; p.knee_hi = 0.5f;
        s2a_hdr_grade_rgb(&p, 1.0f, 0.0f, lin, b);
        CHECK(diff3(a, b) < 1e-6f, "lo==hi：上屏 %.6f / CPU %.6f", (double)a[0], (double)b[0]);
        OK("退化拐点 lo==hi：两侧整帧都给 head（%.6f）", (double)a[0]);
    }

    // 场景三：反向拐点 lo > hi
    {
        float code[3] = {0.5f, 0.5f, 0.5f}, lin[3], a[3], b[3];
        for (int i = 0; i < 3; i++) lin[i] = s2a_hdr_to_linear(code[i]);
        msl_fs(code, 0.9f, 0.3f, 2.0f, 1.0f, 0.0f, a);
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.9f; p.knee_hi = 0.3f;
        s2a_hdr_grade_rgb(&p, 1.0f, 0.0f, lin, b);
        CHECK(diff3(a, b) < 1e-6f, "lo>hi：上屏 %.6f / CPU %.6f", (double)a[0], (double)b[0]);
        OK("反向拐点 lo>hi：两侧一致（%.6f）", (double)a[0]);
    }

    // 场景四：head == 1（关掉扩张）应是恒等
    {
        int ident = 1;
        for (int i = 0; i < 9; i++) {
            float code[3] = {codes[i], codes[i] * 0.6f, codes[i] * 0.2f}, lin[3], a[3];
            for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(code[k]);
            msl_fs(code, 0.55f, 1.0f, 1.0f, 1.0f, 0.0f, a);
            for (int k = 0; k < 3; k++) if (fabsf(a[k] - lin[k]) > 1e-6f) ident = 0;
        }
        CHECK(ident, "head==1 时上屏应恒等（不等于线性输入）");
        OK("head==1：9 组颜色上屏输出 = 线性输入（恒等）");
    }

    // 场景五：中性灰在任何饱和度下逐位不动（灰恒为灰）
    {
        int same = 1;
        for (int i = 1; i < 9; i++) {
            float code[3] = {codes[i], codes[i], codes[i]}, a[3], g0[3];
            msl_fs(code, 0.55f, 1.0f, 2.0f, 1.0f, 0.0f, g0);
            for (int si = 1; si < 3; si++) {
                msl_fs(code, 0.55f, 1.0f, 2.0f, sats[si], 0.0f, a);
                for (int k = 0; k < 3; k++) if (a[k] != g0[k]) same = 0;
            }
        }
        CHECK(same, "中性灰在 sat=1.15/2.0 下应逐位不变");
        OK("中性灰：sat 1.15 / 2.0 下仍逐位等于 sat=1（8 组亮度）");
    }

    // 场景六：饱和度 0（全去色）与 p3 > 1（过冲夹紧）两侧一致
    {
        float code[3] = {0.9f, 0.2f, 0.05f}, lin[3], a[3], b[3];
        for (int k = 0; k < 3; k++) lin[k] = s2a_hdr_to_linear(code[k]);
        msl_fs(code, 0.4f, 0.9f, 2.0f, 0.0f, 0.0f, a);
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.4f; p.knee_hi = 0.9f;
        s2a_hdr_grade_rgb(&p, 0.0f, 0.0f, lin, b);
        CHECK(diff3(a, b) < 1e-6f, "sat=0：上屏 (%.4f,%.4f,%.4f) / CPU (%.4f,%.4f,%.4f)",
              (double)a[0], (double)a[1], (double)a[2], (double)b[0], (double)b[1], (double)b[2]);
        msl_fs(code, 0.4f, 0.9f, 2.0f, 1.0f, 1.5f, a);
        s2a_hdr_grade_rgb(&p, 1.0f, 1.5f, lin, b);
        CHECK(diff3(a, b) < 1e-6f, "p3=1.5：上屏 %.6f / CPU %.6f", (double)a[0], (double)b[0]);
        OK("边界旋钮：sat=0 全去色、p3=1.5 过冲夹紧，两侧一致");
    }

    printf("%s t_hdr_shader_mirror：上屏 shader 对账 %d 项（失败 %d）\n", fails ? "✗" : "✓", checks, fails);
    return fails ? 1 : 0;
}
