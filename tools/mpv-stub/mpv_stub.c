/*
 * libmpv 兼容层（阶段 3.5a，进程隔离架构）
 *
 * 对 wiliwili 呈现 mpv client API 的一个子集；内部是 IPC 客户端，
 * 通过 Unix domain socket 控制独立播放子进程 slplayer_daemon。
 * wiliwili 源码零改动。
 *
 * 数据面（视频/音频）由 daemon 侧的 SLVideo/SLAudio 走硬件层，不经过 IPC；
 * 本文件只跑控制面（命令）与事件面（事件 + 属性）。
 *
 * 事件语义严格模拟 mpv：START_FILE → FILE_LOADED → PLAYBACK_RESTART，
 * PROPERTY_CHANGE 带观察时注册的 reply_userdata。
 */
#include <mpv/client.h>
#include <mpv/render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <time.h>

#include "IpcProtocol.h"

/* 实现定义在下方；恢复失败时要恢复 UI 的不透明清屏。 */
static void transparency_disable(mpv_handle *ctx);
static void update_prop_and_notify(mpv_handle *ctx, const char *name,
                                   mpv_format fmt, void *value);
static void d27_log(mpv_handle *ctx, const char *source, uint64_t gen,
                    uint64_t req, const char *fmt, ...);
static void sync_end_epoch_locked(mpv_handle *ctx, const char *status,
                                  const char *reason);
static int send_meta_and_command(mpv_handle *ctx, uint64_t gen, uint64_t req,
                                 const char *reason, int64_t targetMs,
                                 const char *desired, const char *commandFrame);
static double open_cmd_start(const char *cmd);

/* ---- GL 函数加载（保留原桩逻辑，borealis 透明层补丁依赖）----
 * borealis 自带的 glad 是桌面 GL 加载器，在 GLES2 上下文中不会加载 GL 3.0 的
 * FBO 函数，wiliwili 的 MPV_NO_FB 绘制路径会直接调用它们，不打补丁会跳到地址 0。
 */
extern void *SDL_GL_GetProcAddress(const char *proc);
extern void *glad_glBindFramebuffer;
extern void *glad_glGenFramebuffers;
extern void *glad_glFramebufferTexture2D;
extern void *glad_glCheckFramebufferStatus;
extern void *glad_glDeleteFramebuffers;

static void load_missing_gl_functions(void) {
    if (!glad_glBindFramebuffer) glad_glBindFramebuffer = SDL_GL_GetProcAddress("glBindFramebuffer");
    if (!glad_glGenFramebuffers) glad_glGenFramebuffers = SDL_GL_GetProcAddress("glGenFramebuffers");
    if (!glad_glFramebufferTexture2D)
        glad_glFramebufferTexture2D = SDL_GL_GetProcAddress("glFramebufferTexture2D");
    if (!glad_glCheckFramebufferStatus)
        glad_glCheckFramebufferStatus = SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    if (!glad_glDeleteFramebuffers)
        glad_glDeleteFramebuffers = SDL_GL_GetProcAddress("glDeleteFramebuffers");
}

/* ================= 内部状态 ================= */

#define EVT_QUEUE_MAX 256
#define OBS_MAX 64
#define DAEMON_PATH_DEFAULT "/tmp/slplayer/slplayer_daemon"
static int g_propDebug = 0;

/* 3.5c 默认值。数值均可用环境变量覆盖，便于真机对照校准：
 *   SL_WATCHDOG_POLL_MS      心跳轮询间隔
 *   SL_WATCHDOG_STALL_MS     无新帧判定卡死的阈值
 *   SL_WATCHDOG_NOHB_MS      完全收不到心跳回复的判定阈值
 *   SL_WATCHDOG_GRACE_MS     resume/恢复后首帧宽限期
 *   SL_WATCHDOG_DISABLE=1    临时关闭 watchdog（只用于对照实验）
 * 这些是保守初值，最终值需以真机日志校准后回填文档。 */
#define WATCHDOG_POLL_MS_DEFAULT 500
#define WATCHDOG_STALL_MS_DEFAULT 1500
#define WATCHDOG_NOHB_MS_DEFAULT 3000
#define WATCHDOG_GRACE_MS_DEFAULT 2000
#define HEALTH_WINDOW_MS_DEFAULT 8000
#define HEALTH_PROGRESS_MS_DEFAULT 250
#define HEALTH_NET_GRACE_MS_DEFAULT 3000
#define SL_HEALTH_PROP "sl-health-stall"
/* 1=软卡顿（帧仍在提交，但呈现位置不推进），2=watchdog 硬卡顿。 */
#define SL_HEALTH_HINT_SOFT 1
#define SL_HEALTH_HINT_HARD 2
#define SL_HEALTH_OBSERVER_ID 22
#define DAEMON_RESTART_MAX 3
#define DAEMON_RESTART_WINDOW_MS 30000
/* 故障恢复可快速补杀；正常退出要给 daemon 完成 stop→FreeStream→FreeContext 的时间。 */
#define DAEMON_REAP_RECOVERY_MS 500
#define DAEMON_REAP_TERMINATE_MS 5000

typedef struct {
    uint64_t epoch;
    uint64_t generation;
    uint64_t requestId;
    int64_t targetMs;
    char reason[SL_SYNC_REASON_MAX];
    char desired[SL_SYNC_DESIRED_MAX];
    int targetResolved;
    int active;
} sl_active_epoch_t;

struct mpv_handle {
    int sock;                 /* 到 daemon 的连接；由 daemonMutex 保护 */
    pid_t daemonPid;          /* 由 daemonMutex 保护 */
    int initialized;
    int shuttingDown;         /* 正常终止中，禁止自动重启 */

    /* daemon 生命周期状态（启动/回收/重启/心跳共用） */
    pthread_mutex_t daemonMutex;
    char daemonPath[512];
    char socketPath[128];
    int daemonGeneration;     /* 每次成功启动 daemon 递增，防止旧状态串到新实例 */
    int watchdogKilledGeneration;
    int suppressWatchdogReplay; /* D22: 自动降级已发起，取消本次 watchdog 旧流重放 */
    long long restartTimes[DAEMON_RESTART_MAX];
    int restartCursor;

    /* P0 文件代次与请求序号：与 daemonGeneration（进程代次）分开。
     * 每次 loadfile replace / 备用源切换 / watchdog 回放都递增文件代次；
     * 每个 pause/resume/seek 分配唯一请求号，完成事件原样回传。 */
    uint64_t fileGeneration;
    uint64_t requestSeq;
    uint64_t lastCommandReq;
    /* 本文件 open 命令的 start= 目标，仅用于 FILE_LOADED 后等价 seek 去重。 */
    double initialSeekTarget;
    int hasInitialSeekTarget;

    /* D27：当前文件实例的 debug session；由 replace 的 loadfile extra 建立，
     * 备用源切换和 watchdog 回放复用。 */
    sl_session_meta_t sessionMeta;
    int hasSessionMeta;
    pthread_mutex_t metaMutex;

    /* D28：compat 是 epoch 唯一所有者，只保留一个 active epoch。 */
    sl_active_epoch_t activeEpoch;
    uint64_t epochSeq;

    /* 断连恢复所需的播放快照 */
    int hasLastOpen;
    char lastOpenCmd[65536];
    int playbackActive;       /* 是否处于可恢复播放（EOF/stop 后为 0）*/
    int expectedPause;        /* UI 期望的暂停态 */
    int restorePausePending;  /* 重启恢复后，等 PLAYBACK_START 再补发 pause */
    int suppressFileLoadedOnce; /* 恢复回放不再伪造 START_FILE/FILE_LOADED，避免触发自动 resume */
    double resumePosition;    /* 只由 daemon 的 progress/seeked 更新：实际已呈现时间 */

    /* watchdog 心跳快照（接收线程写，watchdog 读） */
    long long heartbeatReceivedMs;
    long long lastResumeMs;
    long long hbLastFrameTimeMs;
    int hbStage;
    int hbPlaying;
    int hbInTransition;
    int hbVideoFrames;
    int hbWorkerAlive;
    int hbCurrentPaused;
    int hbTargetPaused;
    int hbSeeking;
    int hbValid;
    double hbPresentationMs;
    unsigned hbVideoQueueMs;
    long long hbNetworkErrorAgeMs;
    int hbBackpressure;
    int hbHealthValid;
    uint64_t hbRequestId;

    /* D22 软卡顿状态：按文件 generation 重置，每个实例至多提示一次。 */
    uint64_t healthGeneration;
    int healthHintShown;
    int healthBaselineValid;
    long long healthBaselineMs;
    double healthBaselinePresentationMs;
    int healthBaselineFrames;

    pthread_t watchdogThread;
    int watchdogRunning;
    int watchdogThreadCreated;

    /* 事件队列 */
    mpv_event *queue[EVT_QUEUE_MAX];
    int qHead, qCount;
    pthread_mutex_t qMutex;
    mpv_event *lastReturned; /* 上次 wait_event 返回的事件，取新事件前释放（mpv 语义）*/

    /* wakeup 回调 */
    void (*wakeupCb)(void *);
    void *wakeupCtx;
    pthread_mutex_t cbMutex;

    /* 观察表：name -> reply_userdata */
    struct { char name[64]; uint64_t userdata; mpv_format fmt; } obs[OBS_MAX];
    int obsCount;
    pthread_mutex_t obsMutex;

    /* 发送锁：命令来自 UI 线程、备用源切换来自接收线程，必须串行化整帧写入 */
    pthread_mutex_t txMutex;

    /* 备用源队列：mpv 语义下 loadfile method="append" 只入播放列表、不立即播放。
     * wiliwili 的 setBackupUrl() 用它追加备用 CDN；主源出错时才依次尝试。 */
    char backup[8][8192];
    int backupCount;

    /* 属性缓存（由接收线程更新，get_property 读取） */
    pthread_mutex_t propMutex;
    int64_t duration;         /* 秒 */
    double playbackTime;
    double percentPos;
    int pause;                /* 1 = 暂停 */
    int eofReached;
    int coreIdle;
    int seeking;
    int volume;
    double speed;
    char path[1024];

    /* 接收线程 */
    pthread_t rxThread;
    int rxRunning;
    int rxThreadCreated;
};

struct mpv_render_context { int unused; };
static struct mpv_render_context g_render_ctx;

/* ================= 工具 ================= */

static int send_payload(int fd, const char *payload, size_t len) {
    unsigned char h[SL_IPC_HEADER_SIZE];
    uint32_t n = (uint32_t)len;
    h[0] = n & 0xff; h[1] = (n >> 8) & 0xff; h[2] = (n >> 16) & 0xff; h[3] = (n >> 24) & 0xff;
    size_t off = 0;
    while (off < SL_IPC_HEADER_SIZE) {
        ssize_t w = send(fd, h + off, SL_IPC_HEADER_SIZE - off, MSG_NOSIGNAL);
        if (w > 0) off += (size_t)w;
        else if (w < 0 && errno == EINTR) continue;
        else return -1;
    }
    off = 0;
    while (off < len) {
        ssize_t w = send(fd, payload + off, len - off, MSG_NOSIGNAL);
        if (w > 0) off += (size_t)w;
        else if (w < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* 带发送锁的整帧发送（多线程共享同一 socket 时防帧交错） */
static int send_frame(mpv_handle *ctx, const char *payload, size_t len) {
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->daemonMutex);
    if (ctx->sock < 0) {
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }
    int fd = ctx->sock;
    pthread_mutex_lock(&ctx->txMutex);
    int rc = send_payload(fd, payload, len);
    pthread_mutex_unlock(&ctx->txMutex);
    pthread_mutex_unlock(&ctx->daemonMutex);
    return rc;
}

static int send_cmd(mpv_handle *ctx, const char *cmd) {
    if (!ctx) return MPV_ERROR_INVALID_PARAMETER;
    if (send_frame(ctx, cmd, strlen(cmd)) < 0) return MPV_ERROR_GENERIC;
    return MPV_ERROR_SUCCESS;
}

/* open 专用入口：D28 的 meta 由通用 send_meta_and_command() 连续发出。
 * 无 session 时仍发送 sync meta；正式启动路径行为不变。 */
static int send_session_meta_and_open(mpv_handle *ctx, uint64_t gen, uint64_t req,
                                      const char *openCmd, const char *reason,
                                      int64_t targetMs) {
    if (!ctx || !openCmd) return MPV_ERROR_INVALID_PARAMETER;
    return send_meta_and_command(ctx, gen, req, reason ? reason : "open",
                                 targetMs, SL_SYNC_DESIRED_PLAY, openCmd) == 0
               ? MPV_ERROR_SUCCESS
               : MPV_ERROR_GENERIC;
}

/* P0 统一命令信封：name<TAB>fileGen<TAB>requestId<TAB>可选参数。 */
static int send_context_cmd(mpv_handle *ctx, const char *name, const char *fmt, ...) {
    if (!ctx || !name) return MPV_ERROR_INVALID_PARAMETER;
    uint64_t gen = 0, req = 0;
    pthread_mutex_lock(&ctx->daemonMutex);
    gen = ctx->fileGeneration;
    req = ++ctx->requestSeq;
    ctx->lastCommandReq = req;
    pthread_mutex_unlock(&ctx->daemonMutex);

    char body[1024];
    body[0] = '\0';
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
    }
    char frame[1200];
    snprintf(frame, sizeof(frame), "%s\t%llu\t%llu\t%s", name,
             (unsigned long long)gen, (unsigned long long)req, body);
    return send_cmd(ctx, frame);
}

static int recv_payload(int fd, char *buf, size_t cap, size_t *outLen) {
    unsigned char h[SL_IPC_HEADER_SIZE];
    size_t off = 0;
    while (off < SL_IPC_HEADER_SIZE) {
        ssize_t r = read(fd, h + off, SL_IPC_HEADER_SIZE - off);
        if (r > 0) off += (size_t)r;
        else if (r < 0 && errno == EINTR) continue;
        else return -1;
    }
    uint32_t n = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    if (n > SL_IPC_MAX_PAYLOAD || n + 1 > cap) return -1;
    off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r > 0) off += (size_t)r;
        else if (r < 0 && errno == EINTR) continue;
        else return -1;
    }
    buf[n] = '\0';
    *outLen = n;
    return 0;
}

