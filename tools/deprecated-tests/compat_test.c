/*
 * DEPRECATED / 已封存：不要用于音视频同步验收或作为回归脚本运行。
 * 实测存在“有声音无画面 / 有画面无声音”的环境差异，只能作为历史 IPC/watchdog
 * 生命周期探针备查。默认 CMake 目标已关闭，需显式 -DBUILD_DEPRECATED_COMPAT_TEST=ON。
 *
 * compat_test - libmpv 兼容层独立验证程序（阶段 3.5a）
 *
 * 模拟 wiliwili 的真实调用序列，验证兼容层：
 *   mpv_create → set_option → initialize（fork daemon + connect）
 *   → set_wakeup_callback → command_async(loadfile) → wait_event 循环
 *   → command_async(set pause) → command_async(seek) → terminate_destroy
 *
 * 链接 libmpv.a（兼容层），不依赖 wiliwili UI。
 * 需要提供兼容层引用的 SDL_GL_GetProcAddress / glad_gl* 桩符号。
 *
 * 用法: compat_test <视频路径或URL> [运行秒数]
 */
#include <mpv/client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ---- 兼容层引用的外部符号桩（真实环境由 SDL2/glad 提供）---- */
void *SDL_GL_GetProcAddress(const char *proc) {
    (void)proc;
    return NULL;
}
void *glad_glBindFramebuffer = NULL;
void *glad_glGenFramebuffers = NULL;
void *glad_glFramebufferTexture2D = NULL;
void *glad_glCheckFramebufferStatus = NULL;
void *glad_glDeleteFramebuffers = NULL;

static int g_wakeups = 0;

static void on_wakeup(void *ctx) {
    (void)ctx;
    g_wakeups++;
}

static const char *event_name(int id) {
    switch (id) {
    case MPV_EVENT_NONE: return "NONE";
    case MPV_EVENT_SHUTDOWN: return "SHUTDOWN";
    case MPV_EVENT_START_FILE: return "START_FILE";
    case MPV_EVENT_END_FILE: return "END_FILE";
    case MPV_EVENT_FILE_LOADED: return "FILE_LOADED";
    case MPV_EVENT_PLAYBACK_RESTART: return "PLAYBACK_RESTART";
    case MPV_EVENT_PROPERTY_CHANGE: return "PROPERTY_CHANGE";
    case MPV_EVENT_TICK: return "TICK";
    default: return "OTHER";
    }
}

static void send_seek(mpv_handle *mpv, const char *value, const char *mode) {
    const char *args[] = {"seek", value, mode, NULL};
    printf("[test] seek %s %s\n", value, mode);
    (void)mpv_command_async(mpv, 0, args);
}

static void send_pause(mpv_handle *mpv, int paused) {
    const char *args[] = {"set", "pause", paused ? "yes" : "no", NULL};
    printf("[test] set pause %s\n", paused ? "yes" : "no");
    (void)mpv_command_async(mpv, 0, args);
}

static void send_loadfile(mpv_handle *mpv, const char *url, const char *extra) {
    const char *args[] = {"loadfile", url, "replace", extra, NULL};
    printf("[test] loadfile replace: %s (%s)\n", url, extra);
    (void)mpv_command_async(mpv, 0, args);
}

