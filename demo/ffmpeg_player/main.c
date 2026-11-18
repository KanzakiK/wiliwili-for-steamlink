/*
 * FFmpeg -> SLVideo/SLAudio 本地文件联调
 *
 * 用法: ./ffmpeg_player <file.mp4|file.flv> [fps]
 *
 * 流程:
 *   1. FFmpeg 解封装，视频经 h264_mp4toannexb 转 Annex-B 后缓存
 *   2. AAC 解码 + swresample 转 48kHz/2ch S16 PCM 后缓存
 *   3. 按 fps 把视频帧喂给 SLVideo，同时独立线程把 PCM 喂给 SLAudio
 */

#define _DEFAULT_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <SLAudio.h>
#include <SLVideo.h>

#define MAX_BUFFER_BYTES (64L * 1024 * 1024)
#define AUDIO_FRAME_BYTES (48 * 20 * 2 * (int)sizeof(int16_t)) /* 48kHz, 2ch, 20ms */

/* ---------------- 视频包缓存 ---------------- */

typedef struct {
    uint8_t **data;
    int *size;
    int count;
    int capacity;
    long total_bytes;
} VideoBuffer;

static int video_buffer_add(VideoBuffer *buffer, const uint8_t *data, int size)
{
    if (buffer->total_bytes + size > MAX_BUFFER_BYTES) {
        fprintf(stderr, "视频缓存超过 %ld 字节上限\n", MAX_BUFFER_BYTES);
        return -1;
    }

    if (buffer->count == buffer->capacity) {
        int capacity = buffer->capacity ? buffer->capacity * 2 : 256;
        uint8_t **new_data = realloc(buffer->data, (size_t)capacity * sizeof(*new_data));
        int *new_size = realloc(buffer->size, (size_t)capacity * sizeof(*new_size));
        if (!new_data || !new_size) {
            free(new_data);
            free(new_size);
            return -1;
        }
        buffer->data = new_data;
        buffer->size = new_size;
        buffer->capacity = capacity;
    }

    uint8_t *copy = malloc((size_t)size);
    if (!copy)
        return -1;
    memcpy(copy, data, (size_t)size);

    buffer->data[buffer->count] = copy;
    buffer->size[buffer->count] = size;
    buffer->count++;
    buffer->total_bytes += size;
    return 0;
}

static void video_buffer_free(VideoBuffer *buffer)
{
    for (int i = 0; i < buffer->count; i++)
        free(buffer->data[i]);
    free(buffer->data);
    free(buffer->size);
    memset(buffer, 0, sizeof(*buffer));
}

/* ---------------- PCM 缓存 ---------------- */

typedef struct {
    uint8_t *data;
    long size;
    long capacity;
} PcmBuffer;

static int pcm_buffer_append(PcmBuffer *buffer, const uint8_t *data, long size)
{
    if (buffer->size + size > MAX_BUFFER_BYTES) {
        fprintf(stderr, "PCM 缓存超过 %ld 字节上限\n", MAX_BUFFER_BYTES);
        return -1;
    }

    if (buffer->size + size > buffer->capacity) {
        long capacity = buffer->capacity ? buffer->capacity : 1024 * 1024;
        while (capacity < buffer->size + size)
            capacity *= 2;
        uint8_t *new_data = realloc(buffer->data, (size_t)capacity);
        if (!new_data)
            return -1;
        buffer->data = new_data;
        buffer->capacity = capacity;
    }

    memcpy(buffer->data + buffer->size, data, (size_t)size);
    buffer->size += size;
    return 0;
}

/* ---------------- 媒体加载 ---------------- */

typedef struct {
    VideoBuffer video;
    PcmBuffer audio;
    int fps;
    int width;
    int height;
    int audio_rate;
    int audio_channels;
} Media;

