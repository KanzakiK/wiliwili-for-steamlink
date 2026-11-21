#!/bin/sh
# 生成两种 Steam Link 包：
#   - tar.gz：SSH/手动部署，顶层为 wiliwili/，解压到 /home/apps/
#   - zip：U 盘自动安装，顶层为 steamlink/apps/wiliwili/，解压到 FAT32 U 盘根目录
set -e

PROJECT_DIR=${PROJECT_DIR:-/mnt/d/bilibili-steamlink}
HERE=$(cd "$(dirname "$0")" && pwd)
WILI_BUILD=${WILI_BUILD:-$HOME/build-wiliwili}
PLAYER_BUILD=${PLAYER_BUILD:-$HOME/build-slplayer}
OUT_DIR=${OUT_DIR:-$PROJECT_DIR/dist}
VERSION=${VERSION:-$(git -C "$PROJECT_DIR" describe --tags --always --dirty 2>/dev/null || echo dev)}
COMMIT=${COMMIT:-$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)}
PKG_NAME=wiliwili-steamlink-${VERSION}

for f in "$WILI_BUILD/wiliwili" "$PLAYER_BUILD/slplayer_daemon"; do
    [ -f "$f" ] || { echo "缺少产物: $f" >&2; exit 1; }
done
[ -d "$WILI_BUILD/resources" ] || { echo "缺少资源目录: $WILI_BUILD/resources" >&2; exit 1; }

STAGE=$(mktemp -d)
APP_DIR="$STAGE/wiliwili"
mkdir -p "$APP_DIR/bin"

populate_app_dir() {
    target=$1
    mkdir -p "$target/bin"
    cp "$HERE/toc.txt" "$HERE/wiliwili.sh" "$HERE/wiliwili.png" "$HERE/INSTALL.txt" "$target/"
    cp "$PROJECT_DIR/LICENSE" "$target/LICENSE"
    cp "$WILI_BUILD/wiliwili" "$target/bin/wiliwili"
    cp "$PLAYER_BUILD/slplayer_daemon" "$target/bin/slplayer_daemon"
    cp -R "$WILI_BUILD/resources" "$target/resources"
    printf 'version=%s\ncommit=%s\nbuilt=%s\n' "$VERSION" "$COMMIT" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$target/VERSION.txt"
    chmod 755 "$target/wiliwili.sh" "$target/bin/wiliwili" "$target/bin/slplayer_daemon"
}

populate_app_dir "$APP_DIR"

mkdir -p "$OUT_DIR"
tar -czf "$OUT_DIR/$PKG_NAME.tar.gz" -C "$STAGE" wiliwili
rm -rf "$STAGE"

echo "package: $OUT_DIR/$PKG_NAME.tar.gz"

USB_PKG_NAME=wiliwili-steamlink-usb-${VERSION}
command -v zip >/dev/null 2>&1 || { echo "缺少 zip：无法生成 U 盘自动安装包" >&2; exit 1; }
USB_STAGE=$(mktemp -d)
populate_app_dir "$USB_STAGE/steamlink/apps/wiliwili"
(cd "$USB_STAGE" && zip -qr "$OUT_DIR/$USB_PKG_NAME.zip" steamlink)
rm -rf "$USB_STAGE"
echo "package: $OUT_DIR/$USB_PKG_NAME.zip"
