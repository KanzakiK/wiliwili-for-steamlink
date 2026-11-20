/*
 * D27/D28 compat 发送端集成测试。
 *
 * 子进程是假 daemon：只接收并打印 IPC 帧，不打开 SlPlayer、不碰 VPU。
 * 父进程真实调用 mpv_create/mpv_initialize/mpv_command_async(loadfile)，
 * 验证 meta 是否紧跟 open 发出，且带同一 generation/request；同时验证
 * D28 sync meta 可独立存在，并发普通命令不会插到 meta/open 之间。
 */
#include <mpv/client.h>

#include "IpcProtocol.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* mpv_stub.c 引用这些 GL/SDL 桩符号；本测试不渲染。 */
void *SDL_GL_GetProcAddress(const char *proc) { (void)proc; return NULL; }
void *glad_glBindFramebuffer = NULL;
void *glad_glGenFramebuffers = NULL;
void *glad_glFramebufferTexture2D = NULL;
void *glad_glCheckFramebufferStatus = NULL;
void *glad_glDeleteFramebuffers = NULL;

#ifdef SL_D28_TEST_HOOKS
extern void sl_d28_test_end_active_epoch(mpv_handle *ctx, const char *status,
                                         const char *reason);
extern uint64_t sl_d28_test_active_epoch(mpv_handle *ctx);
extern int64_t sl_d28_test_active_epoch_target(mpv_handle *ctx);
#endif

#define MAX_FRAMES 64
#define FRAME_CAP 16384

static int frame_ident(const char *frame, char *name, size_t nameCap,
                       char *gen, size_t genCap, char *req, size_t reqCap) {
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "%%%zu[^\t]\t%%%zu[^\t]\t%%%zu[^\t]",
             nameCap - 1, genCap - 1, reqCap - 1);
    return sscanf(frame, fmt, name, gen, req) == 3 ? 0 : -1;
}

static void send_event_frame(int fd, const char *name, const char *gen,
                             const char *req, const char *args) {
    char payload[256];
    int n = snprintf(payload, sizeof(payload), "%s\t%s\t%s\t%s", name, gen, req,
                     args);
    if (n <= 0 || (size_t)n >= sizeof(payload))
        return;
    unsigned char h[SL_IPC_HEADER_SIZE];
    uint32_t len = (uint32_t)n;
    h[0] = len & 0xff;
    h[1] = (len >> 8) & 0xff;
    h[2] = (len >> 16) & 0xff;
    h[3] = (len >> 24) & 0xff;
    if (send(fd, h, sizeof(h), MSG_NOSIGNAL) == (ssize_t)sizeof(h))
        (void)send(fd, payload, (size_t)n, MSG_NOSIGNAL);
}

