#!/bin/bash
# 编译并打包 ChargeLimit 守护进程。配套的设置页 App 见 app/build.sh（必须是用户 App，不能放进 deb，见 README）。
#   ./build.sh            单元测试 + 编译 + 打包成 build/*.deb
#   ./build.sh install    再通过 SSH（别名 ipad，需先运行 iproxy 2222 22）安装到越狱 iPad
# 签名要 ldid、打包要 dpkg-deb：Mac 上没有就借用越狱 iPad 上自带的。
set -euo pipefail
cd "$(dirname "$0")"

export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
PKG=io.github.francisngg.chargelimit
VERSION=$(sed -n 's/^Version: //p' DEBIAN/control)
DEB=build/${PKG}_${VERSION}_iphoneos-arm.deb
ROOT=build/root

rm -rf build
mkdir -p "$ROOT/usr/libexec"

echo "== 单元测试 =="
xcrun --sdk macosx clang -Wall -Wextra -I src tools/test_logic.c -o build/test_logic
build/test_logic | tail -1

echo "== 编译 =="
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=12.2 \
    -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" \
    -O2 -Wall -Wextra -framework CoreFoundation -framework IOKit \
    src/chargelimitd.c -o "$ROOT/usr/libexec/chargelimitd"

cp -R layout/Library "$ROOT/"
mkdir -p "$ROOT/DEBIAN"
cp DEBIAN/control DEBIAN/postinst DEBIAN/prerm "$ROOT/DEBIAN/"
chmod 755 "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/prerm" "$ROOT/usr/libexec/chargelimitd"
find "$ROOT" -name .DS_Store -delete

echo "== 签名并打包 =="
if command -v ldid >/dev/null && command -v dpkg-deb >/dev/null; then
    ldid -Ssrc/entitlements.plist "$ROOT/usr/libexec/chargelimitd"
    dpkg-deb -Zgzip -b "$ROOT" "$DEB"
else
    REMOTE=/tmp/chargelimit-build
    ssh ipad "rm -rf $REMOTE && mkdir -p $REMOTE"
    COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT" -cf - . | ssh ipad "tar --no-same-owner -C $REMOTE -xf -"
    scp -q src/entitlements.plist ipad:/tmp/chargelimit-ents.plist
    ssh ipad "chown -R 0:0 $REMOTE && ldid -S/tmp/chargelimit-ents.plist $REMOTE/usr/libexec/chargelimitd && dpkg-deb -Zgzip -b $REMOTE /tmp/chargelimit.deb"
    scp -q ipad:/tmp/chargelimit.deb "$DEB"
fi
echo "已生成 $DEB"

if [[ "${1:-}" == "install" ]]; then
    echo "== 安装到 iPad =="
    scp -q "$DEB" ipad:/tmp/chargelimit.deb
    ssh ipad "apt-get install -y --allow-unauthenticated --reinstall /tmp/chargelimit.deb"
fi
