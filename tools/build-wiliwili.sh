#!/bin/sh
# 交叉编译 wiliwili 整仓。
#
# 正式构建：sh tools/build-wiliwili.sh
#   - SOURCE 默认就是本仓库根目录，正式包不包含 dev hook patch。
#   - 11 个 wiliwili 正式修改已内源化，构建脚本只检查标记，不再应用 patch。
#   - 2 个 borealis patch 必须在子模块上严格应用；缺失、失配或应用失败立即停止。
#
# Dev 构建：SL_DEV_BUILD=1 sh tools/build-wiliwili.sh
#   额外严格应用 6 个 dev patch（--open-bv、P0 seek ×2、SL_MEDIA_TAG、
#   debug session、debug controller）。这些 patch 会修改 SOURCE 工作树，
#   建议在 SOURCE_DIR 指向的临时 worktree 中使用。
#
# 产物：~/build-wiliwili/wiliwili（资源在 ~/build-wiliwili/resources）
set -e

export PATH=$HOME/steamlink-sdk/toolchain/bin:$PATH
export LD_LIBRARY_PATH=$HOME/steamlink-sdk/toolchain/lib:$LD_LIBRARY_PATH
export CC=armv7a-cros-linux-gnueabi-gcc
export CXX=armv7a-cros-linux-gnueabi-g++
export MARVELL_SDK_PATH=${MARVELL_SDK_PATH:-$HOME/steamlink-sdk}
export MARVELL_ROOTFS=${MARVELL_ROOTFS:-$HOME/steamlink-sdk/rootfs}

PROJECT_DIR=${PROJECT_DIR:-/mnt/d/bilibili-steamlink}
PATCH_DIR=$PROJECT_DIR/tools/patches
STUB=$HOME/mpv-stub
STUB_SRC=$PROJECT_DIR/tools/mpv-stub/mpv_stub.c
SOURCE=${SOURCE_DIR:-$PROJECT_DIR}
BUILD=${BUILD_DIR:-$HOME/build-wiliwili}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

require_marker() {
    file=$1
    marker=$2
    label=$3
    [ -f "$file" ] || die "$label: 文件不存在 $file"
    grep -q "$marker" "$file" || die "$label: 缺少内源化标记 '$marker' ($file)"
    echo "ok: $label"
}

apply_strict() {
    patch=$1
    dir=$2
    label=$3
    [ -f "$patch" ] || die "$label: patch 不存在 $patch"
    [ -d "$dir" ] || die "$label: 源码目录不存在 $dir"
    if git -C "$dir" apply --reverse --check --recount "$patch" >/dev/null 2>&1; then
        echo "already applied: $label"
        return
    fi
    git -C "$dir" apply --check --recount "$patch" || die "$label: patch 失配 $patch"
    git -C "$dir" apply --recount "$patch" || die "$label: patch 应用失败 $patch"
    echo "applied: $label"
}

# 1) mpv 头文件（v0.37.0）
mkdir -p "$STUB/include/mpv" "$STUB/lib" "$STUB/build"
for f in client.h render.h render_gl.h; do
    if [ ! -f "$STUB/include/mpv/$f" ]; then
        echo "download mpv/$f"
        curl -sSLf -o "$STUB/include/mpv/$f" \
            "https://raw.githubusercontent.com/mpv-player/mpv/v0.37.0/libmpv/$f"
    fi
done

# 2) libmpv 兼容层静态库（IPC 客户端，不链进正式 wiliwili 以外的进程）
cp -f "$PROJECT_DIR/player/include/IpcProtocol.h" "$STUB/include/IpcProtocol.h"
if [ ! -f "$STUB/lib/libmpv.a" ] || [ "$STUB_SRC" -nt "$STUB/lib/libmpv.a" ] \
   || [ "$PROJECT_DIR/player/include/IpcProtocol.h" -nt "$STUB/lib/libmpv.a" ]; then
    echo "build mpv compat layer"
    armv7a-cros-linux-gnueabi-gcc -O2 -fPIC -marm -mfloat-abi=hard -D_DEFAULT_SOURCE \
        --sysroot="$MARVELL_ROOTFS" -I"$STUB/include" \
        -c "$STUB_SRC" -o "$STUB/build/mpv_stub.o" -pthread
    armv7a-cros-linux-gnueabi-ar rcs "$STUB/lib/libmpv.a" "$STUB/build/mpv_stub.o"
fi

