/*
 * Steam Link SLVideo 最小出画面验证
 *
 * 用法: ./slvideo_test <annexb.h264> [fps]
 *
 * 输入要求: H.264 Annex-B 裸流（ffmpeg -f h264 或 x264 输出），
 *           默认每帧单 slice（x264/ffmpeg 默认设置即可）。
 */

#define _DEFAULT_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SLVideo.h>
#include <SLAudio.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static const char *log_level_name(ESLVideoLog level)
{
    switch (level) {
    case k_ESLVideoLogDebug:
        return "DEBUG";
    case k_ESLVideoLogInfo:
        return "INFO";
    case k_ESLVideoLogWarning:
        return "WARN";
    case k_ESLVideoLogError:
        return "ERROR";
    default:
        return "?";
    }
}

static void slvideo_log(void *context, ESLVideoLog level, const char *message)
{
    (void)context;
    fprintf(stderr, "[SLVideo][%s] %s", log_level_name(level), message);
}

static unsigned char *read_file(const char *path, long *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size <= 0) {
        fprintf(stderr, "文件为空: %s\n", path);
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    unsigned char *data = malloc((size_t)size);
    if (!data) {
        fprintf(stderr, "内存不足: %ld 字节\n", size);
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "读取文件失败: %s\n", path);
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    *out_size = size;
    return data;
}

/* 返回 00 00 01 起始码中第一个 00 的位置；找不到返回 -1 */
static long find_start_code(const unsigned char *data, long size, long from)
{
    for (long i = from; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return i;
    }
    return -1;
}

static int submit_frame(CSLVideoStream *stream, const unsigned char *data, long size, int index)
{
    int ret;
    int retries = 0;

    /* BeginFrame 返回 -1 表示解码器跟不上（背压），稍等后重试 */
    while ((ret = SLVideo_BeginFrame(stream, (int)size)) != 0) {
        if (++retries > 100) {
            fprintf(stderr, "第 %d 帧 BeginFrame 失败: %d（已重试 %d 次）\n", index, ret, retries);
            return -1;
        }
        usleep(5000);
    }

    ret = SLVideo_WriteFrameData(stream, (void *)data, (int)size);
    if (ret != 0) {
        fprintf(stderr, "第 %d 帧 WriteFrameData 失败: %d\n", index, ret);
        return -1;
    }

    ret = SLVideo_SubmitFrame(stream);
    if (ret != 0) {
        fprintf(stderr, "第 %d 帧 SubmitFrame 失败: %d\n", index, ret);
        return -1;
    }

    return 0;
}

/* ---------------- GL 叠层（SDL2 + 原生 EGL/GLES2） ---------------- */

