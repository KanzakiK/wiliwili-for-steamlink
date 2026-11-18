cmake_minimum_required(VERSION 3.16)
project(bilibili_sl CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# borealis 平台选项（必须在 include commonOption.cmake 之前设置）
set(PLATFORM_DESKTOP ON CACHE BOOL "" FORCE)
set(USE_SDL2 ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_SDL2 ON CACHE BOOL "" FORCE)
set(USE_GLES2 ON CACHE BOOL "" FORCE)
set(USE_EGL ON CACHE BOOL "" FORCE)
set(BRLS_RESOURCES_DIR "." CACHE STRING "" FORCE)

set(WILIWILI_ROOT "$ENV{HOME}/refs/wiliwili" CACHE PATH "wiliwili 源码目录")
set(BOREALIS_LIBRARY "${WILIWILI_ROOT}/library/borealis/library")

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

if (CMAKE_SYSROOT)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SYSROOT}/usr")
endif ()

add_subdirectory(${BOREALIS_LIBRARY} ${CMAKE_BINARY_DIR}/borealis)

# cpr（使用自编的 libcurl 7.79.1；SDK 的 7.26 太老）
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
set(CURL_INCLUDE_DIR "$ENV{HOME}/steamlink-curl/include" CACHE PATH "" FORCE)
set(CURL_LIBRARY "$ENV{HOME}/steamlink-curl/lib/libcurl.a" CACHE FILEPATH "" FORCE)
set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS_SSL OFF CACHE BOOL "" FORCE)
set(CPR_ENABLE_SSL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${WILIWILI_ROOT}/library/cpr ${CMAKE_BINARY_DIR}/cpr)

# pystring
add_library(pystring STATIC ${WILIWILI_ROOT}/library/pystring/pystring.cpp)
target_include_directories(pystring PUBLIC ${WILIWILI_ROOT}/library/pystring)
set_property(TARGET pystring PROPERTY CXX_STANDARD 17)

# wiliwili API（先接热门/搜索/视频详情，不接 mpv）
set(API_SRC
    ${WILIWILI_ROOT}/wiliwili/source/api/bilibili.cpp
    ${WILIWILI_ROOT}/wiliwili/source/api/home_api.cpp
    ${WILIWILI_ROOT}/wiliwili/source/api/search_api.cpp
    ${WILIWILI_ROOT}/wiliwili/source/api/video_detail_api.cpp
    ${WILIWILI_ROOT}/wiliwili/source/api/util/http.cpp
    ${WILIWILI_ROOT}/wiliwili/source/api/util/json.cpp
    ${WILIWILI_ROOT}/wiliwili/source/api/util/wbi.cpp
    ${WILIWILI_ROOT}/wiliwili/source/utils/number_helper.cpp
)

find_package(Threads REQUIRED)

add_executable(bilibili_sl main.cpp ${API_SRC})
target_include_directories(bilibili_sl PRIVATE
    ${WILIWILI_ROOT}/wiliwili/include
    ${WILIWILI_ROOT}/wiliwili/include/api
)
target_link_libraries(bilibili_sl PRIVATE borealis cpr pystring Threads::Threads z ssl crypto rt dl)
target_link_options(bilibili_sl PRIVATE
    -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib
    -Wl,-rpath-link,${CMAKE_SYSROOT}/lib
)
