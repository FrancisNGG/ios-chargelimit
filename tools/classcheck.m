// 在真机上确认设置 App 的主列表控制器叫什么、父类链和关键方法/成员变量是否存在。
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>

static void describe(const char *name) {
    Class c = objc_getClass(name);
    if (!c) { printf("  %-28s 不存在\n", name); return; }
    printf("  %-28s 存在  父类链:", name);
    for (Class s = class_getSuperclass(c); s; s = class_getSuperclass(s)) printf(" %s", class_getName(s));
    printf("\n");
}

int main(void) {
    @autoreleasepool {
        void *h1 = dlopen("/System/Library/PrivateFrameworks/Preferences.framework/Preferences", RTLD_NOW);
        void *h2 = dlopen("/System/Library/PrivateFrameworks/PreferencesUI.framework/PreferencesUI", RTLD_NOW);
        printf("加载 Preferences=%s PreferencesUI=%s\n", h1 ? "成功" : "失败", h2 ? "成功" : "失败");

        printf("候选类:\n");
        const char *names[] = { "PrefsListController", "PSUIPrefsListController", "PSListController",
                                "PSRootController", "PSUIPrefsRootController", "PSSpecifier" };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) describe(names[i]);

        printf("名字里同时含 Prefs 和 ListController 的类:\n");
        unsigned n = 0;
        Class *all = objc_copyClassList(&n);
        for (unsigned i = 0; i < n; i++) {
            const char *cn = class_getName(all[i]);
            if (strstr(cn, "Prefs") && strstr(cn, "ListController")) printf("  %s\n", cn);
        }
        free(all);

        Class lc = objc_getClass("PSListController");
        if (lc) {
            printf("PSListController 的成员变量 _specifiers: %s\n", class_getInstanceVariable(lc, "_specifiers") ? "有" : "没有");
            const char *sels[] = { "specifiers", "getGroup:row:ofSpecifierID:", "indexOfGroup:", "specifiersInGroup:",
                                   "loadSpecifiersFromPlistName:target:", "lazyLoadBundle:", "controllerForSpecifier:" };
            for (size_t i = 0; i < sizeof sels / sizeof *sels; i++)
                printf("  -[PSListController %s]: %s\n", sels[i], class_getInstanceMethod(lc, sel_registerName(sels[i])) ? "有" : "没有");
        }
        Class sp = objc_getClass("PSSpecifier");
        if (sp) {
            const char *ivs[] = { "cellType", "detailControllerClass", "properties" };
            for (size_t i = 0; i < sizeof ivs / sizeof *ivs; i++)
                printf("  PSSpecifier 成员变量 %s: %s\n", ivs[i], class_getInstanceVariable(sp, ivs[i]) ? "有" : "没有");
        }
        const char *syms[] = { "SpecifiersFromPlist", "PSBundlePathKey", "PSLazilyLoadedBundleKey", "PSBundleIsControllerKey",
                               "PSActionKey", "PSTitleKey", "PSFooterTextGroupKey", "PSStaticTextGroupKey" };
        for (size_t i = 0; i < sizeof syms / sizeof *syms; i++)
            printf("  符号 %-26s %s\n", syms[i], dlsym(RTLD_DEFAULT, syms[i]) ? "有" : "没有");
        Class ui = objc_getClass("PSUIPrefsListController");
        if (ui) printf("PSUIPrefsListController 自己实现了 -specifiers: %s\n",
                       class_getInstanceMethod(ui, sel_registerName("specifiers")) ? "是（含继承）" : "否");
    }
    return 0;
}
