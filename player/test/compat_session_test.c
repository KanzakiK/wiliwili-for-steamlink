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

static int validate_frames(char frames[][FRAME_CAP], int count) {
    int metas = 0;
    for (int i = 0; i < count; ++i) {
        if (strncmp(frames[i], "meta\t", 5) != 0)
            continue;
        if (i + 1 >= count || strncmp(frames[i + 1], "open\t", 5) != 0) {
            fprintf(stderr, "FAIL: meta frame %d is not followed by open\n", i);
            return 1;
        }
        char name[32], gen[32], req[32];
        char openName[32], openGen[32], openReq[32];
        if (frame_ident(frames[i], name, sizeof(name), gen, sizeof(gen), req,
                        sizeof(req)) < 0 ||
            frame_ident(frames[i + 1], openName, sizeof(openName), openGen,
                        sizeof(openGen), openReq, sizeof(openReq)) < 0 ||
            strcmp(gen, openGen) != 0 || strcmp(req, openReq) != 0) {
            fprintf(stderr, "FAIL: meta/open identity mismatch at frame %d\n", i);
            return 1;
        }
        if (!strstr(frames[i], "epoch=") ||
            !strstr(frames[i], "target_ms=") ||
            !strstr(frames[i], "reason=open") ||
            !strstr(frames[i], "desired=play")) {
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
                !strstr(frames[i], "target_ms=5000")) {
                fprintf(stderr, "FAIL: second meta must be sync-only\n");
                return 1;
            }
        }
        ++metas;
    }
    if (metas != 2) {
        fprintf(stderr, "FAIL: expected 2 meta frames, got %d\n", metas);
        return 1;
    }
    printf("D28_COMPAT frames=%d metas=%d adjacency=ok sync_only=ok\n", count,
           metas);
    return 0;
}

static int fake_daemon(const char *path) {
    static char frames[MAX_FRAMES][FRAME_CAP];
    int frameCount = 0;
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
        "replace", "start=5", NULL};
    if (mpv_command_async(ctx, 2, args2) < 0) return 1;
    usleep(300000);

#ifdef SL_D28_TEST_HOOKS
    CHECK(sl_d28_test_active_epoch(ctx) == 2);
    sl_d28_test_end_active_epoch(ctx, "error", "watchdog");
    sl_d28_test_end_active_epoch(ctx, "error", "watchdog");
    CHECK(sl_d28_test_active_epoch(ctx) == 0);
    printf("D28_EPOCH error_terminal=1 idempotent=1\n");
#endif
    mpv_terminate_destroy(ctx);
    return 0;
}