static int load_media(const char *path, Media *media)
{
    memset(media, 0, sizeof(*media));
    media->fps = 30;

    AVFormatContext *format = NULL;
    int ret = avformat_open_input(&format, path, NULL, NULL);
    if (ret < 0) {
        char error[128];
        av_strerror(ret, error, sizeof(error));
        fprintf(stderr, "avformat_open_input 失败: %s\n", error);
        return -1;
    }

    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0) {
        fprintf(stderr, "avformat_find_stream_info 失败\n");
        avformat_close_input(&format);
        return -1;
    }

    av_dump_format(format, 0, path, 0);

    int video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (video_index < 0) {
        fprintf(stderr, "没有找到视频流\n");
        avformat_close_input(&format);
        return -1;
    }

    AVStream *video_stream = format->streams[video_index];
    media->width = video_stream->codecpar->width;
    media->height = video_stream->codecpar->height;

    AVRational frame_rate = video_stream->avg_frame_rate;
    if (frame_rate.num <= 0 || frame_rate.den <= 0)
        frame_rate = video_stream->r_frame_rate;
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        double fps = av_q2d(frame_rate);
        if (fps > 1.0 && fps < 120.0)
            media->fps = (int)(fps + 0.5);
    }

    printf("视频: %s %dx%d, %d fps\n",
           avcodec_get_name(video_stream->codecpar->codec_id),
           media->width, media->height, media->fps);

    const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter) {
        fprintf(stderr, "找不到 h264_mp4toannexb\n");
        avformat_close_input(&format);
        return -1;
    }

    AVBSFContext *bsf = NULL;
    ret = av_bsf_alloc(filter, &bsf);
    if (ret < 0) {
        fprintf(stderr, "av_bsf_alloc 失败\n");
        avformat_close_input(&format);
        return -1;
    }

    avcodec_parameters_copy(bsf->par_in, video_stream->codecpar);
    bsf->time_base_in = video_stream->time_base;
    if (av_bsf_init(bsf) < 0) {
        fprintf(stderr, "av_bsf_init 失败\n");
        av_bsf_free(&bsf);
        avformat_close_input(&format);
        return -1;
    }

    AVCodecContext *audio_decoder = NULL;
    SwrContext *resampler = NULL;

    if (audio_index >= 0) {
        AVStream *audio_stream = format->streams[audio_index];
        media->audio_rate = audio_stream->codecpar->sample_rate;
        media->audio_channels = audio_stream->codecpar->ch_layout.nb_channels;
        printf("音频: %s %d Hz, %d 声道\n",
               avcodec_get_name(audio_stream->codecpar->codec_id),
               media->audio_rate, media->audio_channels);

        const AVCodec *codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (codec) {
            audio_decoder = avcodec_alloc_context3(codec);
            if (audio_decoder) {
                avcodec_parameters_to_context(audio_decoder, audio_stream->codecpar);
                if (avcodec_open2(audio_decoder, codec, NULL) < 0)
                    avcodec_free_context(&audio_decoder);
            }
        }
        if (!audio_decoder)
            fprintf(stderr, "AAC 解码器打开失败，将跳过音频\n");
    }

    AVPacket *packet = av_packet_alloc();
    AVPacket *output = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!packet || !output || !frame) {
        fprintf(stderr, "内存不足\n");
        return -1;
    }

    int failed = 0;

    while (!failed && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video_index) {
            ret = av_bsf_send_packet(bsf, packet);
            if (ret < 0) {
                fprintf(stderr, "av_bsf_send_packet 失败: %d\n", ret);
                failed = 1;
            } else {
                while (av_bsf_receive_packet(bsf, output) == 0) {
                    if (video_buffer_add(&media->video, output->data, output->size) < 0)
                        failed = 1;
                    av_packet_unref(output);
                    if (failed)
                        break;
                }
            }
        } else if (packet->stream_index == audio_index && audio_decoder) {
            ret = avcodec_send_packet(audio_decoder, packet);
            if (ret >= 0) {
                while (avcodec_receive_frame(audio_decoder, frame) >= 0) {
                    if (!resampler) {
                        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
                        ret = swr_alloc_set_opts2(&resampler,
                                                  &output_layout, AV_SAMPLE_FMT_S16, 48000,
                                                  &frame->ch_layout,
                                                  (enum AVSampleFormat)frame->format,
                                                  frame->sample_rate, 0, NULL);
                        if (ret < 0 || swr_init(resampler) < 0) {
                            fprintf(stderr, "重采样器初始化失败\n");
                            swr_free(&resampler);
                        }
                    }

                    if (resampler) {
                        int max_output = (int)av_rescale_rnd(
                            swr_get_delay(resampler, frame->sample_rate) + frame->nb_samples,
                            48000, frame->sample_rate, AV_ROUND_UP);
                        uint8_t *pcm = malloc((size_t)max_output * 2 * sizeof(int16_t));
                        if (pcm) {
                            int converted = swr_convert(resampler, &pcm, max_output,
                                                        (const uint8_t **)frame->data,
                                                        frame->nb_samples);
                            if (converted > 0) {
                                if (pcm_buffer_append(&media->audio, pcm,
                                                      (long)converted * 2 * sizeof(int16_t)) < 0)
                                    failed = 1;
                            }
                            free(pcm);
                        }
                    }

                    av_frame_unref(frame);
                    if (failed)
                        break;
                }
            }
        }
        av_packet_unref(packet);
    }

    /* 冲掉 BSF */
    av_bsf_send_packet(bsf, NULL);
    while (av_bsf_receive_packet(bsf, output) == 0) {
        if (video_buffer_add(&media->video, output->data, output->size) < 0)
            failed = 1;
        av_packet_unref(output);
    }

    /* 冲掉音频解码器 */
    if (audio_decoder) {
        avcodec_send_packet(audio_decoder, NULL);
        while (avcodec_receive_frame(audio_decoder, frame) >= 0) {
            if (resampler) {
                int max_output = (int)av_rescale_rnd(
                    swr_get_delay(resampler, frame->sample_rate) + frame->nb_samples,
                    48000, frame->sample_rate, AV_ROUND_UP);
                uint8_t *pcm = malloc((size_t)max_output * 2 * sizeof(int16_t));
                if (pcm) {
                    int converted = swr_convert(resampler, &pcm, max_output,
                                                (const uint8_t **)frame->data,
                                                frame->nb_samples);
                    if (converted > 0)
                        pcm_buffer_append(&media->audio, pcm,
                                          (long)converted * 2 * sizeof(int16_t));
                    free(pcm);
                }
            }
            av_frame_unref(frame);
        }
    }

    av_packet_free(&packet);
    av_packet_free(&output);
    av_frame_free(&frame);
    if (resampler)
        swr_free(&resampler);
    if (audio_decoder)
        avcodec_free_context(&audio_decoder);
    av_bsf_free(&bsf);
    avformat_close_input(&format);

    if (failed) {
        video_buffer_free(&media->video);
        free(media->audio.data);
        memset(&media->audio, 0, sizeof(media->audio));
        return -1;
    }

    printf("加载完成: 视频 %d 帧 / %ld 字节 Annex-B, PCM %ld 字节 (%.2f 秒)\n",
           media->video.count, media->video.total_bytes,
           media->audio.size,
           media->audio.size > 0 ? (double)media->audio.size / (48000.0 * 2 * 2) : 0.0);
    return 0;
}

