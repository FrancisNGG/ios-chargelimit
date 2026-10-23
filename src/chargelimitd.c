// chargelimitd — iOS 12 充电上限守护进程（以 root 运行，由 launchd 常驻）
//
// 电量到达上限就停止充电并保持；降到「恢复线」以下才重新充电。
// 可选：电量高于上限时断开外部供电、改由电池放电到上限。
//
// 设置来自「系统设置 → 充电限制」：设置页把值写进 mobile 用户的偏好域，
// 本进程每 10 秒（或收到变更通知时）读取；运行状态也写回同一个域，供设置页显示。
// 控制充电的方式：往电源注册表（AppleARMPMUCharger）写 IsCharging / ExternalConnected 标志位，
// 退出、禁用或卸载时会恢复默认值。
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <notify.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "logic.h"

#define CL_VERSION       "1.0.1"
#define APP_ID           CFSTR("io.github.francisngg.chargelimit")
#define USER_MOBILE      CFSTR("mobile")
#define NOTIFY_CHANGED   "io.github.francisngg.chargelimit/changed"
#define LOG_PATH         "/var/log/chargelimit.log"
#define LOG_PATH_OLD     "/var/log/chargelimit.log.1"
#define POLL_SECONDS     10
#define HEARTBEAT_TICKS  30     // 约 5 分钟记一行
#define WARN_TICKS       3      // 连续几次发现「请求停充却仍在充电」才报警
#define WARN_AMP_MA      50
#define STATUS_REFRESH   60     // 状态没变化时，每隔多少秒也刷新一次「更新时间」，让界面判断守护进程是否还活着

typedef struct {
    int level;          // 电量百分比
    bool ext;           // 是否接了外部电源（读回值，不受本进程写入影响）
    bool charging;      // IsCharging 标志
    int amp;            // 电池实时电流（mA，正为充入、负为放出）
    int mv;             // 电压
} batt_t;

typedef enum {
    APPLIED_NONE = 0,   // 启动后还没下发过
    APPLIED_UNKNOWN,    // 拔过电源，下次接电要重新下发
    APPLIED_ALLOW,
    APPLIED_HOLD,
    APPLIED_DRAIN,
} applied_t;

// SDK 头文件把它标成 iOS 不可用，但设备上的 IOKit 里有这个函数，运行时取地址
typedef kern_return_t (*set_props_fn)(io_registry_entry_t, CFTypeRef);

static set_props_fn g_set_props;
static io_service_t g_ps;
static cl_state g_state;
static applied_t g_applied = APPLIED_NONE;
static int g_bad_ticks;
static bool g_warned;
static bool g_hold_confirmed;
static unsigned g_ticks;
static char g_last_status[200];
static time_t g_status_time;

// ---------------------------------------------------------------- 日志

static void lg(const char *fmt, ...) {
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > 256 * 1024) rename(LOG_PATH, LOG_PATH_OLD);
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tmv);
    fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- 偏好（mobile 用户的域）

static CFTypeRef pref_copy(const char *key) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFPreferencesCopyValue(k, APP_ID, USER_MOBILE, kCFPreferencesAnyHost);
    CFRelease(k);
    return v;
}

static int pref_int(const char *key, int def) {
    CFTypeRef v = pref_copy(key);
    int out = def;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            int x;
            if (CFNumberGetValue(v, kCFNumberIntType, &x)) out = x;
        } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
            out = CFBooleanGetValue(v) ? 1 : 0;
        } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
            char buf[32];
            if (CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8)) out = atoi(buf);
        }
        CFRelease(v);
    }
    return out;
}

static void pref_put(const char *key, CFTypeRef value) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFPreferencesSetValue(k, value, APP_ID, USER_MOBILE, kCFPreferencesAnyHost);
    CFRelease(k);
}

static void pref_set(const char *key, CFTypeRef value) {
    pref_put(key, value);
    CFPreferencesSynchronize(APP_ID, USER_MOBILE, kCFPreferencesAnyHost);
}

// 状态文字写回偏好域，设置页的「当前」行和 App 首页读它；statusTime 是写入时间（Unix 秒）。
// 内容没变且距上次写入不到 STATUS_REFRESH 秒就不写，避免无谓的磁盘写入。
static void set_status(const char *fmt, ...) {
    char buf[sizeof g_last_status];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    if (!strcmp(buf, g_last_status) && now - g_status_time < STATUS_REFRESH) return;
    strcpy(g_last_status, buf);
    g_status_time = now;
    CFStringRef s = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
    pref_put("statusText", s);
    CFRelease(s);
    long long t = (long long)now;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &t);
    pref_put("statusTime", n);
    CFRelease(n);
    CFPreferencesSynchronize(APP_ID, USER_MOBILE, kCFPreferencesAnyHost);
}

// ---------------------------------------------------------------- 电池

static io_service_t find_power_source(void) {
    io_service_t s = IOServiceGetMatchingService(0, IOServiceMatching("AppleARMPMUCharger"));
    if (!s) s = IOServiceGetMatchingService(0, IOServiceMatching("IOPMPowerSource"));
    return s;
}

