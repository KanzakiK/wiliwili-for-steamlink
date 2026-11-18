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
static void transparency_disable(void);
static void update_prop_and_notify(mpv_handle *ctx, const char *name,
                                   mpv_format fmt, void *value);

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
#define DAEMON_RESTART_MAX 3
#define DAEMON_RESTART_WINDOW_MS 30000
/* 故障恢复可快速补杀；正常退出要给 daemon 完成 stop→FreeStream→FreeContext 的时间。 */
#define DAEMON_REAP_RECOVERY_MS 500
#define DAEMON_REAP_TERMINATE_MS 5000

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
    long long restartTimes[DAEMON_RESTART_MAX];
    int restartCursor;

    /* P0 文件代次与请求序号：与 daemonGeneration（进程代次）分开。
     * 每次 loadfile replace / 备用源切换 / watchdog 回放都递增文件代次；
     * 每个 pause/resume/seek 分配唯一请求号，完成事件原样回传。 */
    uint64_t fileGeneration;
    uint64_t requestSeq;
    /* 本文件 open 命令的 start= 目标，仅用于 FILE_LOADED 后等价 seek 去重。 */
    double initialSeekTarget;
    int hasInitialSeekTarget;

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

/* P0 统一命令信封：name<TAB>fileGen<TAB>requestId<TAB>可选参数。 */
static int send_context_cmd(mpv_handle *ctx, const char *name, const char *fmt, ...) {
    if (!ctx || !name) return MPV_ERROR_INVALID_PARAMETER;
    uint64_t gen = 0, req = 0;
    pthread_mutex_lock(&ctx->daemonMutex);
    gen = ctx->fileGeneration;
    req = ++ctx->requestSeq;
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

/* 新文件代次生效时丢弃上一文件尚未交付的 mpv 事件。
 * 不释放 lastReturned：UI 可能仍持有它的指针，按 mpv 语义由下一次 wait_event 释放。 */
static void clear_queued_events(mpv_handle *ctx) {
    pthread_mutex_lock(&ctx->qMutex);
    for (int i = 0; i < EVT_QUEUE_MAX; i++) {
        free_event(ctx->queue[i]);
        ctx->queue[i] = NULL;
    }
    ctx->qHead = 0;
    ctx->qCount = 0;
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
            fprintf(stderr, "[mpv-prop] %s=%d\n", name, *(int *)value ? 1 : 0);
        else if (fmt == MPV_FORMAT_DOUBLE)
            fprintf(stderr, "[mpv-prop] %s=%.3f\n", name, *(double *)value);
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

static void log_daemon_exit(pid_t pid, int status, const char *reason) {
    if (WIFEXITED(status)) {
        fprintf(stderr, "[mpv-compat] daemon 退出(reason=%s): pid=%d code=%d\n",
                reason ? reason : "-", (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr,
                "[mpv-compat] daemon 退出(reason=%s): pid=%d signal=%d(%s)%s\n",
                reason ? reason : "-", (int)pid, sig, signalName(sig),
                WCOREDUMP(status) ? " core" : "");
    } else {
        fprintf(stderr, "[mpv-compat] daemon 退出(reason=%s): pid=%d status=0x%x\n",
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
        fprintf(stderr,
                "[mpv-compat] daemon 重启过于频繁（%d 次/%dms），暂停自动重启\n",
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
        fprintf(stderr, "[mpv-compat] daemon 未按时退出，补发 SIGKILL: pid=%d\n", (int)pid);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        r = pid;
    }
    if (r == pid) log_daemon_exit(pid, status, reason);
    else fprintf(stderr, "[mpv-compat] daemon 回收失败: pid=%d errno=%s\n",
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
        fprintf(stderr, "[mpv-compat] fork daemon 失败: %s\n", strerror(errno));
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

    int fd = connect_daemon_socket(ctx);
    if (fd < 0) {
        fprintf(stderr, "[mpv-compat] connect daemon 失败\n");
        kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        ctx->daemonPid = 0;
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }
    ctx->sock = fd;
    ctx->initialized = 1;
    fprintf(stderr, "[mpv-compat] daemon 已启动(reason=%s): pid=%d generation=%d\n",
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
        fprintf(stderr, "[mpv-compat] 正常终止中，等待 daemon 清理 VPU\n");
        reap_daemon_locked(ctx, reason, DAEMON_REAP_TERMINATE_MS);
        if (ctx->sock >= 0) {
            close(ctx->sock);
            ctx->sock = -1;
        }
        pthread_mutex_unlock(&ctx->daemonMutex);
        return -1;
    }
    fprintf(stderr, "[mpv-compat] IPC 接收断开，启动恢复流程(reason=%s, generation=%d)\n",
            reason, gen);

    reap_daemon_locked(ctx, reason, DAEMON_REAP_RECOVERY_MS);
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
    if (record_restart_locked(ctx) < 0) {
        ctx->initialized = 0;
        transparency_disable();
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
        transparency_disable();
        return -1;
    }

    if (hasReplay) {
        char restored[65536];
        const char *openCmd = replay;
        if (rewrite_open_frame(replay, restored, sizeof(restored),
                               replayGen, replayReq, pos) == 0)
            openCmd = restored;
        pthread_mutex_lock(&ctx->daemonMutex);
        ctx->suppressFileLoadedOnce = 1; /* 避免 wiliwili 把恢复当成新文件而自动 resume */
        ctx->restorePausePending = expPause;
        ctx->resumePosition = pos;
        snprintf(ctx->lastOpenCmd, sizeof(ctx->lastOpenCmd), "%s", openCmd);
        pthread_mutex_unlock(&ctx->daemonMutex);
        fprintf(stderr, "[mpv-compat] 恢复播放: gen=%llu req=%llu pos=%.3f pause=%d\n",
                (unsigned long long)replayGen, (unsigned long long)replayReq,
                pos, expPause);
        if (send_cmd(ctx, openCmd) != MPV_ERROR_SUCCESS)
            fprintf(stderr, "[mpv-compat] 恢复播放的 open 命令发送失败\n");
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
    const char *disable = getenv("SL_WATCHDOG_DISABLE");
    if (disable && strcmp(disable, "1") == 0) {
        fprintf(stderr, "[mpv-compat] watchdog 已由 SL_WATCHDOG_DISABLE=1 关闭\n");
        return NULL;
    }
    fprintf(stderr,
            "[mpv-compat] watchdog 启动: poll=%lldms stall=%lldms nohb=%lldms grace=%lldms\n",
            pollMs, stallMs, noHbMs, graceMs);

    while (1) {
        pthread_mutex_lock(&ctx->daemonMutex);
        int running = ctx->watchdogRunning && !ctx->shuttingDown;
        int active = ctx->playbackActive;
        int gen = ctx->daemonGeneration;
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
            pthread_mutex_lock(&ctx->daemonMutex);
            if (ctx->watchdogRunning && !ctx->shuttingDown &&
                ctx->daemonGeneration == gen && ctx->daemonPid > 0) {
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
            }
            pthread_mutex_unlock(&ctx->daemonMutex);

            if (curGen == gen && pid > 0) {
                int noHeartbeat = hbAge > noHbMs;
                int workerDead = hbValid && !workerAlive;
                int stalled = frameAge > stallMs && playing && !inTransition &&
                              !expPause && resumeAge > graceMs;
                if (noHeartbeat || workerDead || stalled) {
                    pthread_mutex_lock(&ctx->daemonMutex);
                    if (ctx->daemonGeneration == gen && ctx->daemonPid > 0 &&
                        !ctx->shuttingDown && ctx->playbackActive) {
                        fprintf(stderr,
                                "[mpv-compat] watchdog 判定卡死: gen=%d pid=%d "
                                "hbAge=%lldms frameAge=%lldms playing=%d transition=%d "
                                "pause=%d workerAlive=%d action=%s\n",
                                gen, pid, hbAge, frameAge, playing, inTransition, expPause,
                                workerAlive,
                                noHeartbeat ? "无心跳" :
                                (workerDead ? "播放线程已退出" : "无新帧"));
                        kill((pid_t)pid, SIGKILL);
                        ctx->watchdogKilledGeneration = gen;
                    }
                    pthread_mutex_unlock(&ctx->daemonMutex);
                }
            }
        } else {
            usleep((useconds_t)(pollMs * 1000));
        }
    }
    fprintf(stderr, "[mpv-compat] watchdog 退出\n");
    return NULL;
}

/* ================= 接收线程 ================= */

/* 透明层运行时开关。
 * borealis 补丁 borealis-steamlink-alpha.patch 在 SDLVideoContext::clear() 里
 * 逐帧 getenv("SL_TRANSPARENT")，所以同进程内 setenv/unsetenv 当场生效，
 * 不需要重编 UI、也不需要额外 IPC。语义上只在「播放页」透明：
 * 首页/详情页保持不透明，避免露出没有视频的空视频平面（会呈黑底）。 */
static void transparency_enable(void) {
    setenv("SL_TRANSPARENT", "1", 1);
    fprintf(stderr, "[mpv-compat] 播放页透明开启（UI clear alpha=0）\n");
}
static void transparency_disable(void) {
    unsetenv("SL_TRANSPARENT");
    fprintf(stderr, "[mpv-compat] 透明关闭（UI 恢复不透明）\n");
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
    fprintf(stderr, "[mpv-compat] 主源失败，切换备用源: gen=%llu req=%llu（剩余 %d）\n",
            (unsigned long long)failoverGen, (unsigned long long)failoverReq,
            ctx->backupCount);
    send_cmd(ctx, openCmd);
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
        char *fields[16];
        int nf = split_fields(buf, fields, 16);
        if (nf < SL_IPC_FRAME_ARGS_FIELD) continue;
        const char *evt = fields[0];
        uint64_t evtGen = strtoull(fields[SL_IPC_FRAME_FILE_GEN_FIELD], NULL, 10);
        uint64_t evtReq = strtoull(fields[SL_IPC_FRAME_REQUEST_FIELD], NULL, 10);
        (void)evtReq; /* 当前兼容层按事件类型消费，请求号用于 daemon 回传链自检 */
        uint64_t currentGen = 0;
        pthread_mutex_lock(&ctx->daemonMutex);
        currentGen = ctx->fileGeneration;
        pthread_mutex_unlock(&ctx->daemonMutex);
        if (evtGen != currentGen) {
            if (strcmp(evt, SL_EVT_PROGRESS) != 0 &&
                strcmp(evt, SL_EVT_HEARTBEAT) != 0) {
                fprintf(stderr,
                        "[mpv-compat] 丢弃过期文件事件: %s gen=%llu current=%llu\n",
                        evt, (unsigned long long)evtGen,
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
            int restorePause = ctx->restorePausePending;
            ctx->restorePausePending = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            if (restorePause) send_context_cmd(ctx, SL_CMD_PAUSE, "");
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
        } else if (strcmp(evt, SL_EVT_RESUMED) == 0) {
            int *v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "pause", MPV_FORMAT_FLAG, v);
            v = malloc(sizeof(int)); *v = 0;
            update_prop_and_notify(ctx, "core-idle", MPV_FORMAT_FLAG, v);
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->lastResumeMs = monotonicMs();
            pthread_mutex_unlock(&ctx->daemonMutex);
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
            ctx->playbackActive = 0;
            ctx->hasLastOpen = 0; /* 自然结束后 daemon 再死不应重开旧视频 */
            ctx->hasInitialSeekTarget = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            /* 播放结束：恢复不透明，避免 UI 透出没有视频的空视频平面（黑底） */
            transparency_disable();
        } else if (strcmp(evt, SL_EVT_ERROR) == 0 || strcmp(evt, SL_EVT_FATAL) == 0) {
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
            ctx->playbackActive = 0;
            ctx->hasLastOpen = 0;
            ctx->hasInitialSeekTarget = 0;
            pthread_mutex_unlock(&ctx->daemonMutex);
            transparency_disable();
            mpv_event *e = alloc_event(ctx, MPV_EVENT_END_FILE);
            if (e) {
                mpv_event_end_file *end = calloc(1, sizeof(mpv_event_end_file));
                end->reason = MPV_END_FILE_REASON_ERROR;
                end->error = MPV_ERROR_GENERIC;
                e->data = end;
            }
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
        fprintf(stderr, "[mpv-compat] 接收线程创建失败\n");
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
        fprintf(stderr, "[mpv-compat] watchdog 线程创建失败，继续无 watchdog 运行\n");
    } else {
        ctx->watchdogThreadCreated = 1;
    }
    return MPV_ERROR_SUCCESS;
}

void mpv_terminate_destroy(mpv_handle *ctx) {
    if (!ctx) return;
    transparency_disable(); /* 进程退出前恢复不透明，避免残留透明状态影响下次 UI */

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
static void log_command(const char **args) {
    char dbg[192];
    size_t off = 0;
    dbg[0] = '\0';
    for (int i = 0; args[i] && i < 6 && off + 2 < sizeof(dbg); i++) {
        if (i) dbg[off++] = ' ';
        const char *s = args[i];
        while (*s && off + 1 < sizeof(dbg)) dbg[off++] = *s++;
        dbg[off] = '\0';
    }
    fprintf(stderr, "[mpv-compat] cmd: %s\n", dbg);
}

/* 生命周期边界日志：与 daemon/SlPlayer 的单调时钟同尺度，便于对齐 VPU 调用顺序。 */
static void lifecycle_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[mpv-compat][life] %lld ", monotonicMs());
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

int mpv_command_async(mpv_handle *ctx, uint64_t reply_userdata, const char **args) {
    (void)reply_userdata;
    if (!ctx || !args || !args[0]) return MPV_ERROR_INVALID_PARAMETER;
    const char *cmd = args[0];

    if (strcmp(cmd, "loadfile") != 0 && strcmp(cmd, "show-text") != 0 &&
        strcmp(cmd, "change-list") != 0)
        log_command(args);

    if (strcmp(cmd, "loadfile") == 0) {
        /* args: loadfile, url, method, [0,] [extra] */
        const char *url = args[1] ? args[1] : "";
        const char *method = args[2] ? args[2] : "replace";
        const char *extra = NULL;
        for (int i = 3; args[i]; i++) {
            /* extra 是含 '=' 且不是纯 "0" 的串 */
            if (strcmp(args[i], "0") != 0 && strchr(args[i], '=')) { extra = args[i]; break; }
        }
        char buf[65000];
        build_open_cmd(url, method, extra, buf, sizeof(buf));
        pthread_mutex_lock(&ctx->daemonMutex);
        uint64_t openGen = ctx->fileGeneration;
        uint64_t openReq = ++ctx->requestSeq;
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
            fprintf(stderr, "[mpv-compat] 备用源入队(%d): %.70s\n", queued, url);
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
        ctx->resumePosition = open_cmd_start(frame);
        ctx->initialSeekTarget = ctx->resumePosition;
        ctx->hasInitialSeekTarget = ctx->initialSeekTarget > 0.0;
        ctx->lastResumeMs = monotonicMs();
        pthread_mutex_unlock(&ctx->daemonMutex);
        clear_queued_events(ctx);
        reset_props_for_new_file(ctx, url);
        transparency_enable();
        lifecycle_log("loadfile replace gen=%llu req=%llu playbackActive=%d send open",
                      (unsigned long long)openGen, (unsigned long long)openReq,
                      ctx->playbackActive);
        const char *mediaTag = getenv("SL_MEDIA_TAG");
        fprintf(stderr, "[mpv-compat] loadfile: gen=%llu req=%llu tag=%s %s\n",
                (unsigned long long)openGen, (unsigned long long)openReq,
                mediaTag ? mediaTag : "-", url);
        return send_cmd(ctx, frame);
    }
    if (strcmp(cmd, "set") == 0) {
        const char *prop = args[1] ? args[1] : "";
        const char *val = args[2] ? args[2] : "";
        if (strcmp(prop, "pause") == 0) {
            int yes = strcmp(val, "yes") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0;
            /* 先更新期望态：daemon 若在事件上报前被杀，恢复时仍能让 UI 按钮一致。 */
            pthread_mutex_lock(&ctx->daemonMutex);
            ctx->expectedPause = yes;
            if (!yes) ctx->lastResumeMs = monotonicMs();
            pthread_mutex_unlock(&ctx->daemonMutex);
            return send_context_cmd(ctx, yes ? SL_CMD_PAUSE : SL_CMD_RESUME, "");
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
            fprintf(stderr,
                    "[mpv-compat] seek 与 start= 等价，跳过重复定位: %.3f s\n",
                    target);
            return MPV_ERROR_SUCCESS;
        }

        int *v = malloc(sizeof(int)); *v = 1;
        update_prop_and_notify(ctx, "seeking", MPV_FORMAT_FLAG, v);
        int rc = send_context_cmd(ctx, SL_CMD_SEEK, "%.3f", target);
        fprintf(stderr, "[mpv-compat] seek %s -> %.3f s (current=%.3f duration=%.3f)\n",
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
        fprintf(stderr, "[mpv-compat] playlist-clear：丢弃 %d 个备用源，当前播放继续\n", dropped);
        return MPV_ERROR_SUCCESS;
    }
    if (strcmp(cmd, "stop") == 0) {
        lifecycle_log("stop enter gen=%llu playbackActive=%d",
                      (unsigned long long)ctx->fileGeneration, ctx->playbackActive);
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
        transparency_disable(); /* 离开播放：恢复不透明 UI */
        lifecycle_log("stop done rc=%d gen=%llu", rc,
                      (unsigned long long)ctx->fileGeneration);
        return rc;
    }
    if (strcmp(cmd, "quit") == 0) {
        lifecycle_log("quit enter gen=%llu", (unsigned long long)ctx->fileGeneration);
        transparency_disable();
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