static SDL_Window *g_window;
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static GLuint g_program;
static GLint g_pos_attr = -1;
static GLint g_color_uniform = -1;
static int g_overlay_ready;

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "shader 编译失败: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int overlay_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_InitSubSystem 失败: %s\n", SDL_GetError());
        return -1;
    }

    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0) {
        fprintf(stderr, "SDL_GetDesktopDisplayMode 失败: %s\n", SDL_GetError());
        return -1;
    }
    printf("SDL 桌面分辨率: %dx%d\n", mode.w, mode.h);

    g_window = SDL_CreateWindow("SL GL overlay test",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                mode.w, mode.h,
                                SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow 失败: %s\n", SDL_GetError());
        return -1;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(g_window, &info) < 0 || info.subsystem != SDL_SYSWM_VIVANTE) {
        fprintf(stderr, "SDL_GetWindowWMInfo 失败或不是 Vivante: %s\n", SDL_GetError());
        return -1;
    }

    SDL_ShowWindow(g_window);

    g_egl_display = eglGetDisplay((EGLNativeDisplayType)info.info.vivante.display);
    if (g_egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay 失败\n");
        return -1;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "eglInitialize 失败\n");
        return -1;
    }
    printf("EGL 版本: %d.%d (%s)\n", major, minor,
           eglQueryString(g_egl_display, EGL_VERSION));

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint attribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 0,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_SAMPLES, 0,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(g_egl_display, attribs, &config, 1, &num_configs) || num_configs < 1) {
        fprintf(stderr, "eglChooseConfig 失败\n");
        return -1;
    }

    EGLint alpha_size = 0;
    eglGetConfigAttrib(g_egl_display, config, EGL_ALPHA_SIZE, &alpha_size);
    printf("EGL 配置 alpha=%d\n", alpha_size);

    g_egl_surface = eglCreateWindowSurface(g_egl_display, config,
                                           (EGLNativeWindowType)info.info.vivante.window, NULL);
    if (g_egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreateWindowSurface 失败: 0x%x\n", eglGetError());
        return -1;
    }

    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext 失败: 0x%x\n", eglGetError());
        return -1;
    }

    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        fprintf(stderr, "eglMakeCurrent 失败: 0x%x\n", eglGetError());
        return -1;
    }

    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    static const char *vertex_shader =
        "attribute vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char *fragment_shader =
        "precision mediump float;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = u_color; }\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return -1;
    }

    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glBindAttribLocation(g_program, 0, "a_pos");
    glLinkProgram(g_program);

    GLint linked = GL_FALSE;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(g_program, sizeof(log), NULL, log);
        fprintf(stderr, "shader 链接失败: %s\n", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return -1;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    g_pos_attr = glGetAttribLocation(g_program, "a_pos");
    g_color_uniform = glGetUniformLocation(g_program, "u_color");

    glViewport(0, 0, mode.w, mode.h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    g_overlay_ready = 1;
    return 0;
}

static void overlay_render(void)
{
    if (!g_overlay_ready)
        return;

    static const GLfloat quad[] = {
        -0.8f, -0.6f,
         0.8f, -0.6f,
        -0.8f,  0.6f,
         0.8f,  0.6f,
    };

    /* 透明背景，让视频层透出来 */
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(g_program);
    glEnableVertexAttribArray((GLuint)g_pos_attr);
    glVertexAttribPointer((GLuint)g_pos_attr, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glUniform4f(g_color_uniform, 1.0f, 0.0f, 1.0f, 0.5f); /* 半透明洋红 */
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    eglSwapBuffers(g_egl_display, g_egl_surface);
}

static void overlay_destroy(void)
{
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(g_egl_display, g_egl_surface);
        if (g_egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(g_egl_display, g_egl_context);
        eglTerminate(g_egl_display);
    }
    if (g_window)
        SDL_DestroyWindow(g_window);
    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* ---------------- SLAudio 正弦波 ---------------- */

static CSLAudioContext *g_audio_context;
static CSLAudioStream *g_audio_stream;
static pthread_t g_audio_thread;
static volatile int g_audio_stop;
static int g_audio_frame_bytes;

static void *audio_thread_main(void *arg)
{
    (void)arg;
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / 48000.0;

    while (!g_audio_stop) {
        int16_t *buffer = (int16_t *)SLAudio_BeginFrame(g_audio_stream);
        if (buffer) {
            int samples = g_audio_frame_bytes / (int)sizeof(int16_t);
            for (int i = 0; i + 1 < samples; i += 2) {
                int16_t value = (int16_t)(sin(phase) * 8000.0);
                buffer[i] = value;
                buffer[i + 1] = value;
                phase += step;
                if (phase >= 2.0 * M_PI)
                    phase -= 2.0 * M_PI;
            }
            SLAudio_SubmitFrame(g_audio_stream);
        }
        usleep(20000);
    }
    return NULL;
}

static int audio_init(void)
{
    g_audio_context = SLAudio_CreateContext();
    if (!g_audio_context) {
        fprintf(stderr, "SLAudio_CreateContext 失败\n");
        return -1;
    }

    int speakers = SLAudio_GetSpeakerCount(g_audio_context);
    g_audio_frame_bytes = 48 * 20 * 2 * (int)sizeof(int16_t); /* 48kHz, 2ch, 20ms */

    g_audio_stream = SLAudio_CreateStream(g_audio_context, 48000, 2,
                                          g_audio_frame_bytes, 1);
    if (!g_audio_stream) {
        fprintf(stderr, "SLAudio_CreateStream 失败\n");
        SLAudio_FreeContext(g_audio_context);
        g_audio_context = NULL;
        return -1;
    }

    printf("SLAudio: %d 声道, 48kHz, 20ms/帧, 440Hz 正弦波\n", speakers);

    g_audio_stop = 0;
    if (pthread_create(&g_audio_thread, NULL, audio_thread_main, NULL) != 0) {
        fprintf(stderr, "音频线程创建失败\n");
        SLAudio_FreeStream(g_audio_stream);
        g_audio_stream = NULL;
        SLAudio_FreeContext(g_audio_context);
        g_audio_context = NULL;
        return -1;
    }
    return 0;
}

static void audio_destroy(void)
{
    if (g_audio_stream) {
        g_audio_stop = 1;
        pthread_join(g_audio_thread, NULL);
        SLAudio_FreeStream(g_audio_stream);
        g_audio_stream = NULL;
    }
    if (g_audio_context) {
        SLAudio_FreeContext(g_audio_context);
        g_audio_context = NULL;
    }
}

/* ---------------- 资源基线采样 ---------------- */

static long read_status_kb(const char *key)
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp)
        return -1;

    char line[256];
    long value = -1;
    size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) == 0) {
            sscanf(line + key_len, "%ld", &value);
            break;
        }
    }
    fclose(fp);
    return value;
}