/* TAB 切分到固定数组 */
static int split_fields(char *s, char *fields[], int max) {
    int n = 0;
    char *p = s;
    while (n < max) {
        fields[n++] = p;
        char *tab = strchr(p, SL_IPC_FIELD_SEP);
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return n;
}

/* ================= 事件队列 ================= */

/* 释放事件及其 data（mpv 语义：事件在下次 wait_event 后失效）*/
static void free_event(mpv_event *e) {
    if (!e) return;
    if (e->data) {
        if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *p = (mpv_event_property *)e->data;
            free((void *)p->name);
            free(p->data);
        }
        free(e->data);
    }
    free(e);
}

static mpv_event *alloc_event(mpv_handle *ctx, mpv_event_id id) {
    pthread_mutex_lock(&ctx->qMutex);
    if (ctx->qCount >= EVT_QUEUE_MAX) {
        pthread_mutex_unlock(&ctx->qMutex);
        return NULL;
    }
    int idx = (ctx->qHead + ctx->qCount) % EVT_QUEUE_MAX;
    mpv_event *e = calloc(1, sizeof(mpv_event));
    if (!e) { pthread_mutex_unlock(&ctx->qMutex); return NULL; }
    e->event_id = id;
    ctx->queue[idx] = e;
    ctx->qCount++;
    pthread_mutex_unlock(&ctx->qMutex);
    return e;
}

static void push_prop_event(mpv_handle *ctx, const char *name, uint64_t userdata,
                            mpv_format fmt, void *value) {
    pthread_mutex_lock(&ctx->obsMutex);
    int observed = 0;
    for (int i = 0; i < ctx->obsCount; i++) {
        if (strcmp(ctx->obs[i].name, name) == 0) { userdata = ctx->obs[i].userdata; fmt = ctx->obs[i].fmt; observed = 1; break; }
    }
    pthread_mutex_unlock(&ctx->obsMutex);
    if (!observed) { free(value); return; } /* 只给观察中的属性发 PROPERTY_CHANGE */

    mpv_event *e = alloc_event(ctx, MPV_EVENT_PROPERTY_CHANGE);
    if (!e) return;
    e->reply_userdata = userdata;
    mpv_event_property *p = calloc(1, sizeof(mpv_event_property));
    p->name = strdup(name);
    p->format = fmt;
    p->data = value;         /* 调用方传入已分配、生命周期交给事件 */
    e->data = p;
}

static void fire_wakeup(mpv_handle *ctx) {
    pthread_mutex_lock(&ctx->cbMutex);
    void (*cb)(void *) = ctx->wakeupCb;
    void *ud = ctx->wakeupCtx;
    pthread_mutex_unlock(&ctx->cbMutex);
    if (cb) cb(ud);
}

/* 健康提示必须在 watchdog 恢复清队列时保留，否则刚发出的 Toast 会被丢弃。 */
static int is_health_hint_event(mpv_event *e) {
    if (!e || e->event_id != MPV_EVENT_PROPERTY_CHANGE || !e->data) return 0;
    mpv_event_property *p = (mpv_event_property *)e->data;
    return p->name && strcmp(p->name, SL_HEALTH_PROP) == 0;
}

/* 新文件代次生效时丢弃上一文件尚未交付的 mpv 事件，仅保留健康提示。
 * 不释放 lastReturned：UI 可能仍持有它的指针，按 mpv 语义由下一次 wait_event 释放。 */
static void clear_queued_events(mpv_handle *ctx) {
    mpv_event *health[EVT_QUEUE_MAX];
    int healthCount = 0;
    pthread_mutex_lock(&ctx->qMutex);
    for (int i = 0; i < EVT_QUEUE_MAX; i++) {
        if (is_health_hint_event(ctx->queue[i]))
            health[healthCount++] = ctx->queue[i];
        else
            free_event(ctx->queue[i]);
        ctx->queue[i] = NULL;
    }
    ctx->qHead = 0;
    ctx->qCount = healthCount;
    for (int i = 0; i < healthCount; i++) ctx->queue[i] = health[i];
    pthread_mutex_unlock(&ctx->qMutex);
}

/* 新文件的属性缓存直接重置，不生成 PROPERTY_CHANGE：
 * wiliwili 在 FILE_LOADED 里自己把进度和时长显示清零。 */
static void reset_props_for_new_file(mpv_handle *ctx, const char *url) {
    pthread_mutex_lock(&ctx->propMutex);
    ctx->duration = 0;
    ctx->playbackTime = 0;
    ctx->percentPos = 0;
    ctx->pause = 0;
    ctx->eofReached = 0;
    ctx->coreIdle = 1;
    ctx->seeking = 0;
    snprintf(ctx->path, sizeof(ctx->path), "%s", url ? url : "");
    pthread_mutex_unlock(&ctx->propMutex);

    /* 属性缓存清零但 UI 没收到事件会留下上一文件的 pause/eof 状态，
     * 导致新文件按钮不切换、END_OF_FILE 被误拦截。显式同步一次。 */
    int *pause = malloc(sizeof(int)); *pause = 0;
    update_prop_and_notify(ctx, "pause", MPV_FORMAT_FLAG, pause);
    int *eof = malloc(sizeof(int)); *eof = 0;
    update_prop_and_notify(ctx, "eof-reached", MPV_FORMAT_FLAG, eof);
    int *idle = malloc(sizeof(int)); *idle = 1;
    update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, idle);
    int *seeking = malloc(sizeof(int)); *seeking = 0;
    update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, seeking);
    double *time = malloc(sizeof(double)); *time = 0.0;
    update_prop_and_notify(ctx, "playback-time", MPV_FORMAT_DOUBLE, time);
    double *percent = malloc(sizeof(double)); *percent = 0.0;
    update_prop_and_notify(ctx, "percent-pos", MPV_FORMAT_DOUBLE, percent);
}

/* 属性缓存更新 + 触发对应 PROPERTY_CHANGE。
 * value 需为按 fmt 分配的堆内存，事件消费后由 mpv_free 释放。 */
static void update_prop_and_notify(mpv_handle *ctx, const char *name,
                                    mpv_format fmt, void *value) {
    pthread_mutex_lock(&ctx->propMutex);
    if (strcmp(name, "duration") == 0)
        ctx->duration = (fmt == MPV_FORMAT_INT64) ? *(int64_t *)value : (int64_t)*(double *)value;
    else if (strcmp(name, "playback-time") == 0) ctx->playbackTime = *(double *)value;
    else if (strcmp(name, "percent-pos") == 0) ctx->percentPos = *(double *)value;
    else if (strcmp(name, "pause") == 0) ctx->pause = *(int *)value;
    else if (strcmp(name, "eof-reached") == 0) ctx->eofReached = *(int *)value;
    else if (strcmp(name, "core-idle") == 0) ctx->coreIdle = *(int *)value;
    else if (strcmp(name, "seeking") == 0) ctx->seeking = *(int *)value;
    else if (strcmp(name, "volume") == 0) ctx->volume = (int)*(int64_t *)value;
    else if (strcmp(name, "speed") == 0) ctx->speed = *(double *)value;
    else if (strcmp(name, "path") == 0) snprintf(ctx->path, sizeof(ctx->path), "%s", (char *)value);
    pthread_mutex_unlock(&ctx->propMutex);
    if (g_propDebug &&
        (strcmp(name, "core-idle") == 0 || strcmp(name, "playback-time") == 0 ||
         strcmp(name, "pause") == 0 || strcmp(name, "speed") == 0)) {
        if (fmt == MPV_FORMAT_FLAG)
            d27_log(ctx, "PROP", 0, 0, "%s=%d", name, *(int *)value ? 1 : 0);
        else if (fmt == MPV_FORMAT_DOUBLE)
            d27_log(ctx, "PROP", 0, 0, "%s=%.3f", name, *(double *)value);
    }
    push_prop_event(ctx, name, 0, fmt, value);
}

/* ================= 3.5c：进程生命周期与 watchdog ================= */

static long long monotonicMs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static long long envMs(const char *name, long long fallback, long long minMs, long long maxMs) {
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (!end || *end || v < minMs || v > maxMs) return fallback;
    return v;
}

/* D27 统一日志格式：
 * [HH:MM:SS.mmm][SOURCE][run=...][session=...][gen=...][req=...] message
 * metaMutex 独立于 daemonMutex，watchdog 持有 daemonMutex 时也能安全记录。 */
