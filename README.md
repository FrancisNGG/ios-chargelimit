# ChargeLimit（iOS 12 越狱设备充电上限）

电量达到上限就停止充电并保持，降到「恢复线」以下才重新充电；可选地在电量高于上限时改由电池供电、放电到上限。设置页集成在系统「设置」里。

在 iPad Air 一代（iPad4,1，A7）、iOS 12.5.8、checkra1n 上开发和验证。其他设备能否停充取决于电池和电源管理芯片，见「兼容性」。

## 组成

| 部分 | 作用 | 安装方式 |
|---|---|---|
| **守护进程** `chargelimitd`（deb） | 每 10 秒读电量，决定允许充电 / 停充保持 / 放电，写电源标志。以 root 常驻，由 launchd 开机自启、崩溃自动拉起 | `./build.sh install` |
| **设置页 App**（IPA） | 带一份 `Settings.bundle`，让「设置」最底部出现「充电限制」页；App 首页只显示守护进程状态 | `./app/build.sh install` |

两者通过同一个偏好文件通信：`/var/mobile/Library/Preferences/io.github.francisngg.chargelimit.plist`。设置页写 `enabled`、`upper`、`hysteresis`、`drain`；守护进程写 `statusText`、`statusTime`。

## 为什么是「deb + IPA」两件，而不是一个包

- 限制要一直在后台盯着，App 在后台或被划掉就停了，所以必须有守护进程。
- 往系统「设置」里加页面只能靠 App 自带的 `Settings.bundle`。
- **「设置」只认用户安装的 App 的设置页。**把 App 放进 deb、装到 `/Applications`（系统级）后，设置页不出现，已在真机上验证过。
- 系统级 App 才能用 `SBAppTags=hidden` 隐藏桌面图标，用户 App 上这个标签无效（也已验证），所以图标无法隐藏。两个需求在 iOS 12 上互相冲突。
- 没有用 PreferenceLoader：它在 BigBoss 源里，源连不上；老版源码钩的 `PrefsListController` 在 iOS 12 上也已不存在（主列表控制器是 `PSUIPrefsListController`）。

IPA 需要「无沙盒、无容器」权限（`app/entitlements.plist`），让它的偏好落在 `/var/mobile/Library/Preferences/`，守护进程才读得到。它靠 AppSync Unified 安装（越狱设备上装）。

## 使用

1. `./build.sh install` 装守护进程（需要先 `iproxy 2222 22`，SSH 别名 `ipad`）。
2. `./app/build.sh install` 装设置页 App（USB 连接）。
3. 「设置」→ 最底部「充电限制」：打开「启用充电限制」，选「停充上限」和「恢复线回差」。
4. 改完设置最多约 10 秒生效；「当前」状态行要退出再进入页面才会刷新。

默认是**关闭**状态，装完不会改变任何充电行为。

### 命令行

```
chargelimitd --status                 打印设置、电池和状态
chargelimitd --set <key> <整数>       key: enabled | upper | hysteresis | drain
chargelimitd --reset                  无条件恢复默认充电状态
```

日志：`/var/log/chargelimit.log`（含状态切换，以及停充后的自检结果）。

## 决策逻辑

见 `src/logic.h`（纯函数，`tools/test_logic.c` 在 Mac 上做单元测试）：

- 电量 ≥ 上限 → 停充保持；电量 ≤ 上限 − 回差 → 允许充电；两者之间维持原状态（滞回）。
- 电量 ≤ 20% 时，无论怎么设置都允许充电（防止把电池放空）。
- 开启「放电」后，电量 **高于**上限时断开外部供电，由电池供电，降到上限再转为保持。
- 没有外部电源时不写任何标志。

## 硬件层的实测结论

- 往电源注册表（`AppleARMPMUCharger`）写 `IsCharging=NO`，实时电流在几秒内降到 0 mA；写回 `YES` 后恢复。
- 写 `ExternalConnected=NO` 后设备改由电池供电（开着屏幕约 −1.3 到 −1.8 A）。读回的 `ExternalConnected` 不变，仍反映真实的接电状态。USB 数据链路不受影响（OpenDisplay 在放电期间保持连接）。
- `IORegistryEntrySetCFProperties` 在 SDK 头文件里被标成 iOS 不可用，但设备上存在，运行时用 `dlsym` 取。
- 实时电流字段大约 20 秒才刷新一次；电池在 100% 附近会自己「补一阵、停一阵」，所以在满电附近验证停充容易被自然脉冲干扰。
- 可执行文件必须带权限声明签名（`ldid -S<ents>`），否则直接 `Killed: 9`。所需权限见 `src/entitlements.plist`。

## 构建

需要 Xcode（iOS SDK）。签名要 `ldid`、打包要 `dpkg-deb`：Mac 上没有时，`build.sh` 借用已连接的越狱 iPad 上自带的。

```
./build.sh              单元测试 + 编译 + 打包到 build/*.deb
./app/build.sh          编译 App 并打成 app/build/ChargeLimit.ipa
python3 tools/make_icon.py app/icons    重新生成图标
```

`tools/` 下是开发时的验证工具，都不属于产品：`probe.c` 验证硬件（`probe toggle|inflow <秒>`，任何退出方式都会恢复默认），`prefs_test.c` 验证 root 进程能写入 mobile 用户的偏好域，`classcheck.m` 确认设置 App 里的类名和私有接口。它们需要带权限声明签名后在设备上运行，用法见各文件开头的注释。

## 已知限制

- **重启 iPad 后需要重新越狱**（checkra1n 是半不完美越狱），守护进程才会运行；这期间不会限制充电，也没有副作用，充电回到系统默认行为。
- 「放电」模式下功耗大，从 100% 降到 50% 需要好几个小时；显示的电量在最顶上一小段下降得很慢。
- 「当前」状态行不会实时刷新；设置页改动要等下一次轮询（≤10 秒）。
- 桌面上有一个「充电限制」图标，无法隐藏（见上）。
- 能不能停充取决于电池和电源芯片。判断办法：先在 100% 以外的电量下用 `probe toggle 120` 观察电流是否跟着变化。
- 日志只在状态切换时记录，加上每 5 分钟一行心跳。

## 兼容性

只在 iPad4,1 / iOS 12.5.8 上验证过。原理（写 `IsCharging` 等标志位）与 [lich4/ChargeLimiter](https://github.com/lich4/ChargeLimiter) 相同，该项目声称支持 iOS 12 到 17；本项目代码为独立实现，没有复制它的内容。