/* ---------------- SLAudio 播放线程 ---------------- */

typedef struct {
    CSLAudioContext *context;
    CSLAudioStream *stream;
    const uint8_t *pcm;
    long size;
    long position;
    volatile int stop;
    volatile int done;
} AudioPlayer;

static void *audio_thread_main(void *argument)
{
    AudioPlayer *player = argument;

    while (!player->stop && player->position < player->size) {
        void *buffer = SLAudio_BeginFrame(player->stream);
        if (!buffer) {
            usleep(5000);
            continue;
        }

        long remaining = player->size - player->position;
        long bytes = remaining < AUDIO_FRAME_BYTES ? remaining : AUDIO_FRAME_BYTES;
        memcpy(buffer, player->pcm + player->position, (size_t)bytes);
        if (bytes < AUDIO_FRAME_BYTES)
            memset((uint8_t *)buffer + bytes, 0, (size_t)(AUDIO_FRAME_BYTES - bytes));

        SLAudio_SubmitFrame(player->stream);
        player->position += bytes;
        usleep(20000);
    }

    player->done = 1;
    return NULL;
}

static int audio_player_start(AudioPlayer *player, const PcmBuffer *pcm)
{
    memset(player, 0, sizeof(*player));
    player->pcm = pcm->data;
    player->size = pcm->size;

    player->context = SLAudio_CreateContext();
    if (!player->context) {
        fprintf(stderr, "SLAudio_CreateContext 失败\n");
        return -1;
    }

    player->stream = SLAudio_CreateStream(player->context, 48000, 2, AUDIO_FRAME_BYTES, 1);
    if (!player->stream) {
        fprintf(stderr, "SLAudio_CreateStream 失败\n");
        SLAudio_FreeContext(player->context);
        player->context = NULL;
        return -1;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, audio_thread_main, player) != 0) {
        fprintf(stderr, "音频线程创建失败\n");
        SLAudio_FreeStream(player->stream);
        SLAudio_FreeContext(player->context);
        player->stream = NULL;
        player->context = NULL;
        return -1;
    }

    pthread_detach(thread);
    return 0;
}

