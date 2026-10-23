// 验证：root 进程能不能通过 CFPreferences 写入 mobile 用户的偏好域，且 mobile 进程能读到。
//   prefs_test set <key> <字符串>   以 root 身份写入 mobile 的域
//   prefs_test get <key>            以 root 身份读 mobile 的域
//   prefs_test getme <key>          以当前用户身份读（用 mobile 登录时代表系统设置读到的值）
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

#define APP CFSTR("io.github.francisngg.chargelimit.test")

static void print_value(CFTypeRef v) {
    if (!v) { printf("(空)\n"); return; }
    char buf[256] = "";
    if (CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
    printf("%s\n", buf);
    CFRelease(v);
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    CFStringRef key = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
    if (!strcmp(argv[1], "set") && argc >= 4) {
        CFStringRef val = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
        CFPreferencesSetValue(key, val, APP, CFSTR("mobile"), kCFPreferencesAnyHost);
        Boolean ok = CFPreferencesSynchronize(APP, CFSTR("mobile"), kCFPreferencesAnyHost);
        printf("写入并同步: %s\n", ok ? "成功" : "失败");
    } else if (!strcmp(argv[1], "get")) {
        CFPreferencesSynchronize(APP, CFSTR("mobile"), kCFPreferencesAnyHost);
        print_value(CFPreferencesCopyValue(key, APP, CFSTR("mobile"), kCFPreferencesAnyHost));
    } else if (!strcmp(argv[1], "getme")) {
        CFPreferencesAppSynchronize(APP);
        print_value(CFPreferencesCopyAppValue(key, APP));
    }
    return 0;
}
