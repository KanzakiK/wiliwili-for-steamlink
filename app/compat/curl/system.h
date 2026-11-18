/*
 * 兼容垫片：SDK 的 libcurl 7.26 缺少 cpr 1.10 用到的新类型/常量。
 * 只用于编译；multipart 相关调用会变成空操作（当前项目不用 multipart）。
 */
#pragma once

#include <curl/curl.h>

#ifndef CURL_PROGRESSFUNC_CONTINUE
#define CURL_PROGRESSFUNC_CONTINUE 0x10000001
#endif

#if LIBCURL_VERSION_NUM < 0x073800 /* 7.56.0：curl_mime API */
typedef struct Curl_mime curl_mime;
typedef struct curl_mimepart curl_mimepart;

#define curl_mime_init(handle) ((curl_mime*)0)
#define curl_mime_free(mime) ((void)0)
#define curl_mime_addpart(mime) ((curl_mimepart*)0)
#define curl_mime_name(part, name) ((void)0)
#define curl_mime_data(part, data, size) ((void)0)
#define curl_mime_filedata(part, path) ((void)0)
#define curl_mime_filename(part, name) ((void)0)
#define curl_mime_type(part, type) ((void)0)

#ifndef CURLOPT_MIMEPOST
#define CURLOPT_MIMEPOST 269
#endif
#endif