static void d27_log(mpv_handle *ctx, const char *source, uint64_t gen,
                    uint64_t req, const char *fmt, ...) {
    sl_session_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (ctx) {
        pthread_mutex_lock(&ctx->metaMutex);
        meta = ctx->sessionMeta;
        pthread_mutex_unlock(&ctx->metaMutex);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr,
            "[%02d:%02d:%02d.%03ld][%s][run=%s][session=%s]"
            "[gen=%llu][req=%llu] ",
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            (long)(ts.tv_nsec / 1000000L), source,
            meta.run[0] ? meta.run : "-",
            meta.session[0] ? meta.session : "-",
            (unsigned long long)gen, (unsigned long long)req);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/* D28：compat 独占 epoch 生命周期。以下函数都要求持有 daemonMutex；
 * d27_log 只用 metaMutex，因此不会在这里死锁。 */
static void sync_end_epoch_locked(mpv_handle *ctx, const char *status,
                                  const char *reason) {
    if (!ctx || !ctx->activeEpoch.active)
        return;
    sl_active_epoch_t epoch = ctx->activeEpoch;
    ctx->activeEpoch.active = 0;
    d27_log(ctx, "SYNC", epoch.generation, epoch.requestId,
            "SYNC_END epoch=%llu status=%s reason=%s target_ms=%lld "
            "desired=%s epoch_reason=%s",
            (unsigned long long)epoch.epoch, status ? status : "-",
            reason ? reason : "-", (long long)epoch.targetMs,
            epoch.desired[0] ? epoch.desired : "-",
            epoch.reason[0] ? epoch.reason : "-");
}

static void sync_begin_epoch_locked(mpv_handle *ctx, uint64_t gen, uint64_t req,
                                    const char *reason, int64_t targetMs,
                                    const char *desired) {
    if (!ctx)
        return;
    sync_end_epoch_locked(ctx, "superseded", "new_epoch");
    memset(&ctx->activeEpoch, 0, sizeof(ctx->activeEpoch));
    ctx->activeEpoch.epoch = ++ctx->epochSeq;
    ctx->activeEpoch.generation = gen;
    ctx->activeEpoch.requestId = req;
    ctx->activeEpoch.targetMs = targetMs;
    ctx->activeEpoch.targetResolved = targetMs != SL_SYNC_TARGET_PENDING;
    snprintf(ctx->activeEpoch.reason, sizeof(ctx->activeEpoch.reason), "%s",
             reason && *reason ? reason : "open");
    snprintf(ctx->activeEpoch.desired, sizeof(ctx->activeEpoch.desired), "%s",
             desired && *desired ? desired : SL_SYNC_DESIRED_PLAY);
    ctx->activeEpoch.active = 1;

    char target[32];
    if (targetMs == SL_SYNC_TARGET_PENDING)
        snprintf(target, sizeof(target), "pending");
    else
        snprintf(target, sizeof(target), "%lld", (long long)targetMs);
    d27_log(ctx, "SYNC", gen, req,
            "SYNC_BEGIN epoch=%llu target_ms=%s reason=%s desired=%s",
            (unsigned long long)ctx->activeEpoch.epoch, target,
            ctx->activeEpoch.reason, ctx->activeEpoch.desired);
}

/* 构造 meta 帧；把 D27 session kv 和 D28 sync kv 放进同一帧。
 * 无 session 时只写 sync kv，绝不伪造 run/session。 */
static int build_meta_frame_locked(mpv_handle *ctx, uint64_t gen, uint64_t req,
                                   const char *reason, int64_t targetMs,
                                   const char *desired, char *frame, size_t cap) {
    if (!ctx || !frame || cap == 0)
        return -1;
    sl_session_meta_t session;
    int hasSession = 0;
    pthread_mutex_lock(&ctx->metaMutex);
    hasSession = ctx->hasSessionMeta;
    if (hasSession) session = ctx->sessionMeta;
    pthread_mutex_unlock(&ctx->metaMutex);

    char sessionArgs[1024] = {0};
    char syncArgs[256] = {0};
    char args[1400] = {0};
    size_t sessionLen = hasSession
                            ? sl_session_meta_format_args(&session, sessionArgs,
                                                          sizeof(sessionArgs))
                            : 0;
    sl_sync_meta_t sync;
    sl_sync_meta_clear(&sync);
    sync.epoch = ctx->activeEpoch.epoch;
    sync.target_ms = targetMs;
    snprintf(sync.reason, sizeof(sync.reason), "%s",
             reason && *reason ? reason : "open");
    snprintf(sync.desired, sizeof(sync.desired), "%s",
             desired && *desired ? desired : SL_SYNC_DESIRED_PLAY);
    size_t syncLen = sl_sync_meta_format_args(&sync, syncArgs, sizeof(syncArgs));
    if (syncLen == 0)
        return -1;

    int n = 0;
    if (sessionLen > 0)
        n = snprintf(args, sizeof(args), "%s\t%s", sessionArgs, syncArgs);
    else
        n = snprintf(args, sizeof(args), "%s", syncArgs);
    if (n <= 0 || (size_t)n >= sizeof(args))
        return -1;
    n = snprintf(frame, cap, "%s\t%llu\t%llu\t%s", SL_CMD_META,
                 (unsigned long long)gen, (unsigned long long)req, args);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int send_meta_and_command_locked(mpv_handle *ctx, uint64_t gen,
                                        uint64_t req, const char *reason,
                                        int64_t targetMs, const char *desired,
                                        const char *commandFrame) {
    if (!ctx || !commandFrame)
        return -1;
    if (ctx->sock < 0)
        return -1;
    int fd = ctx->sock;
    sync_begin_epoch_locked(ctx, gen, req, reason, targetMs, desired);

    char metaFrame[2048];
    if (build_meta_frame_locked(ctx, gen, req, reason, targetMs, desired,
                                metaFrame, sizeof(metaFrame)) < 0) {
        sync_end_epoch_locked(ctx, "error", "meta_format");
        return -1;
    }

    pthread_mutex_lock(&ctx->txMutex);
    int rc = send_payload(fd, metaFrame, strlen(metaFrame));
    if (rc == 0)
        rc = send_payload(fd, commandFrame, strlen(commandFrame));
    pthread_mutex_unlock(&ctx->txMutex);
    if (rc != 0) {
        sync_end_epoch_locked(ctx, "error", "send_failed");
        return -1;
    }

    sl_session_meta_t meta;
    pthread_mutex_lock(&ctx->metaMutex);
    meta = ctx->sessionMeta;
    pthread_mutex_unlock(&ctx->metaMutex);
    d27_log(ctx, "CMD", gen, req,
            "meta gen=%llu req=%llu run=%s session=%s sync_epoch=%llu "
            "status=sent",
            (unsigned long long)gen, (unsigned long long)req,
            meta.run[0] ? meta.run : "-", meta.session[0] ? meta.session : "-",
            (unsigned long long)ctx->activeEpoch.epoch);
    return 0;
}

static int send_meta_and_command(mpv_handle *ctx, uint64_t gen, uint64_t req,
                                 const char *reason, int64_t targetMs,
                                 const char *desired, const char *commandFrame) {
    if (!ctx || !commandFrame)
        return -1;
    pthread_mutex_lock(&ctx->daemonMutex);
    int rc = send_meta_and_command_locked(ctx, gen, req, reason, targetMs,
                                          desired, commandFrame);
    pthread_mutex_unlock(&ctx->daemonMutex);
    return rc;
}

/* 不改变时间线的命令继续走 send_context_cmd；这里只服务带 sync meta 的控制命令。 */
static int send_sync_command(mpv_handle *ctx, const char *name,
                             const char *reason, int64_t targetMs,
                             const char *desired, const char *fmt, ...) {
    if (!ctx || !name)
        return MPV_ERROR_INVALID_PARAMETER;
    uint64_t gen = 0, req = 0;
    pthread_mutex_lock(&ctx->daemonMutex);
    gen = ctx->fileGeneration;
    req = ++ctx->requestSeq;
    ctx->lastCommandReq = req;
    pthread_mutex_unlock(&ctx->daemonMutex);

    char body[1024] = {0};
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
    }
    char frame[1200];
    int n = snprintf(frame, sizeof(frame), "%s\t%llu\t%llu\t%s", name,
                     (unsigned long long)gen, (unsigned long long)req, body);
    if (n <= 0 || (size_t)n >= sizeof(frame))
        return MPV_ERROR_GENERIC;
    return send_meta_and_command(ctx, gen, req, reason, targetMs, desired,
                                 frame) == 0
               ? MPV_ERROR_SUCCESS
               : MPV_ERROR_GENERIC;
}

/* 播放健康提示出口：软/硬卡顿共用，具体文案由 UI 根据 reason 选择。 */
static void fire_health_hint(mpv_handle *ctx, int reason) {
    int *flag = malloc(sizeof(int));
    if (!flag) return;
    *flag = reason;
    push_prop_event(ctx, SL_HEALTH_PROP, 0, MPV_FORMAT_FLAG, flag);
    fire_wakeup(ctx);
}

static const char *signalName(int sig) {
    switch (sig) {
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    default:      return "UNKNOWN";
    }
}

static void log_daemon_exit(mpv_handle *ctx, pid_t pid, int status,
                            const char *reason) {
    if (WIFEXITED(status)) {
        d27_log(ctx, "COMPAT", 0, 0,
                "daemon exit reason=%s pid=%d code=%d",
                reason ? reason : "-", (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        d27_log(ctx, "COMPAT", 0, 0,
                "daemon exit reason=%s pid=%d signal=%d(%s)%s",
                reason ? reason : "-", (int)pid, sig, signalName(sig),
                WCOREDUMP(status) ? " core" : "");
    } else {
        d27_log(ctx, "COMPAT", 0, 0,
                "daemon exit reason=%s pid=%d status=0x%x",
                reason ? reason : "-", (int)pid, status);
    }
}

/* open 命令格式：open<TAB>gen<TAB>req<TAB>url<TAB>referrer<TAB>start<TAB>end<TAB>audio... */
static double open_cmd_start(const char *cmd) {
    char work[8192];
    if (!cmd || strlen(cmd) >= sizeof(work)) return 0.0;
    snprintf(work, sizeof(work), "%s", cmd);
    char *fields[16];
    int n = split_fields(work, fields, 16);
    return n >= 6 ? atof(fields[5]) : 0.0;
}

/* watchdog 回放/备用源切换：替换代次、请求号和 start，保留 URL/音轨/end。 */
static int rewrite_open_frame(const char *src, char *dst, size_t cap,
                              uint64_t gen, uint64_t req, double seconds) {
    if (!src || !dst) return -1;
    size_t srcLen = strlen(src);
    char *work = (char *)malloc(srcLen + 1);
    char *out = (char *)malloc(srcLen + 256);
    if (!work || !out) {
        free(work);
        free(out);
        return -1;
    }
    memcpy(work, src, srcLen + 1);
    char *fields[16];
    int n = split_fields(work, fields, 16);
    if (n < 7) {
        free(work);
        free(out);
        return -1;
    }

    size_t outCap = srcLen + 256;
    int len = snprintf(out, outCap, "%s\t%llu\t%llu\t%s\t%s\t%.3f",
                       fields[0], (unsigned long long)gen,
                       (unsigned long long)req, fields[3], fields[4], seconds);
    if (len < 0 || (size_t)len >= outCap) {
        free(work);
        free(out);
        return -1;
    }
    for (int i = 6; i < n; i++) {
        int w = snprintf(out + len, outCap - (size_t)len, "\t%s", fields[i]);
        if (w < 0 || (size_t)len + (size_t)w >= outCap) {
            free(work);
            free(out);
            return -1;
        }
        len += w;
    }
    snprintf(dst, cap, "%s", out);
    free(work);
    free(out);
    return 0;
}

/* 调用者持有 daemonMutex。返回 0 表示本轮允许重启。 */
static int record_restart_locked(mpv_handle *ctx) {
    long long now = monotonicMs();
    int recent = 0;
    for (int i = 0; i < DAEMON_RESTART_MAX; i++) {
        if (ctx->restartTimes[i] > 0 && now - ctx->restartTimes[i] < DAEMON_RESTART_WINDOW_MS)
            recent++;
    }
    if (recent >= DAEMON_RESTART_MAX) {
        d27_log(ctx, "COMPAT", 0, 0,
                "restart limit reached count=%d window=%dms",
                recent, DAEMON_RESTART_WINDOW_MS);
        return -1;
    }
    ctx->restartTimes[ctx->restartCursor % DAEMON_RESTART_MAX] = now;
    ctx->restartCursor++;
    return 0;
}

/* 调用者持有 daemonMutex；旧进程可能已死，也可能只是 socket 异常断开。 */
static void reap_daemon_locked(mpv_handle *ctx, const char *reason, int maxWaitMs) {
    pid_t pid = ctx->daemonPid;
    if (pid <= 0) return;

    int status = 0;
    pid_t r = 0;
    for (int i = 0; i < maxWaitMs / 50; i++) {
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) break;
        usleep(50 * 1000);
    }
    if (r == 0) {
        d27_log(ctx, "COMPAT", 0, 0,
                "daemon reap timeout pid=%d send SIGKILL", (int)pid);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        r = pid;
    }
    if (r == pid) log_daemon_exit(ctx, pid, status, reason);
    else d27_log(ctx, "COMPAT", 0, 0,
                 "daemon reap failed pid=%d errno=%s",
                 (int)pid, strerror(errno));
    ctx->daemonPid = 0;
}

static int connect_daemon_socket(mpv_handle *ctx) {
    for (int attempt = 0; attempt < 50; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t pathLen = strlen(ctx->socketPath);
        if (pathLen >= sizeof(addr.sun_path)) { close(fd); return -1; }
        memcpy(addr.sun_path, ctx->socketPath, pathLen + 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        usleep(100 * 1000);
    }
    return -1;
}

/* 启动 daemon 并等 IPC 连接就绪。内部加锁，成功后 ctx->sock/daemonPid 均有效。 */
static int start_daemon_process(mpv_handle *ctx, const char *reason) {
    pthread_mutex_lock(&ctx->daemonMutex);
    if (ctx->shuttingDown) {
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }

    pid_t parentPid = getpid();
    char parentArg[32];
    snprintf(parentArg, sizeof(parentArg), "%d", (int)parentPid);
    pid_t pid = fork();
    if (pid == 0) {
        /* PDEATHSIG 可能在 exec 后、daemon 安装 handler 前到达；
         * 先在子进程中阻塞 SIGTERM，daemon 装好 handler 后再自行解除。 */
        sigset_t termSet;
        sigemptyset(&termSet);
        sigaddset(&termSet, SIGTERM);
        sigprocmask(SIG_BLOCK, &termSet, NULL);
        /* PDEATHSIG 语义是父线程死亡；设完后再由 daemon 用 getppid() 复核父进程。 */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parentPid) _exit(0);
        execl(ctx->daemonPath, ctx->daemonPath, "--expected-parent", parentArg,
              (char *)NULL);
        fprintf(stderr, "[mpv-compat] exec daemon failed: %s\n", strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        d27_log(ctx, "COMPAT", 0, 0, "fork daemon failed errno=%s", strerror(errno));
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }

    ctx->daemonPid = pid;
    ctx->daemonGeneration++;
    ctx->watchdogKilledGeneration = -1;
    long long now = monotonicMs();
    ctx->heartbeatReceivedMs = now;
    ctx->lastResumeMs = now;
    ctx->hbLastFrameTimeMs = 0;
    ctx->hbStage = ctx->hbPlaying = ctx->hbInTransition = ctx->hbVideoFrames = 0;
    ctx->hbWorkerAlive = 0;
    ctx->hbCurrentPaused = 0;
    ctx->hbTargetPaused = 0;
    ctx->hbSeeking = 0;
    ctx->hbValid = 0;
    ctx->hbPresentationMs = 0;
    ctx->hbVideoQueueMs = 0;
    ctx->hbNetworkErrorAgeMs = 0;
    ctx->hbBackpressure = 0;
    ctx->hbHealthValid = 0;
    ctx->healthGeneration = UINT64_MAX;
    ctx->healthHintShown = 0;
    ctx->healthBaselineValid = 0;

    int fd = connect_daemon_socket(ctx);
    if (fd < 0) {
    d27_log(ctx, "COMPAT", 0, 0, "connect daemon failed");
        kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        ctx->daemonPid = 0;
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }
    ctx->sock = fd;
    ctx->initialized = 1;
    d27_log(ctx, "COMPAT", (uint64_t)ctx->daemonGeneration, 0,
            "daemon started reason=%s pid=%d generation=%d",
            reason ? reason : "-", (int)pid, ctx->daemonGeneration);
    pthread_mutex_unlock(&ctx->daemonMutex);
    return 0;
}

/* 接收线程专用的断连恢复入口。返回 0 表示连接已恢复，可继续 recv。 */
static int recover_daemon_connection(mpv_handle *ctx) {
    pthread_mutex_lock(&ctx->daemonMutex);
    int gen = ctx->daemonGeneration;
    const char *reason = (ctx->watchdogKilledGeneration == gen)
                             ? "watchdog 强杀" : "IPC 断开";

    if (ctx->shuttingDown) {
        /* 正常退出要给 daemon 完成 stop → FreeStream → FreeContext 的时间，
         * 否则 SIGKILL 会留下 VPU 脏标记。 */
        d27_log(ctx, "COMPAT", (uint64_t)gen, 0,
                "shutdown waiting for daemon VPU cleanup");
        reap_daemon_locked(ctx, reason, DAEMON_REAP_TERMINATE_MS);
        if (ctx->sock >= 0) {
            close(ctx->sock);
            ctx->sock = -1;
        }
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }
    d27_log(ctx, "COMPAT", (uint64_t)gen, 0,
            "recovery enter reason=%s daemon_generation=%d", reason, gen);

    reap_daemon_locked(ctx, reason, DAEMON_REAP_RECOVERY_MS);
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
    if (record_restart_locked(ctx) < 0) {
        ctx->initialized = 0;
        transparency_disable(ctx);
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }

    int hasReplay = ctx->hasLastOpen;
    int expPause = ctx->expectedPause;
    double pos = ctx->resumePosition;
    /* watchdog 回放是新文件实例：递增代次，旧 daemon 的迟到事件全部失效。 */
    ctx->fileGeneration++;
    uint64_t replayGen = ctx->fileGeneration;
    uint64_t replayReq = ++ctx->requestSeq;
    ctx->hasInitialSeekTarget = 0;
    /* 新 daemon 从启动到首帧之间可能要清洗脏 VPU/重新开流，
     * 这段时间必须挂起 watchdog，否则可能被误判为卡死。PLAYBACK_START 再重新武装。 */
    ctx->playbackActive = 0;
    char replay[65536];
    if (hasReplay) snprintf(replay, sizeof(replay), "%s", ctx->lastOpenCmd);
    pthread_mutex_unlock(&ctx->daemonMutex);
    clear_queued_events(ctx);

    if (start_daemon_process(ctx, reason) < 0) {
        transparency_disable(ctx);
        return -1;
    }

    /* watchdog 强杀恢复时，UI 可能需要一个事件循环才把自动降级决定送到
     * compat。给一个短决策窗口，避免旧流 replay 抢先发出；非 hard 恢复不等待。 */
    if (hasReplay && strcmp(reason, "watchdog 强杀") == 0) {
        for (int i = 0; i < 30; ++i) {
            int decided = 0;
            pthread_mutex_lock(&ctx->daemonMutex);
            decided = ctx->suppressWatchdogReplay;
            pthread_mutex_unlock(&ctx->daemonMutex);
            if (decided) break;
            usleep(10 * 1000);
        }
    }

    if (hasReplay) {
        char restored[65536];
        const char *openCmd = replay;
        pthread_mutex_lock(&ctx->daemonMutex);
        uint64_t currentGen = ctx->fileGeneration;
        int suppressReplay = ctx->suppressWatchdogReplay;
        int newerLoadfile = currentGen != replayGen;
        if (suppressReplay || newerLoadfile) {
            ctx->suppressWatchdogReplay = 0;
            /* 新 loadfile 已经改写快照时保留它；只清掉旧代次的 replay 快照。 */
            if (!newerLoadfile) {
                ctx->hasLastOpen = 0;
                ctx->lastOpenCmd[0] = '\0';
            }
        }
        pthread_mutex_unlock(&ctx->daemonMutex);

        if (suppressReplay || newerLoadfile) {
            d27_log(ctx, "COMPAT", replayGen, replayReq,
                    "watchdog replay cancelled reason=%s",
                    suppressReplay ? "auto_quality_downgrade" : "newer_loadfile");
        } else {
            if (rewrite_open_frame(replay, restored, sizeof(restored),
                                   replayGen, replayReq, pos) == 0)
                openCmd = restored;
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->suppressFileLoadedOnce = 1; /* 避免 wiliwili 把恢复当成新文件而自动 resume */
            ctx->restorePausePending = expPause;
            ctx->resumePosition = pos;
            snprintf(ctx->lastOpenCmd, sizeof(ctx->lastOpenCmd), "%s", openCmd);
            pthread_mutex_unlock(&ctx->daemonMutex);
            d27_log(ctx, "COMPAT", replayGen, replayReq,
                    "replay open pos=%.3f pause=%d status=sent", pos, expPause);
            if (send_session_meta_and_open(ctx, replayGen, replayReq, openCmd,
                                           "watchdog_replay",
                                           (int64_t)llround(pos * 1000.0)) !=
                MPV_ERROR_SUCCESS)
                d27_log(ctx, "COMPAT", replayGen, replayReq,
                        "replay open status=error");
        }
    }
    fire_wakeup(ctx);
    return 0;
}

static void *watchdog_thread(void *arg) {
    mpv_handle *ctx = (mpv_handle *)arg;
    long long pollMs = envMs("SL_WATCHDOG_POLL_MS", WATCHDOG_POLL_MS_DEFAULT, 100, 5000);
    long long stallMs = envMs("SL_WATCHDOG_STALL_MS", WATCHDOG_STALL_MS_DEFAULT, 500, 30000);
    long long noHbMs = envMs("SL_WATCHDOG_NOHB_MS", WATCHDOG_NOHB_MS_DEFAULT, 1000, 60000);
    long long graceMs = envMs("SL_WATCHDOG_GRACE_MS", WATCHDOG_GRACE_MS_DEFAULT, 0, 10000);
    long long healthWindowMs = envMs("SL_HEALTH_WINDOW_MS", HEALTH_WINDOW_MS_DEFAULT,
                                     1000, 60000);
    long long healthProgressMs = envMs("SL_HEALTH_PROGRESS_MS", HEALTH_PROGRESS_MS_DEFAULT,
                                       1, 10000);
    long long healthNetGraceMs = envMs("SL_HEALTH_NET_GRACE_MS", HEALTH_NET_GRACE_MS_DEFAULT,
                                       0, 30000);
    int healthForce = getenv("SL_HEALTH_FORCE") && strcmp(getenv("SL_HEALTH_FORCE"), "1") == 0;
    const char *disable = getenv("SL_WATCHDOG_DISABLE");
    if (disable && strcmp(disable, "1") == 0) {
        d27_log(ctx, "COMPAT", 0, 0, "watchdog disabled by SL_WATCHDOG_DISABLE=1");
        return NULL;
    }
    d27_log(ctx, "COMPAT", 0, 0,
            "watchdog start poll=%lldms stall=%lldms nohb=%lldms grace=%lldms",
            pollMs, stallMs, noHbMs, graceMs);
    d27_log(ctx, "COMPAT", 0, 0,
            "health start window=%lldms progress=%lldms net_grace=%lldms force=%d",
            healthWindowMs, healthProgressMs, healthNetGraceMs, healthForce ? 1 : 0);

    while (1) {
        pthread_mutex_lock(&ctx->daemonMutex);
        int running = ctx->watchdogRunning && !ctx->shuttingDown;
        int active = ctx->playbackActive;
        int gen = ctx->daemonGeneration;
        uint64_t fileGen = 0;
        int hasDaemon = ctx->daemonPid > 0 && ctx->sock >= 0;
        pthread_mutex_unlock(&ctx->daemonMutex);
        if (!running) break;

        if (hasDaemon && active) {
            (void)send_context_cmd(ctx, SL_CMD_HEARTBEAT, "");
            usleep((useconds_t)(pollMs * 1000));

            long long now = monotonicMs();
            long long hbAge = 0, frameAge = -1, resumeAge = 0;
            int playing = 0, inTransition = 1, expPause = 1, pid = 0, curGen = 0;
            int workerAlive = 1, hbValid = 0;
            int hbHealthValid = 0;
            int hbVideoFrames = 0;
            double hbPresentationMs = 0.0;
            unsigned hbQueueMs = 0;
            long long hbNetErrorAgeMs = 0;
            int hbBackpressure = 0;
            uint64_t hbRequestId = 0;
            pthread_mutex_lock(&ctx->daemonMutex);
            if (ctx->watchdogRunning && !ctx->shuttingDown &&
                ctx->daemonGeneration == gen && ctx->daemonPid > 0) {
                fileGen = ctx->fileGeneration;
                curGen = ctx->daemonGeneration;
                pid = (int)ctx->daemonPid;
                hbAge = now - ctx->heartbeatReceivedMs;
                resumeAge = now - ctx->lastResumeMs;
                if (ctx->hbLastFrameTimeMs > 0)
                    frameAge = now - ctx->hbLastFrameTimeMs;
                playing = ctx->hbPlaying;
                inTransition = ctx->hbInTransition;
                expPause = ctx->expectedPause;
                workerAlive = ctx->hbWorkerAlive;
                hbValid = ctx->hbValid;
                hbHealthValid = ctx->hbHealthValid;
                hbVideoFrames = ctx->hbVideoFrames;
                hbPresentationMs = ctx->hbPresentationMs;
                hbQueueMs = ctx->hbVideoQueueMs;
                hbNetErrorAgeMs = ctx->hbNetworkErrorAgeMs;
                hbBackpressure = ctx->hbBackpressure;
                hbRequestId = ctx->hbRequestId;
                if (ctx->healthGeneration != fileGen) {
                    ctx->healthGeneration = fileGen;
                    ctx->healthHintShown = 0;
                    ctx->healthBaselineValid = 0;
                }
            }
            pthread_mutex_unlock(&ctx->daemonMutex);

            int noHeartbeat = 0;
            int workerDead = 0;
            int stalled = 0;
            if (curGen == gen && pid > 0) {
                noHeartbeat = hbAge > noHbMs;
                workerDead = hbValid && !workerAlive;
                stalled = frameAge > stallMs && playing && !inTransition &&
                          !expPause && resumeAge > graceMs;
                if (noHeartbeat || workerDead || stalled) {
                    pthread_mutex_lock(&ctx->daemonMutex);
                    if (ctx->daemonGeneration == gen && ctx->daemonPid > 0 &&
                        !ctx->shuttingDown && ctx->playbackActive) {
                        d27_log(ctx, "COMPAT", (uint64_t)gen, hbRequestId,
                                "watchdog stall pid=%d hb_age=%lldms frame_age=%lldms "
                                "playing=%d transition=%d pause=%d worker_alive=%d "
                                "bp=%d vq=%ums pos=%.1fms action=%s status=kill",
                                pid, hbAge, frameAge, playing, inTransition,
                                expPause, workerAlive, hbBackpressure, hbQueueMs,
                                hbPresentationMs,
                                noHeartbeat ? "no_heartbeat" :
                                (workerDead ? "worker_dead" : "no_frame"));
                        if (!ctx->healthHintShown) {
                            ctx->healthHintShown = 1;
                            d27_log(ctx, "COMPAT", (uint64_t)fileGen, hbRequestId,
                                    "health stall detected reason=hard "
                                    "frame_age=%lld action=%s -> hint shown",
                                    (long long)frameAge,
                                    noHeartbeat ? "no_heartbeat" :
                                    (workerDead ? "worker_dead" : "no_frame"));
                            fire_health_hint(ctx, SL_HEALTH_HINT_HARD);
                        }
                        kill((pid_t)pid, SIGKILL);
                        ctx->watchdogKilledGeneration = gen;
                        if (ctx->activeEpoch.active &&
                            ctx->activeEpoch.generation == fileGen)
                            sync_end_epoch_locked(ctx, "error", "watchdog");
                    }
                    pthread_mutex_unlock(&ctx->daemonMutex);
                }
            }

            /* D22：硬故障已由 watchdog 处理；软卡顿只提示一次，不动播放状态。 */
            int hardAction = noHeartbeat || workerDead || stalled;
            int healthEligible = !hardAction && curGen == gen && pid > 0 &&
                                 hbValid && hbHealthValid && workerAlive &&
                                 playing && !inTransition && !expPause &&
                                 resumeAge > graceMs && frameAge >= 0 &&
                                 frameAge < stallMs && hbVideoFrames > 0;
            if (healthEligible && hbNetErrorAgeMs >= 0 &&
                hbNetErrorAgeMs < healthNetGraceMs)
                healthEligible = 0;
            if (healthEligible) {
                if (!ctx->healthBaselineValid) {
                    ctx->healthBaselineValid = 1;
                    ctx->healthBaselineMs = now;
                    ctx->healthBaselinePresentationMs = hbPresentationMs;
                    ctx->healthBaselineFrames = hbVideoFrames;
                } else if (hbPresentationMs >=
                           ctx->healthBaselinePresentationMs + (double)healthProgressMs) {
                    ctx->healthBaselineMs = now;
                    ctx->healthBaselinePresentationMs = hbPresentationMs;
                    ctx->healthBaselineFrames = hbVideoFrames;
                } else if (!ctx->healthHintShown && hbVideoFrames > ctx->healthBaselineFrames &&
                           (now - ctx->healthBaselineMs >= healthWindowMs || healthForce)) {
                    d27_log(ctx, "COMPAT", (uint64_t)fileGen, hbRequestId,
                            "health stall detected reason=soft present=%.0f base=%.0f queue=%u "
                            "frames=%d frame_age=%lld net_age=%lld bp=%d force=%d -> hint shown",
                            hbPresentationMs, ctx->healthBaselinePresentationMs,
                            hbQueueMs, hbVideoFrames, frameAge,
                            hbNetErrorAgeMs, hbBackpressure, healthForce ? 1 : 0);
                    ctx->healthHintShown = 1;
                    fire_health_hint(ctx, SL_HEALTH_HINT_SOFT);
                }
            } else {
                ctx->healthBaselineValid = 0;
            }
        } else {
            usleep((useconds_t)(pollMs * 1000));
        }
    }
    d27_log(ctx, "COMPAT", 0, 0, "watchdog exit");
    return NULL;
}

/* ================= 接收线程 ================= */

/* 透明层运行时开关。
 * borealis 补丁 borealis-steamlink-alpha.patch 在 SDLVideoContext::clear() 里
 * 逐帧 getenv("SL_TRANSPARENT")，所以同进程内 setenv/unsetenv 当场生效，
 * 不需要重编 UI、也不需要额外 IPC。语义上只在「播放页」透明：
 * 首页/详情页保持不透明，避免露出没有视频的空视频平面（会呈黑底）。 */
static void transparency_enable(mpv_handle *ctx) {
    setenv("SL_TRANSPARENT", "1", 1);
    d27_log(ctx, "COMPAT", 0, 0, "transparent on");
}
static void transparency_disable(mpv_handle *ctx) {
    unsetenv("SL_TRANSPARENT");
    d27_log(ctx, "COMPAT", 0, 0, "transparent off");
}

/* 主源播放失败 → 依次尝试 wiliwili 追加的备用源（等价 mpv playlist 换项）。
 * 返回 1 表示已切到备用源，调用方不要再向 wiliwili 报错误事件。 */
static int try_failover(mpv_handle *ctx) {
    char cmd[8192];
    int has = 0;
    pthread_mutex_lock(&ctx->txMutex);
    if (ctx->backupCount > 0) {
        snprintf(cmd, sizeof(cmd), "%s", ctx->backup[0]);
        memmove(ctx->backup[0], ctx->backup[1],
                sizeof(ctx->backup[0]) * (size_t)(ctx->backupCount - 1));
        ctx->backupCount--;
        has = 1;
    }
    pthread_mutex_unlock(&ctx->txMutex);
    if (!has) return 0;
    /* 备用源是新文件实例：新代次隔离主源的迟到事件。 */
    pthread_mutex_lock(&ctx->daemonMutex);
    ctx->fileGeneration++;
    uint64_t failoverGen = ctx->fileGeneration;
    uint64_t failoverReq = ++ctx->requestSeq;
    double failoverStart = open_cmd_start(cmd);
    ctx->hasLastOpen = 1;
    ctx->playbackActive = 1;
    ctx->resumePosition = failoverStart;
    ctx->hasInitialSeekTarget = 0;
    ctx->suppressFileLoadedOnce = 1;
    pthread_mutex_unlock(&ctx->daemonMutex);

    char failoverCmd[8192];
    const char *openCmd = cmd;
    if (rewrite_open_frame(cmd, failoverCmd, sizeof(failoverCmd),
                           failoverGen, failoverReq, failoverStart) == 0)
        openCmd = failoverCmd;
    pthread_mutex_lock(&ctx->daemonMutex);
    snprintf(ctx->lastOpenCmd, sizeof(ctx->lastOpenCmd), "%s", openCmd);
    pthread_mutex_unlock(&ctx->daemonMutex);
    clear_queued_events(ctx);
    reset_props_for_new_file(ctx, "");
    d27_log(ctx, "COMPAT", failoverGen, failoverReq,
            "failover to backup gen=%llu req=%llu remaining=%d",
            (unsigned long long)failoverGen, (unsigned long long)failoverReq,
            ctx->backupCount);
    send_session_meta_and_open(ctx, failoverGen, failoverReq, openCmd,
                               "failover",
                               (int64_t)llround(failoverStart * 1000.0));
    return 1;
}

static void *rx_thread(void *arg) {
    mpv_handle *ctx = (mpv_handle *)arg;
    char *buf = malloc(SL_IPC_MAX_PAYLOAD + 1);
    if (!buf) return NULL;
    size_t len = 0;

    while (1) {
        pthread_mutex_lock(&ctx->daemonMutex);
        int running = ctx->rxRunning;
        int fd = ctx->sock;
        pthread_mutex_unlock(&ctx->daemonMutex);
        if (!running || fd < 0) break;

        if (recv_payload(fd, buf, SL_IPC_MAX_PAYLOAD + 1, &len) < 0) {
            if (recover_daemon_connection(ctx) < 0) break;
            continue;
        }
        char *fields[16] = {0};
        int nf = split_fields(buf, fields, 16);
        if (nf < SL_IPC_FRAME_ARGS_FIELD) continue;
        const char *evt = fields[0];
        uint64_t evtGen = strtoull(fields[SL_IPC_FRAME_FILE_GEN_FIELD], NULL, 10);
        uint64_t evtReq = strtoull(fields[SL_IPC_FRAME_REQUEST_FIELD], NULL, 10);
        uint64_t currentGen = 0;
        pthread_mutex_lock(&ctx->daemonMutex);
        currentGen = ctx->fileGeneration;
        pthread_mutex_unlock(&ctx->daemonMutex);
        if (evtGen != currentGen) {
            if (strcmp(evt, SL_EVT_PROGRESS) != 0 &&
                strcmp(evt, SL_EVT_HEARTBEAT) != 0) {
                d27_log(ctx, "COMPAT", evtGen, evtReq,
                        "drop stale event=%s current_gen=%llu", evt,
                        (unsigned long long)currentGen);
            }
            continue;
        }

        if (strcmp(evt, SL_EVT_OPENED) == 0) {
            /* mpv 事件时序：START_FILE → FILE_LOADED；duration 属性初值。
             * watchdog 恢复/备用源切换时 suppress，避免触发 wiliwili 的自动 resume。 */
            pthread_mutex_lock(&ctx->daemonMutex);
            int suppress = ctx->suppressFileLoadedOnce;
            ctx->suppressFileLoadedOnce = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            if (!suppress) {
                mpv_event *e = alloc_event(ctx, MPV_EVENT_START_FILE); (void)e;
            }
            double dur = nf >= 4 ? atof(fields[3]) : 0.0;
            /* wiliwili 观察 duration 用 MPV_FORMAT_INT64（mpv_core.cpp 第 400 行）*/
            int64_t *d = malloc(sizeof(int64_t)); *d = (int64_t)dur;
            update_prop_and_notify(ctx, "duration", MPV_FORMAT_INT64, d);
            if (!suppress) {
                mpv_event *e = alloc_event(ctx, MPV_EVENT_FILE_LOADED); (void)e;
            }
            d27_log(ctx, "EVT", evtGen, evtReq, "opened duration=%.3f status=ok", dur);
        } else if (strcmp(evt, SL_EVT_PLAYBACK_START) == 0) {
            int *idle = malloc(sizeof(int)); *idle = 0;
            update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, idle);
            mpv_event *e = alloc_event(ctx, MPV_EVENT_PLAYBACK_RESTART);
            (void)e;
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->playbackActive = 1;
            ctx->lastResumeMs = monotonicMs();
            ctx->heartbeatReceivedMs = ctx->lastResumeMs;
            ctx->hbLastFrameTimeMs = 0;
            ctx->hbBackpressure = 0;
            int restorePause = ctx->restorePausePending;
            ctx->restorePausePending = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            if (restorePause) send_context_cmd(ctx, SL_CMD_PAUSE, "");
            d27_log(ctx, "EVT", evtGen, evtReq, "playback start status=ok");
        } else if (strcmp(evt, SL_EVT_PROGRESS) == 0) {
            double pos = nf >= 4 ? atof(fields[3]) : 0.0;
            double dur = nf >= 5 ? atof(fields[4]) : 0.0;
            double *pt = malloc(sizeof(double)); *pt = pos;
            update_prop_and_notify(ctx, "playback-time", MPV_FORMAT_DOUBLE, pt);
            double *pp = malloc(sizeof(double));
            *pp = dur > 0 ? (pos / dur * 100.0) : 0.0;
            update_prop_and_notify(ctx, "percent-pos", MPV_FORMAT_DOUBLE, pp);
            mpv_event *e = alloc_event(ctx, MPV_EVENT_TICK); (void)e;
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->resumePosition = pos;
            pthread_mutex_unlock(&ctx->daemonMutex);
        } else if (strcmp(evt, SL_EVT_PAUSED) == 0) {
            int *v = malloc(sizeof(int)); *v = 1;
            update_prop_and_notify(ctx, "pause", MPV_FORMAT_FLAG, v);
            /* wiliwili 的按钮图标看 core-idle（MPV_IDLE → refreshToggleIcon），
             * 不看 pause 属性；暂停时同步置 true 才能显示播放三角。 */
            v = malloc(sizeof(int)); *v = 1;
            update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, v);
            d27_log(ctx, "EVT", evtGen, evtReq, "paused status=ok");
        } else if (strcmp(evt, SL_EVT_RESUMED) == 0) {
            int *v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "pause", MPV_FORMAT_FLAG, v);
            v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, v);
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->lastResumeMs = monotonicMs();
            pthread_mutex_unlock(&ctx->daemonMutex);
            d27_log(ctx, "EVT", evtGen, evtReq, "resumed status=ok");
        } else if (strcmp(evt, SL_EVT_SEEKED) == 0) {
            double pos = nf >= 4 ? atof(fields[3]) : 0.0;
            double dur = 0.0;
            pthread_mutex_lock(&ctx->propMutex);
            dur = (double)ctx->duration;
            pthread_mutex_unlock(&ctx->propMutex);
            double *pt = malloc(sizeof(double)); *pt = pos;
            update_prop_and_notify(ctx, "playback-time", MPV_FORMAT_DOUBLE, pt);
            double *pp = malloc(sizeof(double));
            *pp = dur > 0 ? (pos / dur * 100.0) : 0.0;
            update_prop_and_notify(ctx, "percent-pos", MPV_FORMAT_DOUBLE, pp);
            int *v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, v);
            mpv_event *e = alloc_event(ctx, MPV_EVENT_PLAYBACK_RESTART);
            (void)e;
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->resumePosition = pos;
            ctx->lastResumeMs = monotonicMs();
            pthread_mutex_unlock(&ctx->daemonMutex);
            d27_log(ctx, "EVT", evtGen, evtReq,
                    "seeked position=%.3f status=ok", pos);
        } else if (strcmp(evt, SL_EVT_EOF) == 0) {
            int *v = malloc(sizeof(int)); *v = 1;
            update_prop_and_notify(ctx, "eof-reached", MPV_FORMAT_FLAG, v);
            v = malloc(sizeof(int)); *v = 1;
            update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, v);
            v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, v);
            /* wiliwili 必须先看到 eof-reached，再看到 END_FILE，
             * 否则 video_stopped=true 会把 END_OF_FILE 条件拦截掉。 */
            mpv_event *e = alloc_event(ctx, MPV_EVENT_END_FILE);
            if (e) {
                mpv_event_end_file *end = calloc(1, sizeof(mpv_event_end_file));
                end->reason = MPV_END_FILE_REASON_EOF;
                e->data = end;
            }
            pthread_mutex_lock(&ctx->daemonMutex);
            sync_end_epoch_locked(ctx, "ok", "eof");
            ctx->playbackActive = 0;
            ctx->hasLastOpen = 0; /* 自然结束后 daemon 再死不应重开旧视频 */
            ctx->hasInitialSeekTarget = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            /* 播放结束：恢复不透明，避免 UI 透出没有视频的空视频平面（黑底） */
            transparency_disable(ctx);
            d27_log(ctx, "EVT", evtGen, evtReq, "eof status=ok");
        } else if (strcmp(evt, SL_EVT_ERROR) == 0 || strcmp(evt, SL_EVT_FATAL) == 0) {
            /* FrameSubmitStalled 只是送帧停滞的上报，播放器仍在尝试恢复。
             * 不能把它当播放结束，否则每次 fatal 都会关闭透明层并让 watchdog
             * 停止工作，用户看到白屏而底层继续空转。 */
            if (strcmp(evt, SL_EVT_FATAL) == 0 && nf >= 4 &&
                strcmp(fields[3], "FrameSubmitStalled") == 0) {
                d27_log(ctx, "COMPAT", evtGen, evtReq,
                        "ignore FrameSubmitStalled, keep watchdog active");
                continue;
            }
            /* 主源失败先试备用源；全失败才向 wiliwili 报 END_FILE(ERROR) */
            if (try_failover(ctx)) {
                fire_wakeup(ctx);
                continue;
            }
            int *idle = malloc(sizeof(int)); *idle = 1;
            update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, idle);
            int *seekingOff = malloc(sizeof(int)); *seekingOff = 0;
            update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, seekingOff);
            pthread_mutex_lock(&ctx->daemonMutex);
            sync_end_epoch_locked(ctx, "error", "playback_error");
            ctx->playbackActive = 0;
            ctx->hasLastOpen = 0;
            ctx->hasInitialSeekTarget = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            transparency_disable(ctx);
            mpv_event *e = alloc_event(ctx, MPV_EVENT_END_FILE);
            if (e) {
                mpv_event_end_file *end = calloc(1, sizeof(mpv_event_end_file));
                end->reason = MPV_END_FILE_REASON_ERROR;
                end->error = MPV_ERROR_GENERIC;
                e->data = end;
            }
            d27_log(ctx, "EVT", evtGen, evtReq, "error status=ok detail=%s",
                    nf >= 4 ? fields[3] : evt);
        } else if (strcmp(evt, SL_EVT_HEARTBEAT) == 0) {
            /* 心跳只喂 watchdog，不给 UI 生成事件，避免250/500ms 级 wakeup 洪泛。 */
            if (nf >= 12) {
                pthread_mutex_lock(&ctx->daemonMutex);
                ctx->hbLastFrameTimeMs = strtoll(fields[3], NULL, 10);
                ctx->hbStage = atoi(fields[4]);
                ctx->hbPlaying = atoi(fields[5]) ? 1 : 0;
                ctx->hbInTransition = atoi(fields[6]) ? 1 : 0;
                ctx->hbVideoFrames = atoi(fields[7]);
                ctx->hbCurrentPaused = atoi(fields[8]) ? 1 : 0;
                ctx->hbTargetPaused = atoi(fields[9]) ? 1 : 0;
                ctx->hbSeeking = atoi(fields[10]) ? 1 : 0;
                ctx->hbWorkerAlive = atoi(fields[11]) ? 1 : 0;
                ctx->hbPresentationMs = nf >= 13 ? atof(fields[12]) : 0.0;
                ctx->hbVideoQueueMs = nf >= 14 ? (unsigned)strtoul(fields[13], NULL, 10) : 0;
                ctx->hbNetworkErrorAgeMs =
                    nf >= 15 ? strtoll(fields[14], NULL, 10) : 0;
                ctx->hbBackpressure =
                    nf >= 16 ? (atoi(fields[15]) ? 1 : 0) : 0;
                ctx->hbHealthValid = nf >= 15 ? 1 : 0;
                ctx->hbRequestId = evtReq;
                ctx->hbValid = 1;
                ctx->heartbeatReceivedMs = monotonicMs();
                pthread_mutex_unlock(&ctx->daemonMutex);
            }
            continue;
        }
        fire_wakeup(ctx);
    }
    free(buf);
    return NULL;
}

