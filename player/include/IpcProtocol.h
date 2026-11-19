/*
 * IpcProtocol - wiliwili 兼容层 <-> slplayer_daemon 的 IPC 协议
 *
 * 进程隔离架构（阶段 3.5a）：
 *   wiliwili 主进程内的 libmpv 兼容层是 IPC 客户端；
 *   SlPlayer 跑在独立进程 slplayer_daemon 里，是 IPC 服务端。
 *   控制面（命令）与事件面（事件）走 Unix domain socket；
 *   数据面（视频/音频）仍由 SLVideo/SLAudio 走硬件层，不经过 IPC。
 *
 * 本头文件是 C 兼容的（兼容层用 C 编译），只定义线上格式常量与
 * 无依赖的小工具函数。payload 为文本：命令名/事件名 + 可选参数，
 * 字段之间用 TAB ('\t') 分隔，便于调试且无需序列化库。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* socket 路径。daemon bind，兼容层 connect。
 * 用环境变量 SL_IPC_SOCKET 可覆盖（多实例/测试用）。 */
#define SL_IPC_DEFAULT_SOCKET "/tmp/slplayer-ipc.sock"
#define SL_IPC_ENV_SOCKET     "SL_IPC_SOCKET"

/* 帧头：4 字节小端长度，后面跟 length 字节 payload */
#define SL_IPC_HEADER_SIZE 4
#define SL_IPC_MAX_PAYLOAD (256 * 1024)
#define SL_IPC_FIELD_SEP   '\t'

/* ---- P0 统一帧信封 ----
 * 所有命令与事件都使用固定前缀：
 *   <name><TAB><file_generation><TAB><request_id><TAB><args...>
 *
 * file_generation 由兼容层在新建文件/备用源切换/watchdog 回放时递增；
 * daemon 回传同一代次，兼容层丢弃过期代次的事件。
 * request_id 由兼容层单调递增；引擎在完成 pause/resume/seek 后随事件回传。
 * 这样旧文件事件和旧 seek 结果都不能覆盖新目标。
 */
#define SL_IPC_FRAME_FILE_GEN_FIELD 1
#define SL_IPC_FRAME_REQUEST_FIELD  2
#define SL_IPC_FRAME_ARGS_FIELD     3

/* ---- D27 调试会话元数据 ----
 * run/session/content/id/bv 由 dev 构建的 UI 侧生成，经 loadfile extra 进入兼容层，
 * 兼容层在 open 前发送 meta 命令；daemon 按 generation 保存并用于日志前缀。
 * bv 对 video/season 必填，live/dynamic/page 可空。
 * 字段值不含 TAB、换行、逗号和 '='，不携带签名 URL 或任何查询参数。
 */
#define SL_CMD_META        "meta"      /* meta<TAB>gen<TAB>req<TAB>run=<id><TAB>session=<id><TAB>content=<type><TAB>id=<primary>[<TAB>bv=<BV>] */
#define SL_META_RUN_KEY    "run"
#define SL_META_SESSION_KEY "session"
#define SL_META_CONTENT_KEY "content"
#define SL_META_ID_KEY      "id"
#define SL_META_BV_KEY      "bv"
#define SL_META_FIELD_MAX   128

typedef struct {
    char run[SL_META_FIELD_MAX];
    char session[SL_META_FIELD_MAX];
    char content[SL_META_FIELD_MAX];
    char id[SL_META_FIELD_MAX];
    char bv[SL_META_FIELD_MAX];
} sl_session_meta_t;

static inline int sl_session_meta_assign(sl_session_meta_t *meta,
                                         const char *key, const char *value) {
    if (!meta || !key || !value)
        return -1;
    char *dst = NULL;
    if (strcmp(key, SL_META_RUN_KEY) == 0)
        dst = meta->run;
    else if (strcmp(key, SL_META_SESSION_KEY) == 0)
        dst = meta->session;
    else if (strcmp(key, SL_META_CONTENT_KEY) == 0)
        dst = meta->content;
    else if (strcmp(key, SL_META_ID_KEY) == 0)
        dst = meta->id;
    else if (strcmp(key, SL_META_BV_KEY) == 0)
        dst = meta->bv;
    if (!dst)
        return 0;
    if (!*value || strchr(value, '=') || strchr(value, ' '))
        return -1;
    return snprintf(dst, SL_META_FIELD_MAX, "%s", value) >= SL_META_FIELD_MAX ? -1 : 0;
}

static inline void sl_session_meta_clear(sl_session_meta_t *meta) {
    if (meta)
        memset(meta, 0, sizeof(*meta));
}

/* 解析 key=value 列表：extra 用 ','，IPC meta 参数用 TAB。
 * 未知键一律忽略；返回 1 表示四个字段齐全，0 表示缺字段，-1 表示非法。 */
