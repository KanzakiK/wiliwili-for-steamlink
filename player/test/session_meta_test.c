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
        "bv=BV1Nvx16TEzY,"
        "cid=43208345449,qn=64,"
        "start=120,audio-file=\"https://example.invalid/a.m4s\"";

    CHECK(sl_session_meta_parse_extra(extra, &meta) == 1);
    CHECK(strcmp(meta.run, "20261119-120000-1234") == 0);
    CHECK(strcmp(meta.session, "20261119-120000-1234-1") == 0);
    CHECK(strcmp(meta.content, "video") == 0);
    CHECK(strcmp(meta.id, "BV1Nvx16TEzY") == 0);
    CHECK(strcmp(meta.bv, "BV1Nvx16TEzY") == 0);
    CHECK(strcmp(meta.cid, "43208345449") == 0);
    CHECK(strcmp(meta.qn, "64") == 0);

    memset(args, 0, sizeof(args));
    CHECK(sl_session_meta_format_args(&meta, args, sizeof(args)) > 0);
    CHECK(strstr(args, "run=20261119-120000-1234\t") == args);
    CHECK(strstr(args, "session=20261119-120000-1234-1") != NULL);
    CHECK(strstr(args, "content=video") != NULL);
    CHECK(strstr(args, "id=BV1Nvx16TEzY") != NULL);
    CHECK(strstr(args, "bv=BV1Nvx16TEzY") != NULL);
    CHECK(strstr(args, "cid=43208345449") != NULL);
    CHECK(strstr(args, "qn=64") != NULL);

    CHECK(sl_session_meta_parse_extra("start=120", &meta) == 0);
    /* 正常启动：只有内容身份，无 run/session，也不能被当成完整 session。 */
    CHECK(sl_session_meta_parse_extra(
              "content=video,id=BV1PROD,bv=BV1PROD,cid=123,start_ms=0",
              &meta) == 0);
    CHECK(sl_session_meta_has_identity(&meta) == 1);
    CHECK(meta.run[0] == '\0');
    CHECK(meta.session[0] == '\0');
    CHECK(strcmp(meta.bv, "BV1PROD") == 0);
    memset(args, 0, sizeof(args));
    CHECK(sl_session_meta_format_args(&meta, args, sizeof(args)) > 0);
    CHECK(strstr(args, "run=") == NULL);
    CHECK(strstr(args, "session=") == NULL);
    CHECK(strstr(args, "content=video") == args);
    CHECK(strstr(args, "bv=BV1PROD") != NULL);
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
        "content=season\tid=323434\tbv=BV1TEST\t"
        "cid=184620662\tqn=64";
    CHECK(sl_session_meta_parse_kv_list(metaArgs, SL_IPC_FIELD_SEP, &meta) == 1);
    CHECK(strcmp(meta.run, "20261119-120000-1234") == 0);
    CHECK(strcmp(meta.session, "20261119-120000-1234-1") == 0);
    CHECK(strcmp(meta.content, "season") == 0);
    CHECK(strcmp(meta.id, "323434") == 0);
    CHECK(strcmp(meta.bv, "BV1TEST") == 0);
    CHECK(strcmp(meta.cid, "184620662") == 0);
    CHECK(strcmp(meta.qn, "64") == 0);

    /* 直播没有 BV，仍是合法 session；不能因为缺 bv 判失败。 */
    const char *liveArgs =
        "run=r2\tsession=r2-1\tcontent=live\tid=123456";
    CHECK(sl_session_meta_parse_kv_list(liveArgs, SL_IPC_FIELD_SEP, &meta) == 1);
    CHECK(strcmp(meta.content, "live") == 0);
    CHECK(meta.bv[0] == '\0');

    /* D28 sync meta：可以独立存在，不要求 run/session/content/id。 */
    sl_sync_meta_t sync;
    const char *syncArgs =
        "epoch=7\ttarget_ms=12345\treason=seek\tdesired=play";
    CHECK(sl_sync_meta_parse_kv_list(syncArgs, SL_IPC_FIELD_SEP, &sync) == 1);
    CHECK(sync.epoch == 7);
    CHECK(sync.target_ms == 12345);
    CHECK(strcmp(sync.reason, "seek") == 0);
    CHECK(strcmp(sync.desired, SL_SYNC_DESIRED_PLAY) == 0);
    CHECK(sl_session_meta_parse_kv_list(syncArgs, SL_IPC_FIELD_SEP, &meta) == 0);

    memset(args, 0, sizeof(args));
    CHECK(sl_sync_meta_format_args(&sync, args, sizeof(args)) > 0);
    CHECK(strstr(args, "epoch=7\t") == args);
    CHECK(strstr(args, "target_ms=12345") != NULL);
    CHECK(strstr(args, "reason=seek") != NULL);
    CHECK(strstr(args, "desired=play") != NULL);

    const char *pendingArgs =
        "epoch=8\ttarget_ms=-1\treason=pause_resume\tdesired=pause";
    CHECK(sl_sync_meta_parse_kv_list(pendingArgs, SL_IPC_FIELD_SEP, &sync) == 1);
    CHECK(sync.epoch == 8);
    CHECK(sync.target_ms == SL_SYNC_TARGET_PENDING);
    CHECK(strcmp(sync.desired, SL_SYNC_DESIRED_PAUSE) == 0);

    /* 同一 meta 帧里两类 kv 并存，各自解析互不覆盖。 */
    const char *combined =
        "run=r1\tsession=r1-1\tcontent=video\tid=BV1TEST\t"
        "epoch=9\ttarget_ms=6789\treason=quality_change\tdesired=play";
    CHECK(sl_session_meta_parse_kv_list(combined, SL_IPC_FIELD_SEP, &meta) == 1);
    CHECK(strcmp(meta.run, "r1") == 0);
    CHECK(strcmp(meta.bv, "") == 0);
    CHECK(sl_sync_meta_parse_kv_list(combined, SL_IPC_FIELD_SEP, &sync) == 1);
    CHECK(sync.epoch == 9);
    CHECK(sync.target_ms == 6789);
    CHECK(strcmp(sync.reason, "quality_change") == 0);

    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=1\ttarget_ms=2\treason=seek", SL_IPC_FIELD_SEP, &sync) == -1);
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=1\ttarget_ms=bad\treason=seek\tdesired=play",
              SL_IPC_FIELD_SEP, &sync) == -1);
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=1\ttarget_ms=2\treason=seek\tdesired=running",
              SL_IPC_FIELD_SEP, &sync) == -1);

    /* D28 收口 E：epoch 不接受符号；pending 目标只允许初始 pause。 */
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=-1\ttarget_ms=2\treason=seek\tdesired=play",
              SL_IPC_FIELD_SEP, &sync) == -1);
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=+1\ttarget_ms=2\treason=seek\tdesired=play",
              SL_IPC_FIELD_SEP, &sync) == -1);
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=1\ttarget_ms=-1\treason=seek\tdesired=pause",
              SL_IPC_FIELD_SEP, &sync) == -1);
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=1\ttarget_ms=-1\treason=pause_resume\tdesired=play",
              SL_IPC_FIELD_SEP, &sync) == -1);
    CHECK(sl_sync_meta_parse_kv_list(
              "epoch=1\treason=pause_resume\tdesired=pause\ttarget_ms=-1",
              SL_IPC_FIELD_SEP, &sync) == 1);

    /* pending context 只允许正确 (gen, req) 消费一次；错配立即丢弃。 */
    sl_sync_context_t context;
    sl_sync_meta_t out;
    int mismatch = 0;
    sl_sync_context_clear(&context);
    CHECK(sl_sync_context_consume(&context, 1, 2, &out, &mismatch) == 0);
    CHECK(mismatch == 0);

    CHECK(sl_sync_meta_parse_kv_list(syncArgs, SL_IPC_FIELD_SEP, &sync) == 1);
    sl_sync_context_set(&context, 1, 2, &sync);
    CHECK(sl_sync_context_consume(&context, 1, 3, &out, &mismatch) == 0);
    CHECK(mismatch == 1);
    CHECK(context.valid == 0);

    sl_sync_context_set(&context, 1, 2, &sync);
    mismatch = 0;
    CHECK(sl_sync_context_consume(&context, 1, 2, &out, &mismatch) == 1);
    CHECK(mismatch == 0);
    CHECK(out.epoch == 7);
    CHECK(context.valid == 0);
    CHECK(sl_sync_context_consume(&context, 1, 2, &out, &mismatch) == 0);

    /* S05：extra 中的控制提示保留毫秒，URL 引号内逗号不能干扰解析。 */
    int64_t hint = -1;
    CHECK(sl_extra_int64(
              "referrer=\"https://example.invalid/?a=1,b=2\","
              "target_ms=5123,pause=1",
              "target_ms", &hint) == 1);
    CHECK(hint == 5123);
    CHECK(sl_extra_int64(
              "referrer=\"https://example.invalid/?a=1,b=2\","
              "target_ms=5123,pause=1",
              "pause", &hint) == 1);
    CHECK(hint == 1);
    CHECK(sl_extra_int64("target_ms=bad", "target_ms", &hint) == 0);

    printf("session meta contract ok\n");
    return 0;
}
