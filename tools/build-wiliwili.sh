#!/bin/sh
# 交叉编译 wiliwili 整仓（阶段 3.2：MPV 桩库 + 原生 UI）
#
# 用法：sh tools/build-wiliwili.sh
# 产物：~/build-wiliwili/wiliwili（资源在 ~/build-wiliwili/resources）
set -e

export PATH=$HOME/steamlink-sdk/toolchain/bin:$PATH
export LD_LIBRARY_PATH=$HOME/steamlink-sdk/toolchain/lib:$LD_LIBRARY_PATH
export CC=armv7a-cros-linux-gnueabi-gcc
export CXX=armv7a-cros-linux-gnueabi-g++
export MARVELL_SDK_PATH=${MARVELL_SDK_PATH:-$HOME/steamlink-sdk}
export MARVELL_ROOTFS=${MARVELL_ROOTFS:-$HOME/steamlink-sdk/rootfs}

PROJECT_DIR=${PROJECT_DIR:-/mnt/d/bilibili-steamlink}
STUB=$HOME/mpv-stub
STUB_SRC=$PROJECT_DIR/tools/mpv-stub/mpv_stub.c
SOURCE=${SOURCE_DIR:-$HOME/refs/wiliwili}
BUILD=${BUILD_DIR:-$HOME/build-wiliwili}

# 1) mpv 头文件（v0.37.0）
mkdir -p "$STUB/include/mpv" "$STUB/lib" "$STUB/build"
for f in client.h render.h render_gl.h; do
    if [ ! -f "$STUB/include/mpv/$f" ]; then
        echo "download mpv/$f"
        curl -sSLf -o "$STUB/include/mpv/$f" \
            "https://raw.githubusercontent.com/mpv-player/mpv/v0.37.0/libmpv/$f"
    fi
done

# 2) libmpv 兼容层静态库（阶段 3.5a：IPC 客户端，驱动 slplayer_daemon）
#    协议头与 daemon 共享，复制到 stub include 目录供编译
cp -f "$PROJECT_DIR/player/include/IpcProtocol.h" "$STUB/include/IpcProtocol.h"
if [ ! -f "$STUB/lib/libmpv.a" ] || [ "$STUB_SRC" -nt "$STUB/lib/libmpv.a" ] \
   || [ "$PROJECT_DIR/player/include/IpcProtocol.h" -nt "$STUB/lib/libmpv.a" ]; then
    echo "build mpv compat layer"
    armv7a-cros-linux-gnueabi-gcc -O2 -fPIC -marm -mfloat-abi=hard -D_DEFAULT_SOURCE \
        --sysroot="$MARVELL_ROOTFS" -I"$STUB/include" \
        -c "$STUB_SRC" -o "$STUB/build/mpv_stub.o" -pthread
    armv7a-cros-linux-gnueabi-ar rcs "$STUB/lib/libmpv.a" "$STUB/build/mpv_stub.o"
fi

# 3) borealis 透明层补丁（GL 层 alpha=8 + SL_TRANSPARENT 透明 clear）
BOREALIS_DIR="$SOURCE/library/borealis"
BOREALIS_PATCH="$PROJECT_DIR/tools/patches/borealis-steamlink-alpha.patch"
if [ -e "$BOREALIS_DIR/.git" ]; then
    if ! git -C "$BOREALIS_DIR" apply --reverse --check "$BOREALIS_PATCH" >/dev/null 2>&1; then
        echo "apply borealis steamlink alpha patch"
        git -C "$BOREALIS_DIR" apply "$BOREALIS_PATCH"
    fi
fi