/* ================= 公共 mpv API ================= */

unsigned long mpv_client_api_version(void) { return MPV_MAKE_VERSION(2, 2); }

const char *mpv_error_string(int error) {
    switch (error) {
    case MPV_ERROR_SUCCESS: return "success";
    case MPV_ERROR_EVENT_QUEUE_FULL: return "event queue full";
    case MPV_ERROR_INVALID_PARAMETER: return "invalid parameter";
    case MPV_ERROR_PROPERTY_UNAVAILABLE: return "property unavailable";
    default: return "mpv-compat error";
    }
}

void mpv_free(void *data) { free(data); }
void mpv_free_node_contents(mpv_node *node) { (void)node; }

mpv_handle *mpv_create(void) {
    mpv_handle *ctx = calloc(1, sizeof(mpv_handle));
    if (!ctx) return NULL;
    ctx->sock = -1;
    pthread_mutex_init(&ctx->daemonMutex, NULL);
    pthread_mutex_init(&ctx->qMutex, NULL);
    pthread_mutex_init(&ctx->cbMutex, NULL);
    pthread_mutex_init(&ctx->obsMutex, NULL);
    pthread_mutex_init(&ctx->propMutex, NULL);
    pthread_mutex_init(&ctx->txMutex, NULL);
    pthread_mutex_init(&ctx->metaMutex, NULL);
    ctx->backupCount = 0;
    ctx->volume = 100;
    ctx->speed = 1.0;
    ctx->coreIdle = 1;
    g_propDebug = getenv("SL_DANMAKU_DEBUG") != NULL;
    return ctx;
}

