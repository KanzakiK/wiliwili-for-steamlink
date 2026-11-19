/*
 * D27 session 元数据契约测试。
 * 只依赖 IpcProtocol.h，可在主机直接编译运行，不访问设备。
 */
#include "IpcProtocol.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void) {
    sl_session_meta_t meta;
    char args[1024];

    const char *extra =
        "referrer=\"https://www.bilibili.com\","
        "run=20261119-120000-1234,"
        "session=20261119-120000-1234-1,"
        "content=video,id=BV1Nvx16TEzY,"
        "start=120,audio-file=\"https://example.invalid/a.m4s\"";

    CHECK(sl_session_meta_parse_extra(extra, &meta) == 1);
    CHECK(strcmp(meta.run, "20261119-120000-1234") == 0);
    CHECK(strcmp(meta.session, "20261119-120000-1234-1") == 0);
    CHECK(strcmp(meta.content, "video") == 0);
    CHECK(strcmp(meta.id, "BV1Nvx16TEzY") == 0);

    memset(args, 0, sizeof(args));
    CHECK(sl_session_meta_format_args(&meta, args, sizeof(args)) > 0);
    CHECK(strstr(args, "run=20261119-120000-1234\t") == args);
    CHECK(strstr(args, "session=20261119-120000-1234-1") != NULL);
    CHECK(strstr(args, "content=video") != NULL);
    CHECK(strstr(args, "id=BV1Nvx16TEzY") != NULL);

    CHECK(sl_session_meta_parse_extra("start=120", &meta) == 0);
    CHECK(sl_session_meta_parse_extra(
              "run=20261119-120000-1234,session=s1,content=video,id=", &meta) == -1);
    CHECK(sl_session_meta_parse_extra(
              "referrer=\"https://example.invalid/?a=1,b=2\","
              "run=r1,session=r1-1,content=video,id=BVTEST,"
              "audio-file=\"https://example.invalid/a.m4s?x=1,y=2\"",
              &meta) == 1);
    CHECK(strcmp(meta.run, "r1") == 0);
    CHECK(strcmp(meta.id, "BVTEST") == 0);

    const char *metaArgs =
        "run=20261119-120000-1234\tsession=20261119-120000-1234-1\t"
        "content=live\tid=123456";
    CHECK(sl_session_meta_parse_kv_list(metaArgs, SL_IPC_FIELD_SEP, &meta) == 1);
    CHECK(strcmp(meta.run, "20261119-120000-1234") == 0);
    CHECK(strcmp(meta.session, "20261119-120000-1234-1") == 0);
    CHECK(strcmp(meta.content, "live") == 0);
    CHECK(strcmp(meta.id, "123456") == 0);

    printf("session meta contract ok\n");
    return 0;
}
