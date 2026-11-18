# 交叉编译用 OpenSSL 查找模块
#
# SDK rootfs 里的 libssl.so / libcrypto.so 没有 SONAME，若用绝对路径链接，
# DT_NEEDED 会被写成 WSL 的 sysroot 路径，设备上无法加载。
# 这里只暴露链接名，让 ld 用 --sysroot 找到 rootfs 里的库。

set(OPENSSL_FOUND TRUE)
set(OPENSSL_VERSION "1.0.1")
set(OPENSSL_INCLUDE_DIR "$ENV{MARVELL_ROOTFS}/usr/include")
set(OPENSSL_INCLUDE_DIRS "${OPENSSL_INCLUDE_DIR}")
set(OPENSSL_SSL_LIBRARY ssl)
set(OPENSSL_CRYPTO_LIBRARY crypto)
set(OPENSSL_LIBRARIES "-lssl;-lcrypto")

if (NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL INTERFACE IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        INTERFACE_LINK_LIBRARIES "-lssl;-lcrypto")
endif ()

if (NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto INTERFACE IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        INTERFACE_LINK_LIBRARIES "-lcrypto")
endif ()
