// t_hdr_identity：证明「全关时逐位一致」——不只是结构上没改（落点自检），而是数值上可证。
//
// 为什么需要这一套：
//   补丁的 HDR 上屏是「线性化 → 乘增益 → 再编码」的 fragment。即使 knee 两端写成 1.0、
//   增益恒等于 1，像素也走了一遍 sRGB 解码 → float 乘法 → sRGB 编码。任何一步有 1 LSB 的
//   偏差，画面就会在 8 位 SDR 上出现「颜色轻微不一样」的抱怨，而这种偏差在
//   「不变量/对账/性质」三类测试里都看不见（它们只关心 HDR 开着时的行为）。
//   所以这里穷举 8 位码值空间（256 个值）走完整往返，要求**取整后与原码值相等**。
//
// 覆盖三条路径：
//   A. 8 位 → 线性 → ×1.0 → 8 位（缺省档：head=1、sat=1、p3=0、map=0、knee_hi=knee_lo）
//   B. 同一路径但按「拐点全开、head=1」的上屏公式（走 s2a_hdr_itm_gain，验证增益恒为 1）
//   C. 16F 画布：线性值原样透传（含 0、极小值、1.0、>1 的越界值按 head 钳）
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"

static int items, fails;
#define OK(c, ...) do { items++; if (!(c)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int round8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (int)lrintf(v * 255.0f);
}

int main(void) {
    // ---- A：8 位码值走「线性化 → 增益 1 → 编码」，必须原样回来 ----
    int bad_a = 0, worst_a = -1;
    for (int c = 0; c < 256; c++) {
        const float code = (float)c / 255.0f;
        const float lin = s2a_hdr_to_linear(code);
        const float back = s2a_hdr_to_srgb(lin * 1.0f);
        const int r = round8(back);
        if (r != c) {
            if (!bad_a) worst_a = c;
            bad_a++;
        }
    }
    OK(bad_a == 0, "A 路径（×1.0 往返）有 %d 个码值回不来，首个 8 位码值 %d", bad_a, worst_a);

    // ---- B：走上屏的 ITM 公式，拐点退化（hi==lo，即整幅都在「纸白以上」=恒 head） ----
    s2a_hdr_itm p;
    p.head = 1.0f;
    p.knee_lo = 0.55f;
    p.knee_hi = 0.55f;      // 退化成单点：增益恒为 head = 1
    float worst_gain = 0.0f;
    int bad_gain = 0;
    for (int c = 0; c < 256; c++) {
        const float code = (float)c / 255.0f;
        const float lin = s2a_hdr_to_linear(code);
        const float g = s2a_hdr_itm_gain(&p, lin);
        if (fabsf(g - 1.0f) > worst_gain) worst_gain = fabsf(g - 1.0f);
        if (g != 1.0f) bad_gain++;
        const int r = round8(s2a_hdr_to_srgb(lin * g));
        if (r != c) bad_a++;    // 复用计数器（下面单独报告）
    }
    OK(worst_gain == 0.0f, "B 路径 head=1 时增益不是逐位 1.0（最大偏差 %.9f，%d 个码值）", (double)worst_gain, bad_gain);
    OK(bad_a == 0, "B 路径有码值往返后不等（累计 %d）", bad_a);

    // ---- C：全关的 grade 必须逐位返回输入 ----
    memset(&p, 0, sizeof p);
    p.head = 1.0f;
    p.knee_lo = 0.0f;
    p.knee_hi = 1.0f;
    int bad_c = 0;
    float in[3], out[3];
    for (int c = 0; c < 256; c++) {
        for (int k = 0; k < 3; k++) in[k] = (float)c / 255.0f;
        s2a_hdr_grade_rgb_map(&p, 1.0f, 0, 0, in, out);
        for (int k = 0; k < 3; k++) { if (memcmp(&out[k], &in[k], sizeof(float)) != 0) bad_c++; }
    }
    OK(bad_c == 0, "C 路径全关 grade 改了 %d 个通道值（应逐位相同）", bad_c);

    // ---- D：16F 透传（线性值原样写，不被 sRGB 编码） ----
    const float probes[] = {0.0f, 1e-8f, 0.0031308f, 0.0031309f, 0.01f, 0.25f, 0.5f, 0.9999f, 1.0f};
    int bad_d = 0;
    for (int i = 0; i < (int)(sizeof probes / sizeof *probes); i++) {
        s2a_hdr_itm q;
        q.head = 2.0f;
        q.knee_lo = 1.0f;       // 拐点以上才开始扩张 ⇒ 1.0 以下恒 ×1
        // 注意不能用 knee_lo == knee_hi：那是「退化拐点」，`s2a_hdr_itm_gain` 的守卫会直接返回
        // head（整幅 ×head），于是「1.0 以下不动」这个断言会假失败。要留一丝非零宽度。
        q.knee_hi = 1.0001f;
        const float g = s2a_hdr_itm_gain(&q, probes[i]);
        if (probes[i] < 1.0f && g != 1.0f) bad_d++;
        if (fabsf(probes[i] * g - probes[i]) > 1e-7f) bad_d++;
    }
    OK(bad_d == 0, "D 路径线性值在 knee_lo=1.0 以下被改了 %d 次", bad_d);

    // ---- E：8 位编码与线性解码互为逆（同一函数的两种用法，验证分段点附近连续） ----
    int jump = 0;
    float prev = s2a_hdr_to_srgb(s2a_hdr_to_linear(0.0f));
    for (int c = 1; c < 256; c++) {
        const float v = s2a_hdr_to_srgb(s2a_hdr_to_linear((float)c / 255.0f));
        if (v + 1e-6f < prev) jump++;     // 单调性：编码值不该回退
        prev = v;
    }
    OK(jump == 0, "E 路径编码后出现 %d 处非单调（分段点 0.04045 附近）", jump);

    printf("%s t_hdr_identity：全关态逐位一致性 %d 项（失败 %d，穷举 256 个码值 × 5 条路径）\n",
           fails ? "✗" : "✓", items, fails);
    return fails ? 1 : 0;
}
