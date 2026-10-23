// 在 Mac 上运行的单元测试：用一串模拟电量检查 cl_decide 的状态转换。
//   cc -I src tools/test_logic.c -o /tmp/test_logic && /tmp/test_logic
#include <stdio.h>
#include <stdlib.h>
#include "logic.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  失败: " __VA_ARGS__); printf("\n"); } } while (0)

static const char *name(cl_mode m) { return m == CL_ALLOW ? "允许" : (m == CL_HOLD ? "保持" : "放电"); }

// 依次喂入电量，返回最后一次的模式，并把每步结果打印出来
static cl_mode run(const cl_params *p, cl_state *s, const int *levels, int n, const cl_mode *expect, const char *title) {
    printf("%s\n  ", title);
    cl_mode m = CL_ALLOW;
    for (int i = 0; i < n; i++) {
        m = cl_decide(p, s, levels[i]);
        printf("%d%%→%s  ", levels[i], name(m));
        CHECK(m == expect[i], "电量 %d%% 期望 %s，实际 %s", levels[i], name(expect[i]), name(m));
    }
    printf("\n");
    return m;
}

int main(void) {
    // 1. 从 40% 充到 50% 停，再降到恢复线以下才重新充
    {
        cl_params p = cl_make_params(50, 5, false);
        cl_state s = {0};
        int lv[]        = {40, 45, 49, 50, 51, 50, 48, 46, 45, 46, 49, 50};
        cl_mode ex[]    = {CL_ALLOW, CL_ALLOW, CL_ALLOW, CL_HOLD, CL_HOLD, CL_HOLD, CL_HOLD, CL_HOLD, CL_ALLOW, CL_ALLOW, CL_ALLOW, CL_HOLD};
        run(&p, &s, lv, 12, ex, "1. 上限 50%、回差 5：充到 50 停，降到 45 才恢复");
        CHECK(p.upper == 50 && p.lower == 45, "参数应为 50/45");
    }
    // 2. 满电开局 + 主动放电：先放电到 50，再保持，降到 45 恢复
    {
        cl_params p = cl_make_params(50, 5, true);
        cl_state s = {0};
        int lv[]        = {100, 80, 51, 50, 49, 47, 45, 44};
        cl_mode ex[]    = {CL_DRAIN, CL_DRAIN, CL_DRAIN, CL_HOLD, CL_HOLD, CL_HOLD, CL_ALLOW, CL_ALLOW};
        run(&p, &s, lv, 8, ex, "2. 满电开局并开启放电：放到 50 转保持，降到 45 恢复充电");
    }
    // 3. 满电开局但不放电：保持在 100%（只是不再充）
    {
        cl_params p = cl_make_params(50, 5, false);
        cl_state s = {0};
        int lv[]        = {100, 100, 99};
        cl_mode ex[]    = {CL_HOLD, CL_HOLD, CL_HOLD};
        run(&p, &s, lv, 3, ex, "3. 满电开局不放电：只停充，不主动降电量");
    }
    // 4. 开局电量处在恢复线和上限之间：按允许充电处理
    {
        cl_params p = cl_make_params(50, 5, false);
        cl_state s = {0};
        int lv[]        = {47, 48, 50, 49};
        cl_mode ex[]    = {CL_ALLOW, CL_ALLOW, CL_HOLD, CL_HOLD};
        run(&p, &s, lv, 4, ex, "4. 开局 47%（处于滞回区间）：继续充，到 50 才停");
    }
    // 5. 电量下限保护：即使在保持状态，掉到 20% 以下也强制允许充电
    {
        cl_params p = cl_make_params(30, 30, true);
        cl_state s = {0};
        int lv[]        = {60, 35, 30, 25, 21, 20, 19, 25, 30};
        cl_mode ex[]    = {CL_DRAIN, CL_DRAIN, CL_HOLD, CL_HOLD, CL_HOLD, CL_ALLOW, CL_ALLOW, CL_ALLOW, CL_HOLD};
        run(&p, &s, lv, 9, ex, "5. 上限 30、回差 30（恢复线被压到保护线 20）：到 20% 一定放行");
        CHECK(p.lower == CL_FLOOR_PERCENT, "恢复线应被抬到保护线 20，实际 %d", p.lower);
    }
    // 6. 参数越界会被收敛
    {
        cl_params p = cl_make_params(5, 0, false);
        CHECK(p.upper == CL_UPPER_MIN, "上限下界 30，实际 %d", p.upper);
        CHECK(p.lower == 29, "恢复线 = max(30-1, 20) = 29，实际 %d", p.lower);
        p = cl_make_params(250, 99, false);
        CHECK(p.upper == CL_UPPER_MAX, "上限上界 100，实际 %d", p.upper);
        CHECK(p.lower == 70, "回差上限 30 → 恢复线 70，实际 %d", p.lower);
        printf("6. 越界参数收敛: 5/0 → 上限 30 恢复线 29；250/99 → 上限 100 恢复线 70\n");
    }
    // 7. 上限 100%：充满后降到 95 才重新充（减少满电区间的反复补电）
    {
        cl_params p = cl_make_params(100, 5, false);
        cl_state s = {0};
        int lv[]        = {97, 100, 98, 96, 95, 96};
        cl_mode ex[]    = {CL_ALLOW, CL_HOLD, CL_HOLD, CL_HOLD, CL_ALLOW, CL_ALLOW};
        run(&p, &s, lv, 6, ex, "7. 上限 100%、回差 5：充满后降到 95 才重新充");
    }

    printf("\n%s（失败 %d 项）\n", g_fail ? "未通过" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