# 3) 源码来源：默认就是本仓库。clean clone 缺子模块时自动初始化。
[ -f "$SOURCE/CMakeLists.txt" ] || die "源码目录缺少 CMakeLists.txt: $SOURCE"
if [ "$SOURCE" = "$PROJECT_DIR" ] && \
   [ ! -f "$SOURCE/library/borealis/library/cmake/commonOption.cmake" ]; then
    echo "init submodules"
    git -C "$SOURCE" submodule update --init --recursive
fi
[ -f "$SOURCE/library/borealis/library/cmake/commonOption.cmake" ] || \
    die "borealis 子模块缺失: $SOURCE/library/borealis"

# 4) 正式修改已内源化：严格检查标记，避免误用未内源化的旧源码树。
require_marker "$SOURCE/wiliwili/source/view/mpv_core.cpp" 'slSafeUrlForLog' 'debug log redaction'
require_marker "$SOURCE/wiliwili/include/utils/image_helper.hpp" '"@320w_180h_1c"' 'image size'
require_marker "$SOURCE/wiliwili/source/utils/config_helper.cpp" 'WILI_VIDEO_QUALITY_LANDSCAPE_MAX 116' 'capability caps'
require_marker "$SOURCE/wiliwili/include/view/video_view.hpp" 'aliveToken' 'VideoView UAF fix'
require_marker "$SOURCE/wiliwili/source/view/video_view.cpp" 'SteamLinkHideSpeed' 'hide speed button'
require_marker "$SOURCE/wiliwili/include/api/bilibili/result/dynamic_video.h" 'std::string mid;' 'dynamic mid string'
require_marker "$SOURCE/wiliwili/source/view/video_view.cpp" 'SteamLinkBottomBarGuard' 'bottom bar NaN guard'
require_marker "$SOURCE/wiliwili/source/utils/config_helper.cpp" 'SteamLinkDefaults' 'Steam Link defaults'
require_marker "$SOURCE/wiliwili/source/view/video_snapshot_core.cpp" 'SteamLinkSnapshotDownscale' 'snapshot downscale'
require_marker "$SOURCE/wiliwili/source/utils/config_helper.cpp" 'SteamLinkDanmakuDefault' 'danmaku area default'
require_marker "$SOURCE/wiliwili/source/utils/config_helper.cpp" 'SteamLinkDanmakuFontDefault' 'danmaku font default'

# 5) borealis 是子模块，两张 Steam Link 修改仍以严格 patch 应用。
apply_strict "$PATCH_DIR/borealis-steamlink-alpha.patch" \
    "$SOURCE/library/borealis" 'borealis steamlink alpha'
apply_strict "$PATCH_DIR/borealis-nanovg-atlas-cap.patch" \
    "$SOURCE/library/borealis" 'borealis nanovg atlas cap'

# 6) dev hook：默认不应用，避免正式包携带测试入口。
if [ "${SL_DEV_BUILD:-0}" = "1" ]; then
    if [ "$SOURCE" = "$PROJECT_DIR" ]; then
        echo "WARNING: SL_DEV_BUILD=1 会修改仓库内源码，建议使用 SOURCE_DIR=<worktree>" >&2
    fi
    for p in wiliwili-open-bv-test.patch \
             wiliwili-p0-seek-test.patch \
             wiliwili-p0-seek-test-absolute.patch \
             wiliwili-media-tag.patch \
             wiliwili-debug-session.patch \
             wiliwili-debug-controller.patch; do
        apply_strict "$PATCH_DIR/$p" "$SOURCE" "$p"
    done
else
    echo "dev hooks disabled (set SL_DEV_BUILD=1 to enable)"
fi

# 7) wiliwili 整仓
# -U OPENSSL_*：让 FindOpenSSL.cmake（tools/cmake）重新生效，避免 sysroot 绝对路径进入 DT_NEEDED
cmake -S "$SOURCE" -B "$BUILD" \
    -U "OPENSSL_*" \
    -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/tools/cmake/steamlink-wiliwili-toolchain.cmake" \
    -DCMAKE_MODULE_PATH="$PROJECT_DIR/tools/cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPLATFORM_DESKTOP=ON \
    -DUSE_SDL2=ON \
    -DUSE_SYSTEM_SDL2=ON \
    -DUSE_GLES2=ON \
    -DUSE_EGL=ON \
    -DMPV_NO_FB=ON \
    -DUSE_SYSTEM_CURL=ON \
    -DMPV_INCLUDE_DIR="$STUB/include" \
    -DMPV_LIBRARY_mpv="$STUB/lib/libmpv.a" \
    -DDISABLE_WEBP=ON

cmake --build "$BUILD" -j4

echo "built: $BUILD/wiliwili"
