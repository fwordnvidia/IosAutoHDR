/* 把「ITU-R BT.2446-1 方法 A 的逆」算出来，与本补丁的 smoothstep 拐点曲线逐码值对照。
 *
 * 为什么要它：`docs/SOURCES.md` §2 写的是「我们的 gain() 就是方法 A 的形状」，那只是**形状**上的
 * 说法，没有数字。方法 A 的完整链路是「感知线性化 → 分段多项式 → 反感知线性化」，
 * 逆过来做 SDR→HDR 时，它会把**整段中间调抬亮**（不是只抬高光），而我们刻意不抬——
 * 因为抬了 UI/文字就跟着变亮。这一条差异值多少 stop，得算出来而不是嘴上说。
 *
 * 方法 A 的参数化（BT.2446-1 §4，1000 nit HDR → 100 nit SDR 的默认设定）：
 *   rho(L) = 1 + 32 * (L/10000)^2.4
 *   感知线性化   y_p = log(1 + (rho_h-1) y') / log(rho_h)
 *   分段曲线     y_c = 1.0770 y_p                     (y_p <= 0.7399)
 *                     = -1.1510 y_p^2 + 2.7811 y_p - 0.6302   (0.7399 < y_p < 0.9909)
 *                     = 0.5000 y_p + 0.5000            (y_p >= 0.9909)
 *   反线性化     y' = (rho^y_c - 1)/(rho - 1)
 *
 * 用法：hdr_curve_bt2446 [目标峰值 nits，缺省 203]
 * 输出两套曲线在所有 8 位码值上的增益，以及两者的最大差（以 stop 计）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn_hdr.h"

static double rho(double nits) { return 1.0 + 32.0 * pow(nits / 10000.0, 2.4); }

/* 方法 A 的分段曲线（HDR→SDR 方向） */
static double tone_a(double yp) {
    if (yp <= 0.7399) return 1.0770 * yp;
    if (yp < 0.9909) return -1.1510 * yp * yp + 2.7811 * yp - 0.6302;
    return 0.5000 * yp + 0.5000;
}

/* 上面那条曲线的逆（逐段解析），用于 SDR→HDR */
static double tone_a_inv(double yc) {
    const double yc1 = 1.0770 * 0.7399;                  /* 第一段终点 */
    const double yc2 = 0.5000 * 0.9909 + 0.5000;         /* 第二段终点 */
    if (yc <= yc1) return yc / 1.0770;
    if (yc < yc2) {
        const double a = 1.1510, b = -2.7811, c = 0.6302 + yc;
        const double disc = b * b - 4.0 * a * c;
        if (disc < 0.0) return 0.9909;                    /* 数值边缘，夹到断点 */
        return (-b - sqrt(disc)) / (2.0 * a);             /* 取小根（< 0.9909 那支） */
    }
    return 2.0 * yc - 1.0;
}

/* SDR（100 nit 参考、BT.709 已编码值）→ 目标峰值下的线性增益倍数 */
static double bt2446_gain(double code01, double target_nits) {
    if (code01 <= 0.0) return 1.0;
    const double rs = rho(100.0), rh = rho(target_nits);
    double yp_sdr = log(1.0 + (rs - 1.0) * code01) / log(rs);   /* SDR 侧感知线性化 */
    double yc = tone_a_inv(yp_sdr);                             /* 反曲线（SDR→HDR 就是取逆） */
    double y_hdr_g = (pow(rh, yc) - 1.0) / (rh - 1.0);          /* 反线性化：1.0 = 目标峰值 */
    double y_hdr_lin = pow(y_hdr_g, 2.4) * target_nits / 100.0; /* 折算到「相对 SDR 白」 */
    double y_sdr_lin = pow(code01, 2.4);
    return y_hdr_lin / y_sdr_lin;
}

int main(int argc, char **argv) {
    double target = argc > 1 ? atof(argv[1]) : 203.0;
    if (!(target > 100.0)) target = 203.0;

    /* 先自检：逆函数必须真的把分段曲线还原（否则下面的对照表全是错的） */
    double worst_inv = 0.0;
    for (int i = 0; i <= 1000; i++) {
        double yp = i / 1000.0;
        double back = tone_a_inv(tone_a(yp));
        double e = fabs(back - yp);
        if (e > worst_inv) worst_inv = e;
    }
    fprintf(stderr, "# 逆函数自检：最大还原误差 %.3e（应 <1e-9）\n", worst_inv);

    s2a_hdr_itm itm = { (float)(target / 100.0), 0.55f, 1.0f };
    printf("# ITU-R BT.2446-1 方法 A 的逆（目标峰值 %.0f nit，rho=%.9f）\n", target, rho(target));
    printf("code,linear,s2a_gain,bt2446_gain,stops\n");
    double worst = 0.0, worst_y = 0.0;
    for (int c = 4; c <= 255; c++) {
        double y = s2a_hdr_to_linear((float)c / 255.0f);
        double g_ours = s2a_hdr_itm_gain(&itm, (float)y);
        double g_bt = bt2446_gain(pow(y, 1.0 / 2.4), target);
        double d = log2(g_bt / g_ours);
        if (fabs(d) > fabs(worst)) { worst = d; worst_y = y; }
        printf("%d,%.6f,%.4f,%.4f,%+.3f\n", c, y, g_ours, g_bt, d);
    }
    printf("# 最大差 %+.3f stop，出现在线性 %.6f（码值约 %d）\n",
           worst, worst_y, (int)(255.0 * pow(worst_y, 1.0 / 2.4) + 0.5));
    return 0;
}
