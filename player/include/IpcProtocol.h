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
#define SL_EVT_HEARTBEAT      "heartbeat"       /* heartbeat<TAB>gen<TAB>req<TAB>lastFrameTimeMs<TAB>stage<TAB>playing<TAB>inTransition<TAB>videoFrames<TAB>currentPaused<TAB>targetPaused<TAB>seeking<TAB>workerAlive */

/* 首帧/进度事件的节流间隔（毫秒），避免 IPC 洪泛 */
#define SL_EVT_PROGRESS_INTERVAL_MS 250

#ifdef __cplusplus
}
#endif
