// HDR 出屏的纯 C 判据（graphics/nvn/gfx/nvn/nvn_hdr.h；nvn_mtl.m present_pipeline 的 MSL 用同一组常数与同一条曲线）。
// 这次改动要证明什么：直连层开 HDR 后，**画面里本来就有的高光**（sRGB 编码值接近 1 的像素）在 16F 目标里
//   线性值必须 > 1.0 才谈得上 EDR；中间调必须原样不动（否则整体变亮 = 洗白，不是 HDR）。
// 期望值全部手算（不跑被测实现）：
//   线性化用 sRGB EOTF 分段式：c ≤ 0.04045 时 c/12.92，否则 ((c+0.055)/1.055)^2.4。
//     sRGB 0.5  → ((0.555)/1.055)^2.4 = 0.526066^2.4 = 0.21396（容差 1e-4）
//     sRGB 0.8  → (0.810427)^2.4      = 0.60383
//     sRGB 1.0  → 1.0
//   扩张曲线：gain(y) = 1 + (head-1)·smoothstep(0.55, 1.0, y)，head 缺省 2.0。
//     y=0 → 1.0；y=0.55 → 1.0（拐点下沿）；y=0.775 → 1.5（smoothstep 中点 0.5）；y=1.0 → 2.0。
//     所以纯白 sRGB(1,1,1) → 线性 1.0 → gain 2.0 → **2.0**（>1.0，进 EDR）。
//     中灰 sRGB(0.5,0.5,0.5) → 线性 0.21396 → y 在拐点下 → gain 1.0 → **0.21396**（不动）。
// 先红后绿：本文件在实现之前编译失败（找不到 nvn_hdr.h），红日志见 PR。
#include "nvn_hdr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_bad, g_n;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_bad++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

static void env_case(const char *env, int want) {
    int got = s2a_hdr_enabled(env);
    CHECK(got == want, "开关 env=%s 期望 %d 实得 %d", env ? env : "(null)", want, got);
}

static void lin_case(float srgb, double want) {
    float got = s2a_hdr_to_linear(srgb);
    CHECK(NEAR(got, want, 1e-4), "线性化 sRGB %.4f 期望 %.5f 实得 %.5f", srgb, want, got);
}