int mpv_initialize(mpv_handle *ctx) {
    if (!ctx) return MPV_ERROR_INVALID_PARAMETER;
    const char *path = getenv(SL_IPC_ENV_SOCKET);
    if (!path || !*path) path = SL_IPC_DEFAULT_SOCKET;

    const char *daemon = getenv("SL_DAEMON_PATH");
    if (!daemon || !*daemon) daemon = DAEMON_PATH_DEFAULT;
    snprintf(ctx->daemonPath, sizeof(ctx->daemonPath), "%s", daemon);
    snprintf(ctx->socketPath, sizeof(ctx->socketPath), "%s", path);

    if (start_daemon_process(ctx, "initialize") < 0) return MPV_ERROR_GENERIC;

    /* 启动接收线程 */
    ctx->rxRunning = 1;
    if (pthread_create(&ctx->rxThread, NULL, rx_thread, ctx) != 0) {
        ctx->rxRunning = 0;
        d27_log(ctx, "COMPAT", 0, 0, "rx thread create failed");
        send_context_cmd(ctx, SL_CMD_QUIT, "");
        pthread_mutex_lock(&ctx->daemonMutex);
        reap_daemon_locked(ctx, "initialize-failed", DAEMON_REAP_TERMINATE_MS);
        pthread_mutex_unlock(&ctx->daemonMutex);
        return MPV_ERROR_GENERIC;
    }
    ctx->rxThreadCreated = 1;

    /* 3.5c：watchdog 独立线程，不阻塞 UI 的 mpv_wait_event 循环 */
    ctx->watchdogRunning = 1;
    if (pthread_create(&ctx->watchdogThread, NULL, watchdog_thread, ctx) != 0) {
        ctx->watchdogRunning = 0;
        d27_log(ctx, "COMPAT", 0, 0,
                "watchdog thread create failed, continue without watchdog");
    } else {
        ctx->watchdogThreadCreated = 1;
    }
    return MPV_ERROR_SUCCESS;
}