static void audio_player_stop(AudioPlayer *player)
{
    if (player->stream) {
        player->stop = 1;
        for (int i = 0; i < 100 && !player->done; i++)
            usleep(10000);
        SLAudio_FreeStream(player->stream);
        player->stream = NULL;
    }
    if (player->context) {
        SLAudio_FreeContext(player->context);
        player->context = NULL;
    }
}

/* ---------------- 资源采样 ---------------- */

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

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.mp4|file.flv> [fps]\n", argv[0]);
        return 1;
    }

    Media media;
    if (load_media(argv[1], &media) != 0)
        return 1;

    int fps = (argc >= 3) ? atoi(argv[2]) : media.fps;
    if (fps <= 0)
        fps = media.fps > 0 ? media.fps : 30;

    double cpu_start = read_cpu_seconds();
    double wall_start = monotonic_seconds();

    SLVideo_SetLogLevel(k_ESLVideoLogInfo);

    CSLVideoContext *video_context = SLVideo_CreateContext();
    if (!video_context) {
        fprintf(stderr, "SLVideo_CreateContext 失败\n");
        video_buffer_free(&media.video);
        free(media.audio.data);
        return 1;
    }

    CSLVideoStream *video_stream = SLVideo_CreateStream(video_context, k_ESLVideoFormatH264, 0);
    if (!video_stream) {
        fprintf(stderr, "SLVideo_CreateStream 失败\n");
        SLVideo_FreeContext(video_context);
        video_buffer_free(&media.video);
        free(media.audio.data);
        return 1;
    }

    SLVideo_SetStreamTargetFramerate(video_stream, (unsigned)fps, 1);
    SLVideo_SetStreamTargetBitrate(video_stream, 1500);

    int display_width = 0;
    int display_height = 0;
    SLVideo_GetDisplayResolution(video_context, &display_width, &display_height);
    printf("显示分辨率: %dx%d, 播放 %d fps\n", display_width, display_height, fps);

    AudioPlayer audio_player;
    memset(&audio_player, 0, sizeof(audio_player));
    if (media.audio.size > 0) {
        if (audio_player_start(&audio_player, &media.audio) != 0)
            fprintf(stderr, "音频播放启动失败，继续静音播放\n");
        else
            printf("音频播放已启动\n");
    }

    int failed = 0;
    for (int i = 0; i < media.video.count; i++) {
        const uint8_t *data = media.video.data[i];
        int size = media.video.size[i];

        int retries = 0;
        int ret;
        while ((ret = SLVideo_BeginFrame(video_stream, size)) != 0) {
            if (++retries > 100) {
                fprintf(stderr, "第 %d 帧 BeginFrame 失败: %d\n", i, ret);
                failed = 1;
                break;
            }
            usleep(5000);
        }
        if (failed)
            break;

        ret = SLVideo_WriteFrameData(video_stream, (void *)data, size);
        if (ret != 0) {
            fprintf(stderr, "第 %d 帧 WriteFrameData 失败: %d\n", i, ret);
            failed = 1;
            break;
        }

        ret = SLVideo_SubmitFrame(video_stream);
        if (ret != 0) {
            fprintf(stderr, "第 %d 帧 SubmitFrame 失败: %d\n", i, ret);
            failed = 1;
            break;
        }

        if ((i + 1) % 30 == 0)
            printf("已播放 %d/%d 帧\n", i + 1, media.video.count);

        usleep(1000000 / (useconds_t)fps);
    }

    printf("视频播放结束，等待音频收尾...\n");
    for (int i = 0; i < 500 && !audio_player.done && audio_player.stream; i++)
        usleep(10000);
    sleep(1);

    double cpu_end = read_cpu_seconds();
    double wall_end = monotonic_seconds();
    long rss_kb = read_status_kb("VmRSS:");
    long peak_rss_kb = read_status_kb("VmHWM:");
    double elapsed = wall_end - wall_start;

    printf("资源: RSS=%ld kB, 峰值RSS=%ld kB\n", rss_kb, peak_rss_kb);
    if (cpu_start >= 0.0 && cpu_end >= 0.0 && elapsed > 0.0)
        printf("资源: CPU 时间 %.2f s / 墙钟 %.2f s = %.1f%% (单核)\n",
               cpu_end - cpu_start, elapsed, (cpu_end - cpu_start) / elapsed * 100.0);

    audio_player_stop(&audio_player);
    SLVideo_FreeStream(video_stream);
    SLVideo_FreeContext(video_context);
    video_buffer_free(&media.video);
    free(media.audio.data);

    printf("结果: %s\n", failed ? "失败" : "成功");
    return failed ? 1 : 0;
}
