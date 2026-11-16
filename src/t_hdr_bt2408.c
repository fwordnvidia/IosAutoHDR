// t_hdr_bt2408：BT.2408-8 §5.1 display-referred 放置方式的单测。
// 这一套守的不是「好看」，而是三条必须成立的性质：
//   1) 黑映射到 0（否则整幅被抬灰，暗场全废）；
//   2) 白正好落在 head（203/100 = 2.03），且 head 可配；
//   3) 中性灰恒为灰（按亮度算比值再乘 RGB 的结构性后果，不是靠容差）。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"

static int total, fails;
static void ck(int ok, const char *msg, float got, float want) {
    total++;
    if (!ok) {
        fails++;
        printf("  ✗ %s：got %.6f want %.6f\n", msg, got, want);
    }
}

int main(void) {
    float in[3], out[3];

    puts("bt2408：端点与形状");
    in[0] = in[1] = in[2] = 0.0f;
    s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
    ck(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "黑映射到 0", out[0], 0.0f);

    in[0] = in[1] = in[2] = 1.0f;
    s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
    ck(fabsf(out[0] - 2.03f) < 1e-5f, "白落在 203 cd/m²（×2.03）", out[0], 2.03f);

    s2a_hdr_bt2408_rgb_lin(in, 4.0f, out);
    ck(fabsf(out[0] - 4.0f) < 1e-5f, "head 可配（白跟着到 4.0）", out[0], 4.0f);

    // 单调：输入越大输出越大（幂函数保序）
    {
        int mono = 1;
        float prev = -1.0f;
        for (int i = 0; i <= 100; i++) {
            float v = (float)i / 100.0f;
            in[0] = in[1] = in[2] = v;
            s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
            if (out[0] < prev - 1e-6f) mono = 0;
            prev = out[0];
        }
        ck(mono, "沿输入单调不减", (float)mono, 1.0f);
    }

    puts("bt2408：与 knee 的差异（写进文档的那几个数）");
    in[0] = in[1] = in[2] = 0.18f;
    s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
    ck(fabsf(out[0] / 0.18f - 1.5696f) < 2e-4f, "中间调 0.18 → ×1.5696", out[0] / 0.18f, 1.5696f);

    in[0] = in[1] = in[2] = 0.50f;
    s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
    ck(fabsf(out[0] / 0.50f - 1.8295f) < 2e-4f, "中间调 0.50 → ×1.8295", out[0] / 0.50f, 1.8295f);

    {
        // 缺省 knee（0.55/1.0）在纸白以下完全不动 —— 这正是与 bt2408 的观感差异所在
        s2a_hdr_itm p = s2a_hdr_itm_params();
        float g18 = s2a_hdr_itm_gain(&p, 0.18f);
        float g50 = s2a_hdr_itm_gain(&p, 0.50f);
        float g100 = s2a_hdr_itm_gain(&p, 1.0f);
        ck(fabsf(g18 - 1.0f) < 1e-6f, "knee 在 0.18 处不动", g18, 1.0f);
        ck(fabsf(g50 - 1.0f) < 1e-6f, "knee 在 0.50 处仍不动（缺省拐点 0.55）", g50, 1.0f);
        ck(fabsf(g100 - 2.0f) < 1e-6f, "knee 在纸白处给满 head", g100, 2.0f);
        in[0] = in[1] = in[2] = 0.18f;
        s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
        ck(out[0] > 0.18f * g18 + 1e-3f, "bt2408 比 knee 更抬中间调", out[0], 0.18f * g18);
    }

    puts("bt2408：色相与彩度");
    {
        // 中性灰：三通道相同 → 输出仍逐位相同
        in[0] = in[1] = in[2] = 0.37f;
        s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
        ck(memcmp(&out[0], &out[1], sizeof(float)) == 0 &&
           memcmp(&out[1], &out[2], sizeof(float)) == 0, "灰恒为灰（逐位）", out[0], out[1]);
    }
    float r0 = 0.0f;
    {
        // 饱和红：三通道比值不变（只乘一个标量）
        in[0] = 0.60f; in[1] = 0.12f; in[2] = 0.12f;
        r0 = in[0] / in[1];
        s2a_hdr_bt2408_rgb_lin(in, S2A_HDR_BT2408_HEAD, out);
        float r1 = out[0] / out[1];
        ck(fabsf(r0 - r1) < 1e-4f, "通道比不变（等比缩放）", r1, r0);
    }
    {
        // 逐通道 gamma 会改彩度：证明我们不是那样做的
        float per[3];
        for (int i = 0; i < 3; i++) per[i] = 2.03f * powf(in[i], 1.15f);
        float r_per = per[0] / per[1];
        ck(fabsf(r_per - r0) > 0.05f, "对照：逐通道 gamma 会改变通道比", r_per, r0);
    }

    puts("bt2408：边界");
    {
        float nan_in[3] = {0.0f / 0.0f, 0.5f, 0.5f};
        s2a_hdr_bt2408_rgb_lin(nan_in, S2A_HDR_BT2408_HEAD, out);
        ck(fabsf(out[0]) < 1e-6f, "NaN 亮度退回 0（不产生 NaN 输出）", out[0], 0.0f);
        ck(out[1] == 0.0f && out[2] == 0.0f, "同上，三通道一起清零", out[1], 0.0f);
    }
    {
        float big[3] = {8.0f, 8.0f, 8.0f};
        s2a_hdr_bt2408_rgb_lin(big, S2A_HDR_BT2408_HEAD, out);
        ck(isfinite(out[0]) && out[0] > 8.0f, "超白输入仍单调（不夹，交给 EDR）", out[0], 8.0f);
    }

    printf("%s t_hdr_bt2408：BT.2408 放置方式 %d 项（失败 %d）\n", fails ? "✗" : "✓", total, fails);
    return fails ? 1 : 0;
}