void mpv_terminate_destroy(mpv_handle *ctx) {
    if (!ctx) return;
    transparency_disable(ctx); /* 进程退出前恢复不透明 */

    pthread_mutex_lock(&ctx->daemonMutex);
    ctx->shuttingDown = 1;
    ctx->rxRunning = 0;
    ctx->watchdogRunning = 0;
    pthread_mutex_unlock(&ctx->daemonMutex);

    if (ctx->sock >= 0) send_context_cmd(ctx, SL_CMD_QUIT, "");

    pthread_mutex_lock(&ctx->daemonMutex);
    if (ctx->sock >= 0) shutdown(ctx->sock, SHUT_RDWR); /* 唤醒阻塞中的 recv */
    pthread_mutex_unlock(&ctx->daemonMutex);

    if (ctx->rxThreadCreated) {
        pthread_join(ctx->rxThread, NULL);
        ctx->rxThreadCreated = 0;
    }
    if (ctx->watchdogThreadCreated) {
        pthread_join(ctx->watchdogThread, NULL);
        ctx->watchdogThreadCreated = 0;
    }

    pthread_mutex_lock(&ctx->daemonMutex);
    reap_daemon_locked(ctx, "terminate", DAEMON_REAP_TERMINATE_MS);
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
    ctx->initialized = 0;
    pthread_mutex_unlock(&ctx->daemonMutex);

    /* 清理队列里剩余事件 */
    for (int i = 0; i < EVT_QUEUE_MAX; i++) free_event(ctx->queue[i]);
    free_event(ctx->lastReturned);
    pthread_mutex_destroy(&ctx->daemonMutex);
    pthread_mutex_destroy(&ctx->qMutex);
    pthread_mutex_destroy(&ctx->cbMutex);
    pthread_mutex_destroy(&ctx->obsMutex);
    pthread_mutex_destroy(&ctx->propMutex);
    pthread_mutex_destroy(&ctx->txMutex);
    pthread_mutex_destroy(&ctx->metaMutex);
    free(ctx);
}

int mpv_set_option(mpv_handle *ctx, const char *name, mpv_format format, void *data) {
    (void)ctx; (void)name; (void)format; (void)data;
    return MPV_ERROR_SUCCESS;
}

int mpv_set_option_string(mpv_handle *ctx, const char *name, const char *data) {
    (void)ctx; (void)name; (void)data;
    return MPV_ERROR_SUCCESS;
}

int mpv_command_string(mpv_handle *ctx, const char *args) {
    (void)args;
    if (!ctx) return MPV_ERROR_INVALID_PARAMETER;
    /* wiliwili 只用 command_string("quit")；按终止处理 */
    return send_context_cmd(ctx, SL_CMD_QUIT, "");
}

/* 把 loadfile 的 extra 串解析成 daemon open 参数。
 * extra 形如: referrer="...",network-timeout=5,start=10,audio-file="url1",audio-file="url2"
 * 输出命令: open<TAB>url<TAB>referrer<TAB>start<TAB>end<TAB>audio1<TAB>audio2...
 */
