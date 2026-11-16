// t_hdr_env：出屏「格式 / EDR 声明」两个开关的组合语义（s2a_hdr_out_plan）。
//
// 为什么单独成一套：这两个开关决定真机上「画面会不会黑」，而它们只在上屏路径里生效——
// 那段是 ObjC，跑不进单测。以前两份手写分支的判定规则实际上自相矛盾：
//   S2A_HDR_FMT=1 会把 S2A_HDR_OUTPUT=1 的 EDR 一起关掉，
//   而三格矩阵里的 T2 正是「FMT=1 + OUTPUT=1，期望 head>1.0」——按旧代码 T2 永远不成立，
//   于是「T2 没通过」会被误读成「这台机器不支持 HDR」。
// 现在决策收进 nvn_hdr.h 的纯函数，矩阵与代码同一份真相，并由这里逐格钉死。
#include <stdio.h>
#include <string.h>
#include "nvn_hdr.h"

static int fails; static int items;
#define CHECK(c, ...) do { items++; if (!(c)) { printf("  ✗ " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void plan(int out, const char *fmt, int *w16, int *edr) {
    *w16 = -1; *edr = -1;
    s2a_hdr_out_plan(out, fmt, w16, edr);
}

int main(void) {
    int w16, edr;

    // 一、全关：与上游逐字节一致（8 位、不声明 EDR）
    plan(0, NULL, &w16, &edr);
    CHECK(w16 == 0 && edr == 0, "什么都没设：应为 8B + 无 EDR，实际 w16=%d edr=%d", w16, edr);

    // 二、总开关：16F + EDR（T2）
    plan(1, NULL, &w16, &edr);
    CHECK(w16 == 1 && edr == 1, "OUTPUT=1：应为 16F + EDR，实际 w16=%d edr=%d", w16, edr);

    // 三、只换格式：16F、不声明 EDR（T1 —— 用来分辨黑屏归因）
    plan(0, "1", &w16, &edr);
    CHECK(w16 == 1 && edr == 0, "FMT=1：应为 16F + 无 EDR，实际 w16=%d edr=%d", w16, edr);

    // 四、只换声明：8 位 + EDR（T3 —— 声明了 EDR 但字节仍是 8 位）
    plan(0, "0", &w16, &edr);
    CHECK(w16 == 0 && edr == 1, "FMT=0：应为 8B + EDR，实际 w16=%d edr=%d", w16, edr);

    // 五、回归：FMT=1 不得把 OUTPUT=1 的 EDR 关掉（旧实现的坑，T2 就栽在这）
    plan(1, "1", &w16, &edr);
    CHECK(w16 == 1 && edr == 1, "FMT=1 + OUTPUT=1：应为 16F + EDR（T2），实际 w16=%d edr=%d", w16, edr);

    // 六、T3 与总开关同向：FMT=0 + OUTPUT=1 仍是 8 位 + EDR（不要变成 16F）
    plan(1, "0", &w16, &edr);
    CHECK(w16 == 0 && edr == 1, "FMT=0 + OUTPUT=1：应为 8B + EDR，实际 w16=%d edr=%d", w16, edr);

    // 七、空串：s2a_hdr_enabled 把空串当「开了」（启动环境里显式给了这个变量），FMT 空串算未设
    plan(0, "", &w16, &edr);
    CHECK(w16 == 0 && edr == 0, "FMT 为空串：应同未设（8B + 无 EDR），实际 w16=%d edr=%d", w16, edr);

    // 八、垃圾值只认第一位：'2' 不是 '1' 也不是 '0' ⇒ 视为未设（而不是「瞎猜成开」）
    plan(0, "2", &w16, &edr);
    CHECK(w16 == 0 && edr == 0, "FMT=2：应同未设，实际 w16=%d edr=%d", w16, edr);
    plan(1, "2", &w16, &edr);
    CHECK(w16 == 1 && edr == 1, "FMT=2 + OUTPUT=1：应等于 OUTPUT=1 单独打开，实际 w16=%d edr=%d", w16, edr);

    // 九、真值表全扫（2 × 7）：只用断言「不该出现的组合」不该出现
    {
        const char *fmts[] = {NULL, "", "0", "1", "2", "true", "false"};
        for (int o = 0; o <= 1; o++) {
            for (int k = 0; k < 7; k++) {
                plan(o, fmts[k], &w16, &edr);
                const int f1 = fmts[k] && fmts[k][0] == '1';
                const int f0 = fmts[k] && fmts[k][0] == '0';
                // 16F 一定来自 OUTPUT 或 FMT=1；8 位一定来自都没开或 FMT=0
                CHECK(w16 == ((o || f1) && !f0 ? 1 : 0),
                      "真值表 w16 不符：output=%d fmt=%s → w16=%d", o, fmts[k] ? fmts[k] : "(null)", w16);
                // EDR 一定来自 OUTPUT，或 FMT=0 的「只声明」
                CHECK(edr == (o || f0 ? 1 : 0),
                      "真值表 EDR 不符：output=%d fmt=%s → edr=%d", o, fmts[k] ? fmts[k] : "(null)", edr);
            }
        }
    }

    printf("%s t_hdr_env：格式/EDR 开关组合 %d 项（失败 %d）\n",
           fails ? "✗" : "✓", items, fails);
    return fails ? 1 : 0;
}
