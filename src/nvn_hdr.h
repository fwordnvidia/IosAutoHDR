// HDR 出屏（S2A_HDR_OUTPUT）：直连层的 drawable 换成 RGBA16Float，层声明 EDR，
//   上屏着色器把 sRGB 编码值线性化后按亮度扩张 headroom（ITM，反色调映射），让画面里本来就有的高光进到 >1.0。
// 只做精度不做扩张不算 HDR：线性 1.0 落在 SDR 参考白上，观感与 SDR 一模一样，只是少了 8 位量化。
//   头文件里那条 gain 曲线才是可见变化的那一半。
//
// 本文件是纯 C，单测 graphics/nvn/tests/t_hdr.c 手算期望；MSL 侧（nvn_mtl.m present_pipeline 的 16F 分支）
//   必须用同一组常数与同一条曲线——改这里就同步改那段字符串，两边任一改动都要让 t_hdr 先红。
#ifndef NVN_HDR_H
#define NVN_HDR_H

#include <math.h>
#include <stdio.h>   // snprintf：诊断行（S2A_HDR_LOG）
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float head;     // 线性 1.0 之上的可用倍数（S2A_HDR_HEADROOM，缺省 2.0，钳 [1,4]）
    float knee_lo;  // 拐点下沿：亮度在这以下完全不动（0.55）
    float knee_hi;  // 拐点上沿：亮度到这里拿满 head（1.0）
} s2a_hdr_itm;

// 运行期的 headroom 覆盖。Apple 的说明是：1.0 对应 SDR 最大亮度，可用 headroom
//   「随时」可能在变（用户拖亮度滑块、换屏），要读当前上界并把内容映射进 [0, 上界]。
//   所以 head 不能只在起局时从环境变量取一次：宿主每帧读一次当前上界，变化超过死区就写这里，
//   上屏 buffer 与 CPU 侧 ITM 从此读同一个值（否则两侧会各按各的 head 扩张）。
//   0 表示「没有覆盖」，回落到 S2A_HDR_HEADROOM / 缺省 2.0。
static float s2a_hdr_head_live;
static inline void s2a_hdr_head_set(float h) {
    if (!(h >= 1.0f)) h = 1.0f;
    if (h > 4.0f) h = 4.0f;
    s2a_hdr_head_live = h;
}
static inline void s2a_hdr_head_clear(void) { s2a_hdr_head_live = 0.0f; }

// 变化死区：相对差小于 S2A_HDR_HEAD_DEADBAND 就不动（读数抖一下不该让画面闪）。
#define S2A_HDR_HEAD_DEADBAND 0.01f
// 返回应当采用的新 head：|cur-prev| / prev <= 死区时保持 prev。
static inline float s2a_hdr_head_track(float prev, float cur) {
    if (!(cur >= 1.0f)) return prev;
    if (cur > 4.0f) cur = 4.0f;
    if (!(prev >= 1.0f)) return cur;
    float d = (cur - prev) / prev;
    if (d < 0.0f) d = -d;
    return d <= S2A_HDR_HEAD_DEADBAND ? prev : cur;
}

// ---- 16F 画布取不到 drawable 时的回退 ------------------------------------------------------
// 为什么需要：`S2A_HDR_FMT=1` 会把 CAMetalLayer 的 pixelFormat 改成 RGBA16Float，
// 由系统决定给不给这个格式的可绘制对象。不支持时 `[layer nextDrawable]` 每一步都返回 nil，
// 而主循环里原本只是 `if (!dr) return 0;` —— 没有日志、没有回退，表现就是**永久黑屏**。
// （真机三格矩阵 T1/T2 要判的正是这个假设。）这层守卫把「静默黑屏」变成「一秒后自动降级到 8 位」。
//
// 判据用「连续 N 帧取不到」而不是「第一帧就退」：首帧拿不到 drawable 是正常的
// （窗口还没上屏、后台切换、画布尺寸正在变），过早退回会让 HDR 在起局阶段被误关。
#define S2A_HDR_NIL_LIMIT 60   // 60 帧 ≈ 1 秒 @60 Hz

