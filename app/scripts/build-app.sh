#!/bin/sh
# 交叉编译 Steam Link B 站客户端（borealis + wiliwili API）
set -e

export PATH=$HOME/steamlink-sdk/toolchain/bin:$PATH
export LD_LIBRARY_PATH=$HOME/steamlink-sdk/toolchain/lib:$LD_LIBRARY_PATH
export CC=armv7a-cros-linux-gnueabi-gcc
export CXX=armv7a-cros-linux-gnueabi-g++
export MARVELL_SDK_PATH=${MARVELL_SDK_PATH:-$HOME/steamlink-sdk}
export MARVELL_ROOTFS=${MARVELL_ROOTFS:-$HOME/steamlink-sdk/rootfs}

BUILD=${BUILD_DIR:-$HOME/build-bilibili}
SOURCE=${SOURCE_DIR:-/mnt/d/bilibili-steamlink/app}

cmake -S "$SOURCE" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE=$MARVELL_SDK_PATH/toolchain/steamlink-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$BUILD" -j4

echo "built: $BUILD/bilibili_sl"
