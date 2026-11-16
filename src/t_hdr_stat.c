// t_hdr_stat：诊断行（S2A_HDR_LOG）的格式契约。
// 为什么值得单测：这行字是**真机三格矩阵唯一能拿到的证据**（本机没有设备、也没有 Metal 编译器），
// 一旦字段名/小数位漂移，远处的人贴回来的日志就没法比对；而 snprintf 的截断行为又很容易写错
// （返回值是「若够大会写多少」，不是实际写入长度）。
#include "nvn_hdr.h"

static int fails, checks;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("  ✗ %s\n", what); }
}

int main(void) {
    char b[256];
    s2a_hdr_stat st;

    // 1) 典型 16F 帧：字段名、小数位、顺序全部钉死
    memset(&st, 0, sizeof st);
    st.fmt16 = 1; st.itm = 1; st.frozen = 0;
    st.head = 2.0f; st.knee_lo = 0.34f; st.knee_hi = 1.0f;
    st.white = 0.79f; st.avg = -1.2f; st.scene = 0.0f;
    int n = s2a_hdr_status_line(b, sizeof b, &st);
    ok(n > 0 && (size_t)n == strlen(b), "返回值等于实际长度（未截断）");
    ok(strcmp(b, "HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1 map=0") == 0,
       "16F 稳态整行逐字节匹配");
    // 内容映射档：knee 还在打（它是判定链路的产物），但 map=1 说明上屏没用它——
    //   这两件事必须分开显示，否则「knee=0.34 却整幅变亮」看起来像 bug。
    st.map = 1;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strcmp(b, "HDR stat: fmt=16F head=2.00 knee=0.3400+0.6600 white=0.7900 avg=-1.20 scene=0.00 frozen=0 itm=1 map=1") == 0,
       "内容映射档整行（map=1，其余字段不变）");
    ok(strstr(b, "knee=") && strstr(b, "map=1"), "map=1 时 knee 仍打印（区分「算了没用」与「没算」）");
    st.map = 0;
    ok(strstr(b, "HDR stat: ") == b, "以固定前缀开头（便于 grep）");

    // 2) 8 位降级帧：格式与 itm 都要显示成 0/8B —— 这正是降级时最容易漏改的一步
    st.fmt16 = 0; st.itm = 0; st.head = 1.0f;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strstr(b, "fmt=8B") != NULL, "8 位画布打 fmt=8B");
    ok(strstr(b, "itm=0") != NULL, "8 位画布打 itm=0");
    ok(strstr(b, "head=1.00") != NULL, "8 位画布 head 归 1");

    // 3) 冻结帧要看得出来（否则「拐点为什么不动」在日志里无从判断）
    st.fmt16 = 1; st.itm = 1; st.frozen = 1;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strstr(b, "frozen=1") != NULL, "冻结帧打 frozen=1");

    // 4) 非有限值不能污染日志（首帧 avg 还没算出来）
    st.frozen = 0; st.avg = NAN;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strstr(b, "avg=n/a") != NULL, "avg=NaN 打 n/a");
    ok(strstr(b, "nan") == NULL, "日志里不出现 nan");
    st.avg = -INFINITY;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strstr(b, "avg=n/a") != NULL, "avg=-inf 打 n/a");
    st.avg = -1.2f; st.scene = NAN;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strstr(b, "scene=n/a") != NULL, "scene=NaN 打 n/a");

    // 5) 截断：返回「够大会写多少」，缓冲仍然是合法的 NUL 结尾字符串
    char small[16];
    memset(small, 'X', sizeof small);
    int need = s2a_hdr_status_line(small, sizeof small, &st);
    ok(need > (int)sizeof small, "截断时返回值仍是完整长度");
    ok(strlen(small) == sizeof small - 1, "截断后长度 = n-1");
    ok(small[sizeof small - 1] == '\0', "截断后以 NUL 结尾");
    ok(strncmp(small, "HDR stat: fmt=1", 15) == 0, "截断内容是前缀");

    // 6) 边界：n=0 / buf=NULL / st=NULL 都不能崩
    ok(s2a_hdr_status_line(NULL, 32, &st) == 0, "buf=NULL 安全");
    ok(s2a_hdr_status_line(b, 0, &st) == 0, "n=0 安全");
    s2a_hdr_status_line(b, sizeof b, NULL);
    ok(strcmp(b, "HDR stat: (none)") == 0, "st=NULL 打 (none)");

    // 7) 拐点退化（纸白 == 裁切点）时仍是可读的 0 delta
    memset(&st, 0, sizeof st);
    st.fmt16 = 1; st.itm = 1; st.head = 2.0f; st.knee_lo = 1.0f; st.knee_hi = 1.0f;
    st.white = 1.0f; st.avg = 0.0f; st.scene = 1.0f;
    s2a_hdr_status_line(b, sizeof b, &st);
    ok(strstr(b, "knee=1.0000+0.0000") != NULL, "退化拐点打 +0.0000（不是负数）");

    printf("%s t_hdr_stat：诊断行格式 %d 项 → 全过\n", fails ? "✗" : "✓", checks);
    return fails ? 1 : 0;
}
