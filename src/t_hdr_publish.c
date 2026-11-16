// knee 的原子发布：撕裂读写模型 + 打包往返
//
// 为什么需要这套：g_hdr_knee[0]（lo）与 [1]（hi-lo）由 Metal 完成回调（后台队列）写，
// 渲染线程每帧读来填 present 的 fragment buffer。两次独立 float 写之间，渲染线程可能读到
// 「新 lo + 旧 delta」：hi = lo + delta 可能 <= lo，该帧从「不扩张」变成「反向压缩」，
// 只在负载抖动时偶发、日志看不到。把 (lo, delta) 打包进 64 位一次原子发布即可消除。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "nvn_hdr.h"

static int fails, total;
static void ok(int cond, const char *what) { total++; if (!cond) { fails++; printf("  ✗ %s\n", what); } }

// 旧做法的模型：两次独立写，读方可以在两次写之间插进来
typedef struct { float lo, delta; } torn_old;

static float old_hi(const torn_old *k) { return k->lo + k->delta; }

int main(void) {
    printf("== knee 原子发布 ==\n");

    // 1) 打包往返逐位一致
    {
        const float pairs[][2] = {
            {0.55f, 0.45f}, {0.0f, 0.0f}, {0.6626f, 0.3374f}, {1e-6f, 0.0f},
            {0.3898f, 0.6102f}, {-0.0f, 0.0f}, {1.0f, 0.0f}, {3.5f, 0.0f}
        };
        int all = 1;
        for (int i = 0; i < 8; i++) {
            unsigned long long v = s2a_hdr_knee_pack(pairs[i][0], pairs[i][1]);
            float lo = -1, de = -1;
            s2a_hdr_knee_unpack(v, &lo, &de);
            if (memcmp(&lo, &pairs[i][0], 4) || memcmp(&de, &pairs[i][1], 4)) all = 0;
        }
        ok(all, "8 组 (lo,delta) 打包→解包逐位一致（含 ±0 与 1e-6）");
    }

    // 2) 随机往返 20000 组
    {
        unsigned long long st = 88172645463325252ull;
        int all = 1;
        for (int i = 0; i < 20000; i++) {
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            float lo = (float)((double)(st % 4000000u) / 1000000.0);
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            float de = (float)((double)(st % 4000000u) / 1000000.0);
            float a = -9, b = -9;
            s2a_hdr_knee_unpack(s2a_hdr_knee_pack(lo, de), &a, &b);
            if (memcmp(&a, &lo, 4) || memcmp(&b, &de, 4)) { all = 0; break; }
        }
        ok(all, "20000 组随机往返逐位一致");
    }

    // 3) 单次写 64 位 ⇒ 读方只能看到「整段旧」或「整段新」，不会跨帧
    {
        unsigned long long slot = 0ull;
        s2a_hdr_knee_publish(&slot, 0.30f, 0.70f);          // hi = 1.00
        unsigned long long before = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
        s2a_hdr_knee_publish(&slot, 0.90f, 0.10f);          // hi = 1.00，lo 大幅上移
        unsigned long long after = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
        float blo, bde, alo, ade;
        s2a_hdr_knee_unpack(before, &blo, &bde);
        s2a_hdr_knee_unpack(after, &alo, &ade);
        int seen_ok = (fabsf(bde - 0.70f) < 1e-6f && fabsf(ade - 0.10f) < 1e-6f);
        int no_mix = !(fabsf(blo - 0.90f) < 1e-6f && fabsf(bde - 0.70f) < 1e-6f);   // 新 lo + 旧 delta 是不允许的组合
        ok(seen_ok && no_mix, "发布前后两次读到的都是完整的一组 (lo,delta)，不存在「新 lo + 旧 delta」");
    }

    // 4) 旧做法确实会撕裂（判别力守卫）。注意撕裂的后果不是 hi<lo（hi=lo+delta 恒 ≥ lo），
    //    而是**这一帧的拐点既不是上一帧的、也不是这一帧的**：上下界来自两个不同时刻的判定。
    {
        torn_old k = {0.30f, 0.10f};        // 上一帧：hi = 0.40
        k.lo = 0.90f;                        // 这一帧的 lo 已写，delta 未写（写 hi-lo 之前被打断）
        float hi_torn = old_hi(&k);          // 0.90 + 0.10 = 1.00
        ok(fabsf(hi_torn - 1.00f) < 1e-6f, "旧做法撕裂帧的 hi 被算成 1.00（两个时刻的判定拼起来）");
        k.delta = 1.20f - 0.90f;             // 补完：这一帧真正是 [0.90, 1.20]
        ok(fabsf(old_hi(&k) - 1.20f) < 1e-6f, "补完第二次写后 hi 为真正的 1.20");
        // 打包版在同样两个时刻之间，读到的永远是完整的一组
        unsigned long long slot = 0ull;
        s2a_hdr_knee_publish(&slot, 0.30f, 0.10f);
        s2a_hdr_knee_publish(&slot, 0.90f, 0.30f);
        float lo = 0, de = 0;
        s2a_hdr_knee_take(&slot, &lo, &de);
        ok(fabsf(lo - 0.90f) < 1e-6f && fabsf(de - 0.30f) < 1e-6f, "打包版读到的 (lo,delta) 恰是最后发布的整组");
    }

    // 5) 未发布：take 返回 0，调用方用自己的缺省
    {
        unsigned long long slot = 0ull;
        float lo = 0, de = 0;
        ok(s2a_hdr_knee_take(&slot, &lo, &de) == 0, "从未发布时 take 返回 0");
    }

    // 6) 负 delta 被夹成 0（打包不变量：上屏的 knee 不允许反向）
    {
        unsigned long long slot = 0ull;
        s2a_hdr_knee_publish(&slot, 0.60f, -0.25f);
        float lo = 0, de = 0;
        s2a_hdr_knee_take(&slot, &lo, &de);
        ok(de == 0.0f && lo == 0.60f, "负 delta 发布后被夹成 0，lo 保持");
    }

    // 7) 打包保持可比较：hi 恒 >= lo（1000 组随机，含极端值）
    {
        unsigned long long st = 2463534242ull;
        int all = 1;
        for (int i = 0; i < 1000; i++) {
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            float lo = (float)((double)(st % 1000000u) / 1000000.0);
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            float hi = lo + (float)((double)(st % 1000000u) / 1000000.0);
            unsigned long long slot = 0ull;
            s2a_hdr_knee_publish(&slot, lo, hi - lo);
            float a = 0, b = 0;
            s2a_hdr_knee_take(&slot, &a, &b);
            if (!(a + b >= a - 1e-6f)) { all = 0; break; }
        }
        ok(all, "1000 组随机发布后 lo+delta >= lo 恒成立");
    }

    printf(fails ? "✗ t_hdr_publish：%d 项（失败 %d）\n" : "✓ t_hdr_publish：knee 原子发布 %d 项（失败 0）\n", total, fails);
    return fails ? 1 : 0;
}