int main(int argc, char **argv) {
    /* 真机脚本按日志关键字推进用例，禁用 stdout 块缓冲。 */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *url = argc > 1 ? argv[1] : "/tmp/slplayer/dash_480p_30s.mp4";
    int seconds = argc > 2 ? atoi(argv[2]) : 20;
    /* 3.5c 回归用：到点后发暂停，再杀 daemon 验证暂停态恢复。
     * 普通运行不设置该变量，行为与 3.5a/3.5b 验证完全一致。 */
    const char *pauseArg = getenv("SL_TEST_PAUSE_AFTER_SEC");
    time_t pauseAt = pauseArg && atoi(pauseArg) > 0 ? time(NULL) + atoi(pauseArg) : 0;
    int pauseSent = 0;
    /* 更稳的注入方式：收到第一个 PLAYBACK_RESTART（首帧）后才暂停。 */
    const char *pauseOnPlayback = getenv("SL_TEST_PAUSE_ON_PLAYBACK");
    int pauseOnFirstFrame = pauseOnPlayback && strcmp(pauseOnPlayback, "1") == 0;
    const char *p0Arg = getenv("SL_P0_TEST");
    int p0Test = p0Arg && strcmp(p0Arg, "1") == 0;
    int p0Step = 0;

    printf("[test] mpv_create\n");
    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        printf("[test] FAIL: mpv_create 返回 NULL\n");
        return 1;
    }

    /* 模拟 wiliwili 的选项设置 */
    mpv_set_option_string(mpv, "config", "yes");
    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "vo", "libmpv");

    printf("[test] mpv_initialize（应 fork daemon 并连接）\n");
    if (mpv_initialize(mpv) < 0) {
        printf("[test] FAIL: mpv_initialize\n");
        return 1;
    }

    printf("[test] version=%s\n", mpv_get_property_string(mpv, "mpv-version"));

    /* 模拟 wiliwili 的属性观察（几个关键项） */
    mpv_observe_property(mpv, 1, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 2, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 3, "duration", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 4, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 6, "percent-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 12, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 14, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 10, "speed", MPV_FORMAT_DOUBLE);

    mpv_set_wakeup_callback(mpv, on_wakeup, NULL);

    /* loadfile，extra 串模拟 wiliwili 的 genExtraUrlParam */
    const char *cmd[] = {"loadfile", url, "replace",
                         "referrer=\"https://www.bilibili.com\",network-timeout=5", NULL};
    printf("[test] loadfile: %s\n", url);
    if (mpv_command_async(mpv, 0, cmd) < 0) {
        printf("[test] FAIL: loadfile\n");
        return 1;
    }

    /* 事件循环：模拟 wiliwili 的 eventMainLoop 轮询 */
    time_t testStart = time(NULL);
    time_t deadline = testStart + seconds;
    time_t nextPos = testStart + 2;
    while (time(NULL) < deadline) {
        double elapsed = (double)(time(NULL) - testStart);
        if (p0Test) {
            if (p0Step == 0 && elapsed >= 4) {
                send_seek(mpv, "3", "absolute");
                p0Step = 1;
            } else if (p0Step == 1 && elapsed >= 7) {
                send_seek(mpv, "3", "relative");
                p0Step = 2;
            } else if (p0Step == 2 && elapsed >= 10) {
                send_seek(mpv, "25", "absolute-percent");
                p0Step = 3;
            } else if (p0Step == 3 && elapsed >= 13) {
                send_pause(mpv, 1);
                p0Step = 4;
            } else if (p0Step == 4 && elapsed >= 15) {
                send_pause(mpv, 0);
                p0Step = 5;
            } else if (p0Step == 5 && elapsed >= 18) {
                send_loadfile(mpv, url,
                              "referrer=\"https://www.bilibili.com\",start=2");
                p0Step = 6;
            } else if (p0Step == 6 && elapsed >= 21) {
                /* 带 start= 的新文件：验证延迟测量后 seek 走首帧对齐分支。 */
                send_seek(mpv, "20", "absolute");
                p0Step = 7;
            }
        }
        if (pauseAt && !pauseSent && time(NULL) >= pauseAt) {
            const char *pause[] = {"set", "pause", "yes", NULL};
            mpv_command_async(mpv, 0, pause);
            pauseSent = 1;
            printf("[test] 模拟用户暂停（3.5c 恢复用例）\n");
        }
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (!ev) continue;
        if (ev->event_id == MPV_EVENT_NONE) {
            usleep(50 * 1000);
        } else {
            printf("[test] event %-17s userdata=%llu\n", event_name(ev->event_id),
                   (unsigned long long)ev->reply_userdata);
            if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                mpv_event_property *p = (mpv_event_property *)ev->data;
                if (p->data) {
                    if (p->format == MPV_FORMAT_INT64)
                        printf("        %s = %lld\n", p->name, (long long)*(int64_t *)p->data);
                    else if (p->format == MPV_FORMAT_DOUBLE)
                        printf("        %s = %.3f\n", p->name, *(double *)p->data);
                    else if (p->format == MPV_FORMAT_FLAG)
                        printf("        %s = %d\n", p->name, *(int *)p->data);
                }
            }
            /* 复刻 wiliwili 的 FILE_LOADED 处理（mpv_core.cpp:926-941）：
             *   先 playlist-clear 丢弃备用链接，再 resume（AUTO_PLAY）。
             * 这正是真机上「视频不开始播放」的现场，必须保持在这条路径上。 */
            if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                const char *clear[] = {"playlist-clear", NULL};
                mpv_command_async(mpv, 0, clear);
                const char *resume[] = {"set", "pause", "no", NULL};
                mpv_command_async(mpv, 0, resume);
                printf("[test] 模拟 wiliwili：playlist-clear + resume\n");
            }
            if (pauseOnFirstFrame && ev->event_id == MPV_EVENT_PLAYBACK_RESTART && !pauseSent) {
                const char *pause[] = {"set", "pause", "yes", NULL};
                mpv_command_async(mpv, 0, pause);
                pauseSent = 1;
                printf("[test] 模拟用户暂停（首帧后，3.5c 恢复用例）\n");
            }
        }
        /* 每 2 秒打一次进度，证明确实在推进（而不是停在 0） */
        if (time(NULL) >= nextPos) {
            nextPos = time(NULL) + 2;
            double pos = 0;
            mpv_get_property(mpv, "playback-time", MPV_FORMAT_DOUBLE, &pos);
            printf("[test] playback-time = %.2f s\n", pos);
        }
    }

    printf("[test] wakeups=%d\n", g_wakeups);
    double speed = 0;
    if (mpv_get_property(mpv, "speed", MPV_FORMAT_DOUBLE, &speed) < 0 ||
        speed < 0.99 || speed > 1.01) {
        printf("[test] FAIL: speed 初值应为 1.0，实际 %.3f\n", speed);
        mpv_terminate_destroy(mpv);
        return 1;
    }
    printf("[test] speed 初值断言通过: %.3f\n", speed);
    if (!p0Test) {
        printf("[test] 发 seek 到 5s\n");
        send_seek(mpv, "5", "absolute");
        sleep(3);
        /* 读几个属性确认缓存 */
        int64_t dur = 0;
        double pos = 0;
        mpv_get_property(mpv, "duration", MPV_FORMAT_INT64, &dur);
        mpv_get_property(mpv, "playback-time", MPV_FORMAT_DOUBLE, &pos);
        printf("[test] 属性: duration=%lld playback-time=%.2f\n",
               (long long)dur, pos);
    } else {
        printf("[test] P0 序列完成: p0Step=%d\n", p0Step);
    }

    printf("[test] mpv_terminate_destroy（应 quit daemon 并回收）\n");
    mpv_terminate_destroy(mpv);
    printf("[test] DONE\n");
    return 0;
}
