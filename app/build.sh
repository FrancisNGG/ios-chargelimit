#!/bin/bash
# 编译 ChargeLimit.app 并打成 ipa（临时签名 + 无沙盒/无容器权限，需要越狱设备装有 AppSync Unified）。
#   ./app/build.sh            只构建
#   ./app/build.sh install    构建后通过 USB 安装到已连接的设备
# 无容器权限的作用：让本 App 与「设置」写的偏好落在 /var/mobile/Library/Preferences/，
# 这样以 root 运行的守护进程才能读到。
set -euo pipefail
cd "$(dirname "$0")"

export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
APP=build/Payload/ChargeLimit.app

rm -rf build
mkdir -p "$APP"

xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=12.2 \
    -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" \
    -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter \
    -framework UIKit -framework Foundation \
    main.m -o "$APP/ChargeLimit"

cp Info.plist "$APP/Info.plist"
plutil -convert binary1 "$APP/Info.plist"
cp -R Settings.bundle "$APP/"
cp icons/*.png "$APP/"
codesign -f -s - --entitlements entitlements.plist --timestamp=none "$APP"

(cd build && zip -qr ChargeLimit.ipa Payload)
echo "已生成 app/build/ChargeLimit.ipa"

if [[ "${1:-}" == "install" ]]; then
    ideviceinstaller install build/ChargeLimit.ipa
fi