# 3b) 真机回归用 --open-bv 入口：只调用 wiliwili 现有 Intent::openBV()，
# 不替换播放链路；用于从正常主菜单启动流程直接打开指定 BV 做端到端验证。
OPEN_BV_PATCH="$PROJECT_DIR/tools/patches/wiliwili-open-bv-test.patch"
if [ -f "$OPEN_BV_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q '"--open-bv"' "$SOURCE/wiliwili/source/main.cpp" 2>/dev/null; then
        echo "apply wiliwili --open-bv test hook"
        git -C "$SOURCE" apply "$OPEN_BV_PATCH"
    fi
fi

# P0 真机复现：起播后自动执行固定 pause/resume + 长 seek 序列。
SEEK_TEST_PATCH="$PROJECT_DIR/tools/patches/wiliwili-p0-seek-test.patch"
if [ -f "$SEEK_TEST_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SL_TEST_SEEK_SEQUENCE' "$SOURCE/wiliwili/source/main.cpp" 2>/dev/null; then
        echo "apply wiliwili P0 seek reproducer"
        git -C "$SOURCE" apply "$SEEK_TEST_PATCH"
    fi
fi

SEEK_ABS_PATCH="$PROJECT_DIR/tools/patches/wiliwili-p0-seek-test-absolute.patch"
if [ -f "$SEEK_ABS_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'MPVCore::instance().seek(60)' "$SOURCE/wiliwili/source/main.cpp" 2>/dev/null; then
        echo "apply wiliwili P0 seek reproducer absolute targets"
        git -C "$SOURCE" apply "$SEEK_ABS_PATCH"
    fi
fi

MEDIA_TAG_PATCH="$PROJECT_DIR/tools/patches/wiliwili-media-tag.patch"
if [ -f "$MEDIA_TAG_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SL_MEDIA_TAG' "$SOURCE/wiliwili/source/main.cpp" 2>/dev/null; then
        echo "apply wiliwili media tag test hook"
        git -C "$SOURCE" apply "$MEDIA_TAG_PATCH"
    fi
fi

# D16：按 Steam Link 实际显示尺寸收敛图片请求，降低 JPEG 解码和 GPU 纹理占用。
# 只改请求后缀常量，不改 wiliwili 布局/样式/Activity 行为。
IMAGE_SIZE_PATCH="$PROJECT_DIR/tools/patches/wiliwili-image-size.patch"
if [ -f "$IMAGE_SIZE_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q '"@320w_180h_1c"' "$SOURCE/wiliwili/include/utils/image_helper.hpp" 2>/dev/null; then
        echo "apply wiliwili image size patch"
        git -C "$SOURCE" apply "$IMAGE_SIZE_PATCH"
    fi
fi

# Steam Link 解码能力卡位：视频最高 1080P60 (qn=116)，音频仅 AAC 高/中/低三档。
# 用构建补丁约束上游选项，不改 UI 布局/样式，也不改 wiliwili 播放链路。
CAPABILITY_CAPS_PATCH="$PROJECT_DIR/tools/patches/wiliwili-capability-caps.patch"
if [ -f "$CAPABILITY_CAPS_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'WILI_VIDEO_QUALITY_LANDSCAPE_MAX 116' "$SOURCE/wiliwili/source/utils/config_helper.cpp" 2>/dev/null; then
        echo "apply wiliwili capability caps patch"
        git -C "$SOURCE" apply --recount "$CAPABILITY_CAPS_PATCH"
    fi
fi

# D18：回移上游 PR #692 的 VideoView 事件重复订阅 UAF 修复（return + aliveToken）。
VIDEO_VIEW_UAF_PATCH="$PROJECT_DIR/tools/patches/wiliwili-video-view-event-uaf.patch"
if [ -f "$VIDEO_VIEW_UAF_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'aliveToken' "$SOURCE/wiliwili/include/view/video_view.hpp" 2>/dev/null; then
        echo "apply wiliwili VideoView UAF patch"
        git -C "$SOURCE" apply --recount "$VIDEO_VIEW_UAF_PATCH"
    fi
fi

# D17：SLVideo 不支持播放倍速，隐藏播放页 OSD 倍速入口并清除焦点。
HIDE_SPEED_PATCH="$PROJECT_DIR/tools/patches/wiliwili-hide-speed-button.patch"
if [ -f "$HIDE_SPEED_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SteamLinkHideSpeed' "$SOURCE/wiliwili/source/view/video_view.cpp" 2>/dev/null; then
        echo "apply wiliwili hide speed button patch"
        git -C "$SOURCE" apply --recount "$HIDE_SPEED_PATCH"
    fi
fi

# D13：回移上游 2f583df，关注 UP 的 mid 可能是字符串，按字符串解析再转换。
DYNAMIC_MID_PATCH="$PROJECT_DIR/tools/patches/wiliwili-dynamic-mid-string.patch"
if [ -f "$DYNAMIC_MID_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'std::string mid;' "$SOURCE/wiliwili/include/api/bilibili/result/dynamic_video.h" 2>/dev/null; then
        echo "apply wiliwili dynamic mid string patch"
        git -C "$SOURCE" apply --recount "$DYNAMIC_MID_PATCH"
    fi
fi

# D20：详情页加载期 duration=0 会让底部进度条宽度变成 NaN 并铺满整屏粉色。
BOTTOM_BAR_GUARD_PATCH="$PROJECT_DIR/tools/patches/wiliwili-bottom-bar-nan.patch"
if [ -f "$BOTTOM_BAR_GUARD_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SteamLinkBottomBarGuard' "$SOURCE/wiliwili/source/view/video_view.cpp" 2>/dev/null; then
        echo "apply wiliwili bottom bar NaN guard patch"
        git -C "$SOURCE" apply --recount "$BOTTOM_BAR_GUARD_PATCH"
    fi
fi

# Steam Link 默认值：图片线程/缓存、TLS、自动全屏、弹幕倍率等按真机配置固化。
STEAM_DEFAULTS_PATCH="$PROJECT_DIR/tools/patches/wiliwili-steamlink-defaults.patch"
if [ -f "$STEAM_DEFAULTS_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SteamLinkDefaults' "$SOURCE/wiliwili/source/utils/config_helper.cpp" 2>/dev/null; then
        echo "apply wiliwili Steam Link defaults patch"
        git -C "$SOURCE" apply --recount "$STEAM_DEFAULTS_PATCH"
    fi
fi

# 降低 NanoVG 字形图集上限，避免满弹幕时申请 51.84MB SHM 连续块。
ATLAS_CAP_PATCH="$PROJECT_DIR/tools/patches/borealis-nanovg-atlas-cap.patch"
if [ -f "$ATLAS_CAP_PATCH" ] && [ -e "$SOURCE/library/borealis/.git" ]; then
    if ! grep -q 'SteamLinkAtlasCap' "$SOURCE/library/borealis/library/lib/extern/nanovg/nanovg.c" 2>/dev/null; then
        echo "apply borealis nanovg atlas cap patch"
        git -C "$SOURCE/library/borealis" apply --recount "$ATLAS_CAP_PATCH"
    fi
fi

# 弹幕显示区域默认 1/4，但保留 1/2、3/4、全屏选项供用户自行选择。
DANMAKU_DEFAULT_PATCH="$PROJECT_DIR/tools/patches/wiliwili-danmaku-area-default.patch"
if [ -f "$DANMAKU_DEFAULT_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SteamLinkDanmakuDefault' "$SOURCE/wiliwili/source/utils/config_helper.cpp" 2>/dev/null; then
        echo "apply wiliwili danmaku area default patch"
        git -C "$SOURCE" apply --recount "$DANMAKU_DEFAULT_PATCH"
    fi
fi

# 弹幕字体默认纯色，避免描边档的二次文本绘制和字形膨胀；用户仍可切回描边/阴影/斜体。
DANMAKU_FONT_DEFAULT_PATCH="$PROJECT_DIR/tools/patches/wiliwili-danmaku-font-default.patch"
if [ -f "$DANMAKU_FONT_DEFAULT_PATCH" ] && [ -d "$SOURCE/.git" ]; then
    if ! grep -q 'SteamLinkDanmakuFontDefault' "$SOURCE/wiliwili/source/utils/config_helper.cpp" 2>/dev/null; then
        echo "apply wiliwili danmaku font default patch"
        git -C "$SOURCE" apply --recount "$DANMAKU_FONT_DEFAULT_PATCH"
    fi
fi

# 4) wiliwili 整仓
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
