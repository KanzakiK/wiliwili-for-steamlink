/*
 * D27 compat 发送端集成测试。
 *
 * 子进程是假 daemon：只接收并打印 IPC 帧，不打开 SlPlayer、不碰 VPU。
 * 父进程真实调用 mpv_create/mpv_initialize/mpv_command_async(loadfile)，
 * 验证 meta 是否紧跟 open 发出，且带同一 generation/request。
 */
#include <mpv/client.h>

#include "IpcProtocol.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

/* mpv_stub.c 引用这些 GL/SDL 桩符号；本测试不渲染。 */
void *SDL_GL_GetProcAddress(const char *proc) { (void)proc; return NULL; }
void *glad_glBindFramebuffer = NULL;
void *glad_glGenFramebuffers = NULL;
void *glad_glFramebufferTexture2D = NULL;
void *glad_glCheckFramebufferStatus = NULL;
void *glad_glDeleteFramebuffers = NULL;

static int fake_daemon(const char *path) {
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
        char payload[SL_IPC_MAX_PAYLOAD + 1];
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
        if (strncmp(payload, SL_CMD_QUIT, strlen(SL_CMD_QUIT)) == 0)
            break;
    }
    close(client);
    close(fd);
    return 0;
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

    const char *extra =
        "referrer=\"https://www.bilibili.com/\",start=0,"
        "run=compat-probe,session=compat-probe-1,content=video,id=BVTEST,"
        "audio-file=\"https://example.invalid/a.m4s?upsig=SECRET_SHOULD_NOT_LOG\"";
    const char *args[] = {
        "loadfile",
        "https://example.invalid/v.m4s?upsig=SECRET_SHOULD_NOT_LOG&trid=SECRET2",
        "replace", extra, NULL};
    if (mpv_command_async(ctx, 1, args) < 0) return 1;
    usleep(500000);
    mpv_terminate_destroy(ctx);
    return 0;
}
