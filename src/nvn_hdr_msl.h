// 上屏 MSL（`src/nvn_mtl.m` 内联的 `src_msl_hdr`）的逐行 C 转写。
//
// 为什么单独成头文件：单测 `t_hdr_shader_mirror.c` 要在色彩空间网格上对账，
// 图像级工具 `tools/hdr_shader_image.c` 要在**真实照片的每个像素**上对账。
// 两边必须用同一份转写，否则「网格对上了、照片没对上」这类分歧会被掩盖。
//
// 与 MSL 原文的对应关系写在各函数上；改动 MSL 时必须同时改这里，`check_msl.py` 只守语法与常数，
// 语义一致性靠 `t_hdr_shader_mirror`（网格）与 `hdr_shader_image`（整幅）两道闸。
#ifndef S2A_NVN_HDR_MSL_H
#define S2A_NVN_HDR_MSL_H
#include <math.h>

// ——— 逐行翻译自 src/nvn_mtl.m 的 src_msl_hdr（fs 主体）———
static inline float msl_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static inline float msl_lab_f(float t) { return t > 0.008856f ? powf(t, 1.0f / 3.0f) : (7.787f * t + 16.0f / 116.0f); }
static inline float msl_lab_finv(float t) { float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.0f / 116.0f) / 7.787f; }

static void msl_lin_lab(const float rgb[3], float lab[3]) {
    float x = 0.4124564f * rgb[0] + 0.3575761f * rgb[1] + 0.1804375f * rgb[2];
    float y = 0.2126729f * rgb[0] + 0.7151522f * rgb[1] + 0.0721750f * rgb[2];
    float z = 0.0193339f * rgb[0] + 0.1191920f * rgb[1] + 0.9503041f * rgb[2];
    lab[0] = 116.0f * msl_lab_f(y) - 16.0f;
    lab[1] = 500.0f * (msl_lab_f(x / 0.95047f) - msl_lab_f(y));
    lab[2] = 200.0f * (msl_lab_f(y) - msl_lab_f(z / 1.08883f));
}
static void msl_lab_lin(const float lab[3], float rgb[3]) {
    float fy = (lab[0] + 16.0f) / 116.0f, fx = fy + lab[1] / 500.0f, fz = fy - lab[2] / 200.0f;
    float x = 0.95047f * msl_lab_finv(fx), y = msl_lab_finv(fy), z = 1.08883f * msl_lab_finv(fz);
    rgb[0] = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    rgb[1] = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    rgb[2] = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;
}
// MSL `s2a_clip_head` 的逐行转写：超 head 等比缩放、负值夹 0。
static void msl_clip_head(const float in[3], float head, float out[3]) {
    if (!(head > 0.0f)) head = 1.0f;
    float m = in[0] > in[1] ? in[0] : in[1];
    if (in[2] > m) m = in[2];
    float k = (m > head) ? head / m : 1.0f;
    for (int i = 0; i < 3; i++) { float v = in[i] * k; out[i] = v > 0.0f ? v : 0.0f; }
}

static float msl_sat_min(const float lab[3], float s) {
    float l2[3] = {lab[0], lab[1] * s, lab[2] * s}, rgb[3];
    msl_lab_lin(l2, rgb);
    return fminf(rgb[0], fminf(rgb[1], rgb[2]));
}
static void msl_sat(const float in[3], float sat, float out[3]) {
    if (sat == 1.0f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    float lab[3];
    msl_lin_lab(in, lab);
    if (fabsf(lab[1]) < 1e-3f && fabsf(lab[2]) < 1e-3f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    float s = sat;
    if (msl_sat_min(lab, s) < 0.0f) {
        float lo = 1.0f, hi = s;
        for (int i = 0; i < 14; i++) {
            float mid = 0.5f * (lo + hi);
            if (msl_sat_min(lab, mid) >= 0.0f) lo = mid; else hi = mid;
        }
        s = lo;
    }
    float l2[3] = {lab[0], lab[1] * s, lab[2] * s}, o[3];
    msl_lab_lin(l2, o);
    out[0] = fmaxf(0.0f, o[0]); out[1] = fmaxf(0.0f, o[1]); out[2] = fmaxf(0.0f, o[2]);
}

// 完整 fs：编码值 → 线性 → P3（可选）→ 饱和度（可选）→ Rec.709 亮度 → smoothstep → 乘 gain
static void msl_fs_map(const float code[3], float lo, float hi, float head, float sat, float p3, int map, float out[3]) {
    float a[3];
    for (int i = 0; i < 3; i++) a[i] = msl_lin(code[i]);
    if (p3 > 0.0f) {
        static const float m[3][3] = {
            {0.822582f, 0.177418f, 0.000000f},
            {0.033197f, 0.966803f, 0.000000f},
            {0.017100f, 0.072400f, 0.910500f},
        };
        float t = fminf(fmaxf(p3, 0.0f), 1.0f), pc[3];
        for (int i = 0; i < 3; i++) {
            float v = m[i][0] * a[0] + m[i][1] * a[1] + m[i][2] * a[2];
            pc[i] = a[i] + (v - a[i]) * t;
        }
        for (int i = 0; i < 3; i++) a[i] = pc[i];
    }
    if (sat != 1.0f) {
        float g[3];
        msl_sat(a, sat, g);
        for (int i = 0; i < 3; i++) a[i] = g[i];
    }
    float y = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
    if (map == 1) {                       // BT.2408：与 knee 分支互斥，且先于退化拐点判定
        float kb = (y > 0.0f) ? head * powf(y, 0.15f) : 0.0f;   // S2A_HDR_BT2408_GAMMA - 1
        for (int i = 0; i < 3; i++) a[i] *= kb;
        msl_clip_head(a, head, out);
        return;
    }
    if (!(hi > lo + 1e-5f)) { for (int i = 0; i < 3; i++) a[i] *= head; msl_clip_head(a, head, out); return; }
    float u = fminf(fmaxf((y - lo) / (hi - lo), 0.0f), 1.0f);
    float k = 1.0f + (head - 1.0f) * (u * u * (3.0f - 2.0f * u));
    for (int i = 0; i < 3; i++) a[i] *= k;
    msl_clip_head(a, head, out);
}

// `c.extra.z` 缺省 0 ⇒ msl_fs 就是 knee 方案（现有调用点全走这里，逐位不变）
static void msl_fs(const float code[3], float lo, float hi, float head, float sat, float p3, float out[3]) {
    msl_fs_map(code, lo, hi, head, sat, p3, 0, out);
}

#endif
