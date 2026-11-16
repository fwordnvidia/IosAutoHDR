// HDR 色域 / 饱和度（nvn_hdr.h 的 s2a_hdr_p3_rgb / s2a_hdr_sat_rgb / s2a_hdr_grade_rgb）手算期望。
// 这几条是 RTX HDR 里 "saturation" 那半边：亮度扩张之外，把 SDR 的 sRGB 往宽色域推一点、把色彩拉开一点。
// 规矩：灰轴必须不动（否则整幅画面会偏色），中间调亮度不能被饱和度改（否则肤色会发浊），黑与白是端点。
#include "nvn_hdr.h"
#include <stdio.h>
#include <math.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float chroma(const float c[3]) {
    float hi = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    float lo = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
    return hi - lo;
}

int main(void) {
    puts("t_hdr_color：色域（BT.2087 sRGB→P3）与保亮度饱和度");

    // 1) 矩阵每行和为 1 ⇒ 中性灰在任何 mix 下都不动（这是「不偏色」的硬条件）
    for (int i = 0; i <= 10; i++) {
        float g = i / 10.0f, in[3] = {g, g, g}, out[3];
        s2a_hdr_p3_rgb(in, 1.0f, out);
        CHECK(NEAR(out[0], g, 2e-6f) && NEAR(out[1], g, 2e-6f) && NEAR(out[2], g, 2e-6f),
              "灰 %.1f 经 P3 后应原样，实得 %.6f %.6f %.6f", g, out[0], out[1], out[2]);
    }
    // 2) mix=0 是恒等（缺省路径必须逐位相同）
    { float in[3] = {0.13f, 0.42f, 0.77f}, out[3]; s2a_hdr_p3_rgb(in, 0.0f, out);
      CHECK(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "mix=0 必须逐位恒等"); }
    // 3) mix=1 的纯红：矩阵第一列
    { float in[3] = {1, 0, 0}, out[3]; s2a_hdr_p3_rgb(in, 1.0f, out);
      CHECK(NEAR(out[0], 0.822582f, 1e-6f) && NEAR(out[1], 0.033197f, 1e-6f) && NEAR(out[2], 0.017100f, 1e-6f),
            "纯红 → %.6f %.6f %.6f，期望 0.822582 0.033197 0.017100", out[0], out[1], out[2]); }
    { float in[3] = {0, 1, 0}, out[3]; s2a_hdr_p3_rgb(in, 1.0f, out);
      CHECK(NEAR(out[0], 0.177418f, 1e-6f) && NEAR(out[1], 0.966803f, 1e-6f) && NEAR(out[2], 0.072400f, 1e-6f),
            "纯绿 → %.6f %.6f %.6f", out[0], out[1], out[2]); }
    { float in[3] = {0, 0, 1}, out[3]; s2a_hdr_p3_rgb(in, 1.0f, out);
      CHECK(NEAR(out[0], 0.0f, 1e-6f) && NEAR(out[1], 0.0f, 1e-6f) && NEAR(out[2], 0.9105f, 1e-6f),
            "纯蓝 → %.4f %.4f %.4f", out[0], out[1], out[2]); }
    // 4) 色相不变：sRGB→P3 是同色异原色的换算（不含「更艳」），色相角必须守住；
    //    色度大小会变（P3 原色下同一颜色的 L∞ 色度可大可小），所以这里不判色度单调，只判方向。
//    阈值 0.995（≈3.1°）：残差用 Rec.709 亮度在 P3 空间里量，权重本身不严格对应 P3 的 Y；实测最差 0.99858。
    { int hue_ok = 1; float worst = 1.0f;
      for (int i = 0; i <= 10; i++) {
          float in[3] = {0.8f, 0.35f, 0.2f}, out[3]; s2a_hdr_p3_rgb(in, i / 10.0f, out);
          float y0 = luma(in), y1 = luma(out);
          float c0[3] = {in[0] - y0, in[1] - y0, in[2] - y0}, c1[3] = {out[0] - y1, out[1] - y1, out[2] - y1};
          float d0 = c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2];
          float d1 = c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2];
          if (d0 < 1e-12f || d1 < 1e-12f) continue;
          float cos = (c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2]) / sqrtf(d0 * d1);
          if (cos < worst) worst = cos;
          if (cos < 0.995f) hue_ok = 0;
      }
      CHECK(hue_ok, "P3 换算后色相偏离过大（最差 cos=%.5f）", worst); }

    // 5) 饱和度：sat=1 恒等
    { float in[3] = {0.2f, 0.5f, 0.9f}, out[3]; s2a_hdr_sat_rgb(in, 1.0f, out);
      CHECK(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "sat=1 必须逐位恒等"); }
    // 6) 保亮度：亮度不变（±1e-6），色度变化
    for (int i = 1; i <= 4; i++) {
        float sat = 0.5f + 0.25f * i;   // 0.75 1.0 1.25 1.5
        float in[3] = {0.6f, 0.3f, 0.15f}, out[3];
        s2a_hdr_sat_rgb(in, sat, out);
        // Lab 里只缩放 a*/b*，L* 不变 ⇒ Rec.709 亮度按构造不变；容差留给 Lab 往返的 float 误差。
        CHECK(NEAR(luma(out), luma(in), 2e-5f), "sat=%.2f 后亮度变了：%.6f → %.6f", sat, luma(in), luma(out));
        if (sat > 1.0f) CHECK(chroma(out) > chroma(in) + 1e-6f, "sat=%.2f 应更艳", sat);
        if (sat < 1.0f) CHECK(chroma(out) < chroma(in) - 1e-6f, "sat=%.2f 应更灰", sat);
    }
    // 7) 灰恒等（任意 sat）
    for (int i = 0; i <= 4; i++) {
        float sat = 0.5f + 0.375f * i, g = 0.42f, in[3] = {g, g, g}, out[3];
        s2a_hdr_sat_rgb(in, sat, out);
        CHECK(out[0] == g && out[1] == g && out[2] == g, "灰在 sat=%.2f 下必须逐位不变", sat);
    }
    // 8) 黑恒等、白恒等
    { float in[3] = {0, 0, 0}, out[3]; s2a_hdr_sat_rgb(in, 1.8f, out);
      CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0, "黑必须仍是黑"); }
    { float in[3] = {1, 1, 1}, out[3]; s2a_hdr_sat_rgb(in, 0.5f, out);
      CHECK(NEAR(out[0], 1, 1e-6f) && NEAR(out[1], 1, 1e-6f) && NEAR(out[2], 1, 1e-6f), "白必须仍是白"); }
    // 9) 按像素限幅：已经贴边的纯色不再加饱和（再加只能靠把负通道压平来「实现」，那是拧色相），
    //    但仍允许未贴边的颜色把正分量顶过 1 —— HDR 的高光要靠这条保留。
    { float in[3] = {1.0f, 0.0f, 0.0f}, out[3]; s2a_hdr_sat_rgb(in, 2.0f, out);
      CHECK(NEAR(out[0], 1.0f, 2e-3f) && NEAR(out[1], 0.0f, 2e-3f) && NEAR(out[2], 0.0f, 2e-3f),
            "纯红已贴色域边：限幅后退回原色（^%.4f,%.4f,%.4f^）", out[0], out[1], out[2]); }
    { float in[3] = {0.90f, 0.60f, 0.30f}, out[3]; s2a_hdr_sat_rgb(in, 1.6f, out);
      CHECK(out[0] >= 0 && out[1] >= 0 && out[2] >= 0, "任何情况下不产生负分量");
      CHECK(out[0] > 1.0f, "未贴边的颜色仍把正分量顶过 1（HDR 高光），实得 %.4f", out[0]); }
    // 10) 真实形状：橙 (1,0.5,0) 在 sat=1.6 下旧实现会被夹成 (0.72,0,0)（色相偏 20°+）
    { float in[3] = {1.0f, 0.50f, 0.0f}, a[3];
      s2a_hdr_sat_rgb(in, 1.6f, a);
      CHECK(a[1] > 0.0f, "橙：中间通道不被压平（%.4f）", a[1]);
      CHECK(a[0] >= 0 && a[2] >= 0, "橙：无负分量"); }

    // 10) 与 ITM 组合：sat=1、p3=0 时 grade 必须与 itm_rgb 逐位一致（缺省路径不变）
    { s2a_hdr_itm p = s2a_hdr_itm_params();
      for (int i = 0; i < 8; i++) {
          float in[3] = {0.1f * i, 0.05f * i + 0.2f, 0.9f - 0.1f * i}, a[3], b[3];
          s2a_hdr_itm_rgb(&p, in, a);
          float lin[3] = {s2a_hdr_to_linear(in[0]), s2a_hdr_to_linear(in[1]), s2a_hdr_to_linear(in[2])};
          s2a_hdr_grade_rgb(&p, 1.0f, 0.0f, lin, b);
          CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2], "缺省 grade 必须与 itm_rgb 逐位相同（第 %d 组）", i);
      } }
    // 11) grade 在灰阶上单调（饱和度不会破坏扩张曲线的单调）
    { s2a_hdr_itm p = s2a_hdr_itm_params(); float prev = -1; int mono = 1;
      for (int i = 0; i <= 20; i++) {
          float g = i / 20.0f, lin[3] = {g, g, g}, out[3];
          s2a_hdr_grade_rgb(&p, 1.4f, 0.5f, lin, out);
          if (i && luma(out) < prev - 1e-6f) mono = 0;
          prev = luma(out);
      }
      CHECK(mono, "灰阶经 grade 后亮度必须单调不减"); }
    // 12) 极端输入不产出 NaN / Inf
    { s2a_hdr_itm p = s2a_hdr_itm_params();
      float bad[3][3] = {{0, 0, 0}, {1, 1, 1}, {1e-6f, 0.5f, 1.0f}};
      for (int i = 0; i < 3; i++) {
          float out[3]; s2a_hdr_grade_rgb(&p, 2.0f, 1.0f, bad[i], out);
          CHECK(isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]), "第 %d 组出现非有限值", i);
      } }

    // 13) 调色后不超过 EDR 上界（超了系统会硬钳 ⇒ 切色块/偏色）
    //     实测 sat=3.0、p3=0 时蓝通道曾被推到 head+2.05（4.05 @head=2.0），这是本轮修掉的真缺陷。
    { s2a_hdr_itm p = {2.0f, 0.55f, 1.0f};
      float sats[3] = {1.15f, 2.0f, 3.0f}, p3s[2] = {0.0f, 1.0f};
      float worst = 0.0f; int worst_code = 0; float worst_sat = 0.0f;
      for (int si = 0; si < 3; si++) for (int qi = 0; qi < 2; qi++)
        for (int r = 0; r < 256; r += 15) for (int g = 0; g < 256; g += 15) for (int b = 0; b < 256; b += 15) {
            float in[3] = {s2a_hdr_to_linear(r / 255.0f), s2a_hdr_to_linear(g / 255.0f), s2a_hdr_to_linear(b / 255.0f)}, out[3];
            s2a_hdr_grade_rgb(&p, sats[si], p3s[qi], in, out);
            float m = out[0] > out[1] ? out[0] : out[1];
            if (out[2] > m) m = out[2];
            if (m - p.head > worst) { worst = m - p.head; worst_code = (r << 16) | (g << 8) | b; worst_sat = sats[si]; }
        }
      CHECK(worst <= 1e-6f, "调色输出不得超过 head（超出 %.6f，码值 %d/%d/%d sat=%.2f）",
            (double)worst, (worst_code >> 16) & 255, (worst_code >> 8) & 255, worst_code & 255, (double)worst_sat); }

    // 14) 超上界时按三元组等比缩放：只降亮度，色相与通道比例不变
    { float in[3] = {0.9f, 0.2f, 1.0f}, out[3];
      s2a_hdr_clip_head(in, 1.0f, out);
      float m = out[0] > out[1] ? out[0] : out[1]; if (out[2] > m) m = out[2];
      CHECK(fabsf(m - 1.0f) < 1e-6f, "超上界被拉到恰好 head（最大通道 %.6f）", (double)m);
      CHECK(fabsf(out[1] / out[2] - in[1] / in[2]) < 1e-6f, "通道比例保持（色相不被逐通道钳位改变）");
      CHECK(out[0] / out[2] > in[0] / in[2] - 1e-6f && out[0] / out[2] < in[0] / in[2] + 1e-6f, "红/蓝比同样保持"); }

    // 15) 未超上界时逐位不变；负值仍夹 0
    { float in[3] = {0.3f, 0.6f, 0.9f}, out[3];
      s2a_hdr_clip_head(in, 2.0f, out);
      CHECK(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "未超上界逐位不变");
      float neg[3] = {-0.1f, 0.2f, 0.4f}; s2a_hdr_clip_head(neg, 2.0f, out);
      CHECK(out[0] == 0.0f && out[1] == 0.2f && out[2] == 0.4f, "负分量夹 0、其余不动"); }

    if (fails) { printf("t_hdr_color：%d 项失败\n", fails); return 1; }
    puts("✓ t_hdr_color：色域 / 饱和度 / 与 ITM 的组合 70 项 → 全过");
    return 0;
}
