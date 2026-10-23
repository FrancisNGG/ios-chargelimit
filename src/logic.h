// 充电限制的决策逻辑：纯函数，不碰硬件，方便在 Mac 上直接做单元测试。
#ifndef CL_LOGIC_H
#define CL_LOGIC_H

#include <stdbool.h>

#define CL_FLOOR_PERCENT 20   // 电量不高于此值时，无论怎么设置都允许充电（防止把电池放空）
#define CL_UPPER_MIN 30
#define CL_UPPER_MAX 100
#define CL_HYST_MIN 1
#define CL_HYST_MAX 30

typedef enum {
    CL_ALLOW = 0,   // 允许充电（默认状态）
    CL_HOLD  = 1,   // 停止充电并保持电量，设备由外部电源供电
    CL_DRAIN = 2,   // 电量高于上限：断开外部供电，改由电池供电，直到降到上限
} cl_mode;

typedef struct {
    int upper;      // 停充上限（%）
    int lower;      // 恢复充电线（%）：保持状态下降到此值以下才重新允许充电
    bool drain;     // 高于上限时是否主动放电
} cl_params;

typedef struct {
    bool holding;   // 是否处于「停充保持」（滞回状态）
} cl_state;

static inline int cl_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline cl_params cl_make_params(int upper, int hysteresis, bool drain) {
    cl_params p;
    p.upper = cl_clamp(upper, CL_UPPER_MIN, CL_UPPER_MAX);
    int lower = p.upper - cl_clamp(hysteresis, CL_HYST_MIN, CL_HYST_MAX);
    p.lower = lower < CL_FLOOR_PERCENT ? CL_FLOOR_PERCENT : lower;
    p.drain = drain;
    return p;
}

// 根据当前电量决定模式，并更新滞回状态。
//   电量 >= 上限            → 进入保持
//   电量 <= 恢复线          → 退出保持，允许充电
//   两者之间                → 维持原状态（滞回，避免在上限附近来回开关）
//   电量 <= CL_FLOOR_PERCENT → 无条件允许充电
static inline cl_mode cl_decide(const cl_params *p, cl_state *s, int level) {
    if (level <= CL_FLOOR_PERCENT) {
        s->holding = false;
        return CL_ALLOW;
    }
    if (level >= p->upper) s->holding = true;
    else if (level <= p->lower) s->holding = false;
    if (!s->holding) return CL_ALLOW;
    return (p->drain && level > p->upper) ? CL_DRAIN : CL_HOLD;
}

#endif