static void build_open_cmd(const char *url, const char *method, const char *extra,
                            char *out, size_t cap) {
    char referrer[512] = "https://www.bilibili.com/";
    char start[32] = "0", end[32] = "0";
    char audios[8][4096]; int nAudio = 0;

    if (extra && *extra) {
        /* wiliwili 的 extra 形如 audio-file="https://...?a=1,b=2"；
         * B 站 URL 内部也会出现逗号，必须只把引号外的逗号当分隔符。 */
        char item[8192];
        size_t itemLen = 0;
        int inQuote = 0;
        for (const char *p = extra; *p; p++) {
            char c = *p;
            if (c == '"') {
                inQuote = !inQuote;
                continue; /* 去掉包裹值的引号，保留 URL 内容 */
            }
            if (c == ',' && !inQuote) {
                item[itemLen] = '\0';
                char *eq = strchr(item, '=');
                if (eq) {
                    *eq = '\0';
                    const char *key = item, *val = eq + 1;
                    if (strcmp(key, "referrer") == 0)
                        snprintf(referrer, sizeof(referrer), "%s", val);
                    else if (strcmp(key, "start") == 0)
                        snprintf(start, sizeof(start), "%s", val);
                    else if (strcmp(key, "end") == 0)
                        snprintf(end, sizeof(end), "%s", val);
                    else if (strcmp(key, "audio-file") == 0 && nAudio < 8)
                        snprintf(audios[nAudio++], sizeof(audios[0]), "%s", val);
                }
                itemLen = 0;
                continue;
            }
            if (itemLen + 1 < sizeof(item))
                item[itemLen++] = c;
        }
        if (itemLen) {
            item[itemLen] = '\0';
            char *eq = strchr(item, '=');
            if (eq) {
                *eq = '\0';
                const char *key = item, *val = eq + 1;
                if (strcmp(key, "referrer") == 0)
                    snprintf(referrer, sizeof(referrer), "%s", val);
                else if (strcmp(key, "start") == 0)
                    snprintf(start, sizeof(start), "%s", val);
                else if (strcmp(key, "end") == 0)
                    snprintf(end, sizeof(end), "%s", val);
                else if (strcmp(key, "audio-file") == 0 && nAudio < 8)
                    snprintf(audios[nAudio++], sizeof(audios[0]), "%s", val);
            }
        }
    }
    /* method = "replace" / "append"：append 是备用源，首条即主源 */
    (void)method;
    snprintf(out, cap, "%s\t%s\t%s\t%s\t%s", SL_CMD_OPEN, url, referrer, start, end);
    char *p = out + strlen(out);
    for (int i = 0; i < nAudio; i++) {
        size_t left = cap - (size_t)(p - out);
        if (left < 2) break;
        int w = snprintf(p, left, "\t%s", audios[i]);
        if (w > 0) p += w;
    }
}

/* 命令审计日志：把 wiliwili 实际下发的命令序列打出来，便于对照 mpv 语义排错。
 * 限长拼接，避免超长 URL 刷屏和无谓的 -Wformat-truncation 告警。 */
static void log_command(mpv_handle *ctx, const char **args) {
    char dbg[192];
    size_t off = 0;
    dbg[0] = '\0';
    for (int i = 0; args[i] && i < 6 && off + 2 < sizeof(dbg); i++) {
        if (i) dbg[off++] = ' ';
        const char *s = args[i];
        while (*s && off + 1 < sizeof(dbg)) dbg[off++] = *s++;
        dbg[off] = '\0';
    }
    uint64_t gen = 0, req = 0;
    pthread_mutex_lock(&ctx->daemonMutex);
    gen = ctx->fileGeneration;
    req = ctx->lastCommandReq;
    pthread_mutex_unlock(&ctx->daemonMutex);
    d27_log(ctx, "CMD", gen, req, "command %s", dbg);
}

/* 生命周期边界日志：与 daemon/SlPlayer 的单调时钟同尺度，便于对齐 VPU 调用顺序。 */
static void lifecycle_log(mpv_handle *ctx, uint64_t gen, uint64_t req,
                          const char *fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    d27_log(ctx, "COMPAT", gen, req, "%s", message);
}

/* D27 日志脱敏：URL 只保留 '?' 前的静态路径文件名；extra 只保留键名。
 * 禁止把 upsig/trid/buvid/audio-file URL 等写进统一日志。 */
static void safe_url_for_log(const char *url, char *out, size_t cap) {
    if (!url || !out || cap == 0) return;
    const char *end = strchr(url, '?');
    size_t scanLen = end ? (size_t)(end - url) : strlen(url);
    const char *base = url;
    for (size_t i = 0; i < scanLen; ++i) {
        if (url[i] == '/') base = url + i + 1;
    }
    size_t len = end ? (size_t)(end - base) : strlen(base);
    if (len >= cap) len = cap - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

static void safe_extra_keys_for_log(const char *extra, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!extra || !*extra) return;
    size_t off = 0;
    int inQuote = 0;
    char key[64];
    size_t keyLen = 0;
    for (const char *p = extra;; ++p) {
        char c = *p;
        if (c == '"') {
            inQuote = !inQuote;
        } else if ((c == ',' && !inQuote) || c == '\0') {
            char *eq = keyLen > 0 && keyLen < sizeof(key) ? strchr(key, '=') : NULL;
            if (eq) {
                *eq = '\0';
                int n = snprintf(out + off, cap - off, "%s%s", off ? "," : "", key);
                if (n > 0 && (size_t)n < cap - off) off += (size_t)n;
            }
            keyLen = 0;
            if (c == '\0') break;
        } else if (keyLen + 1 < sizeof(key)) {
            key[keyLen++] = c;
        }
    }
}

int mpv_command_async(mpv_handle *ctx, uint64_t reply_userdata, const char **args) {
    (void)reply_userdata;
    if (!ctx || !args || !args[0]) return MPV_ERROR_INVALID_PARAMETER;
    const char *cmd = args[0];

    if (strcmp(cmd, "loadfile") != 0 && strcmp(cmd, "set") != 0 &&
        strcmp(cmd, "stop") != 0 && strcmp(cmd, "quit") != 0 &&
        strcmp(cmd, "show-text") != 0 && strcmp(cmd, "change-list") != 0)
        log_command(ctx, args);

    if (strcmp(cmd, "loadfile") == 0) {
        /* args: loadfile, url, method, [0,] [extra] */
        const char *url = args[1] ? args[1] : "";
        const char *method = args[2] ? args[2] : "replace";
        const char *extra = NULL;
        for (int i = 3; args[i]; i++) {
            /* extra 是含 '=' 且不是纯 "0" 的串 */
            if (strcmp(args[i], "0") != 0 && strchr(args[i], '=')) { extra = args[i]; break; }
        }
        sl_session_meta_t session;
        int hasSession = sl_session_meta_parse_extra(extra, &session) == 1;
        double openStart = 0.0;
        char buf[65000];
        build_open_cmd(url, method, extra, buf, sizeof(buf));
        pthread_mutex_lock(&ctx->daemonMutex);
        uint64_t openGen = ctx->fileGeneration;
        uint64_t openReq = ++ctx->requestSeq;
        ctx->lastCommandReq = openReq;
        pthread_mutex_unlock(&ctx->daemonMutex);
        char frame[65536];
        /* build_open_cmd() 的首字段已是 "open"，信封只保留一次命令名。 */
        const char *openBody = strchr(buf, SL_IPC_FIELD_SEP);
        openBody = openBody ? openBody + 1 : buf;
        snprintf(frame, sizeof(frame), "%s\t%llu\t%llu\t%s", SL_CMD_OPEN,
                 (unsigned long long)openGen, (unsigned long long)openReq,
                 openBody);
        if (strcmp(method, "append") == 0) {
            /* mpv 语义：append 只入播放列表、不立即播放。
             * wiliwili 的 setBackupUrl() 用 append 追加备用 CDN；
             * 若这里直接 open，等于每个备用源都重启一次播放
             * （真机现象：一个视频连出 3 次 open，进度被反复打回 0）。 */
            pthread_mutex_lock(&ctx->txMutex);
            if (ctx->backupCount < (int)(sizeof(ctx->backup) / sizeof(ctx->backup[0])))
                memcpy(ctx->backup[ctx->backupCount++], frame,
                       sizeof(ctx->backup[0]));
            int queued = ctx->backupCount;
            pthread_mutex_unlock(&ctx->txMutex);
            char safeUrl[160];
            safe_url_for_log(url, safeUrl, sizeof(safeUrl));
            d27_log(ctx, "COMPAT", openGen, openReq,
                    "backup queued count=%d url=%s", queued, safeUrl);
            return MPV_ERROR_SUCCESS;
        }
        /* replace：新的主源，清空旧备用队列，并进入播放页透明模式 */
        pthread_mutex_lock(&ctx->txMutex);
        ctx->backupCount = 0;
        pthread_mutex_unlock(&ctx->txMutex);
        pthread_mutex_lock(&ctx->daemonMutex);
        ctx->fileGeneration++;
        openGen = ctx->fileGeneration;
        openReq = ++ctx->requestSeq;
        ctx->lastCommandReq = openReq;
        pthread_mutex_unlock(&ctx->daemonMutex);
        snprintf(frame, sizeof(frame), "%s\t%llu\t%llu\t%s", SL_CMD_OPEN,
                 (unsigned long long)openGen, (unsigned long long)openReq,
                 openBody);
        pthread_mutex_lock(&ctx->daemonMutex);
        snprintf(ctx->lastOpenCmd, sizeof(ctx->lastOpenCmd), "%s", frame);
        ctx->hasLastOpen = 1;
        ctx->playbackActive = 0; /* 首帧到达后才让 watchdog 接管 */
        ctx->expectedPause = 0;
        ctx->restorePausePending = 0;
        ctx->suppressFileLoadedOnce = 0;
        ctx->suppressWatchdogReplay = 0;
        ctx->resumePosition = open_cmd_start(frame);
        openStart = ctx->resumePosition;
        ctx->initialSeekTarget = ctx->resumePosition;
        ctx->hasInitialSeekTarget = ctx->initialSeekTarget > 0.0;
        ctx->lastResumeMs = monotonicMs();
        pthread_mutex_lock(&ctx->metaMutex);
        ctx->sessionMeta = session;
        ctx->hasSessionMeta = hasSession;
        pthread_mutex_unlock(&ctx->metaMutex);
        pthread_mutex_unlock(&ctx->daemonMutex);
        clear_queued_events(ctx);
        reset_props_for_new_file(ctx, url);
        transparency_enable(ctx);
        lifecycle_log(ctx, openGen, openReq,
                      "loadfile replace playbackActive=%d send open",
                      ctx->playbackActive);
        char safeUrl[160], safeKeys[256];
        safe_url_for_log(url, safeUrl, sizeof(safeUrl));
        safe_extra_keys_for_log(extra, safeKeys, sizeof(safeKeys));
        d27_log(ctx, "COMPAT", openGen, openReq,
                "loadfile url=%s extra_keys=%s content=%s id=%s bv=%s cid=%s qn=%s",
                safeUrl, safeKeys,
                hasSession && session.content[0] ? session.content : "-",
                hasSession && session.id[0] ? session.id : "-",
                hasSession && session.bv[0] ? session.bv : "-",
                hasSession && session.cid[0] ? session.cid : "-",
                hasSession && session.qn[0] ? session.qn : "-");
        return send_session_meta_and_open(ctx, openGen, openReq, frame, "open",
                                          (int64_t)llround(openStart * 1000.0));
    }
    if (strcmp(cmd, "set") == 0) {
        const char *prop = args[1] ? args[1] : "";
        const char *val = args[2] ? args[2] : "";
        if (strcmp(prop, "sl-health-auto-downgrade") == 0) {
            int enabled = strcmp(val, "yes") == 0 || strcmp(val, "true") == 0 ||
                          strcmp(val, "1") == 0;
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->suppressWatchdogReplay = enabled ? 1 : 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            uint64_t gen = 0, req = 0;
            pthread_mutex_lock(&ctx->daemonMutex);
            gen = ctx->fileGeneration;
            req = ctx->lastCommandReq;
            pthread_mutex_unlock(&ctx->daemonMutex);
            d27_log(ctx, "CMD", gen, req,
                    "set sl-health-auto-downgrade=%s status=sent",
                    enabled ? "yes" : "no");
            return MPV_ERROR_SUCCESS;
        }
        if (strcmp(prop, "pause") == 0) {
            int yes = strcmp(val, "yes") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0;
            /* 先更新期望态：daemon 若在事件上报前被杀，恢复时仍能让 UI 按钮一致。 */
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->expectedPause = yes;
            if (!yes) ctx->lastResumeMs = monotonicMs();
            pthread_mutex_unlock(&ctx->daemonMutex);
            /* S01：pause 开始新 epoch；resume 的 epoch 复用留到 S04。 */
            int rc = yes ? send_sync_command(ctx, SL_CMD_PAUSE, "pause_resume",
                                             SL_SYNC_TARGET_PENDING,
                                             SL_SYNC_DESIRED_PAUSE, "")
                         : send_context_cmd(ctx, SL_CMD_RESUME, "");
            uint64_t pauseGen = 0, pauseReq = 0;
            pthread_mutex_lock(&ctx->daemonMutex);
            pauseGen = ctx->fileGeneration;
            pauseReq = ctx->lastCommandReq;
            pthread_mutex_unlock(&ctx->daemonMutex);
            d27_log(ctx, "CMD", pauseGen, pauseReq,
                    "set pause=%s status=sent", yes ? "yes" : "no");
            return rc;
        }
        if (strcmp(prop, "volume") == 0) {
            return send_context_cmd(ctx, SL_CMD_VOLUME, "%s", val);
        }
        /* speed/brightness 等：3.5a 只缓存，不下发 daemon */
        pthread_mutex_lock(&ctx->propMutex);
        if (strcmp(prop, "speed") == 0) {
            double newSpeed = atof(val);
            ctx->speed = newSpeed;
            pthread_mutex_unlock(&ctx->propMutex);
            double *v = malloc(sizeof(double)); *v = newSpeed;
            update_prop_and_notify(ctx, "speed", MPV_FORMAT_DOUBLE, v);
            return MPV_ERROR_SUCCESS;
        }
        pthread_mutex_unlock(&ctx->propMutex);
        return MPV_ERROR_SUCCESS;
    }
    if (strcmp(cmd, "seek") == 0) {
        const char *value = args[1] ? args[1] : "0";
        const char *mode = args[2] ? args[2] : "absolute";
        double current = 0.0, duration = 0.0;
        pthread_mutex_lock(&ctx->propMutex);
        current = ctx->playbackTime; /* 只来自 daemon 的 progress/seeked */
        duration = ctx->duration;
        pthread_mutex_unlock(&ctx->propMutex);

        double target = atof(value);
        if (strcmp(mode, "relative") == 0) {
            target = current + target;
        } else if (strcmp(mode, "absolute-percent") == 0) {
            target = duration * target / 100.0;
        }
        if (target < 0.0) target = 0.0;
        if (duration > 0.0 && target > duration - 0.1)
            target = duration - 0.1;

        /* FILE_LOADED 后 wiliwili 会用 lastPlayedPosition 再 seek 一次；
         * 与 open 的 start= 等价时直接回执，避免第二次重建 VPU。
         * 0.05s 容差只用于吸收字符串/浮点误差，待真机校准。 */
        int duplicate = 0;
        pthread_mutex_lock(&ctx->daemonMutex);
        if (ctx->hasInitialSeekTarget && strcmp(mode, "absolute") == 0 &&
            fabs(target - ctx->initialSeekTarget) <= 0.05) {
            duplicate = 1;
            ctx->hasInitialSeekTarget = 0;
        }
        ctx->lastResumeMs = monotonicMs();
        pthread_mutex_unlock(&ctx->daemonMutex);
        if (duplicate) {
            d27_log(ctx, "CMD", ctx->fileGeneration, 0,
                    "seek target=%.3f status=duplicate", target);
            return MPV_ERROR_SUCCESS;
        }

        int *v = malloc(sizeof(int)); *v = 1;
        update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, v);
        int rc = send_sync_command(ctx, SL_CMD_SEEK, "seek",
                                   (int64_t)llround(target * 1000.0),
                                   SL_SYNC_DESIRED_PLAY, "%.3f", target);
        uint64_t seekGen = 0, seekReq = 0;
        pthread_mutex_lock(&ctx->daemonMutex);
        seekGen = ctx->fileGeneration;
        seekReq = ctx->lastCommandReq;
        pthread_mutex_unlock(&ctx->daemonMutex);
        d27_log(ctx, "CMD", seekGen, seekReq,
                "seek mode=%s target=%.3f current=%.3f duration=%.3f status=sent",
                mode, target, current, duration);
        if (rc != MPV_ERROR_SUCCESS) {
            v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, v);
        }
        return rc;
    }
    if (strcmp(cmd, "playlist-clear") == 0) {
        /* mpv 语义：**清空播放列表，但保留当前正在播放的项**。
         * wiliwili 在收到 FILE_LOADED 后立刻发它来丢弃备用链接（mpv_core.cpp:933），
         * 若这里当成 stop 下发，等于每个视频刚加载完就被停掉——
         * 真机现象：视频不开始播放、进度停在 0（且会顺手关掉播放页透明）。 */
        pthread_mutex_lock(&ctx->txMutex);
        int dropped = ctx->backupCount;
        ctx->backupCount = 0;
        pthread_mutex_unlock(&ctx->txMutex);
        d27_log(ctx, "COMPAT", ctx->fileGeneration, ctx->lastCommandReq,
                "playlist clear dropped=%d current playback continues", dropped);
        return MPV_ERROR_SUCCESS;
    }
    if (strcmp(cmd, "stop") == 0) {
        uint64_t stopGen = ctx->fileGeneration;
        uint64_t stopReq = ctx->lastCommandReq;
        lifecycle_log(ctx, stopGen, stopReq,
                      "stop enter playbackActive=%d", ctx->playbackActive);
        pthread_mutex_lock(&ctx->daemonMutex);
        sync_end_epoch_locked(ctx, "ok", "stop");
        pthread_mutex_unlock(&ctx->daemonMutex);
        pthread_mutex_lock(&ctx->txMutex);
        ctx->backupCount = 0;
        pthread_mutex_unlock(&ctx->txMutex);
        /* stop 必须按 daemon 当前代次发送；先递增会让命令被代次过滤丢弃。 */
        int rc = send_context_cmd(ctx, SL_CMD_STOP, "");
        pthread_mutex_lock(&ctx->daemonMutex);
        ctx->fileGeneration++;
        ctx->playbackActive = 0;
        ctx->hasLastOpen = 0;
        ctx->expectedPause = 0;
        ctx->hasInitialSeekTarget = 0;
        pthread_mutex_unlock(&ctx->daemonMutex);
        clear_queued_events(ctx);
        int *idle = malloc(sizeof(int)); *idle = 1;
        update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, idle);
        int *seekingOff = malloc(sizeof(int)); *seekingOff = 0;
        update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, seekingOff);
        transparency_disable(ctx); /* 离开播放：恢复不透明 UI */
        lifecycle_log(ctx, ctx->fileGeneration, stopReq,
                      "stop done rc=%d", rc);
        return rc;
    }
    if (strcmp(cmd, "quit") == 0) {
        lifecycle_log(ctx, ctx->fileGeneration, ctx->lastCommandReq,
                      "quit enter");
        transparency_disable(ctx);
        return send_context_cmd(ctx, SL_CMD_QUIT, "");
    }
    /* show-text / change-list / no-osd 等 UI 类命令：忽略 */
    return MPV_ERROR_SUCCESS;
}