static bool dict_num(CFDictionaryRef d, const char *key, long *out) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, k);
    CFRelease(k);
    if (!v) return false;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) return CFNumberGetValue(v, kCFNumberLongType, out);
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) { *out = CFBooleanGetValue(v) ? 1 : 0; return true; }
    return false;
}

static bool read_batt(batt_t *b) {
    if (!g_ps) return false;
    CFMutableDictionaryRef props = NULL;
    if (IORegistryEntryCreateCFProperties(g_ps, &props, NULL, 0) != KERN_SUCCESS || !props) return false;
    long v;
    bool ok = dict_num(props, "CurrentCapacity", &v);
    b->level = ok ? (int)v : 0;
    b->ext = dict_num(props, "ExternalConnected", &v) && v;
    b->charging = dict_num(props, "IsCharging", &v) && v;
    b->amp = dict_num(props, "InstantAmperage", &v) ? (int)v : 0;
    b->mv = dict_num(props, "Voltage", &v) ? (int)v : 0;
    CFRelease(props);
    return ok && b->level > 0 && b->level <= 100;
}

static bool write_flags(bool charging, bool external) {
    if (!g_set_props || !g_ps) return false;
    const void *keys[3] = { CFSTR("IsCharging"), CFSTR("PredictiveChargingInhibit"), CFSTR("ExternalConnected") };
    const void *vals[3] = { charging ? kCFBooleanTrue : kCFBooleanFalse, kCFBooleanFalse,
                            external ? kCFBooleanTrue : kCFBooleanFalse };
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    kern_return_t kr = g_set_props(g_ps, d);
    CFRelease(d);
    if (kr != KERN_SUCCESS) lg("写入电源标志失败 0x%x", kr);
    return kr == KERN_SUCCESS;
}

// ---------------------------------------------------------------- 主循环

static void tick(void) {
    g_ticks++;
    CFPreferencesSynchronize(APP_ID, USER_MOBILE, kCFPreferencesAnyHost);

    if (!g_ps) g_ps = find_power_source();
    batt_t b;
    if (!read_batt(&b)) {
        if (g_ps) { IOObjectRelease(g_ps); g_ps = 0; }   // 下一轮重新查找
        return;
    }

    bool enabled = pref_int("enabled", 0) != 0;
    cl_params p = cl_make_params(pref_int("upper", 50), pref_int("hysteresis", 5), pref_int("drain", 0) != 0);

    if (g_ticks % HEARTBEAT_TICKS == 1) {
        lg("心跳 电量=%d%% 外部电源=%d 充电标志=%d 电流=%dmA 电压=%dmV 启用=%d 上限=%d 恢复线=%d 放电=%d",
           b.level, b.ext, b.charging, b.amp, b.mv, enabled, p.upper, p.lower, p.drain);
    }

    if (!enabled) {
        if (g_applied != APPLIED_NONE) {
            write_flags(true, true);
            lg("已关闭，恢复默认充电（电量 %d%%）", b.level);
            g_applied = APPLIED_NONE;
        }
        memset(&g_state, 0, sizeof g_state);
        g_bad_ticks = 0;
        set_status("未启用 · 电量 %d%%", b.level);
        return;
    }

    cl_mode want = cl_decide(&p, &g_state, b.level);

    if (!b.ext) {
        // 没有外部电源时不写任何标志（不在拔线状态下伪造外部电源）；下次接电时重新下发。
        if (g_applied != APPLIED_UNKNOWN) lg("外部电源已断开（电量 %d%%）", b.level);
        g_applied = APPLIED_UNKNOWN;
        g_bad_ticks = 0;
        set_status("未接电源 · 电量 %d%%", b.level);
        return;
    }

    switch (want) {
    case CL_ALLOW:
        if (g_applied != APPLIED_ALLOW) {
            write_flags(true, true);
            lg("→ 允许充电（电量 %d%%，上限 %d%%，恢复线 %d%%）", b.level, p.upper, p.lower);
            g_applied = APPLIED_ALLOW;
        }
        g_bad_ticks = 0;
        g_warned = false;
        g_hold_confirmed = false;
        if (b.charging) set_status("充电中 · 电量 %d%%，到 %d%% 停充", b.level, p.upper);
        else set_status("允许充电 · 电量 %d%%（上限 %d%%）", b.level, p.upper);
        break;

    case CL_HOLD:
        if (g_applied != APPLIED_HOLD || b.charging) {
            if (g_applied != APPLIED_HOLD) {
                lg("→ 停充保持（电量 %d%%，上限 %d%%，恢复线 %d%%）", b.level, p.upper, p.lower);
                g_warned = false;
                g_hold_confirmed = false;
                g_bad_ticks = 0;
            }
            write_flags(false, true);
            g_applied = APPLIED_HOLD;
        } else if (!g_hold_confirmed && b.amp <= 20) {
            lg("停充已生效（实时电流 %dmA）", b.amp);
            g_hold_confirmed = true;
        }
        // 自检：请求停充后若仍持续大电流充电，说明这块电池/主板没有听从
        if (b.charging && b.amp > WARN_AMP_MA) g_bad_ticks++; else g_bad_ticks = 0;
        if (g_bad_ticks >= WARN_TICKS && !g_warned) {
            lg("警告：已请求停充，但实时电流仍有 %dmA", b.amp);
            g_warned = true;
        }
        if (g_warned && g_bad_ticks >= WARN_TICKS) set_status("⚠ 已请求停充但仍在充电 · 电量 %d%%", b.level);
        else set_status("已停充并保持 · 电量 %d%%（降到 %d%% 以下恢复）", b.level, p.lower);
        break;

    case CL_DRAIN:
        if (g_applied != APPLIED_DRAIN) {
            lg("→ 放电（电量 %d%% 高于上限 %d%%，改由电池供电）", b.level, p.upper);
            g_applied = APPLIED_DRAIN;
        }
        write_flags(false, false);   // 每轮重申，成本很低
        g_bad_ticks = 0;
        set_status("放电中 · 电量 %d%% → %d%%", b.level, p.upper);
        break;
    }
}