// 降级之后**必须**还有回头路：外接 SDR 屏 → 内建 XDR 屏、系统 HDR 开关被重新打开、
// 窗口从分屏拖回全屏……这些都会让 16F 画布重新可用。只降不升等于「一次意外换来永久 HDR 关」。
// 策略：降级后按固定节奏试探 16F，失败就指数退避（600 帧起，上限 8 倍 ≈ 80 秒），
// 一旦 16F 稳定拿到 drawable 就把退避复位。试探本身不改任何持久设置，失败只是回到 8 位。
#define S2A_HDR_REARM_FRAMES 600   // 首次重试间隔：600 帧 ≈ 10 秒 @60 Hz
#define S2A_HDR_REARM_MAX 8        // 退避上限倍数（600*8 ≈ 80 秒 @60 Hz）

typedef struct {
    int nil_run;   // 连续取不到 drawable 的帧数
    int latched;   // 1 = 已经降级过（守卫不再计 nil 帧，改由 rearm 定时试探）
    int wait;      // 距下次试探还有多久（帧）
    int step;      // 本次试探的间隔（帧），失败后翻倍到上限
} s2a_hdr_fmt_guard;

// 每帧调用一次。got_drawable=0 表示 nextDrawable 返回了 nil。
// want16 表示调用方当前**正在用** 16F 画布（否则无需守卫）。
// 返回 1 = 就在这一帧执行降级（恰好返回一次，随后 latched=1）。
static inline int s2a_hdr_fmt_guard_step(s2a_hdr_fmt_guard *g, int got_drawable, int want16) {
    if (!g) return 0;
    if (got_drawable) { g->nil_run = 0; return 0; }
    if (!want16 || g->latched) return 0;
    if (g->nil_run < S2A_HDR_NIL_LIMIT) g->nil_run++;
    if (g->nil_run < S2A_HDR_NIL_LIMIT) return 0;
    g->latched = 1;
    g->wait = 0;
    if (g->step < S2A_HDR_REARM_FRAMES) g->step = S2A_HDR_REARM_FRAMES;   // 保住已有的退避倍数
    return 1;
}

// 每帧调用一次（仅在已降级时才有动作）。want16 = 调用方**希望**用 16F 画布
// （不是「当前是不是 16F」——降级之后当前必然不是）。
// 返回 1 = 就在这一帧把画布切回 16F 再试一次；失败会再次降级，并按退避放大间隔。
static inline int s2a_hdr_fmt_guard_rearm(s2a_hdr_fmt_guard *g, int want16) {
    if (!g || !g->latched || !want16) return 0;
    if (g->step <= 0) g->step = S2A_HDR_REARM_FRAMES;
    if (g->wait < g->step) { g->wait++; return 0; }
    g->wait = 0;
    // 失败退避：下一次等更久，上限 S2A_HDR_REARM_FRAMES * S2A_HDR_REARM_MAX
    if (g->step < S2A_HDR_REARM_FRAMES * S2A_HDR_REARM_MAX) g->step *= 2;
    g->nil_run = 0;
    g->latched = 0;          // 解除锁存：切回 16F 后由 step() 重新累计 nil 帧
    return 1;
}

// 16F 画布稳定工作时调用：把退避复位，下次真的降级时仍从 10 秒开始试探。
static inline void s2a_hdr_fmt_guard_ok(s2a_hdr_fmt_guard *g) {
    if (!g || g->latched) return;
    g->wait = 0;
    g->step = S2A_HDR_REARM_FRAMES;
}

// 缺省参数。S2A_HDR_HEADROOM 只读一次并缓存（设置链在起局前写入，运行期由 s2a_hdr_head_set 覆盖）。
static inline s2a_hdr_itm s2a_hdr_itm_params(void) {
    static s2a_hdr_itm p;
    static int done;
    if (!done) {
        p.head = 2.0f; p.knee_lo = 0.55f; p.knee_hi = 1.0f;
        const char *e = getenv("S2A_HDR_HEADROOM");
        if (e && *e) {
            char *end = NULL;
            double v = strtod(e, &end);
            if (end && end != e && v >= 1.0 && v <= 4.0) p.head = (float)v;
        }
        done = 1;
    }
    // 注意不能改缓存里的 p：运行期覆盖一旦写进缓存，覆盖撤销后就再也回不到基线
    //   （实测：2.0 → 覆盖 3.0 → 覆盖 1.0 → 撤销，缓存里留着 1.0）。所以按值返回副本。
    s2a_hdr_itm r = p;
    if (s2a_hdr_head_live > 0.0f) r.head = s2a_hdr_head_live;   // 运行期覆盖优先
    return r;
}