static int validate_frames(char frames[][FRAME_CAP], int count) {
    int metas = 0;
    int seekFrames = 0;
    for (int i = 0; i < count; ++i) {
        if (strncmp(frames[i], "seek\t", 5) == 0) {
            ++seekFrames;
            if (strstr(frames[i], "\t30.000") == NULL) {
                fprintf(stderr, "FAIL: unexpected seek frame at %d\n", i);
                return 1;
            }
        }
        if (strncmp(frames[i], "meta\t", 5) != 0)
            continue;
        if (i + 1 >= count ||
            (strncmp(frames[i + 1], "open\t", 5) != 0 &&
             strncmp(frames[i + 1], "pause\t", 6) != 0 &&
             strncmp(frames[i + 1], "resume\t", 7) != 0 &&
             strncmp(frames[i + 1], "seek\t", 5) != 0)) {
            fprintf(stderr, "FAIL: meta frame %d is not followed by a sync command\n", i);
            return 1;
        }
        char name[32], gen[32], req[32];
        char cmdName[32], cmdGen[32], cmdReq[32];
        if (frame_ident(frames[i], name, sizeof(name), gen, sizeof(gen), req,
                        sizeof(req)) < 0 ||
            frame_ident(frames[i + 1], cmdName, sizeof(cmdName), cmdGen,
                        sizeof(cmdGen), cmdReq, sizeof(cmdReq)) < 0 ||
            strcmp(gen, cmdGen) != 0 || strcmp(req, cmdReq) != 0) {
            fprintf(stderr, "FAIL: meta/command identity mismatch at frame %d\n", i);
            return 1;
        }
        if (!strstr(frames[i], "epoch=") ||
            !strstr(frames[i], "target_ms=")) {
            fprintf(stderr, "FAIL: meta frame %d lacks sync fields\n", i);
            return 1;
        }
        if (metas == 0) {
            if (!strstr(frames[i], "run=compat-probe") ||
                !strstr(frames[i], "epoch=1") ||
                !strstr(frames[i], "target_ms=0")) {
                fprintf(stderr, "FAIL: first meta lacks D27 session\n");
                return 1;
            }
        } else if (metas == 1) {
            if (strstr(frames[i], "run=compat-probe") ||
                !strstr(frames[i], "epoch=2") ||
                !strstr(frames[i], "target_ms=5123")) {
                fprintf(stderr, "FAIL: second meta must be sync-only\n");
                return 1;
            }
        } else if (metas == 2) {
            if (!strstr(frames[i], "epoch=3") ||
                !strstr(frames[i], "target_ms=-1") ||
                !strstr(frames[i], "reason=pause_resume") ||
                !strstr(frames[i], "desired=pause") ||
                strcmp(cmdName, SL_CMD_PAUSE) != 0) {
                fprintf(stderr, "FAIL: pause meta contract\n");
                return 1;
            }
        } else if (metas == 3) {
            if (!strstr(frames[i], "epoch=4") ||
                !strstr(frames[i], "target_ms=30000") ||
                !strstr(frames[i], "reason=seek") ||
                !strstr(frames[i], "desired=pause") ||
                strcmp(cmdName, SL_CMD_SEEK) != 0) {
                fprintf(stderr, "FAIL: paused seek meta contract\n");
                return 1;
            }
        } else if (metas == 4) {
            if (!strstr(frames[i], "epoch=4") ||
                !strstr(frames[i], "target_ms=30000") ||
                !strstr(frames[i], "reason=pause_resume") ||
                !strstr(frames[i], "desired=play") ||
                strcmp(cmdName, SL_CMD_RESUME) != 0) {
                fprintf(stderr, "FAIL: resume meta must reuse paused seek epoch\n");
                return 1;
            }
        }
        ++metas;
    }
    if (metas != 5) {
        fprintf(stderr, "FAIL: expected 5 meta frames, got %d\n", metas);
        return 1;
    }
    /* 第一处 duplicate seek 不得下发；只有暂停中的主动 seek 允许一帧。 */
    if (seekFrames != 1) {
        fprintf(stderr, "FAIL: expected one paused seek frame, got %d\n",
                seekFrames);
        return 1;
    }
    printf("D28_COMPAT frames=%d metas=%d adjacency=ok pause_seek_resume=ok\n", count,
           metas);
    return 0;
}

static int fake_daemon(const char *path) {
    static char frames[MAX_FRAMES][FRAME_CAP];
    int frameCount = 0;
    int sentPaused = 0;
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return 1;
    }
    int client = accept(fd, NULL, NULL);
    if (client < 0) { close(fd); return 1; }

    struct timeval tv = {2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        unsigned char h[SL_IPC_HEADER_SIZE];
        size_t off = 0;
        while (off < SL_IPC_HEADER_SIZE) {
            ssize_t n = read(client, h + off, SL_IPC_HEADER_SIZE - off);
            if (n > 0) off += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else { close(client); close(fd); return 0; }
        }
        uint32_t len = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
        char payload[FRAME_CAP];
        if (len >= sizeof(payload)) {
            close(client); close(fd); return 1;
        }
        off = 0;
        while (off < len) {
            ssize_t n = read(client, payload + off, len - off);
            if (n > 0) off += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else { close(client); close(fd); return 0; }
        }
        payload[len] = '\0';
        printf("FRAME\t%s\n", payload);
        fflush(stdout);
        if (frameCount < MAX_FRAMES) {
            snprintf(frames[frameCount], sizeof(frames[frameCount]), "%s",
                     payload);
            ++frameCount;
        }
        if (!sentPaused && strncmp(payload, SL_CMD_PAUSE, 5) == 0 &&
            strncmp(payload, "pause\t", 6) == 0) {
            char name[32], gen[32], req[32];
            if (frame_ident(payload, name, sizeof(name), gen, sizeof(gen), req,
                            sizeof(req)) == 0) {
                send_event_frame(client, SL_EVT_PAUSED, gen, req, "6789");
                sentPaused = 1;
            }
        }
        if (strncmp(payload, SL_CMD_QUIT, strlen(SL_CMD_QUIT)) == 0)
            break;
    }
    close(client);
    close(fd);
    return validate_frames(frames, frameCount);
}

