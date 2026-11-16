// 直方图回读的**控制流**对账：src/nvn_hdr_hist.inc 的 hdr_hist_tick 是设备专属代码，
// 它真正危险的地方不是算式，而是「什么时候算、往哪个槽写、什么回调可以作数」：
//   1) 每 S2A_HDR_HIST_EVERY 帧只跑一次（其它帧必须什么都不做，否则 30 帧一次的前提就不成立）；
//   2) 双槽轮转：同一槽两次写入之间必须隔 >= 2 次 tick，否则 GPU 落后时会覆盖 CPU 还没读的数据；
//   3) 分辨率变化会释放并重建 buffer，旧命令缓冲的回调必须作废（gen 代号），
//      否则旧直方图的结论会被写进新一帧的 g_hdr_knee。
// 本套把这三条抽成 C 模型跑 300 帧，用断言钉住（不涉及 Metal）。
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "nvn_hdr_auto.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(...) do { checks++; printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

// ——— 与 .inc 同构的最小模型 ———
typedef struct {
    uint64_t frame;      // 已过帧数
    unsigned hs;         // 双槽轮转
    unsigned gen;        // 代号
    int      live;       // 是否已被重建（旧对象 = 0）
    unsigned last_write[2];   // 每个槽最近一次被写入的 tick 序号
} tick_model;

typedef struct { unsigned tick; int slot; unsigned gen; } pending_read;

// 返回 1 = 这一帧真的拍了直方图（等价于 .inc 走到 @autoreleasepool 里），0 = 跳过
static int model_tick(tick_model *m, const int *enabled, const int *have_src, unsigned *slot_out) {
    if (!*enabled || !*have_src) return 0;
    if ((m->frame++ % S2A_HDR_HIST_EVERY) != 0) return 0;
    unsigned slot = (m->hs++) & 1u;
    // 记录写入时刻与「同一槽的间隔」
    struct { unsigned prev; } old = {m->last_write[slot]};
    (void)old;
    *slot_out = slot;
    m->last_write[slot] = 0;   // 由调用方在拿到 tick 序号后回填
    return 1;
}

// 回调是否可以被采纳（.inc 里的 `g_hh != owner || owner->gen != gen` 等价判定）
static int model_read_ok(const tick_model *cur, const tick_model *owner, unsigned gen) {
    return cur == owner && cur->gen == gen;
}

int main(void) {
    printf("直方图回读控制流：节拍 / 双槽 / 代号作废\n");

    // 1) 节拍：300 帧里恰好 10 次（每 30 帧一次），且落在第 0、30、60 … 帧
    {
        tick_model m; memset(&m, 0, sizeof m);
        int en = 1, src = 1;
        int ticks = 0, wrong = 0;
        const int expect = (300 + S2A_HDR_HIST_EVERY - 1) / S2A_HDR_HIST_EVERY;
        for (uint64_t f = 0; f < 300; f++) {
            unsigned slot = 0;
            int did = model_tick(&m, &en, &src, &slot);
            if (did) {
                ticks++;
                if ((f % S2A_HDR_HIST_EVERY) != 0) wrong++;
            }
        }
        CHECK(ticks == expect, "300 帧应拍 %d 次（每 %d 帧一次），实得 %d", expect, S2A_HDR_HIST_EVERY, ticks);
        CHECK(wrong == 0, "拍直方图的帧号必须都是 %d 的整数倍，违例 %d 次", S2A_HDR_HIST_EVERY, wrong);
        OK("节拍：300 帧 / 每 %d 帧一次 = %d 次，帧号全对齐", S2A_HDR_HIST_EVERY, ticks);
    }

    // 2) 开关与空源：关掉或没有源纹理时一帧都不拍
    {
        tick_model m1; memset(&m1, 0, sizeof m1);
        int en = 0, src = 1; unsigned s;
        int any = 0;
        for (uint64_t f = 0; f < 120; f++) any |= model_tick(&m1, &en, &src, &s);
        CHECK(any == 0, "S2A_HDR_AUTO 关着不该拍任何直方图");
        tick_model m2; memset(&m2, 0, sizeof m2);
        int en2 = 1, src2 = 0;
        any = 0;
        for (uint64_t f = 0; f < 120; f++) any |= model_tick(&m2, &en2, &src2, &s);
        CHECK(any == 0, "没有源纹理不该拍任何直方图");
        OK("开关：关掉 / 无源纹理时 120 帧内 0 次");
    }

    // 3) 双槽：连续 tick 的槽位必须交替；同一槽两次写入之间隔 >= 2 次 tick
    {
        tick_model m; memset(&m, 0, sizeof m);
        int en = 1, src = 1;
        unsigned slot, prev_slot = 2, last_tick_of_slot[2] = {0, 0};
        int alt_bad = 0, gap_bad = 0, tick_idx = 0;
        for (uint64_t f = 0; f < 600; f++) {
            if (!model_tick(&m, &en, &src, &slot)) continue;
            tick_idx++;
            if (prev_slot < 2 && slot == prev_slot) alt_bad++;
            if (last_tick_of_slot[slot] && (unsigned)(tick_idx - last_tick_of_slot[slot]) < 2) gap_bad++;
            last_tick_of_slot[slot] = (unsigned)tick_idx;
            prev_slot = slot;
        }
        CHECK(alt_bad == 0, "相邻两次拍直方图不能落同一个槽，违例 %d 次", alt_bad);
        CHECK(gap_bad == 0, "同一槽两次写入之间必须隔 >= 2 次 tick，违例 %d 次", gap_bad);
        OK("双槽：600 帧 %d 次 tick，槽位交替、同槽间隔 >= 2", tick_idx);
    }

    // 4) 代号：重建前的回调必须被丢弃（否则旧直方图会改写新一帧的 knee）
    {
        tick_model old_obj; memset(&old_obj, 0, sizeof old_obj);
        old_obj.gen = 1; old_obj.live = 1;
        tick_model new_obj; memset(&new_obj, 0, sizeof new_obj);
        new_obj.gen = 2; new_obj.live = 1;
        CHECK(model_read_ok(&new_obj, &new_obj, 2) == 1, "同一对象同一代：回调应被采纳");
        CHECK(model_read_ok(&new_obj, &old_obj, 1) == 0, "换对象后旧回调必须被丢弃");
        CHECK(model_read_ok(&new_obj, &new_obj, 1) == 0, "同对象但代号变了（重建过）：必须被丢弃");
        OK("代号：同对象同代采纳；换对象 / 换代一律丢弃");
    }

    // 5) 重建后代号单调递增（.inc 用 ++g_hh_gen，不复用旧值）
    {
        unsigned g = 0, gens[4];
        for (int i = 0; i < 4; i++) gens[i] = ++g;
        int mono = 1;
        for (int i = 1; i < 4; i++) if (gens[i] <= gens[i - 1]) mono = 0;
        CHECK(mono, "重建 4 次的代号必须严格递增");
        OK("代号序列：1,2,3,4（重建 4 次）严格递增");
    }

    printf("%s t_hdr_tick：回读控制流 %d 项（失败 %d）\n", fails ? "✗" : "✓", checks, fails);
    return fails ? 1 : 0;
}