// 0 = 走原来的 BGRA8 路径（逐字节同旧行为）；1 = RGBA16Float + EDR。
//   只认明确的 0 / false / no / off（不分大小写）为关；没设也算关——未启用时行为一点不变。
//   设了空串算开：启动环境显式给了这个变量，意图就是开。
static inline int s2a_hdr_enabled(const char *env) {
    if (!env) return 0;
    if (*env == '0') return 0;
    if (!strcasecmp(env, "false") || !strcasecmp(env, "no") || !strcasecmp(env, "off")) return 0;
    return 1;
}

// 出屏「格式 / EDR 声明」的决策，抽成纯函数：真机三格矩阵全靠它，而这两个开关决定「会不会黑屏」。
//   S2A_HDR_OUTPUT=1  总开关：16F 画布 + 声明 EDR（T2）
//   S2A_HDR_FMT=1     只换画布：16F，但不声明 EDR（T1，用来分辨黑屏是格式还是 EDR 造成的）
//   S2A_HDR_FMT=0     只换声明：仍是 8 位字节，但声明 EDR（T3）
// 组合语义（旧实现把 FMT=1 当「永远不声明 EDR」，于是 FMT=1 + OUTPUT=1 会悄悄失去 EDR，
//   而 T2 正是这么写的 —— 矩阵与代码自相矛盾的坑，回归用例在 t_hdr_env）：
//   FMT=1 + OUTPUT=1 ⇒ 16F + EDR（OUTPUT 是显式打开，诊断开关不该把它关掉）
//   FMT=0 + OUTPUT=*  ⇒ 8B  + EDR（FMT=0 的意图就是「只声明」）
//   其它/空串/垃圾值 ⇒ 同未设
static inline void s2a_hdr_out_plan(int output_on, const char *fmtenv, int *want16, int *want_edr) {
    const int fmt = (fmtenv && *fmtenv == '1') ? 1 : ((fmtenv && *fmtenv == '0') ? 0 : -1);
    if (fmt == 1) { *want16 = 1; *want_edr = output_on ? 1 : 0; }
    else if (fmt == 0) { *want16 = 0; *want_edr = 1; }
    else { *want16 = output_on ? 1 : 0; *want_edr = output_on ? 1 : 0; }
}

