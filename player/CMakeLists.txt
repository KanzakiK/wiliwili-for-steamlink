cmake_minimum_required(VERSION 3.13)
project(slplayer C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT FFMPEG_PREFIX)
    set(FFMPEG_PREFIX $ENV{HOME}/steamlink-ffmpeg)
endif()

find_package(Threads REQUIRED)

add_library(slplayer STATIC
    src/HttpSource.cpp
    src/AudioPipeline.cpp
    src/H264Sps.cpp
    src/SlPlayer.cpp)

target_include_directories(slplayer
    PUBLIC include
    PRIVATE src ${FFMPEG_PREFIX}/include)

target_compile_options(slplayer PRIVATE -Wall -Wextra -mfpu=neon)
target_compile_definitions(slplayer PRIVATE _DEFAULT_SOURCE)

target_link_libraries(slplayer PUBLIC
    -Wl,--start-group
    ${FFMPEG_PREFIX}/lib/libavformat.a
    ${FFMPEG_PREFIX}/lib/libavcodec.a
    ${FFMPEG_PREFIX}/lib/libswresample.a
    ${FFMPEG_PREFIX}/lib/libavutil.a
    -Wl,--end-group
    SLVideo SLAudio curl m Threads::Threads)

add_executable(slplayer_test test/test_main.cpp)
target_compile_options(slplayer_test PRIVATE -Wall -Wextra -mfpu=neon)
target_link_libraries(slplayer_test PRIVATE slplayer SDL2)

# 阶段 3.5a：播放子进程，通过 Unix socket 接受 wiliwili 兼容层控制。
# 不链 SDL2（控制面全部走 IPC），pthread 用于 recv 超时/信号兜底。
add_executable(slplayer_daemon daemon/slplayer_daemon.cpp)
target_compile_options(slplayer_daemon PRIVATE -Wall -Wextra -mfpu=neon)
target_link_libraries(slplayer_daemon PRIVATE slplayer pthread)

# IPC 链路调试探针（纯 C，不链 SlPlayer）
add_executable(ipc_probe daemon/ipc_probe.c)
target_include_directories(ipc_probe PRIVATE include)
target_compile_options(ipc_probe PRIVATE -Wall -Wextra -D_DEFAULT_SOURCE)

# 兼容层独立验证程序（链 libmpv.a，模拟 wiliwili 调用序列）
# 兼容层库路径由 -DMPV_COMPAT_LIB 指定，默认 ~/mpv-stub/lib/libmpv.a
if(NOT MPV_COMPAT_LIB)
    set(MPV_COMPAT_LIB $ENV{HOME}/mpv-stub/lib/libmpv.a)
endif()
add_executable(compat_test daemon/compat_test.c)
target_include_directories(compat_test PRIVATE
    include
    $ENV{HOME}/mpv-stub/include)
target_compile_options(compat_test PRIVATE -Wall -Wextra -D_DEFAULT_SOURCE)
target_link_libraries(compat_test PRIVATE ${MPV_COMPAT_LIB} pthread)
