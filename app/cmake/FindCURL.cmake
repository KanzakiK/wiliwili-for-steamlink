# 强制使用 ~/steamlink-curl 里的静态 libcurl，避免 CMake 的 FindCURL
# 同时把 rootfs 的旧 libcurl.so 带进链接。

set(CURL_FOUND TRUE)
set(CURL_VERSION_STRING "7.79.1")
set(CURL_INCLUDE_DIRS "$ENV{HOME}/steamlink-curl/include")
set(CURL_LIBRARIES "$ENV{HOME}/steamlink-curl/lib/libcurl.a")

if (NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl UNKNOWN IMPORTED)
    set_target_properties(CURL::libcurl PROPERTIES
        IMPORTED_LOCATION "$ENV{HOME}/steamlink-curl/lib/libcurl.a"
        INTERFACE_INCLUDE_DIRECTORIES "$ENV{HOME}/steamlink-curl/include")
endif ()