// sRGB 编码值（0..1）→ 线性。分段式与 IEC 61966-2-1 一致，也是 Metal 采样 sRGB 纹理时的解码曲线。
static inline float s2a_hdr_to_linear(float c) {
    // 用 !(c > 0) 而不是 c <= 0：NaN 走这一支会返回 0，否则会一路 powf 到 NaN 传进直方图。
    if (!(c > 0.0f)) return 0.0f;
    if (c <= 0.04045f) return c / 12.92f;
    if (c >= 1.0f) return 1.0f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

// 线性 -> sRGB 编码值（0..1）。上屏前用，与上一条互逆；两段在 0.0031308 处连续。
static inline float s2a_hdr_to_srgb(float c) {
    if (!(c > 0.0f)) return 0.0f;
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static inline float s2a_hdr_smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// 给一个亮度 y（线性）算扩张倍数。y ≤ knee_lo 恒为 1（中间调不动），y ≥ knee_hi 恒为 head（不外推）。
static inline float s2a_hdr_itm_gain(const s2a_hdr_itm *p, float y) {
    if (!p) return 1.0f;
    // 退化 / 反向的拐点：上屏的 fs 有 `if (!(hi > lo + 1e-5)) return a * head;` 的早退分支，
    //   那边整帧直接吃满 head。CPU 侧原来没有这条，于是 lo == hi 时 smoothstep 的
    //   (x-e0)/0 走进 <=0 分支给 gain 1，两侧差整整一个 head（实测 0.214041 vs 0.428082）。
    //   这里补上同一分支，CPU 与上屏从此一致（t_hdr_shader_mirror 场景二）。
    if (!(p->knee_hi > p->knee_lo + 1e-5f)) return p->head;
    return 1.0f + (p->head - 1.0f) * s2a_hdr_smoothstep(p->knee_lo, p->knee_hi, y);
}

// 三通道（输入是**线性光**）：按 Rec.709 亮度算 gain，再乘回三通道（同一个倍数 → 保色相，不偏色）。
static inline void s2a_hdr_itm_rgb_lin(const s2a_hdr_itm *p, const float lin[3], float out[3]) {
    float y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
    float k = s2a_hdr_itm_gain(p, y);
    out[0] = lin[0] * k; out[1] = lin[1] * k; out[2] = lin[2] * k;
}

// 同上，但输入是**sRGB 编码值**（0..1）。两套约定并存过一段时间，踩过一次坑：把线性值喂给这个函数
// 会被再解码一遍，输出小两个数量级。需要线性入口请用 s2a_hdr_itm_rgb_lin。
static inline void s2a_hdr_itm_rgb(const s2a_hdr_itm *p, const float in[3], float out[3]) {
    float lin[3];
    for (int i = 0; i < 3; i++) lin[i] = s2a_hdr_to_linear(in[i]);
    s2a_hdr_itm_rgb_lin(p, lin, out);
}


// ---------- 另一种放置方式：BT.2408-8 §5.1 的 display-referred SDR→HDR ----------
// 本模块默认用「纸白以下不动、纸白以上按 smoothstep 抬」的 knee 方案（保留 SDR 的对比观感，
//   只把高光抬上去）。BT.2408-8 §5.1 给的是另一种官方做法：
//     1) SDR 峰值白 100 cd/m² 线性放大到 HDR Reference White 203 cd/m²（×2.03，+1.02 stop）；
//     2) 再做 OOTF gamma 校正 L' = L^1.15（BBC 主观测试；ARIB 为 1.16），
//        否则中调会显得被冲淡 —— 也就是「白抬 1.02 stop，但中调抬得比白少」。
//   合起来：L_out = head * L^1.15（head 缺省 2.03）。黑仍映射到 0，白色正好落在 head。
//   与 knee 方案的实测差异：线性 0.18 中间调 knee ×1.0000 / bt2408 ×1.5696；
//   线性 0.50 knee ×1.0000（缺省拐点 0.55 之下）/ bt2408 ×1.8295；线性 1.00 knee ×2.0000（head=2）/ bt2408 ×2.0300。
//   也就是说 bt2408 把整幅按幂函数抬起来（黑仍为 0），knee 只抬纸白以上 —— 两种观感，
//   不是谁更对：bt2408 更接近「把 SDR 放进 HDR 容器」的官方定义，knee 更接近 RTX HDR
//   那种「保住 SDR 观感、只把高光顶出去」。
//   逐通道做 gamma 会改变彩度（三个通道各自幂运算），所以这里按 Rec.709 亮度算比值再乘 RGB，
//   与 s2a_hdr_itm_rgb 同口径；这是相对文献的自觉偏离，写在 docs/SOURCES.md。
#ifndef S2A_HDR_BT2408_HEAD
#define S2A_HDR_BT2408_HEAD 2.03f
#endif
#ifndef S2A_HDR_BT2408_GAMMA
#define S2A_HDR_BT2408_GAMMA 1.15f
#endif

static inline void s2a_hdr_bt2408_rgb_lin(const float lin[3], float head, float out[3]) {
    float y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
    float k;
    if (!(head > 0.0f)) head = S2A_HDR_BT2408_HEAD;
    if (!(y > 0.0f)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    k = head * powf(y, S2A_HDR_BT2408_GAMMA - 1.0f);   // 相对亮度 y 的等比倍数
    out[0] = lin[0] * k; out[1] = lin[1] * k; out[2] = lin[2] * k;
}

// ---------- 色域 / 饱和度（对标 RTX HDR 的 "saturation"，取 BT.2087 的 sRGB→Display P3 矩阵）----------
// RTX HDR 除了亮度扩张还会把 SDR 的 sRGB 色彩往宽色域推一点，并用一个饱和度旋钮补齐观感；
//   libplacebo 对应 gamut_expansion / saturation。这里默认全关（sat=1、p3=0），所以缺省包逐字节同以前。
// 两个旋钮都保持「灰不变」：sRGB→P3 矩阵每行和为 1，饱和度按亮度做中心缩放 → 中性灰在任何组合下都不动。
#ifndef S2A_HDR_SAT_MIN
#define S2A_HDR_SAT_MIN 0.5f
#endif
#ifndef S2A_HDR_SAT_MAX
#define S2A_HDR_SAT_MAX 2.0f
#endif

// S2A_HDR_SAT：颜色饱和度倍数（缺省 1.0，钳 [0.5,2]）。1 = 不动。
static inline float s2a_hdr_sat(void) {
    static float v; static int done;
    if (!done) {
        v = 1.0f;
        const char *e = getenv("S2A_HDR_SAT");
        if (e && *e) {
            char *end = NULL; double d = strtod(e, &end);
            if (end && end != e) { if (d < S2A_HDR_SAT_MIN) d = S2A_HDR_SAT_MIN; if (d > S2A_HDR_SAT_MAX) d = S2A_HDR_SAT_MAX; v = (float)d; }
        }
        done = 1;
    }
    return v;
}
// S2A_HDR_P3：往 Display P3 推多少（缺省 0，钳 [0,1]）。0 = 不动，1 = 按 P3 原色解释。
//   不是「换色域标签」：矩阵只把三原色往宽色域挪一点，白点与灰轴不变。
static inline float s2a_hdr_p3(void) {
    static float v; static int done;
    if (!done) {
        v = 0.0f;
        const char *e = getenv("S2A_HDR_P3");
        if (e && *e) {
            char *end = NULL; double d = strtod(e, &end);
            if (end && end != e) { if (d < 0.0) d = 0.0; if (d > 1.0) d = 1.0; v = (float)d; }
        }
        done = 1;
    }
    return v;
}
// 线性光下的 sRGB → Display P3（BT.2087 推导的 3×3；每行和为 1，所以中性灰不变）
// 原始 BT.2087 数值每行和是 0.9999 / 1.0001 / 1.0000（四舍五入留下 1e-4 的偏差，灰会偏 1e-4 量级）；
//   这里按行归一化，让「灰不变」变成结构性的而不是靠容差。
#define S2A_HDR_P3_R0 0.822582f, 0.177418f, 0.000000f
#define S2A_HDR_P3_R1 0.033197f, 0.966803f, 0.000000f
#define S2A_HDR_P3_R2 0.017100f, 0.072400f, 0.910500f
static inline void s2a_hdr_p3_rgb(const float in[3], float mix, float out[3]) {
    float m[3][3] = {{S2A_HDR_P3_R0}, {S2A_HDR_P3_R1}, {S2A_HDR_P3_R2}};
    if (mix <= 0.0f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    if (mix > 1.0f) mix = 1.0f;
    for (int i = 0; i < 3; i++) {
        float v = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
        out[i] = in[i] + (v - in[i]) * mix;
    }
}
// 保亮度的饱和度：以 Rec.709 亮度为中心往外拉。sat=1 恒等；sat>1 更艳，sat<1 更灰；纯灰恒等。
static inline float s2a_hdr_lab_f(float t) {
    return t > 0.008856f ? powf(t, 1.0f / 3.0f) : (7.787f * t + 16.0f / 116.0f);
}

static inline float s2a_hdr_lab_finv(float t) {
    float t3 = t * t * t;
    return t3 > 0.008856f ? t3 : (t - 16.0f / 116.0f) / 7.787f;
}

// S2A_HDR_SAT_SPACE：1 = 在 Lab 里缩放色度（保色相，默认），0 = 线性锚定（旧行为）
static inline int s2a_hdr_sat_space(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("S2A_HDR_SAT_SPACE");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

// ---- 线性 sRGB ⇄ CIELAB（D65）----
// 为什么需要它：把饱和度乘在「偏离亮度的量」上（y + (c-y)*s）在 Lab 里不是保色相的，
// 真实照片 sat=1.6 实测 p99 色相误差 9.9–20.1°、sat=2.0 到 17–30°（Monument 最差）。
// 直接在 Lab 里乘 a*/b*（等价于在 LCh 里乘 C*）则色相角按构造不变：实测三张照片 0.00°。
static inline void s2a_hdr_lab_from_lin(const float rgb[3], float lab[3]) {
    float x = 0.4124564f * rgb[0] + 0.3575761f * rgb[1] + 0.1804375f * rgb[2];
    float y = 0.2126729f * rgb[0] + 0.7151522f * rgb[1] + 0.0721750f * rgb[2];
    float z = 0.0193339f * rgb[0] + 0.1191920f * rgb[1] + 0.9503041f * rgb[2];
    float fx = s2a_hdr_lab_f(x / 0.95047f), fy = s2a_hdr_lab_f(y), fz = s2a_hdr_lab_f(z / 1.08883f);
    lab[0] = 116.0f * fy - 16.0f;
    lab[1] = 500.0f * (fx - fy);
    lab[2] = 200.0f * (fy - fz);
}

static inline void s2a_hdr_lin_from_lab(const float lab[3], float rgb[3]) {
    float fy = (lab[0] + 16.0f) / 116.0f;
    float fx = fy + lab[1] / 500.0f, fz = fy - lab[2] / 200.0f;
    float x = 0.95047f * s2a_hdr_lab_finv(fx);
    float y = s2a_hdr_lab_finv(fy);
    float z = 1.08883f * s2a_hdr_lab_finv(fz);
    rgb[0] = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    rgb[1] = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    rgb[2] = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;
}

static inline float s2a_hdr_sat_scale_min(const float lab[3], float s) {
    float t[3] = {lab[0], lab[1] * s, lab[2] * s}, rgb[3];
    s2a_hdr_lin_from_lab(t, rgb);
    return rgb[0] < rgb[1] ? (rgb[0] < rgb[2] ? rgb[0] : rgb[2])
                           : (rgb[1] < rgb[2] ? rgb[1] : rgb[2]);
}

// 保色相饱和度：在 Lab 里缩放 a*/b*；穿出线性 RGB 负区时二分退回最大的安全倍数。
static inline void s2a_hdr_sat_lab_rgb(const float in[3], float sat, float out[3]) {
    float lab[3];
    s2a_hdr_lab_from_lin(in, lab);
    // 中性色（含灰、白、黑）在 Lab 里 a*=b*=0，缩放没有意义，而往返换算有 ~1e-4 的
    // float 误差 —— 直接短路，保证「灰恒为灰」是逐位性质而不是近似性质。
    if (lab[1] > -1e-3f && lab[1] < 1e-3f && lab[2] > -1e-3f && lab[2] < 1e-3f) {
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
        return;
    }
    float s = sat;
    if (s2a_hdr_sat_scale_min(lab, s) < 0.0f) {
        float lo = 1.0f, hi = s;                     // lo 一定安全（原色本身可显示）
        for (int i = 0; i < 14; i++) {
            float mid = 0.5f * (lo + hi);
            if (s2a_hdr_sat_scale_min(lab, mid) >= 0.0f) lo = mid; else hi = mid;
        }
        s = lo;
    }
    float t[3] = {lab[0], lab[1] * s, lab[2] * s};
    s2a_hdr_lin_from_lab(t, out);
    for (int i = 0; i < 3; i++) if (out[i] < 0.0f) out[i] = 0.0f;
}

static inline void s2a_hdr_sat_rgb(const float in[3], float sat, float out[3]) {
    if (sat == 1.0f) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }   // 缺省路径逐位不变
    if (s2a_hdr_sat_space() == 1) { s2a_hdr_sat_lab_rgb(in, sat, out); return; }
    float y = 0.2126f * in[0] + 0.7152f * in[1] + 0.0722f * in[2];
    float dmin = 0.0f;
    for (int i = 0; i < 3; i++) { float d = in[i] - y; if (d < dmin) dmin = d; }
    float ss = sat;
    if (dmin < 0.0f && y > 0.0f) { float smax = y / -dmin; if (smax < ss) ss = smax; }
    out[0] = y + (in[0] - y) * ss;
    out[1] = y + (in[1] - y) * ss;
    out[2] = y + (in[2] - y) * ss;
    for (int i = 0; i < 3; i++) if (out[i] < 0.0f) out[i] = 0.0f;   // 只夹负侧：>1 的高光是 HDR 要保留的东西
}
// 完整链路（线性光）：色域 → 饱和度 → ITM。与 MSL 的 16F 分支同序同常数。
// 调色后可能超上界：Display P3 混色与饱和度缩放都会把某个通道推高（实测 sat=3.0 时
// 蓝通道可到 head+2.05）。EDR 画布超过上界的部分由系统**直接钳**，那会切出色块和色相偏移——
// 正是保持色相的努力要避免的东西。这里改成等比缩放：整条三元组乘 head/m，只降亮度、不动色相。
static inline void s2a_hdr_clip_head(const float in[3], float head, float out[3]) {
    if (!(head > 0.0f)) head = 1.0f;
    float m = in[0] > in[1] ? in[0] : in[1];
    if (in[2] > m) m = in[2];
    float k = (m > head) ? head / m : 1.0f;
    for (int i = 0; i < 3; i++) {
        float v = in[i] * k;
        out[i] = v > 0.0f ? v : 0.0f;
    }
}

static inline void s2a_hdr_grade_rgb(const s2a_hdr_itm *p, float sat, float p3, const float lin_in[3], float out[3]) {
    if (sat == 1.0f && p3 <= 0.0f) {   // 两个旋钮都没动：直接走 ITM，逐位等同 s2a_hdr_itm_rgb
        float y0 = 0.2126f * lin_in[0] + 0.7152f * lin_in[1] + 0.0722f * lin_in[2];
        float k0 = s2a_hdr_itm_gain(p, y0);
        out[0] = lin_in[0] * k0; out[1] = lin_in[1] * k0; out[2] = lin_in[2] * k0;
        return;
    }
    float a[3];
    s2a_hdr_p3_rgb(lin_in, p3, a);
    s2a_hdr_sat_rgb(a, sat, a);
    float y = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
    float k = s2a_hdr_itm_gain(p, y);
    float g[3] = {a[0] * k, a[1] * k, a[2] * k};
    s2a_hdr_clip_head(g, p->head, out);
}

// ---------- 内容映射（RTX HDR 那一路）：把整个 SDR 画面放进 HDR 容器 ----------
// 上面两种放置方式以前只有 hdr_convert（工具）能选，设备上永远是 knee 方案。于是「开了 HDR 之后
// 画面几乎没变」是必然的：纸白定在 99.5 分位 ⇒ 99% 以上像素落在拐点之下（docs/FIDELITY.md 实测
// 码值有变化的像素只有 0.02%–0.41%）。游戏内容没有系统 UI 要保护，用户想要的是整体抬起来。
//
// 所以把选择权交给设备：S2A_HDR_MAP=1 → bt2408（L_out = head * L^1.15，黑仍 0、白正好落 head）。
// 缺省 0 保持 knee，逐字节同以前（缺省包与上游的差异仍只有「有没有画 16F」这一条）。
#ifndef S2A_HDR_MAP_DEFAULT
#define S2A_HDR_MAP_DEFAULT 0
#endif

// 纯函数版本的解析（单测直接喂字符串，不必 setenv + 静态缓存）
static inline int s2a_hdr_map_parse(const char *e) {
    // 只认显式的 1 / bt2408 / content；其它（空、0、knee、拼错）一律回缺省，
    //   因为这一档会把纸白以下一起抬起来，拼错一个字就整幅变样。
    if (!e) return S2A_HDR_MAP_DEFAULT;
    if (e[0] == '1' || !strncmp(e, "bt2408", 6) || !strncmp(e, "content", 7)) return 1;
    return S2A_HDR_MAP_DEFAULT;
}

static inline int s2a_hdr_map(void) {
    static int v = -1;
    if (v < 0) v = s2a_hdr_map_parse(getenv("S2A_HDR_MAP"));
    return v;
}

// map: 0 = knee（缺省） 1 = BT.2408 内容映射。两路的色域/饱和度顺序一致（p3 → sat → 亮度映射），
// 只有「乘多少」不同：knee 走 smoothstep 拐点，bt2408 走 head * y^(gamma-1)。
static inline void s2a_hdr_grade_rgb_map(const s2a_hdr_itm *p, float sat, float p3, int map,
                                         const float lin_in[3], float out[3]) {
    if (map != 1) { s2a_hdr_grade_rgb(p, sat, p3, lin_in, out); return; }
    float a[3];
    if (sat == 1.0f && p3 <= 0.0f) { a[0] = lin_in[0]; a[1] = lin_in[1]; a[2] = lin_in[2]; }
    else {
        s2a_hdr_p3_rgb(lin_in, p3, a);
        s2a_hdr_sat_rgb(a, sat, a);
    }
    float y = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
    float head = (p && p->head > 0.0f) ? p->head : S2A_HDR_BT2408_HEAD;
    float k = (y > 0.0f) ? head * powf(y, S2A_HDR_BT2408_GAMMA - 1.0f) : 0.0f;
    float g[3] = {a[0] * k, a[1] * k, a[2] * k};
    s2a_hdr_clip_head(g, head, out);
}

#ifdef __cplusplus
}
#endif


// —— knee 的发布/消费 ——
// 写入方是 Metal 完成回调（后台队列，hdr_hist_tick 里每 8 帧一次），读取方是渲染线程每帧。
// 拆成两个 float 写会有撕裂窗口：lo 已更新、delta 还是旧的 ⇒ hi = lo + delta 算出来可能 < lo，
// 该帧不是「不扩张」而是「反向压缩」，画面会闪一下（而且只在负载抖动时偶发，日志里看不到）。
// 打包进一个 64 位再原子发布，读方一次拿到自洽的 (lo, delta)。
// 打包用 float 的位模式（不变量：delta >= 0 由调用方保证），release/acquire 保证渲染线程
// 不会读到「新 lo + 旧 delta」这种跨帧组合。
static inline unsigned long long s2a_hdr_knee_pack(float lo, float delta) {
    unsigned int a = 0u, b = 0u;
    memcpy(&a, &lo, sizeof a);
    memcpy(&b, &delta, sizeof b);
    return ((unsigned long long)a << 32) | (unsigned long long)b;
}

static inline void s2a_hdr_knee_unpack(unsigned long long v, float *lo, float *delta) {
    unsigned int a = (unsigned int)(v >> 32), b = (unsigned int)(v & 0xffffffffull);
    if (lo) memcpy(lo, &a, sizeof a);
    if (delta) memcpy(delta, &b, sizeof b);
}

static inline void s2a_hdr_knee_publish(unsigned long long *slot, float lo, float delta) {
    if (!slot) return;
    if (!(delta >= 0.0f)) delta = 0.0f;      // 反向 knee 不上屏（打包不变量）
    __atomic_store_n(slot, s2a_hdr_knee_pack(lo, delta), __ATOMIC_RELEASE);
}

// 返回 1 表示拿到已发布过的值；0 表示从未发布（调用方用自己的缺省 0.55/0.45）
static inline int s2a_hdr_knee_take(const unsigned long long *slot, float *lo, float *delta) {
    if (!slot) return 0;
    unsigned long long v = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (v == 0ull) return 0;
    s2a_hdr_knee_unpack(v, lo, delta);
    return 1;
}



// ---- 诊断行（S2A_HDR_LOG=1）--------------------------------------------------------------
// HDR 是「看不见」的功能：用户没法从画面上判断它到底有没有生效，真机三格矩阵（T1/T2/T3）也只能靠日志判。
// 这一行的格式固定、字段名固定、小数位固定，好让 grep/awk 与人工比对都不出错：
//   HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1 map=0
// map 是放置方式（0 = 拐点，1 = BT.2408 内容映射）。它**不在** itm 里：itm 只回答「有没有走扩张」，
//   而 map=1 时 knee 整套不参与运算（只作日志），这两件事混在一个字段里，reviewer 一定先问
//   「knee 明明打印了 0.34 为什么画面是整幅亮的」——所以单独打。旧日志没有这个字段也能解析（缺省 0）。
// fmt 打的是**当前画布**（16F/8B），itm 打的是上屏是否真的走扩张（8 位画布恒 0）——
// 这两个字段分开打，是因为「格式换过但 shader 没换」正好是降级时最容易搞错的一步。
#define S2A_HDR_LOG_EVERY 600   // 600 帧 ≈10 秒 @60 Hz

typedef struct {
    int   fmt16;      // 1 = 画布是 RGBA16Float
    int   itm;        // 1 = 上屏走 ITM 扩张
    int   frozen;     // 1 = 本帧被判为淡出/暗场，拐点保持
    int   map;        // 0 = 拐点方案, 1 = BT.2408 内容映射（knee 不参与）
    float head, knee_lo, knee_hi, white, avg, scene;
} s2a_hdr_stat;

// 返回 snprintf 的「若缓冲足够大会写多少」（>n-1 表示被截断）。
static inline int s2a_hdr_status_line(char *buf, size_t n, const s2a_hdr_stat *st) {
    if (!buf || n == 0) return 0;
    if (!st) { return snprintf(buf, n, "HDR stat: (none)"); }
    char avg[32];
    if (isfinite(st->avg)) snprintf(avg, sizeof avg, "%.2f", (double)st->avg);
    else snprintf(avg, sizeof avg, "n/a");       // 首帧 / 空直方图：不要打 nan/-inf 去污染日志
    char scene[16];
    if (isfinite(st->scene)) snprintf(scene, sizeof scene, "%.2f", (double)st->scene);
    else snprintf(scene, sizeof scene, "n/a");
    return snprintf(buf, n,
                    "HDR stat: fmt=%s head=%.2f knee=%.4f+%.4f white=%.4f avg=%s scene=%s frozen=%d itm=%d map=%d",
                    st->fmt16 ? "16F" : "8B", (double)st->head,
                    (double)st->knee_lo, (double)(st->knee_hi - st->knee_lo),
                    (double)st->white, avg, scene, st->frozen ? 1 : 0, st->itm ? 1 : 0,
                    st->map ? 1 : 0);
}

#endif