int mpv_observe_property(mpv_handle *ctx, uint64_t reply_userdata, const char *name, mpv_format format) {
    if (!ctx) return MPV_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&ctx->obsMutex);
    if (ctx->obsCount < OBS_MAX) {
        snprintf(ctx->obs[ctx->obsCount].name, sizeof(ctx->obs[0].name), "%s", name);
        ctx->obs[ctx->obsCount].userdata = reply_userdata;
        ctx->obs[ctx->obsCount].fmt = format;
        ctx->obsCount++;
    }
    pthread_mutex_unlock(&ctx->obsMutex);
    /* mpv observes get an initial value event. wiliwili's video_speed defaults
     * to 0 until speed=1 arrives; without this, scrolling danmaku use factor 0
     * and never advance while playing. */
    if (strcmp(name, "speed") == 0 && format == MPV_FORMAT_DOUBLE) {
        double *v = malloc(sizeof(double));
        pthread_mutex_lock(&ctx->propMutex);
        *v = ctx->speed;
        pthread_mutex_unlock(&ctx->propMutex);
        update_prop_and_notify(ctx, "speed", MPV_FORMAT_DOUBLE, v);
    }
    return MPV_ERROR_SUCCESS;
}

static int prop_value_from_cache(mpv_handle *ctx, const char *name, mpv_format fmt, void *data) {
    pthread_mutex_lock(&ctx->propMutex);
    int rc = MPV_ERROR_SUCCESS;
    if (strcmp(name, "duration") == 0) {
        if (fmt == MPV_FORMAT_INT64) *(int64_t *)data = ctx->duration;
        else if (fmt == MPV_FORMAT_DOUBLE) *(double *)data = (double)ctx->duration;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "playback-time") == 0 || strcmp(name, "time-pos") == 0) {
        if (fmt == MPV_FORMAT_DOUBLE) *(double *)data = ctx->playbackTime;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "percent-pos") == 0) {
        if (fmt == MPV_FORMAT_DOUBLE) *(double *)data = ctx->percentPos;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "pause") == 0) {
        if (fmt == MPV_FORMAT_FLAG) *(int *)data = ctx->pause;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "eof-reached") == 0) {
        if (fmt == MPV_FORMAT_FLAG) *(int *)data = ctx->eofReached;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "core-idle") == 0) {
        if (fmt == MPV_FORMAT_FLAG) *(int *)data = ctx->coreIdle;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "seeking") == 0) {
        if (fmt == MPV_FORMAT_FLAG) *(int *)data = ctx->seeking;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "volume") == 0) {
        if (fmt == MPV_FORMAT_INT64) *(int64_t *)data = ctx->volume;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "speed") == 0) {
        if (fmt == MPV_FORMAT_DOUBLE) *(double *)data = ctx->speed;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else if (strcmp(name, "paused-for-cache") == 0) {
        if (fmt == MPV_FORMAT_FLAG) *(int *)data = 0;
        else rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    } else {
        rc = MPV_ERROR_PROPERTY_UNAVAILABLE;
    }
    pthread_mutex_unlock(&ctx->propMutex);
    return rc;
}

int mpv_get_property(mpv_handle *ctx, const char *name, mpv_format format, void *data) {
    if (!ctx || !name || !data) return MPV_ERROR_INVALID_PARAMETER;
    return prop_value_from_cache(ctx, name, format, data);
}

char *mpv_get_property_string(mpv_handle *ctx, const char *name) {
    if (!ctx) return strdup("");
    if (strcmp(name, "mpv-version") == 0) return strdup("mpv-compat 3.5c");
    if (strcmp(name, "ffmpeg-version") == 0) return strdup("n/a (SLVideo)");
    if (strcmp(name, "path") == 0) {
        pthread_mutex_lock(&ctx->propMutex);
        char *r = strdup(ctx->path);
        pthread_mutex_unlock(&ctx->propMutex);
        return r;
    }
    if (strcmp(name, "hwdec-current") == 0) return strdup("slvideo");
    return strdup("");
}

void mpv_set_wakeup_callback(mpv_handle *ctx, void (*cb)(void *), void *d) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->cbMutex);
    ctx->wakeupCb = cb;
    ctx->wakeupCtx = d;
    pthread_mutex_unlock(&ctx->cbMutex);
    if (cb) cb(d); /* 初次触发一次，让 wiliwili 消费初始事件 */
}

mpv_event *mpv_wait_event(mpv_handle *ctx, double timeout) {
    static mpv_event none_event;
    if (!ctx) return NULL;
    (void)timeout; /* 3.5a 只支持非阻塞取（wiliwili 传 0） */
    memset(&none_event, 0, sizeof(none_event));
    none_event.event_id = MPV_EVENT_NONE;

    pthread_mutex_lock(&ctx->qMutex);
    if (ctx->qCount == 0) {
        pthread_mutex_unlock(&ctx->qMutex);
        free_event(ctx->lastReturned); /* 队列空也释放上一事件（mpv 语义）*/
        ctx->lastReturned = NULL;
        return &none_event;
    }
    mpv_event *e = ctx->queue[ctx->qHead];
    ctx->queue[ctx->qHead] = NULL;
    ctx->qHead = (ctx->qHead + 1) % EVT_QUEUE_MAX;
    ctx->qCount--;
    free_event(ctx->lastReturned);
    ctx->lastReturned = e;
    pthread_mutex_unlock(&ctx->qMutex);
    return e;
}

/* ================= render context（no-op）================= */

int mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv, mpv_render_param *params) {
    (void)mpv; (void)params;
    if (!res) return MPV_ERROR_INVALID_PARAMETER;
    load_missing_gl_functions();
    *res = &g_render_ctx;
    return MPV_ERROR_SUCCESS;
}

void mpv_render_context_set_update_callback(mpv_render_context *ctx, mpv_render_update_fn callback, void *callback_ctx) {
    (void)ctx; (void)callback; (void)callback_ctx;
}

uint64_t mpv_render_context_update(mpv_render_context *ctx) { (void)ctx; return 0; }
int mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params) { (void)ctx; (void)params; return 0; }
void mpv_render_context_report_swap(mpv_render_context *ctx) { (void)ctx; }
void mpv_render_context_free(mpv_render_context *ctx) { (void)ctx; }

#ifdef SL_D28_TEST_HOOKS
/* 仅供 S01 协议测试：直接验证 active epoch 终态的幂等性。
 * 这些符号不会进入正式构建。 */
void sl_d28_test_end_active_epoch(mpv_handle *ctx, const char *status,
                                  const char *reason) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->daemonMutex);
    sync_end_epoch_locked(ctx, status, reason);
    pthread_mutex_unlock(&ctx->daemonMutex);
}

uint64_t sl_d28_test_active_epoch(mpv_handle *ctx) {
    if (!ctx) return 0;
    pthread_mutex_lock(&ctx->daemonMutex);
    uint64_t epoch = ctx->activeEpoch.active ? ctx->activeEpoch.epoch : 0;
    pthread_mutex_unlock(&ctx->daemonMutex);
    return epoch;
}
#endif