static void *volume_noise(void *arg) {
    mpv_handle *ctx = (mpv_handle *)arg;
    for (int i = 0; i < 12; ++i) {
        const char *args[] = {"set", "volume", "50", NULL};
        (void)mpv_command_async(ctx, 0, args);
        usleep(2000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *sock = "/tmp/slplayer-d27-compat-test.sock";
    /* mpv_initialize() 启动 daemon 时传 --expected-parent；这里必须直接进入
     * 假 daemon，不能再次走完整测试，否则会递归 fork。 */
    if (argc > 1 && (strcmp(argv[1], "--fake-daemon") == 0 ||
                    strcmp(argv[1], "--expected-parent") == 0))
        return fake_daemon(sock);

    setenv("SL_IPC_SOCKET", sock, 1);
    setenv("SL_DAEMON_PATH", argv[0], 1);
    unlink(sock);

    mpv_handle *ctx = mpv_create();
    if (!ctx) return 1;
    if (mpv_initialize(ctx) < 0) return 1;

    pthread_t noise;
    pthread_create(&noise, NULL, volume_noise, ctx);

    const char *extra =
        "referrer=\"https://www.bilibili.com/\",start=0,"
        "run=compat-probe,session=compat-probe-1,content=video,id=BVTEST,"
        "bv=BVTEST,cid=43208345449,qn=64,"
        "audio-file=\"https://example.invalid/a.m4s?upsig=SECRET_SHOULD_NOT_LOG\"";
    const char *args[] = {
        "loadfile",
        "https://example.invalid/v.m4s?upsig=SECRET_SHOULD_NOT_LOG&trid=SECRET2",
        "replace", extra, NULL};
    if (mpv_command_async(ctx, 1, args) < 0) return 1;
    pthread_join(noise, NULL);
    usleep(300000);

    /* 第二次 loadfile 不带 D27 session：sync meta 必须独立发送。 */
    const char *args2[] = {
        "loadfile",
        "https://example.invalid/v2.m4s",
        "replace", "start=5.123,target_ms=5123", NULL};
    if (mpv_command_async(ctx, 2, args2) < 0) return 1;
    usleep(300000);

    /* open 已带同一目标：MPV_LOADED 后的等价 seek 必须被 compat 去重。 */
    const char *dupSeek[] = {"seek", "5.123", "absolute", NULL};
    if (mpv_command_async(ctx, 3, dupSeek) < 0) return 1;
    usleep(100000);

    /* pause 建立新 epoch；假 daemon 回传 paused position_ms=6789；
     * resume 必须沿用同一个 epoch，不再创建新 epoch。 */
    const char *pauseArgs[] = {"set", "pause", "yes", NULL};
    if (mpv_command_async(ctx, 3, pauseArgs) < 0) return 1;
    usleep(300000);
    /* 暂停中主动 seek：新 epoch，target=30s，desired 仍为 pause。 */
    const char *pausedSeekArgs[] = {"seek", "30", "absolute", NULL};
    if (mpv_command_async(ctx, 4, pausedSeekArgs) < 0) return 1;
    usleep(200000);
    const char *resumeArgs[] = {"set", "pause", "no", NULL};
    if (mpv_command_async(ctx, 5, resumeArgs) < 0) return 1;
    usleep(300000);

#ifdef SL_D28_TEST_HOOKS
    CHECK(sl_d28_test_active_epoch(ctx) == 4);
    CHECK(sl_d28_test_active_epoch_target(ctx) == 30000);
    sl_d28_test_end_active_epoch(ctx, "error", "watchdog");
    sl_d28_test_end_active_epoch(ctx, "error", "watchdog");
    CHECK(sl_d28_test_active_epoch(ctx) == 0);
    printf("D28_EPOCH error_terminal=1 idempotent=1 target_finalized=1\n");
#endif
    mpv_terminate_destroy(ctx);
    return 0;
}
