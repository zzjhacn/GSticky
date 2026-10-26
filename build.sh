#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="GSticky"
BUILD="$SCRIPT_DIR/build"
APP="$BUILD/$APP_NAME.app"
BIN="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"

# 图标源：优先 icon.png，兼容旧的 logo200.png / logo.png（任一存在即可）
LOGO_SRC=""
for cand in "$SCRIPT_DIR/icon.png" "$SCRIPT_DIR/logo200.png" "$SCRIPT_DIR/logo.png"; do
    if [ -f "$cand" ]; then LOGO_SRC="$cand"; break; fi
done

echo "==> [1/4] 用 swiftc 编译（AppKit，原生）"
swiftc -O -framework Cocoa -target arm64-apple-macosx13.0 \
    "$SCRIPT_DIR/Sources/main.swift" -o "$BUILD/$APP_NAME"

echo "==> [2/4] 组装 .app 包"
rm -rf "$APP"
mkdir -p "$BIN" "$RES"
cp "$BUILD/$APP_NAME" "$BIN/$APP_NAME"

# 打包图标：规范为包内 icon.png（菜单栏/应用图标共用），并生成 AppIcon.icns 供 Finder 使用
if [ -n "$LOGO_SRC" ]; then
    echo "    图标源: $LOGO_SRC"
    cp "$LOGO_SRC" "$RES/icon.png"
    # 菜单栏图标：去白底 + 裁边，避免 18×18 出现白方块
    if swift "$SCRIPT_DIR/tools/make_menubar_icon.swift" "$LOGO_SRC" "$RES/menubar.png" >/dev/null 2>&1; then
        echo "    已生成菜单栏图标 menubar.png"
    else
        cp "$LOGO_SRC" "$RES/menubar.png"
        echo "    ⚠️ 菜单栏图标处理失败，回退原图"
    fi
    ICONSET="$BUILD/AppIcon.iconset"
    rm -rf "$ICONSET"; mkdir -p "$ICONSET"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$LOGO_SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null 2>&1 || true
        d=$((s * 2))
        sips -z $d $d "$LOGO_SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null 2>&1 || true
    done
    if iconutil -c icns "$ICONSET" -o "$RES/AppIcon.icns" >/dev/null 2>&1; then
        echo "    已生成 AppIcon.icns"
    else
        echo "    ⚠️ icns 生成失败（Finder 图标可能不显示，不影响运行）"
    fi
    rm -rf "$ICONSET"
else
    echo "    ⚠️ 未找到图标文件（icon.png / logo200.png / logo.png），跳过"
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>GSticky</string>
    <key>CFBundleDisplayName</key><string>GSticky</string>
    <key>CFBundleExecutable</key><string>GSticky</string>
    <key>CFBundleIdentifier</key><string>com.gsticky.app</string>
    <key>CFBundleVersion</key><string>1.0</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>LSUIElement</key><true/>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "==> [3/4] ad-hoc 签名 + 去除隔离属性"
codesign --force --deep --sign - "$APP"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true

# 刷新 Finder / LaunchServices 图标缓存，避免反复重建后 Finder 仍显示默认/旧图标
touch "$APP"
LSREG="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
if [ -x "$LSREG" ]; then
    "$LSREG" -f "$APP" >/dev/null 2>&1 || true
    echo "    已刷新图标缓存 (lsregister -f)"
fi

echo "==> [4/4] 启动 GSticky"
open "$APP"
echo "完成。应用位置: $APP"