static void restore_and_exit(const char *why) {
    if (g_applied == APPLIED_HOLD || g_applied == APPLIED_DRAIN) {
        write_flags(true, true);
        lg("%s：已恢复默认充电状态", why);
    } else {
        lg("%s", why);
    }
    set_status("守护进程未运行");
    exit(0);
}

// ---------------------------------------------------------------- 命令行

static void cli_status(void) {
    CFPreferencesSynchronize(APP_ID, USER_MOBILE, kCFPreferencesAnyHost);
    cl_params p = cl_make_params(pref_int("upper", 50), pref_int("hysteresis", 5), pref_int("drain", 0) != 0);
    printf("设置: 启用=%d 上限=%d%% 恢复线=%d%% 放电=%d\n", pref_int("enabled", 0), p.upper, p.lower, p.drain);
    batt_t b;
    if (read_batt(&b))
        printf("电池: 电量=%d%% 外部电源=%d 充电标志=%d 实时电流=%dmA 电压=%dmV\n", b.level, b.ext, b.charging, b.amp, b.mv);
    else
        printf("电池: 读取失败\n");
    CFTypeRef st = pref_copy("statusText");
    char buf[200] = "";
    if (st && CFGetTypeID(st) == CFStringGetTypeID()) CFStringGetCString(st, buf, sizeof buf, kCFStringEncodingUTF8);
    if (st) CFRelease(st);
    printf("状态: %s\n", buf[0] ? buf : "(无)");
}

static int cli_set(const char *key, const char *value) {
    static const char *allowed[] = { "enabled", "upper", "hysteresis", "drain" };
    bool ok = false;
    for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++) ok |= !strcmp(key, allowed[i]);
    if (!ok) { fprintf(stderr, "只能设置: enabled upper hysteresis drain\n"); return 1; }
    int n = atoi(value);
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &n);
    pref_set(key, num);
    CFRelease(num);
    notify_post(NOTIFY_CHANGED);
    printf("%s = %d\n", key, n);
    return 0;
}

int main(int argc, char **argv) {
    g_set_props = (set_props_fn)dlsym(RTLD_DEFAULT, "IORegistryEntrySetCFProperties");

    if (argc >= 2) {
        if (!strcmp(argv[1], "--version")) { printf("chargelimitd %s\n", CL_VERSION); return 0; }
        if (!strcmp(argv[1], "--status")) { g_ps = find_power_source(); cli_status(); return 0; }
        if (!strcmp(argv[1], "--set") && argc >= 4) return cli_set(argv[2], argv[3]);
        if (!strcmp(argv[1], "--reset")) {   // 卸载前由 prerm 调用：无条件恢复默认
            g_ps = find_power_source();
            bool ok = write_flags(true, true);
            printf("恢复默认充电状态: %s\n", ok ? "成功" : "失败");
            return ok ? 0 : 1;
        }
        fprintf(stderr, "用法: chargelimitd [--version | --status | --set <key> <整数> | --reset]\n");
        return 1;
    }

    lg("守护进程启动 v%s", CL_VERSION);
    dispatch_queue_t q = dispatch_get_main_queue();

    // 设置页改了值会发这个通知；偏好写盘有延迟，稍等一下再读
    int token;
    notify_register_dispatch(NOTIFY_CHANGED, &token, q, ^(int t) {
        (void)t;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), q, ^{ tick(); });
    });

    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), POLL_SECONDS * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(timer, ^{ tick(); });
    dispatch_resume(timer);

    int sigs[] = { SIGTERM, SIGINT };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        signal(sigs[i], SIG_IGN);
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0, q);
        dispatch_source_set_event_handler(src, ^{ restore_and_exit("收到终止信号"); });
        dispatch_resume(src);
    }

    dispatch_main();
}
