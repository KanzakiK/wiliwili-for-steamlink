#!/bin/sh
# 交叉编译 cpr 需要的静态 libcurl（SDK rootfs 的 7.26 太老）
set -e

ROOTFS=${MARVELL_ROOTFS:-$HOME/steamlink-sdk/rootfs}
SRC=$HOME/curl-7.79.1
PREFIX=$HOME/steamlink-curl

export PATH=$HOME/steamlink-sdk/toolchain/bin:$PATH
export LD_LIBRARY_PATH=$HOME/steamlink-sdk/toolchain/lib:$LD_LIBRARY_PATH
export CC="armv7a-cros-linux-gnueabi-gcc --sysroot=$ROOTFS -marm -mfloat-abi=hard"
export CXX="armv7a-cros-linux-gnueabi-g++ --sysroot=$ROOTFS -marm -mfloat-abi=hard"
export LDFLAGS="-L$ROOTFS/usr/lib -L$ROOTFS/lib"
export CPPFLAGS="-I$ROOTFS/usr/include"

cd "$SRC"
if [ ! -f config.status ]; then
    ./configure --host=armv7a-cros-linux-gnueabi --prefix="$PREFIX" \
        --disable-shared --enable-static \
        --with-ssl="$ROOTFS/usr" --with-zlib="$ROOTFS/usr" \
        --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
        --disable-ldap --disable-ldaps --without-libidn2 --without-nghttp2 \
        --without-brotli --without-zstd --disable-manual \
        --disable-ftp --disable-tftp --disable-telnet --disable-dict \
        --disable-gopher --disable-smtp --disable-imap --disable-pop3 --disable-rtsp
fi
make -j4
make install

echo "curl installed to $PREFIX"
