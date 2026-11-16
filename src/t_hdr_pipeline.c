// t_hdr_pipeline：内容侧整条链的端到端校验（合成画面 → 直方图 → L1 → ITM → sRGB 编码）。
//   不依赖 Metal、不需要真机：把一个浮点画面当成一帧，按 GPU 的同一套约定跑一遍，
//   再对输出的**可观察性质**断言——中性不偏色、色相不变、峰值不越 head、中间调不动、
//   暗场景扩张得比亮场景多、黑仍是黑。
//   这里固定的约定（改 MSL 必须同步）：直方图 256 格、log2 范围 [-10,4]、每 8 帧一次、
//   gain 按 Rec.709 亮度算、三通道乘同一个倍数。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"
#include "nvn_hdr_auto.h"
#include "nvn_hdr_auto_l1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  ✗ %s\n", msg); fails++; } } while (0)

#define W 64
#define H 36

// 一帧：sRGB 编码值，RGB 三通道
typedef struct { float px[W * H][3]; } frame;

static void fill(frame *f, float r, float g, float b) {
    for (int i = 0; i < W * H; i++) { f->px[i][0] = r; f->px[i][1] = g; f->px[i][2] = b; }
}

// 与 MSL 侧 hdr_hist 相同的口径：线性亮度 → bin，逐像素累加
static void hist_of(const frame *f, uint32_t hist[S2A_HDR_HIST_BINS]) {
    memset(hist, 0, sizeof(uint32_t) * S2A_HDR_HIST_BINS);
    for (int i = 0; i < W * H; i++) {
        float r = s2a_hdr_to_linear(f->px[i][0]);
        float g = s2a_hdr_to_linear(f->px[i][1]);
        float b = s2a_hdr_to_linear(f->px[i][2]);
        float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        hist[s2a_hdr_hist_bin(y)]++;
    }
}

// 把 L1 的 knee 变成 ITM 参数（head 由环境决定，测试里固定 2.0）
static s2a_hdr_itm itm_of(const s2a_hdr_auto_l1 *r, float head) {
    s2a_hdr_itm p; p.head = head; p.knee_lo = r->knee_lo; p.knee_hi = r->knee_hi;
    return p;
}

// 16F 画布（MTLPixelFormatRGBA16Float）不是 sRGB 格式：采样不解码、写入不编码，
//   所以上屏着色器的输出就是**线性**值，可以 >1.0。out 里的数就是线性光。
static void apply(const s2a_hdr_itm *p, const frame *in, frame *out) {
    for (int i = 0; i < W * H; i++) s2a_hdr_itm_rgb(p, in->px[i], out->px[i]);
}