static double read_cpu_seconds(void)
{
    FILE *fp = fopen("/proc/self/stat", "r");
    if (!fp)
        return -1.0;

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1.0;
    }
    fclose(fp);

    char *p = strrchr(line, ')');
    if (!p)
        return -1.0;
    p++;

    char state;
    long ppid, pgrp, session, tty_nr, tpgid;
    unsigned int flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
    int matched = sscanf(p, " %c %ld %ld %ld %ld %ld %u %lu %lu %lu %lu %lu %lu",
                         &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
                         &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime);
    if (matched < 13)
        return -1.0;

    long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0)
        ticks = 100;
    return (double)(utime + stime) / (double)ticks;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <annexb.h264> [fps]\n", argv[0]);
        return 1;
    }

    int fps = (argc >= 3) ? atoi(argv[2]) : 30;
    if (fps <= 0)
        fps = 30;

    int enable_overlay = 0;
    int enable_audio = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "overlay") == 0)
            enable_overlay = 1;
        else if (strcmp(argv[i], "audio") == 0)
            enable_audio = 1;
    }

    long file_size = 0;
    unsigned char *data = read_file(argv[1], &file_size);
    if (!data)
        return 1;
    printf("输入: %s (%ld 字节, %d fps)\n", argv[1], file_size, fps);

    if (enable_overlay) {
        printf("启用 GL 叠层测试\n");
        if (overlay_init() != 0) {
            fprintf(stderr, "GL 叠层初始化失败\n");
            free(data);
            return 1;
        }
    }

    if (enable_audio) {
        printf("启用 SLAudio 正弦波测试\n");
        if (audio_init() != 0) {
            fprintf(stderr, "SLAudio 初始化失败\n");
            if (enable_overlay)
                overlay_destroy();
            free(data);
            return 1;
        }
    }

    double cpu_start = read_cpu_seconds();
    double wall_start = monotonic_seconds();

    SLVideo_SetLogLevel(k_ESLVideoLogInfo);
    SLVideo_SetLogFunction(slvideo_log, NULL);

    CSLVideoContext *context = SLVideo_CreateContext();
    if (!context) {
        fprintf(stderr, "SLVideo_CreateContext 失败\n");
        free(data);
        return 1;
    }

    int display_w = 0;
    int display_h = 0;
    SLVideo_GetDisplayResolution(context, &display_w, &display_h);
    printf("显示分辨率: %dx%d\n", display_w, display_h);

    CSLVideoStream *stream = SLVideo_CreateStream(context, k_ESLVideoFormatH264, 0);
    if (!stream) {
        fprintf(stderr, "SLVideo_CreateStream 失败\n");
        SLVideo_FreeContext(context);
        free(data);
        return 1;
    }
    SLVideo_SetStreamTargetFramerate(stream, (unsigned)fps, 1);
    SLVideo_SetStreamTargetBitrate(stream, 1500);

    long first = find_start_code(data, file_size, 0);
    if (first < 0) {
        fprintf(stderr, "找不到 Annex-B 起始码，确认输入是 H.264 裸流\n");
        SLVideo_FreeStream(stream);
        SLVideo_FreeContext(context);
        free(data);
        return 1;
    }

    long au_start = first;
    long pos = first;
    int frame_index = 0;
    int failed = 0;

    while (pos >= 0) {
        long next = find_start_code(data, file_size, pos + 3);
        long nal_end = (next >= 0) ? next : file_size;

        if (pos + 3 >= file_size) {
            fprintf(stderr, "码流在 NAL 头处截断\n");
            failed = 1;
            break;
        }

        int nal_type = data[pos + 3] & 0x1f;

        /* 1 = 非 IDR 片，5 = IDR 片：每个 VCL NAL 视为一帧 */
        if (nal_type == 1 || nal_type == 5) {
            long frame_size = nal_end - au_start;
            if (submit_frame(stream, data + au_start, frame_size, frame_index) != 0) {
                failed = 1;
                break;
            }
            frame_index++;

            if (frame_index == 1) {
                int width = 0;
                int height = 0;
                for (int i = 0; i < 50; i++) {
                    SLVideo_GetStreamResolution(stream, &width, &height);
                    if (width > 0 && height > 0)
                        break;
                    usleep(10000);
                }
                printf("码流分辨率: %dx%d\n", width, height);
            }

            if (frame_index % 30 == 0)
                printf("已提交 %d 帧\n", frame_index);

            if (enable_overlay)
                overlay_render();

            au_start = nal_end;
            usleep(1000000 / (useconds_t)fps);
        }

        pos = next;
    }

    printf("共提交 %d 帧，等待尾部帧显示...\n", frame_index);
    if (enable_overlay) {
        for (int i = 0; i < 60; i++) {
            overlay_render();
            usleep(33000);
        }
    } else {
        sleep(2);
    }

    uint32_t queued = SLVideo_GetQueuedVideoMS(stream);
    printf("队列中剩余 %u ms\n", queued);

    double cpu_end = read_cpu_seconds();
    double wall_end = monotonic_seconds();
    long rss_kb = read_status_kb("VmRSS:");
    long peak_rss_kb = read_status_kb("VmHWM:");
    double elapsed = wall_end - wall_start;

    printf("资源: RSS=%ld kB, 峰值RSS=%ld kB\n", rss_kb, peak_rss_kb);
    if (cpu_start >= 0.0 && cpu_end >= 0.0 && elapsed > 0.0)
        printf("资源: CPU 时间 %.2f s / 墙钟 %.2f s = %.1f%% (单核)\n",
               cpu_end - cpu_start, elapsed, (cpu_end - cpu_start) / elapsed * 100.0);

    if (enable_audio)
        audio_destroy();
    SLVideo_FreeStream(stream);
    SLVideo_FreeContext(context);
    if (enable_overlay)
        overlay_destroy();
    free(data);

    if (failed) {
        printf("结果: 失败\n");
        return 1;
    }

    printf("结果: 成功\n");
    return 0;
}