static inline int sl_session_meta_parse_kv_list(const char *list, char sep,
                                                sl_session_meta_t *meta) {
    if (!list || !meta)
        return 0;
    sl_session_meta_clear(meta);
    const char *p = list;
    while (*p) {
        const char *delim = strchr(p, sep);
        size_t len = delim ? (size_t)(delim - p) : strlen(p);
        if (len > 0 && len < 512) {
            char pair[512];
            memcpy(pair, p, len);
            pair[len] = '\0';
            char *eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                if (sl_session_meta_assign(meta, pair, eq + 1) < 0)
                    return -1;
            }
        }
        if (!delim)
            break;
        p = delim + 1;
    }
    return meta->run[0] && meta->session[0] && meta->content[0] && meta->id[0] ? 1 : 0;
}

/* loadfile extra 里 audio-file URL 可能含逗号，必须按引号外的逗号切分。
 * 与 build_open_cmd 的 referrer/audio-file 解析保持同一套语义。 */
static inline int sl_session_meta_parse_extra(const char *extra, sl_session_meta_t *meta) {
    if (!extra || !meta)
        return 0;
    sl_session_meta_clear(meta);
    char item[512];
    size_t len = 0;
    int inQuote = 0;
    for (const char *p = extra;; ++p) {
        char c = *p;
        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }
        if ((c == ',' && !inQuote) || c == '\0') {
            item[len] = '\0';
            char *eq = strchr(item, '=');
            if (eq) {
                *eq = '\0';
                if (sl_session_meta_assign(meta, item, eq + 1) < 0)
                    return -1;
            }
            if (c == '\0')
                break;
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(item))
            item[len++] = c;
    }
    return meta->run[0] && meta->session[0] && meta->content[0] && meta->id[0] ? 1 : 0;
}

/* 生成 meta 命令参数字段；返回写入长度，cap 不足时返回 0。 */
static inline size_t sl_session_meta_format_args(const sl_session_meta_t *meta,
                                                 char *dst, size_t cap) {
    if (!meta || !dst || cap == 0)
        return 0;
    int n = snprintf(dst, cap,
                     "%s=%s\t%s=%s\t%s=%s\t%s=%s",
                     SL_META_RUN_KEY, meta->run,
                     SL_META_SESSION_KEY, meta->session,
                     SL_META_CONTENT_KEY, meta->content,
                     SL_META_ID_KEY, meta->id);
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    size_t len = (size_t)n;
    if (meta->bv[0]) {
        n = snprintf(dst + len, cap - len, "\t%s=%s", SL_META_BV_KEY, meta->bv);
        if (n <= 0 || (size_t)n >= cap - len)
            return 0;
        len += (size_t)n;
    }
    return len;
}

/* ---- 命令（兼容层 -> daemon）---- */
#define SL_CMD_OPEN     "open"     /* open<TAB>gen<TAB>req<TAB>videoUrl<TAB>referrer<TAB>start<TAB>end<TAB>audioUrl1... */
#define SL_CMD_PAUSE    "pause"    /* pause<TAB>gen<TAB>req */
#define SL_CMD_RESUME   "resume"   /* resume<TAB>gen<TAB>req */
#define SL_CMD_SEEK     "seek"     /* seek<TAB>gen<TAB>req<TAB>seconds */
#define SL_CMD_STOP     "stop"     /* stop<TAB>gen<TAB>req */
#define SL_CMD_QUIT     "quit"     /* quit<TAB>gen<TAB>req：优雅退出并清 Dirty Flag */
#define SL_CMD_VOLUME   "volume"   /* volume<TAB>gen<TAB>req<TAB>0-100 */
#define SL_CMD_HEARTBEAT "heartbeat" /* heartbeat<TAB>gen<TAB>req */

/* ---- 事件（daemon -> 兼容层）---- */
#define SL_EVT_OPENED         "opened"          /* opened<TAB>gen<TAB>req<TAB>duration<TAB>width<TAB>height */
#define SL_EVT_PLAYBACK_START "playback_start"  /* playback_start<TAB>gen<TAB>req */
#define SL_EVT_PROGRESS       "progress"        /* progress<TAB>gen<TAB>req<TAB>position<TAB>duration */
#define SL_EVT_PAUSED         "paused"          /* paused<TAB>gen<TAB>req */
#define SL_EVT_RESUMED        "resumed"         /* resumed<TAB>gen<TAB>req */
#define SL_EVT_SEEKED         "seeked"          /* seeked<TAB>gen<TAB>req<TAB>position */
#define SL_EVT_EOF            "eof"             /* eof<TAB>gen<TAB>req */
#define SL_EVT_ERROR          "error"           /* error<TAB>gen<TAB>req<TAB>message */
#define SL_EVT_FATAL          "fatal"           /* fatal<TAB>gen<TAB>req<TAB>type<TAB>message */
#define SL_EVT_HEARTBEAT      "heartbeat"       /* heartbeat<TAB>gen<TAB>req<TAB>lastFrameTimeMs<TAB>stage<TAB>playing<TAB>inTransition<TAB>videoFrames<TAB>currentPaused<TAB>targetPaused<TAB>seeking<TAB>workerAlive<TAB>presentationMs<TAB>videoQueueMs<TAB>networkErrorAgeMs */

/* 首帧/进度事件的节流间隔（毫秒），避免 IPC 洪泛 */
#define SL_EVT_PROGRESS_INTERVAL_MS 250

#ifdef __cplusplus
}
#endif
