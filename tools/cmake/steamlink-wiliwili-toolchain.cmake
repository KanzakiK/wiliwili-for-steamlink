# Steam Link 工具链包装（wiliwili 整仓构建用）
# 在 SDK 工具链基础上补充：
# 1. rootfs 共享库的 rpath-link，避免链接时找不到间接依赖
# 2. GCC 9.2 的 std::filesystem 需要单独链接 libstdc++fs

include($ENV{HOME}/steamlink-sdk/toolchain/steamlink-toolchain.cmake)

set(CMAKE_EXE_LINKER_FLAGS
    "-static-libgcc -static-libstdc++ -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib -Wl,-rpath-link,${CMAKE_SYSROOT}/lib")

set(CMAKE_CXX_STANDARD_LIBRARIES "-lstdc++fs")
