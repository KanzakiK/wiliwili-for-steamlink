#!/bin/sh
# 生成 Steam Link 标准 app 包：解压 wiliwili 目录到 /home/apps/ 即可使用。
set -e

PROJECT_DIR=${PROJECT_DIR:-/mnt/d/bilibili-steamlink}
HERE=$(cd "$(dirname "$0")" && pwd)
WILI_BUILD=${WILI_BUILD:-$HOME/build-wiliwili}
PLAYER_BUILD=${PLAYER_BUILD:-$HOME/build-slplayer}
OUT_DIR=${OUT_DIR:-$PROJECT_DIR/dist}
VERSION=${VERSION:-$(git -C "$PROJECT_DIR" describe --tags --always --dirty 2>/dev/null || echo dev)}
COMMIT=$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
PKG_NAME=wiliwili-steamlink-${VERSION}

for f in "$WILI_BUILD/wiliwili" "$PLAYER_BUILD/slplayer_daemon"; do
    [ -f "$f" ] || { echo "缺少产物: $f" >&2; exit 1; }
done
[ -d "$WILI_BUILD/resources" ] || { echo "缺少资源目录: $WILI_BUILD/resources" >&2; exit 1; }

STAGE=$(mktemp -d)
APP_DIR="$STAGE/$PKG_NAME/wiliwili"
mkdir -p "$APP_DIR/bin"

cp "$HERE/toc.txt" "$HERE/wiliwili.sh" "$HERE/wiliwili.png" "$HERE/INSTALL.txt" "$APP_DIR/"
cp "$PROJECT_DIR/LICENSE" "$APP_DIR/LICENSE"
cp "$WILI_BUILD/wiliwili" "$APP_DIR/bin/wiliwili"
cp "$PLAYER_BUILD/slplayer_daemon" "$APP_DIR/bin/slplayer_daemon"
cp -R "$WILI_BUILD/resources" "$APP_DIR/resources"
printf 'version=%s\ncommit=%s\nbuilt=%s\n' "$VERSION" "$COMMIT" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$APP_DIR/VERSION.txt"
chmod 755 "$APP_DIR/wiliwili.sh" "$APP_DIR/bin/wiliwili" "$APP_DIR/bin/slplayer_daemon"

mkdir -p "$OUT_DIR"
tar -czf "$OUT_DIR/$PKG_NAME.tar.gz" -C "$STAGE" "$PKG_NAME"
if command -v zip >/dev/null 2>&1; then
    (cd "$STAGE" && zip -qr "$OUT_DIR/$PKG_NAME.zip" "$PKG_NAME")
fi
rm -rf "$STAGE"

echo "package: $OUT_DIR/$PKG_NAME.tar.gz"
if [ -f "$OUT_DIR/$PKG_NAME.zip" ]; then
    echo "package: $OUT_DIR/$PKG_NAME.zip"
fi