int main(void) {
    uint32_t h[S2A_HDR_HIST_BINS];
    s2a_hdr_auto_l1 r;
    const float head = 2.0f;

    // 1 sRGB 编解码互逆（含分段边界两侧）
    {
        const float xs[] = {0.0f, 0.0001f, 0.0031308f, 0.02f, 0.04045f, 0.18f, 0.5f, 0.9f, 1.0f};
        int bad = 0;
        for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
            float back = s2a_hdr_to_srgb(s2a_hdr_to_linear(xs[i]));
            float d = fabsf(back - xs[i]);
            if (d > 2e-6f) { printf("  roundtrip x=%.6f back=%.6f d=%.2e\n", xs[i], back, d); bad++; }
        }
        CHECK(bad == 0, "sRGB 编解码互逆（|Δ| ≤ 2e-6）");
    }
    CHECK(s2a_hdr_to_srgb(0.0f) == 0.0f, "线性 0 → 编码 0");
    CHECK(s2a_hdr_to_srgb(-1.0f) == 0.0f, "负值钳到 0");

    // 2 中间调不动：y < knee_lo 时 gain 恰为 1
    {
        s2a_hdr_itm p; p.head = head; p.knee_lo = 0.55f; p.knee_hi = 1.0f;
        CHECK(s2a_hdr_itm_gain(&p, 0.10f) == 1.0f, "y=0.10 < knee_lo → gain 恰 1");
        CHECK(s2a_hdr_itm_gain(&p, 0.55f) == 1.0f, "y=knee_lo → gain 恰 1");
        CHECK(s2a_hdr_itm_gain(&p, 1.00f) == head, "y=knee_hi → gain = head");
        CHECK(s2a_hdr_itm_gain(&p, 4.00f) == head, "y>knee_hi 不外推，仍 = head");
    }

    // 3 gain 对 y 单调不减（中间凸起会让画面出现条带）
    {
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.30f; p.knee_hi = 1.10f;
        float prev = 0.0f; int mono = 1;
        for (int i = 0; i <= 200; i++) {
            float y = (float)i / 200.0f * 1.5f;
            float g = s2a_hdr_itm_gain(&p, y);
            if (g < prev - 1e-7f) mono = 0;
            prev = g;
        }
        CHECK(mono, "gain 对 y 单调不减");
    }

    // 4 中性不偏色：R=G=B 输入，输出三通道逐位相等
    {
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.30f; p.knee_hi = 0.90f;
        float out[3]; int ok = 1;
        for (int i = 0; i <= 100; i++) {
            float v = (float)i / 100.0f;
            float in[3] = {v, v, v};
            s2a_hdr_itm_rgb(&p, in, out);
            if (out[0] != out[1] || out[1] != out[2]) ok = 0;
        }
        CHECK(ok, "灰输入 → 输出三通道逐位相等（不偏色）");
    }

    // 5 色相保持：三通道同乘一个 gain，通道比值不变
    {
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.20f; p.knee_hi = 0.60f;
        const float cols[][3] = {{0.9f, 0.35f, 0.1f}, {0.2f, 0.8f, 0.3f}, {0.5f, 0.5f, 0.95f}};
        int ok = 1;
        for (int c = 0; c < 3; c++) {
            float in[3] = {cols[c][0], cols[c][1], cols[c][2]}, out[3];
            s2a_hdr_itm_rgb(&p, in, out);
            float li0 = s2a_hdr_to_linear(in[0]), li1 = s2a_hdr_to_linear(in[1]);
            float lo0 = out[0], lo1 = out[1];
            if (li1 > 1e-4f && fabsf(lo0 / lo1 - li0 / li1) > 2e-5f * (li0 / li1 + 1.0f)) ok = 0;
        }
        CHECK(ok, "彩色输入的通道比值不变（保色相）");
    }

    // 6 峰值不越 head：SDR 输入最亮 1.0，乘上去最多 head
    {
        s2a_hdr_itm p; p.head = 2.0f; p.knee_lo = 0.10f; p.knee_hi = 0.20f;
        frame in, out;
        fill(&in, 1.0f, 1.0f, 1.0f);
        apply(&p, &in, &out);
        float lin = out.px[0][0];
        CHECK(fabsf(lin - head) < 1e-4f, "纯白在低 knee 下恰输出 head 倍（不外溢）");
        CHECK(s2a_hdr_to_srgb(1.0f) <= 1.0f + 1e-6f, "SDR 参考白编码仍 ≤ 1（8 位回退路径不爆）");
        CHECK(s2a_hdr_to_srgb(lin) > 1.0f, "head 倍线性值编码后 >1（说明 16F 必须走线性写入）");
    }

    // 7 暗场景扩张得比亮场景多（L1 的意义所在）
    {
        frame dark, bright;
        fill(&dark, 0.10f, 0.10f, 0.10f);
        fill(&bright, 0.85f, 0.85f, 0.85f);
        s2a_hdr_auto_l1 rd, rb;
        hist_of(&dark, h);  CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &rd) == 1, "暗场景可算");
        hist_of(&bright, h); CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &rb) == 1, "亮场景可算");
        CHECK(rd.knee_hi < rb.knee_hi, "暗场景 knee_hi 更低（拐点左移）");
        s2a_hdr_itm pd = itm_of(&rd, head), pb = itm_of(&rb, head);
        float y = 0.18f;   // 两个场景都有的中间调
        CHECK(s2a_hdr_itm_gain(&pd, y) > s2a_hdr_itm_gain(&pb, y), "同一中间调在暗场景里获得更多扩张");
        CHECK(rd.knee_hi >= S2A_HDR_KNEE_L1_MIN && rb.knee_hi <= S2A_HDR_KNEE_L1_MAX, "knee 都在允许区间");
    }

    // 8 整帧链路：暗场景里 18% 灰被抬、纯黑仍是 0
    {
        frame in, out;
        fill(&in, 0.18f, 0.18f, 0.18f);
        in.px[0][0] = in.px[0][1] = in.px[0][2] = 0.0f;      // 一个纯黑像素
        in.px[1][0] = in.px[1][1] = in.px[1][2] = 1.0f;      // 一个纯白像素（UI）
        hist_of(&in, h);
        CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &r) == 1, "整帧可算");
        s2a_hdr_itm p = itm_of(&r, head);
        apply(&p, &in, &out);
        float y18 = s2a_hdr_to_linear(0.18f);
        CHECK(out.px[2][0] > y18 * 1.05f, "18% 灰被抬亮（线性值高于输入）");
        CHECK(out.px[0][0] == 0.0f && out.px[0][1] == 0.0f, "纯黑仍是纯黑");
        CHECK(out.px[1][0] >= 1.0f - 1e-6f, "纯白不下降");
        CHECK(out.px[1][0] <= head + 1e-4f, "纯白输出不超过 head");
    }

    // 9 连续帧稳定性：同一场景每 8 帧喂一次，knee 不应抖动
    {
        frame scene;
        fill(&scene, 0.25f, 0.25f, 0.25f);
        hist_of(&scene, h);
        float prev_hi = 1.0f, prev_avg = 0.0f; int have = 0;
        float first = 0.0f, last = 0.0f; int flips = 0; float prev = 1.0f;
        for (int f = 0; f < 40; f++) {
            s2a_hdr_auto_l1 s;
            CHECK(s2a_hdr_auto_l1_step(h, prev_hi, prev_avg, have, &s) == 1, "稳态帧可算");
            if (f == 0) first = s.knee_hi;
            last = s.knee_hi;
            if (f > 0 && (s.knee_hi > prev) != (f > 1 && last > prev)) flips++;
            prev = s.knee_hi; prev_hi = s.knee_hi; prev_avg = s.avg; have = 1;
        }
        CHECK(fabsf(last - first) < 1e-3f, "稳态 40 次迭代后 knee 收敛（不再漂）");
    }

    // 10 白点场景：大片 UI 白（≥0.5%）把白点定在 1.0，knee 到 1.25
    {
        frame ui;
        fill(&ui, 0.18f, 0.18f, 0.18f);
        for (int i = 0; i < W * H / 10; i++) ui.px[i][0] = ui.px[i][1] = ui.px[i][2] = 1.0f;  // 10% 白
        hist_of(&ui, h);
        CHECK(s2a_hdr_auto_l1_step(h, 1.0f, 0.0f, 0, &r) == 1, "UI 白场景可算");
        CHECK(r.white > 0.9f, "白点 ≈ 1.0（大面积白被认成漫反射白）");
        CHECK(r.knee_hi > 1.0f, "knee_hi 被推到 1.0 以上（纯白不再吃满 head）");
        s2a_hdr_itm p = itm_of(&r, head);
        CHECK(s2a_hdr_itm_gain(&p, 1.0f) - 1.0f < 0.6f * (head - 1.0f), "纯白只吃到不到 60% 的扩张（V 形坑被削掉）");
    }

    printf("%s t_hdr_pipeline：内容侧端到端（合成帧 / 色相 / 峰值 / 稳定性）%d 项 → %s（失败 %d）\n",
           fails ? "✗" : "✓", checks, fails ? "有失败" : "全过", fails);
    return fails != 0;
}
