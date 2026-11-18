/*
 * ipc_probe - IPC 链路独立验证工具（阶段 3.5a 调试用）
 *
 * 连到 slplayer_daemon，发送 open（播放本地 mp4），打印收到的事件，
 * 若干秒后发 quit。用来在不开 wiliwili UI 的情况下验证 IPC 控制面/事件面。
 *
 * 用法: ipc_probe [socket路径] [视频路径] [运行秒数]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

#include "IpcProtocol.h"

static int send_payload(int fd, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    unsigned char h[4] = {n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff, (n >> 24) & 0xff};
    if (write(fd, h, 4) != 4) return -1;
    if (write(fd, s, n) != (ssize_t)n) return -1;
    return 0;
}

static int recv_payload(int fd, char *buf, size_t cap) {
    unsigned char h[4];
    size_t off = 0;
    while (off < 4) {
        ssize_t r = read(fd, h + off, 4 - off);
        if (r > 0) off += r;
        else if (r < 0 && errno == EINTR) continue;
        else return -1;
    }
    uint32_t n = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
    if (n > cap - 1) return -1;
    off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r > 0) off += r;
        else if (r < 0 && errno == EINTR) continue;
        else return -1;
    }
    buf[n] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *sock = argc > 1 ? argv[1] : SL_IPC_DEFAULT_SOCKET;
    const char *video = argc > 2 ? argv[2] : "/tmp/slplayer/dash_480p_30s.mp4";
    int seconds = argc > 3 ? atoi(argv[3]) : 8;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect %s failed: %s\n", sock, strerror(errno));
        return 1;
    }
    printf("connected\n");

    /* P0 帧信封：open<TAB>gen<TAB>req<TAB>url<TAB>referrer<TAB>start<TAB>end */
    char cmd[2048];
    unsigned long long fileGen = 1;
    unsigned long long requestId = 1;
    snprintf(cmd, sizeof(cmd), "%s\t%llu\t%llu\t%s\t%s\t%s\t%s", SL_CMD_OPEN,
             fileGen, requestId++, video, "https://www.bilibili.com/", "0", "0");
    send_payload(fd, cmd);
    printf("sent: open %s\n", video);

    struct timeval tv = {0, 200000}; /* 200ms 超时，便于按墙钟检查 */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[SL_IPC_MAX_PAYLOAD + 1];
    /* seconds <= 0 表示无限循环播放（收到 eof 自动重发 open），便于人工截图验证 */
    time_t deadline = seconds > 0 ? time(NULL) + seconds : 0;
    int loops = 0;
    while (seconds <= 0 || time(NULL) < deadline) {
        if (recv_payload(fd, buf, sizeof(buf)) == 0) {
            printf("[evt] %s\n", buf);
            char eventName[64] = {0};
            sscanf(buf, "%63s", eventName);
            if (seconds <= 0 && strcmp(eventName, SL_EVT_EOF) == 0) {
                loops++;
                printf("--- loop %d: reopen ---\n", loops);
                snprintf(cmd, sizeof(cmd), "%s\t%llu\t%llu\t%s\t%s\t%s\t%s",
                         SL_CMD_OPEN, ++fileGen, requestId++, video,
                         "https://www.bilibili.com/", "0", "0");
                send_payload(fd, cmd);
            }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* timeout, keep waiting until wall-clock deadline */
        } else {
            printf("disconnected: %s\n", strerror(errno));
            break;
        }
    }
    if (seconds > 0) {
        char quit[128];
        snprintf(quit, sizeof(quit), "%s\t%llu\t%llu", SL_CMD_QUIT,
                 fileGen, requestId++);
        send_payload(fd, quit);
        printf("sent quit\n");
    } else {
        char stop[128];
        snprintf(stop, sizeof(stop), "%s\t%llu\t%llu", SL_CMD_STOP,
                 fileGen, requestId++);
        send_payload(fd, stop);
        printf("sent stop（无限循环模式退出）\n");
    }
    close(fd);
    return 0;
}
