// 探测工具：验证往电源注册表写标志位，能不能真的控制这块电池的充电。
//   probe show              打印当前电池状态
//   probe toggle <秒>        先强制开充一半时间，再强制停充一半时间，观察电流是否跟着变
//   probe inflow <秒>        把 ExternalConnected 置 NO（模拟拔电）<秒> 秒，观察是否改由电池供电
// 任何情况下退出（含被信号打断、SSH 断线）都会恢复默认状态。
// 不属于最终产品，只用来在真机上确认硬件是否支持。
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static io_service_t g_ps = 0;

// SDK 头文件把它标成 iOS 不可用，但设备上的 IOKit 里有这个函数，运行时取地址
typedef kern_return_t (*set_props_fn)(io_registry_entry_t, CFTypeRef);
static set_props_fn p_set_props;

static io_service_t find_power_source(void) {
    io_service_t s = IOServiceGetMatchingService(0, IOServiceMatching("AppleARMPMUCharger"));
    if (!s) s = IOServiceGetMatchingService(0, IOServiceMatching("IOPMPowerSource"));
    return s;
}

static long get_num(CFDictionaryRef d, const char *key) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, k);
    CFRelease(k);
    long out = -999999;
    if (!v) return out;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberLongType, &out);
    else if (CFGetTypeID(v) == CFBooleanGetTypeID()) out = CFBooleanGetValue(v) ? 1 : 0;
    return out;
}

static void show(const char *tag) {
    CFMutableDictionaryRef props = NULL;
    if (IORegistryEntryCreateCFProperties(g_ps, &props, NULL, 0) != KERN_SUCCESS || !props) {
        printf("%s 读取失败\n", tag);
        return;
    }
    printf("%-9s 电量=%ld%% 充电中=%ld 外部电源=%ld 已充满=%ld 实时电流=%ld mA 电压=%ld mV\n",
           tag, get_num(props, "CurrentCapacity"), get_num(props, "IsCharging"),
           get_num(props, "ExternalConnected"), get_num(props, "FullyCharged"),
           get_num(props, "InstantAmperage"), get_num(props, "Voltage"));
    fflush(stdout);
    CFRelease(props);
}

static kern_return_t set_flags(int is_charging, int external) {
    const void *keys[3] = { CFSTR("IsCharging"), CFSTR("PredictiveChargingInhibit"), CFSTR("ExternalConnected") };
    const void *vals[3] = { is_charging ? kCFBooleanTrue : kCFBooleanFalse, kCFBooleanFalse,
                            external ? kCFBooleanTrue : kCFBooleanFalse };
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    kern_return_t kr = p_set_props ? p_set_props(g_ps, d) : KERN_FAILURE;
    CFRelease(d);
    return kr;
}

static void restore(void) {
    if (g_ps) set_flags(1, 1);   // 默认状态：允许充电、外部电源已连接
}

static void on_signal(int sig) {
    restore();
    _exit(128 + sig);
}

static void watch(const char *prefix, int from, int to) {
    for (int t = from; t <= to; t += 2) {
        sleep(2);
        char tag[24];
        snprintf(tag, sizeof tag, "%s+%ds", prefix, t);
        show(tag);
    }
}

int main(int argc, char **argv) {
    p_set_props = (set_props_fn)dlsym(RTLD_DEFAULT, "IORegistryEntrySetCFProperties");
    g_ps = find_power_source();
    if (!g_ps) { printf("找不到电源服务\n"); return 2; }
    if (argc >= 2 && !strcmp(argv[1], "show")) { show("当前"); return 0; }
    if (argc < 3) { printf("用法: probe show | probe toggle <秒> | probe inflow <秒>\n"); return 1; }

    int secs = atoi(argv[2]);
    if (secs < 4) secs = 4;
    if (secs > 900) secs = 900;
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    atexit(restore);
    show("基线");
    kern_return_t kr;

    if (!strcmp(argv[1], "toggle")) {
        kr = set_flags(1, 1);
        printf("写入 IsCharging=YES → 0x%x\n", kr);
        watch("强制开充", 2, secs / 2);
        kr = set_flags(0, 1);
        printf("写入 IsCharging=NO  → 0x%x\n", kr);
        watch("强制停充", 2, secs / 2);
    } else if (!strcmp(argv[1], "inflow")) {
        kr = set_flags(1, 0);
        printf("写入 ExternalConnected=NO → 0x%x\n", kr);
        watch("模拟拔电", 2, secs);
    } else {
        printf("未知模式\n");
        return 1;
    }
    restore();
    printf("已恢复默认状态\n");
    watch("恢复", 2, 10);
    return 0;
}