int main(void) {
    // ① 开关：只有明确写 0 / false / no / off 才算关；没设 = 关（旧路径逐字节不变）；其余任何非空值 = 开
    env_case(NULL, 0);
    env_case("0", 0);
    env_case("false", 0);
    env_case("no", 0);
    env_case("off", 0);
    env_case("OFF", 0);
    env_case("1", 1);
    env_case("true", 1);
    env_case("yes", 1);
    env_case("", 1);          // 设了空串：意图是开（启动环境显式给了），不当关
    env_case("2", 1);         // 旧片段 strcmp(e,"0") 之外的写法也要一致：非 0 数字算开

    // ② sRGB → 线性：分段点两侧与几个已知值
    lin_case(0.0f, 0.0);
    lin_case(0.04045f, 0.0031308);
    lin_case(0.02f, 0.02 / 12.92);   // 分段点以下：0.00154799
    lin_case(0.5f, 0.21396);
    lin_case(0.8f, 0.60383);
    lin_case(1.0f, 1.0);

    // ③ 扩张曲线：拐点下沿不动、中点上 1.5、白点 2.0、单调不减
    s2a_hdr_itm p = s2a_hdr_itm_params();
    CHECK(NEAR(p.head, 2.0f, 1e-6), "缺省 head 期望 2.0 实得 %.4f", p.head);
    CHECK(NEAR(p.knee_lo, 0.55f, 1e-6) && NEAR(p.knee_hi, 1.0f, 1e-6), "缺省拐点期望 0.55/1.0 实得 %.3f/%.3f", p.knee_lo, p.knee_hi);
    CHECK(NEAR(s2a_hdr_itm_gain(&p, 0.0f), 1.0f, 1e-6), "gain(0) 期望 1 实得 %.6f", s2a_hdr_itm_gain(&p, 0.0f));
    CHECK(NEAR(s2a_hdr_itm_gain(&p, 0.55f), 1.0f, 1e-6), "gain(拐点下沿) 期望 1 实得 %.6f", s2a_hdr_itm_gain(&p, 0.55f));
    CHECK(NEAR(s2a_hdr_itm_gain(&p, 0.775f), 1.5f, 1e-5), "gain(中点) 期望 1.5 实得 %.6f", s2a_hdr_itm_gain(&p, 0.775f));
    CHECK(NEAR(s2a_hdr_itm_gain(&p, 1.0f), 2.0f, 1e-6), "gain(白点) 期望 2 实得 %.6f", s2a_hdr_itm_gain(&p, 1.0f));
    CHECK(NEAR(s2a_hdr_itm_gain(&p, 1.7f), 2.0f, 1e-6), "gain(超白) 期望锁在 2（不外推）实得 %.6f", s2a_hdr_itm_gain(&p, 1.7f));
    float prev = 1.0f;
    for (int i = 0; i <= 100; i++) {
        float g = s2a_hdr_itm_gain(&p, i / 100.0f);
        CHECK(g >= prev - 1e-6f, "gain 单调不减：y=%.2f 处 %.6f < 上一档 %.6f", i / 100.0f, g, prev);
        prev = g;
    }

    // ④ 三通道：纯白进 EDR（>1.0）、中灰不动、高光保色相（三通道同乘一个 gain）
    float white[3] = {1.0f, 1.0f, 1.0f}, out[3];
    s2a_hdr_itm_rgb(&p, white, out);
    CHECK(NEAR(out[0], 2.0f, 1e-5) && NEAR(out[1], 2.0f, 1e-5) && NEAR(out[2], 2.0f, 1e-5),
          "纯白 期望 2,2,2 实得 %.4f,%.4f,%.4f", out[0], out[1], out[2]);
    CHECK(out[0] > 1.0f, "纯白必须 >1.0 才算进了 EDR，实得 %.4f", out[0]);

    float gray[3] = {0.5f, 0.5f, 0.5f};
    s2a_hdr_itm_rgb(&p, gray, out);
    CHECK(NEAR(out[0], 0.21396, 1e-4) && NEAR(out[1], 0.21396, 1e-4) && NEAR(out[2], 0.21396, 1e-4),
          "中灰 期望 0.21396 三通道都不动，实得 %.5f,%.5f,%.5f", out[0], out[1], out[2]);

    // 高光偏色（sRGB 1.0,0.9,0.8）：三通道乘同一个 gain，比值保持
    float warm[3] = {1.0f, 0.9f, 0.8f};
    s2a_hdr_itm_rgb(&p, warm, out);
    double l0 = s2a_hdr_to_linear(1.0f), l1 = s2a_hdr_to_linear(0.9f), l2 = s2a_hdr_to_linear(0.8f);
    double yv = 0.2126 * l0 + 0.7152 * l1 + 0.0722 * l2;
    double k = 1.0 + (2.0 - 1.0) * ((yv <= 0.55) ? 0.0 : (yv >= 1.0 ? 1.0 : (yv - 0.55) / 0.45 * (yv - 0.55) / 0.45 * (3 - 2 * (yv - 0.55) / 0.45)));
    CHECK(NEAR(out[0], l0 * k, 1e-4) && NEAR(out[1], l1 * k, 1e-4) && NEAR(out[2], l2 * k, 1e-4),
          "暖白 期望三通道各乘 %.5f，实得 %.5f,%.5f,%.5f", k, out[0], out[1], out[2]);
    CHECK(NEAR(out[0] / out[1], l0 / l1, 1e-4), "暖白保色相：R/G 期望 %.4f 实得 %.4f", l0 / l1, out[0] / out[1]);

    // ⑤ 三通道用的是「同一个 gain」，而且这个 gain 等于同亮度单独算出来的那个：
    //   不是逐通道各自扩张（那样 R=1.0 会被推到 2.0、G=0.4 不动，色相全乱）
    float a[3] = {1.0f, 0.4f, 0.7f}, oa[3];
    s2a_hdr_itm_rgb(&p, a, oa);
    double ya = 0.2126 * s2a_hdr_to_linear(1.0f) + 0.7152 * s2a_hdr_to_linear(0.4f) + 0.0722 * s2a_hdr_to_linear(0.7f);
    double ka = s2a_hdr_itm_gain(&p, (float)ya);
    CHECK(NEAR(oa[0], s2a_hdr_to_linear(1.0f) * ka, 1e-5) && NEAR(oa[1], s2a_hdr_to_linear(0.4f) * ka, 1e-5) &&
          NEAR(oa[2], s2a_hdr_to_linear(0.7f) * ka, 1e-5),
          "三通道应同乘 gain %.5f，实得 %.5f,%.5f,%.5f（逐通道各自扩张会变成 2,0.13,0.45）", ka, oa[0], oa[1], oa[2]);
    CHECK(NEAR(ka, 1.0, 1e-4), "本例亮度 %.4f 在拐点下，gain 应恰好 1，实得 %.5f", ya, ka);

    // ⑥ 自定义 head：1.0 等于关（gain 恒 1），4.0 时白点 4.0
    s2a_hdr_itm q = p; q.head = 1.0f;
    CHECK(NEAR(s2a_hdr_itm_gain(&q, 1.0f), 1.0f, 1e-6), "head=1 白点应 1，实得 %.6f", s2a_hdr_itm_gain(&q, 1.0f));
    q.head = 4.0f;
    CHECK(NEAR(s2a_hdr_itm_gain(&q, 1.0f), 4.0f, 1e-6), "head=4 白点应 4，实得 %.6f", s2a_hdr_itm_gain(&q, 1.0f));

    if (g_bad) { printf("✗ t_hdr：%d/%d 项不符\n", g_bad, g_n); return 1; }
    printf("✓ t_hdr：HDR 出屏线性化 + 扩张曲线 %d 项 → 全过\n", g_n);
    return 0;
}
