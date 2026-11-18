/*
 * B 站 360p MP4 流式播放验证
 *
 * 用法: ./ffmpeg_stream <url|@url_file> [播放秒数]
 *
 * 流程:
 *   libcurl 后台下载 -> 自定义 AVIO 供 FFmpeg 读取 -> h264_mp4toannexb
 *   -> SLVideo 硬解；AAC 解码 + swresample -> SLAudio
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <SLAudio.h>
#include <SLVideo.h>

#include <SDL2/SDL.h>

#define AVIO_BUFFER_SIZE (64 * 1024)
#define STREAM_BUFFER_LIMIT (8L * 1024 * 1024)
#define STREAM_CHUNK_SIZE (256 * 1024)
#define STREAM_BACK_KEEP (512 * 1024)
#define STREAM_PIN_BYTES (1024 * 1024)
#define STREAM_MAX_CHUNKS 64
#define AUDIO_FRAME_BYTES (48 * 20 * 2 * (int)sizeof(int16_t))
/* seek 后显示管线已热，只需短延迟 */
#define AUDIO_RESEEK_DELAY_MS 300
/* 视频队列失控保护：实测 480p 的 GetQueuedVideoMS 稳定在 ~2.1s（管线固有水位，
   不是积压），所以阈值必须高于它；超过才降速，避免误伤正常播放 */
#define VIDEO_QUEUE_MAX_MS 4000
/* 音频包队列：主线程只管入队，解码/重采样在音频线程 */
#define AUDIO_PACKET_QUEUE_MAX 96
#define AUDIO_PACKET_QUEUE_BYTES (4 * 1024 * 1024)
#define AUDIO_PREROLL_LIMIT (2 * 1024 * 1024)
/* SLAudio 队列限深（GetQueuedAudioSamples 单位），避免解码跑太快 BeginFrame 丢帧 */
#define AUDIO_QUEUE_TARGET 9600
#define PROGRESS_WIDTH 512
#define PROGRESS_HEIGHT 24

#define USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

/* ---------------- 下载缓冲 ---------------- */

typedef struct {
    uint8_t *data;
    size_t size;
    int64_t offset;
} StreamChunk;

typedef struct {
    char *url;
    StreamChunk chunks[STREAM_MAX_CHUNKS];
    int chunk_count;
    int64_t base_offset;
    int64_t end_offset;
    size_t total_bytes;
    size_t limit;
    int64_t read_pos;
    long content_length;
    int eof;
    int error;
    int stop;
    int thread_started;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} StreamBuffer;

static void stream_buffer_init(StreamBuffer *buffer, const char *url)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->url = strdup(url);
    buffer->limit = STREAM_BUFFER_LIMIT;
    pthread_mutex_init(&buffer->lock, NULL);
    pthread_cond_init(&buffer->cond, NULL);
}

static void stream_buffer_free(StreamBuffer *buffer)
{
    for (int i = 0; i < buffer->chunk_count; i++)
        free(buffer->chunks[i].data);
    free(buffer->url);
    pthread_mutex_destroy(&buffer->lock);
    pthread_cond_destroy(&buffer->cond);
    memset(buffer, 0, sizeof(*buffer));
}

/* 只在窗口满时调用；aggressive=1 时允许淘汰到读位置之前的所有块 */
static void buffer_evict_locked(StreamBuffer *buffer, int aggressive)
{
    int64_t threshold = aggressive
        ? buffer->read_pos
        : (buffer->read_pos > STREAM_BACK_KEEP
               ? buffer->read_pos - STREAM_BACK_KEEP
               : 0);

    int index = 0;
    while (index < buffer->chunk_count) {
        StreamChunk *chunk = &buffer->chunks[index];
        int64_t end = chunk->offset + (int64_t)chunk->size;

        /* 跳过文件头部（moov），但继续淘汰它后面的块 */
        if (end <= STREAM_PIN_BYTES) {
            index++;
            continue;
        }
        if (end > threshold)
            break;

        size_t removed = chunk->size;
        free(chunk->data);
        memmove(&buffer->chunks[index], &buffer->chunks[index + 1],
                (size_t)(buffer->chunk_count - index - 1) * sizeof(StreamChunk));
        buffer->chunk_count--;
        buffer->total_bytes -= removed;
        /* 保留在 index 位置继续检查搬过来的下一块 */
    }

    buffer->base_offset = buffer->chunk_count
        ? buffer->chunks[0].offset
        : buffer->end_offset;
}

static int buffer_append_locked(StreamBuffer *buffer, const uint8_t *data, size_t bytes)
{
    while (bytes > 0) {
        while (buffer->total_bytes + bytes > buffer->limit &&
               !buffer->stop && !buffer->error) {
            buffer_evict_locked(buffer, 0);
            if (buffer->total_bytes + bytes <= buffer->limit)
                break;
            buffer_evict_locked(buffer, 1);
            if (buffer->total_bytes + bytes <= buffer->limit)
                break;
            pthread_cond_wait(&buffer->cond, &buffer->lock);
        }

        if (buffer->stop || buffer->error)
            return -1;

        StreamChunk *last = buffer->chunk_count
            ? &buffer->chunks[buffer->chunk_count - 1] : NULL;

        if (!last || last->size == STREAM_CHUNK_SIZE) {
            if (buffer->chunk_count >= STREAM_MAX_CHUNKS) {
                buffer_evict_locked(buffer, 1);
                if (buffer->chunk_count >= STREAM_MAX_CHUNKS)
                    return -1;
            }
            StreamChunk *chunk = &buffer->chunks[buffer->chunk_count];
            chunk->data = malloc(STREAM_CHUNK_SIZE);
            if (!chunk->data)
                return -1;
            chunk->size = 0;
            chunk->offset = buffer->end_offset;
            buffer->chunk_count++;
            last = chunk;
        }

        size_t space = STREAM_CHUNK_SIZE - last->size;
        size_t append = bytes < space ? bytes : space;
        memcpy(last->data + last->size, data, append);
        last->size += append;
        buffer->end_offset += (int64_t)append;
        buffer->total_bytes += append;
        data += append;
        bytes -= append;
    }

    pthread_cond_broadcast(&buffer->cond);
    return 0;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    StreamBuffer *buffer = userdata;
    size_t bytes = size * nmemb;

    pthread_mutex_lock(&buffer->lock);
    int result = buffer_append_locked(buffer, (const uint8_t *)ptr, bytes);
    pthread_mutex_unlock(&buffer->lock);
    return result == 0 ? bytes : 0;
}

static size_t curl_header_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    StreamBuffer *buffer = userdata;
    size_t bytes = size * nmemb;

    if (bytes > 15 && strncasecmp(ptr, "Content-Length:", 15) == 0) {
        long length = atol(ptr + 15);
        if (length > 0) {
            pthread_mutex_lock(&buffer->lock);
            buffer->content_length = length;
            pthread_mutex_unlock(&buffer->lock);
        }
    }
    return bytes;
}

static void *download_thread_main(void *argument)
{
    StreamBuffer *buffer = argument;
    CURL *curl = curl_easy_init();
    if (!curl) {
        pthread_mutex_lock(&buffer->lock);
        buffer->error = 1;
        buffer->eof = 1;
        pthread_cond_broadcast(&buffer->cond);
        pthread_mutex_unlock(&buffer->lock);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, buffer->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    CURLcode result = curl_easy_perform(curl);

    pthread_mutex_lock(&buffer->lock);
    if (result != CURLE_OK && !buffer->stop) {
        fprintf(stderr, "curl 下载失败: %s\n", curl_easy_strerror(result));
        buffer->error = 1;
    }
    buffer->eof = 1;
    pthread_cond_broadcast(&buffer->cond);
    pthread_mutex_unlock(&buffer->lock);

    curl_easy_cleanup(curl);
    return NULL;
}

static int avio_read_cb(void *opaque, uint8_t *data, int size)
{
    StreamBuffer *buffer = opaque;

    pthread_mutex_lock(&buffer->lock);
    while (!buffer->stop && !buffer->error && !buffer->eof &&
           buffer->end_offset < buffer->read_pos + size) {
        pthread_cond_wait(&buffer->cond, &buffer->lock);
    }

    int result;
    if (buffer->error) {
        result = AVERROR(EIO);
    } else if (buffer->read_pos < buffer->base_offset) {
        fprintf(stderr, "读取位置已被窗口淘汰: pos=%lld base=%lld\n",
                (long long)buffer->read_pos, (long long)buffer->base_offset);
        result = AVERROR(EIO);
    } else {
        size_t copied = 0;
        int64_t position = buffer->read_pos;

        while (copied < (size_t)size) {
            StreamChunk *chunk = NULL;
            for (int i = 0; i < buffer->chunk_count; i++) {
                StreamChunk *candidate = &buffer->chunks[i];
                if (position >= candidate->offset &&
                    position < candidate->offset + (int64_t)candidate->size) {
                    chunk = candidate;
                    break;
                }
            }
            if (!chunk)
                break;

            size_t offset_in_chunk = (size_t)(position - chunk->offset);
            size_t available = chunk->size - offset_in_chunk;
            size_t remaining = (size_t)size - copied;
            size_t bytes = available < remaining ? available : remaining;
            memcpy(data + copied, chunk->data + offset_in_chunk, bytes);
            copied += bytes;
            position += (int64_t)bytes;
        }

        buffer->read_pos = position;
        if (copied > 0) {
            result = (int)copied;
        } else if (buffer->eof && buffer->read_pos >= buffer->end_offset) {
            result = AVERROR_EOF;
        } else {
            result = AVERROR(EIO);
        }
        pthread_cond_broadcast(&buffer->cond);
    }

    pthread_mutex_unlock(&buffer->lock);
    return result;
}

static int64_t avio_seek_cb(void *opaque, int64_t offset, int whence)
{
    StreamBuffer *buffer = opaque;
    int64_t target;

    if (whence == AVSEEK_SIZE) {
        pthread_mutex_lock(&buffer->lock);
        int64_t size = buffer->content_length > 0 ? buffer->content_length : -1;
        pthread_mutex_unlock(&buffer->lock);
        return size;
    }

    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        pthread_mutex_lock(&buffer->lock);
        target = buffer->read_pos + offset;
        pthread_mutex_unlock(&buffer->lock);
    } else if (whence == SEEK_END) {
        pthread_mutex_lock(&buffer->lock);
        if (buffer->content_length <= 0) {
            pthread_mutex_unlock(&buffer->lock);
            return -1;
        }
        target = buffer->content_length + offset;
        pthread_mutex_unlock(&buffer->lock);
    } else {
        return -1;
    }

    if (target < 0)
        return -1;

    pthread_mutex_lock(&buffer->lock);
    if (target < buffer->base_offset) {
        fprintf(stderr, "回跳超出窗口: target=%lld base=%lld\n",
                (long long)target, (long long)buffer->base_offset);
        pthread_mutex_unlock(&buffer->lock);
        return -1;
    }
    buffer->read_pos = target;
    pthread_cond_broadcast(&buffer->cond);
    pthread_mutex_unlock(&buffer->lock);
    return target;
}

/* ---------------- 媒体源（一个 URL 一套下载/解封装） ---------------- */

typedef struct {
    StreamBuffer buffer;
    uint8_t *avio_buffer;
    AVIOContext *avio;
    AVFormatContext *format;
    int video_index;
    int audio_index;
} MediaSource;

static void source_close(MediaSource *source)
{
    if (source->format)
        avformat_close_input(&source->format);
    if (source->avio)
        avio_context_free(&source->avio);
    else if (source->avio_buffer)
        av_freep(&source->avio_buffer);

    if (source->buffer.url) {
        if (source->buffer.thread_started) {
            pthread_mutex_lock(&source->buffer.lock);
            source->buffer.stop = 1;
            pthread_cond_broadcast(&source->buffer.cond);
            pthread_mutex_unlock(&source->buffer.lock);
            pthread_join(source->buffer.thread, NULL);
            source->buffer.thread_started = 0;
        }
        stream_buffer_free(&source->buffer);
    }
}

static int source_open(MediaSource *source, const char *url)
{
    memset(source, 0, sizeof(*source));

    stream_buffer_init(&source->buffer, url);
    if (pthread_create(&source->buffer.thread, NULL, download_thread_main,
                       &source->buffer) != 0) {
        fprintf(stderr, "下载线程创建失败: %.80s\n", url);
        stream_buffer_free(&source->buffer);
        return -1;
    }
    source->buffer.thread_started = 1;

    source->avio_buffer = av_malloc(AVIO_BUFFER_SIZE);
    if (!source->avio_buffer) {
        fprintf(stderr, "AVIO 缓冲分配失败\n");
        source_close(source);
        return -1;
    }

    source->avio = avio_alloc_context(source->avio_buffer, AVIO_BUFFER_SIZE, 0,
                                      &source->buffer, avio_read_cb, NULL, avio_seek_cb);
    if (!source->avio) {
        fprintf(stderr, "avio_alloc_context 失败\n");
        av_freep(&source->avio_buffer);
        source_close(source);
        return -1;
    }

    source->format = avformat_alloc_context();
    if (!source->format) {
        fprintf(stderr, "avformat_alloc_context 失败\n");
        source_close(source);
        return -1;
    }
    source->format->pb = source->avio;
    source->format->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&source->format, "bilibili", NULL, NULL);
    if (ret < 0) {
        char error[128];
        av_strerror(ret, error, sizeof(error));
        fprintf(stderr, "avformat_open_input 失败: %s\n", error);
        source_close(source);
        return -1;
    }

    ret = avformat_find_stream_info(source->format, NULL);
    if (ret < 0) {
        fprintf(stderr, "avformat_find_stream_info 失败\n");
        source_close(source);
        return -1;
    }

    av_dump_format(source->format, 0, url, 0);
    source->video_index = av_find_best_stream(source->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    source->audio_index = av_find_best_stream(source->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    return 0;
}

/* ---------------- 音频线程 ---------------- */

typedef struct {
    AVCodecContext *decoder;
    SwrContext *resampler;
    CSLAudioStream *stream;
    uint8_t pending[AUDIO_FRAME_BYTES];
    int pending_size;
    int warned;
    uint8_t *preroll;
    size_t preroll_size;
    size_t preroll_capacity;
    int started;
    uint64_t submitted_bytes;
} AudioPipeline;

typedef struct {
    AudioPipeline pipeline;
    CSLAudioContext *context;
    MediaSource *source;
    int self_demux;      /* 1 = 自己读独立音频流（DASH） */
    int audio_index;

    AVPacket *packets[AUDIO_PACKET_QUEUE_MAX];
    int packet_count;
    size_t packet_bytes;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    volatile int abort;
    volatile int paused;
    volatile int started_flag;
    int thread_started;
    int seek_request;
    int seek_done;
    double seek_target_s;
    double start_at;
    int skip_ms;

    volatile uint32_t submitted_ms;
    volatile uint32_t last_queued_sl;
    volatile uint32_t max_queued_sl;
} AudioThread;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int audio_pipeline_init(AudioPipeline *audio, AVStream *stream, CSLAudioContext *context)
{
    memset(audio, 0, sizeof(*audio));

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "找不到音频解码器\n");
        return -1;
    }

    audio->decoder = avcodec_alloc_context3(codec);
    if (!audio->decoder) {
        fprintf(stderr, "音频解码器分配失败\n");
        return -1;
    }

    if (avcodec_parameters_to_context(audio->decoder, stream->codecpar) < 0 ||
        avcodec_open2(audio->decoder, codec, NULL) < 0) {
        fprintf(stderr, "音频解码器打开失败\n");
        avcodec_free_context(&audio->decoder);
        return -1;
    }

    audio->stream = SLAudio_CreateStream(context, 48000, 2, AUDIO_FRAME_BYTES, 1);
    if (!audio->stream) {
        fprintf(stderr, "SLAudio_CreateStream 失败\n");
        avcodec_free_context(&audio->decoder);
        return -1;
    }

    return 0;
}

static void audio_pipeline_submit_direct(AudioThread *thread, const uint8_t *data, int size)
{
    AudioPipeline *audio = &thread->pipeline;

    while (size > 0) {
        int space = AUDIO_FRAME_BYTES - audio->pending_size;
        int bytes = size < space ? size : space;
        memcpy(audio->pending + audio->pending_size, data, (size_t)bytes);
        audio->pending_size += bytes;
        data += bytes;
        size -= bytes;

        if (audio->pending_size < AUDIO_FRAME_BYTES)
            continue;

        while (thread->paused && !thread->abort)
            usleep(10000);
        if (thread->abort)
            return;

        while (!thread->abort) {
            uint32_t queued = SLAudio_GetQueuedAudioSamples(audio->stream);
            thread->last_queued_sl = queued;
            if (queued > thread->max_queued_sl)
                thread->max_queued_sl = queued;
            if (queued <= AUDIO_QUEUE_TARGET)
                break;
            usleep(5000);
        }

        uint8_t *output = SLAudio_BeginFrame(audio->stream);
        if (output) {
            memcpy(output, audio->pending, AUDIO_FRAME_BYTES);
            SLAudio_SubmitFrame(audio->stream);
            audio->submitted_bytes += AUDIO_FRAME_BYTES;
            thread->submitted_ms = (uint32_t)(audio->submitted_bytes / 192);
        } else if (!audio->warned) {
            fprintf(stderr, "SLAudio_BeginFrame 返回空，丢弃一帧音频\n");
            audio->warned = 1;
        }
        audio->pending_size = 0;
    }
}

static void audio_pipeline_preroll(AudioThread *thread, const uint8_t *data, size_t size)
{
    AudioPipeline *audio = &thread->pipeline;

    if (audio->preroll_size + size > AUDIO_PREROLL_LIMIT) {
        if (!audio->warned) {
            fprintf(stderr, "音频预缓冲超过 %d 字节，丢弃\n", AUDIO_PREROLL_LIMIT);
            audio->warned = 1;
        }
        return;
    }

    if (audio->preroll_size + size > audio->preroll_capacity) {
        size_t capacity = audio->preroll_capacity ? audio->preroll_capacity * 2 : 64 * 1024;
        while (capacity < audio->preroll_size + size)
            capacity *= 2;
        uint8_t *data_new = realloc(audio->preroll, capacity);
        if (!data_new)
            return;
        audio->preroll = data_new;
        audio->preroll_capacity = capacity;
    }

    memcpy(audio->preroll + audio->preroll_size, data, size);
    audio->preroll_size += size;
}

static void audio_pipeline_submit(AudioThread *thread, const uint8_t *data, int size)
{
    AudioPipeline *audio = &thread->pipeline;

    if (!audio->started) {
        int can_start = !thread->paused && thread->start_at > 0.0 &&
                        monotonic_seconds() >= thread->start_at;

        if (!can_start) {
            /* 预缓冲满了就等，不能丢帧；等到启动时刻要立刻醒来 */
            while (!thread->abort) {
                can_start = !thread->paused && thread->start_at > 0.0 &&
                            monotonic_seconds() >= thread->start_at;
                if (can_start ||
                    audio->preroll_size + (size_t)size <= AUDIO_PREROLL_LIMIT)
                    break;
                usleep(10000);
            }

            if (thread->abort)
                return;
            if (!can_start) {
                audio_pipeline_preroll(thread, data, (size_t)size);
                return;
            }
        }

        audio->started = 1;
        thread->started_flag = 1;
        size_t skip_bytes = (size_t)thread->skip_ms * 192;
        if (skip_bytes > audio->preroll_size)
            skip_bytes = audio->preroll_size;
        printf("音频启动: 预缓冲 %.2f 秒, 跳过 %.2f 秒\n",
               audio->preroll_size / (48000.0 * 2 * 2),
               skip_bytes / (48000.0 * 2 * 2));
        if (audio->preroll_size > skip_bytes) {
            audio_pipeline_submit_direct(thread, audio->preroll + skip_bytes,
                                         (int)(audio->preroll_size - skip_bytes));
        }
        free(audio->preroll);
        audio->preroll = NULL;
        audio->preroll_size = 0;
        audio->preroll_capacity = 0;
    }

    audio_pipeline_submit_direct(thread, data, size);
}

static void audio_pipeline_frame(AudioThread *thread, AVFrame *frame)
{
    AudioPipeline *audio = &thread->pipeline;

    if (!audio->resampler) {
        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
        int ret = swr_alloc_set_opts2(&audio->resampler,
                                      &output_layout, AV_SAMPLE_FMT_S16, 48000,
                                      &frame->ch_layout,
                                      (enum AVSampleFormat)frame->format,
                                      frame->sample_rate, 0, NULL);
        if (ret < 0 || swr_init(audio->resampler) < 0) {
            fprintf(stderr, "重采样器初始化失败\n");
            swr_free(&audio->resampler);
            return;
        }
    }

    int max_output = (int)av_rescale_rnd(
        swr_get_delay(audio->resampler, frame->sample_rate) + frame->nb_samples,
        48000, frame->sample_rate, AV_ROUND_UP);
    uint8_t *pcm = malloc((size_t)max_output * 2 * sizeof(int16_t));
    if (!pcm)
        return;

    int converted = swr_convert(audio->resampler, &pcm, max_output,
                                (const uint8_t **)frame->data, frame->nb_samples);
    if (converted > 0)
        audio_pipeline_submit(thread, pcm, converted * 2 * (int)sizeof(int16_t));
    free(pcm);
}

static void audio_pipeline_packet(AudioThread *thread, AVPacket *packet, AVFrame *frame)
{
    if (avcodec_send_packet(thread->pipeline.decoder, packet) < 0)
        return;
    while (avcodec_receive_frame(thread->pipeline.decoder, frame) >= 0) {
        audio_pipeline_frame(thread, frame);
        av_frame_unref(frame);
    }
}

/* 只允许音频线程调用：清包队列、flush 解码器、重建 SLAudio stream */
static void audio_pipeline_seek(AudioThread *thread)
{
    AudioPipeline *audio = &thread->pipeline;

    for (int i = 0; i < thread->packet_count; i++)
        av_packet_free(&thread->packets[i]);
    thread->packet_count = 0;
    thread->packet_bytes = 0;

    avcodec_flush_buffers(audio->decoder);
    if (audio->resampler)
        swr_free(&audio->resampler);
    audio->pending_size = 0;
    audio->warned = 0;
    free(audio->preroll);
    audio->preroll = NULL;
    audio->preroll_size = 0;
    audio->preroll_capacity = 0;
    audio->started = 0;
    thread->started_flag = 0;
    audio->submitted_bytes = 0;
    thread->submitted_ms = 0;
    thread->skip_ms = 0;

    if (audio->stream && thread->context) {
        SLAudio_FreeStream(audio->stream);
        audio->stream = SLAudio_CreateStream(thread->context, 48000, 2,
                                             AUDIO_FRAME_BYTES, 1);
        if (!audio->stream)
            fprintf(stderr, "seek 后重建 SLAudio stream 失败\n");
    }

    thread->start_at = 0.0;
    thread->last_queued_sl = 0;
    thread->max_queued_sl = 0;
}

static void audio_pipeline_free(AudioPipeline *audio)
{
    free(audio->preroll);
    audio->preroll = NULL;
    if (audio->resampler)
        swr_free(&audio->resampler);
    if (audio->decoder)
        avcodec_free_context(&audio->decoder);
    if (audio->stream)
        SLAudio_FreeStream(audio->stream);
}

static void *audio_thread_main(void *argument)
{
    AudioThread *thread = argument;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return NULL;

    if (thread->self_demux) {
        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            av_frame_free(&frame);
            return NULL;
        }

        while (!thread->abort) {
            pthread_mutex_lock(&thread->lock);
            int seek = thread->seek_request;
            int paused = thread->paused;
            double target = thread->seek_target_s;
            pthread_mutex_unlock(&thread->lock);

            if (seek) {
                AVStream *stream = thread->source->format->streams[thread->audio_index];
                int64_t ts = av_rescale_q((int64_t)(target * AV_TIME_BASE),
                                          AV_TIME_BASE_Q, stream->time_base);
                if (av_seek_frame(thread->source->format, thread->audio_index, ts,
                                  AVSEEK_FLAG_BACKWARD) < 0)
                    fprintf(stderr, "音频流跳转失败（可能超出缓冲窗口）\n");
                avformat_flush(thread->source->format);
                audio_pipeline_seek(thread);

                pthread_mutex_lock(&thread->lock);
                thread->seek_request = 0;
                thread->seek_done = 1;
                pthread_cond_broadcast(&thread->cond);
                pthread_mutex_unlock(&thread->lock);
                continue;
            }

            if (paused) {
                usleep(10000);
                continue;
            }

            int ret = av_read_frame(thread->source->format, packet);
            if (ret < 0) {
                usleep(ret == AVERROR_EOF ? 50000 : 10000);
                continue;
            }

            if (packet->stream_index == thread->audio_index)
                audio_pipeline_packet(thread, packet, frame);
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        av_frame_free(&frame);
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&thread->lock);
        while (!thread->abort && !thread->seek_request && thread->packet_count == 0)
            pthread_cond_wait(&thread->cond, &thread->lock);

        if (thread->abort) {
            pthread_mutex_unlock(&thread->lock);
            break;
        }

        if (thread->seek_request) {
            pthread_mutex_unlock(&thread->lock);
            audio_pipeline_seek(thread);
            pthread_mutex_lock(&thread->lock);
            thread->seek_request = 0;
            thread->seek_done = 1;
            pthread_cond_broadcast(&thread->cond);
            pthread_mutex_unlock(&thread->lock);
            continue;
        }

        AVPacket *packet = thread->packets[0];
        thread->packet_count--;
        memmove(&thread->packets[0], &thread->packets[1],
                (size_t)thread->packet_count * sizeof(thread->packets[0]));
        thread->packet_bytes -= (size_t)packet->size;
        pthread_cond_broadcast(&thread->cond);
        pthread_mutex_unlock(&thread->lock);

        audio_pipeline_packet(thread, packet, frame);
        av_packet_free(&packet);
    }

    av_frame_free(&frame);
    return NULL;
}

static int audio_thread_start(AudioThread *thread, MediaSource *source, int self_demux,
                              CSLAudioContext *context)
{
    memset(thread, 0, sizeof(*thread));
    thread->context = context;
    thread->source = source;
    thread->self_demux = self_demux;
    thread->audio_index = source->audio_index;
    pthread_mutex_init(&thread->lock, NULL);
    pthread_cond_init(&thread->cond, NULL);

    AVStream *stream = source->format->streams[source->audio_index];
    if (audio_pipeline_init(&thread->pipeline, stream, context) != 0) {
        pthread_mutex_destroy(&thread->lock);
        pthread_cond_destroy(&thread->cond);
        return -1;
    }

    if (pthread_create(&thread->thread, NULL, audio_thread_main, thread) != 0) {
        fprintf(stderr, "音频线程创建失败\n");
        audio_pipeline_free(&thread->pipeline);
        pthread_mutex_destroy(&thread->lock);
        pthread_cond_destroy(&thread->cond);
        return -1;
    }

    thread->thread_started = 1;
    return 0;
}

static void audio_thread_stop(AudioThread *thread)
{
    if (!thread->thread_started)
        return;

    pthread_mutex_lock(&thread->lock);
    thread->abort = 1;
    pthread_cond_broadcast(&thread->cond);
    pthread_mutex_unlock(&thread->lock);
    pthread_join(thread->thread, NULL);

    for (int i = 0; i < thread->packet_count; i++)
        av_packet_free(&thread->packets[i]);
    thread->packet_count = 0;
    thread->packet_bytes = 0;

    audio_pipeline_free(&thread->pipeline);
    pthread_mutex_destroy(&thread->lock);
    pthread_cond_destroy(&thread->cond);
    thread->thread_started = 0;
}

/* 主线程调用：把音频包克隆进队列；队列满时反压解封装 */
static void audio_thread_put_packet(AudioThread *thread, AVPacket *packet)
{
    pthread_mutex_lock(&thread->lock);
    while (!thread->abort && !thread->seek_request &&
           (thread->packet_count >= AUDIO_PACKET_QUEUE_MAX ||
            thread->packet_bytes + (size_t)packet->size > AUDIO_PACKET_QUEUE_BYTES))
        pthread_cond_wait(&thread->cond, &thread->lock);

    if (thread->abort || thread->seek_request) {
        pthread_mutex_unlock(&thread->lock);
        return;
    }

    AVPacket *copy = av_packet_clone(packet);
    if (!copy) {
        pthread_mutex_unlock(&thread->lock);
        return;
    }

    thread->packets[thread->packet_count++] = copy;
    thread->packet_bytes += (size_t)copy->size;
    pthread_cond_broadcast(&thread->cond);
    pthread_mutex_unlock(&thread->lock);
}

/* 主线程调用：安排音频启动时刻（delay_ms 后启动，跳过 skip_ms 内容） */
static void audio_thread_schedule_start(AudioThread *thread, int delay_ms, int skip_ms)
{
    pthread_mutex_lock(&thread->lock);
    thread->skip_ms = skip_ms;
    thread->start_at = monotonic_seconds() + delay_ms / 1000.0;
    pthread_cond_broadcast(&thread->cond);
    pthread_mutex_unlock(&thread->lock);
}

static void audio_thread_set_paused(AudioThread *thread, int paused)
{
    if (thread->paused == paused)
        return;

    thread->paused = paused;
    pthread_mutex_lock(&thread->lock);
    pthread_cond_broadcast(&thread->cond);
    pthread_mutex_unlock(&thread->lock);
}

/* 主线程调用：请求音频线程执行 seek（DASH 模式由音频线程自己跳），并等待完成 */
static void audio_thread_seek(AudioThread *thread, double target_s)
{
    if (!thread->thread_started)
        return;

    pthread_mutex_lock(&thread->lock);
    thread->seek_target_s = target_s;
    thread->seek_request = 1;
    thread->seek_done = 0;
    pthread_cond_broadcast(&thread->cond);
    while (!thread->seek_done && !thread->abort)
        pthread_cond_wait(&thread->cond, &thread->lock);
    thread->seek_done = 0;
    pthread_mutex_unlock(&thread->lock);
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

/* ---------------- 播放控制 ---------------- */

typedef struct {
    int paused;
    int quit;
    int seek_delta;
    SDL_GameController *controller;
} Controls;

static void controls_init(Controls *controls)
{
    memset(controls, 0, sizeof(*controls));

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return;
    }

    int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; i++) {
        if (SDL_IsGameController(i)) {
            controls->controller = SDL_GameControllerOpen(i);
            if (controls->controller) {
                printf("手柄: %s\n", SDL_GameControllerName(controls->controller));
                break;
            }
        }
    }

    if (!controls->controller)
        printf("未找到手柄，使用键盘控制\n");
}

static void controls_poll(Controls *controls)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            controls->quit = 1;
        } else if (event.type == SDL_KEYDOWN) {
            switch (event.key.keysym.sym) {
            case SDLK_SPACE:
                controls->paused = !controls->paused;
                printf("%s\n", controls->paused ? "暂停" : "继续");
                break;
            case SDLK_LEFT:
                controls->seek_delta = -10;
                break;
            case SDLK_RIGHT:
                controls->seek_delta = 10;
                break;
            case SDLK_ESCAPE:
                controls->quit = 1;
                break;
            }
        } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A:
                controls->paused = !controls->paused;
                printf("%s\n", controls->paused ? "暂停" : "继续");
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                controls->seek_delta = -10;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                controls->seek_delta = 10;
                break;
            case SDL_CONTROLLER_BUTTON_B:
                controls->quit = 1;
                break;
            }
        }
    }
}

static void controls_free(Controls *controls)
{
    if (controls->controller) {
        SDL_GameControllerClose(controls->controller);
        controls->controller = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}

/* 视频队列失控保护：队列异常深时稍微降速；正常水位（~2.1s）不触发 */
static void video_wait_queue(CSLVideoStream *stream, Controls *controls)
{
    int waited_ms = 0;

    while (SLVideo_GetQueuedVideoMS(stream) > VIDEO_QUEUE_MAX_MS) {
        if (controls->quit || waited_ms >= 500)
            break;
        usleep(5000);
        waited_ms += 5;
    }
}

/* ---------------- 进度条 overlay ---------------- */

static void progress_update(CSLVideoOverlay *overlay, double progress)
{
    if (!overlay)
        return;

    if (progress < 0.0)
        progress = 0.0;
    if (progress > 1.0)
        progress = 1.0;

    uint32_t *pixels = NULL;
    int pitch = 0;
    SLVideo_GetOverlayPixels(overlay, &pixels, &pitch);
    if (!pixels)
        return;

    int filled = (int)(PROGRESS_WIDTH * progress);
    for (int y = 0; y < PROGRESS_HEIGHT; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
        for (int x = 0; x < PROGRESS_WIDTH; x++)
            row[x] = (x < filled) ? 0xFF00C8FF : 0x80000000;
    }
}

static CSLVideoOverlay *progress_create(CSLVideoContext *context)
{
    CSLVideoOverlay *overlay = SLVideo_CreateOverlay(context, PROGRESS_WIDTH, PROGRESS_HEIGHT);
    if (!overlay) {
        fprintf(stderr, "进度条 overlay 创建失败\n");
        return NULL;
    }

    SLVideo_SetOverlayDisplayArea(overlay, 0.1f, 0.87f, 0.8f, 0.05f);
    progress_update(overlay, 0.0);
    SLVideo_ShowOverlay(overlay);
    return overlay;
}

static void progress_free(CSLVideoOverlay *overlay)
{
    if (!overlay)
        return;
    SLVideo_HideOverlay(overlay);
    SLVideo_FreeOverlay(overlay);
}

/* ---------------- B 站 API ---------------- */

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} HttpBuffer;

static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpBuffer *buffer = userdata;
    size_t bytes = size * nmemb;

    if (buffer->size + bytes > 4 * 1024 * 1024)
        return 0;

    if (buffer->size + bytes + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 64 * 1024;
        while (capacity < buffer->size + bytes + 1)
            capacity *= 2;
        char *data = realloc(buffer->data, capacity);
        if (!data)
            return 0;
        buffer->data = data;
        buffer->capacity = capacity;
    }

    memcpy(buffer->data + buffer->size, ptr, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';
    return bytes;
}

static int http_get(const char *url, HttpBuffer *out)
{
    memset(out, 0, sizeof(*out));

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        fprintf(stderr, "API 请求失败: %s\n", curl_easy_strerror(result));
        free(out->data);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    if (status != 200 || !out->data) {
        fprintf(stderr, "API 返回异常: HTTP %ld\n", status);
        free(out->data);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

/* 找到 "key" 后面第一个 ':' 之后的值起始位置 */
static const char *json_find_key(const char *json, const char *key)
{
    if (!json)
        return NULL;

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p)
        return NULL;

    p += strlen(pattern);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *p = json_find_key(json, key);
    if (!p || *p != '"')
        return -1;

    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':
                out[i++] = '\n';
                break;
            case 't':
                out[i++] = '\t';
                break;
            case 'r':
                out[i++] = '\r';
                break;
            case '"':
                out[i++] = '"';
                break;
            case '\\':
                out[i++] = '\\';
                break;
            case '/':
                out[i++] = '/';
                break;
            case 'u':
                if (strncasecmp(p + 1, "0026", 4) == 0) {
                    out[i++] = '&';
                    p += 4;
                } else {
                    p += 4;
                }
                break;
            default:
                out[i++] = *p;
                break;
            }
            p++;
        } else {
            out[i++] = *p++;
        }
    }

    out[i] = '\0';
    return *p == '"' ? 0 : -1;
}

static long long json_get_number(const char *json, const char *key)
{
    const char *p = json_find_key(json, key);
    if (!p)
        return -1;
    return strtoll(p, NULL, 10);
}

typedef struct {
    char video_url[4096];
    char audio_url[4096];
    int dash;
} PlayUrls;

static const char *json_find_array(const char *json, const char *key)
{
    const char *p = json_find_key(json, key);
    if (!p || *p != '[')
        return NULL;
    return p + 1;
}

static const char *json_object_end(const char *object)
{
    int depth = 0;
    int in_string = 0;

    for (const char *p = object; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_string = 0;
            continue;
        }
        if (*p == '"') {
            in_string = 1;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0)
                return p;
        }
    }
    return NULL;
}

static char *json_object_copy(const char *start, const char *end)
{
    size_t length = (size_t)(end - start + 1);
    char *copy = malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static int resolve_playurl_mp4(const char *bvid, long long cid, char *url, size_t url_size)
{
    char api[1024];
    snprintf(api, sizeof(api),
             "https://api.bilibili.com/x/player/playurl?bvid=%s&cid=%lld"
             "&qn=32&fnver=0&fourk=0&fnval=1",
             bvid, cid);

    HttpBuffer response;
    if (http_get(api, &response) != 0)
        return -1;

    long long code = json_get_number(response.data, "code");
    if (code != 0) {
        char message[256] = "";
        json_get_string(response.data, "message", message, sizeof(message));
        fprintf(stderr, "playurl 失败: code=%lld message=%s\n", code, message);
        free(response.data);
        return -1;
    }

    const char *durl = json_find_key(response.data, "durl");
    if (!durl || json_get_string(durl, "url", url, url_size) != 0) {
        fprintf(stderr, "解析播放地址失败\n");
        free(response.data);
        return -1;
    }

    printf("播放地址: %.120s...\n", url);
    free(response.data);
    return 0;
}

/* 先请求 DASH（480p AVC + 独立 AAC），失败再回退单文件 MP4（360p） */
static int resolve_playurl(const char *bvid, long long cid, PlayUrls *out)
{
    memset(out, 0, sizeof(*out));

    char api[1024];
    snprintf(api, sizeof(api),
             "https://api.bilibili.com/x/player/playurl?bvid=%s&cid=%lld"
             "&qn=64&fnver=0&fourk=0&fnval=16",
             bvid, cid);

    HttpBuffer response;
    if (http_get(api, &response) != 0)
        return -1;

    long long code = json_get_number(response.data, "code");
    if (code != 0) {
        char message[256] = "";
        json_get_string(response.data, "message", message, sizeof(message));
        fprintf(stderr, "playurl(DASH) 失败: code=%lld message=%s\n", code, message);
        free(response.data);
        return -1;
    }

    const char *dash = json_find_key(response.data, "dash");
    if (dash && *dash == '{') {
        /* 视频：取第一个 avc1 编码的条目 */
        const char *video = json_find_array(dash, "video");
        while (video && *video) {
            while (*video == ',' || isspace((unsigned char)*video))
                video++;
            if (*video != '{')
                break;
            const char *end = json_object_end(video);
            if (!end)
                break;

            char *object = json_object_copy(video, end);
            if (object) {
                char codecs[64] = "";
                json_get_string(object, "codecs", codecs, sizeof(codecs));
                if (strncasecmp(codecs, "avc1", 4) == 0 &&
                    (json_get_string(object, "baseUrl", out->video_url,
                                     sizeof(out->video_url)) == 0 ||
                     json_get_string(object, "base_url", out->video_url,
                                     sizeof(out->video_url)) == 0)) {
                    free(object);
                    break;
                }
                free(object);
            }
            video = end + 1;
        }

        /* 音频：取 bandwidth 最大的条目 */
        long long best_bandwidth = -1;
        const char *audio = json_find_array(dash, "audio");
        while (audio && *audio) {
            while (*audio == ',' || isspace((unsigned char)*audio))
                audio++;
            if (*audio != '{')
                break;
            const char *end = json_object_end(audio);
            if (!end)
                break;

            char *object = json_object_copy(audio, end);
            if (object) {
                char url[4096] = "";
                long long bandwidth = json_get_number(object, "bandwidth");
                if (bandwidth > best_bandwidth &&
                    (json_get_string(object, "baseUrl", url, sizeof(url)) == 0 ||
                     json_get_string(object, "base_url", url, sizeof(url)) == 0)) {
                    best_bandwidth = bandwidth;
                    snprintf(out->audio_url, sizeof(out->audio_url), "%s", url);
                }
                free(object);
            }
            audio = end + 1;
        }

        if (out->video_url[0] && out->audio_url[0]) {
            out->dash = 1;
            printf("DASH 视频: %.120s...\n", out->video_url);
            printf("DASH 音频: %.120s... (bandwidth %lld)\n", out->audio_url, best_bandwidth);
            free(response.data);
            return 0;
        }
    }

    free(response.data);

    fprintf(stderr, "未获得 DASH 音视频，回退单文件 MP4\n");
    if (resolve_playurl_mp4(bvid, cid, out->video_url, sizeof(out->video_url)) != 0)
        return -1;
    out->dash = 0;
    return 0;
}

static int resolve_popular(PlayUrls *urls, char *title, size_t title_size)
{
    HttpBuffer response;
    if (http_get("https://api.bilibili.com/x/web-interface/popular?ps=1&pn=1", &response) != 0)
        return -1;

    const char *list = json_find_key(response.data, "list");
    char bvid[64] = "";
    long long cid = list ? json_get_number(list, "cid") : -1;

    if (!list || json_get_string(list, "bvid", bvid, sizeof(bvid)) != 0 || cid <= 0) {
        fprintf(stderr, "解析热门视频失败\n");
        free(response.data);
        return -1;
    }

    json_get_string(list, "title", title, title_size);
    printf("热门视频: %s (bvid=%s, cid=%lld)\n", title, bvid, cid);
    free(response.data);

    return resolve_playurl(bvid, cid, urls);
}

static int resolve_bvid(const char *bvid, PlayUrls *urls, char *title, size_t title_size)
{
    char api[512];
    snprintf(api, sizeof(api),
             "https://api.bilibili.com/x/web-interface/view?bvid=%s", bvid);

    HttpBuffer response;
    if (http_get(api, &response) != 0)
        return -1;

    long long code = json_get_number(response.data, "code");
    long long cid = json_get_number(response.data, "cid");
    if (code != 0 || cid <= 0) {
        fprintf(stderr, "view 失败: code=%lld\n", code);
        free(response.data);
        return -1;
    }

    json_get_string(response.data, "title", title, title_size);
    printf("视频: %s (bvid=%s, cid=%lld)\n", title, bvid, cid);
    free(response.data);

    return resolve_playurl(bvid, cid, urls);
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <url|@url_file|--popular|--bvid BV号> [播放秒数]\n", argv[0]);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    PlayUrls urls;
    memset(&urls, 0, sizeof(urls));
    char title[256] = "";
    int seconds = 0;
    int audio_delay_ms = -1; /* -1 = 自动等视频上屏 */

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--audio-delay") == 0) {
            int value = atoi(argv[i + 1]);
            if (value > 0 && value < 10000)
                audio_delay_ms = value;
        }
    }

    if (strcmp(argv[1], "--popular") == 0) {
        seconds = (argc >= 3) ? atoi(argv[2]) : 0;
        if (resolve_popular(&urls, title, sizeof(title)) != 0) {
            curl_global_cleanup();
            return 1;
        }
    } else if (strcmp(argv[1], "--bvid") == 0) {
        if (argc < 3) {
            fprintf(stderr, "用法: %s --bvid BV号 [播放秒数]\n", argv[0]);
            curl_global_cleanup();
            return 1;
        }
        seconds = (argc >= 4) ? atoi(argv[3]) : 0;
        if (resolve_bvid(argv[2], &urls, title, sizeof(title)) != 0) {
            curl_global_cleanup();
            return 1;
        }
    } else {
        seconds = (argc >= 3) ? atoi(argv[2]) : 0;
        if (argv[1][0] == '@') {
            FILE *fp = fopen(argv[1] + 1, "r");
            if (!fp) {
                perror("打开 URL 文件");
                curl_global_cleanup();
                return 1;
            }
            if (!fgets(urls.video_url, sizeof(urls.video_url), fp)) {
                fprintf(stderr, "URL 文件为空\n");
                fclose(fp);
                curl_global_cleanup();
                return 1;
            }
            fclose(fp);
            urls.video_url[strcspn(urls.video_url, "\r\n")] = '\0';
        } else {
            snprintf(urls.video_url, sizeof(urls.video_url), "%s", argv[1]);
        }
        urls.dash = 0;
    }

    if (urls.video_url[0] == '\0') {
        fprintf(stderr, "URL 为空\n");
        curl_global_cleanup();
        return 1;
    }

    MediaSource video_source;
    if (source_open(&video_source, urls.video_url) != 0) {
        fprintf(stderr, "视频源打开失败\n");
        curl_global_cleanup();
        return 1;
    }

    MediaSource audio_source;
    memset(&audio_source, 0, sizeof(audio_source));
    int have_audio_source = 0;
    if (urls.dash) {
        if (source_open(&audio_source, urls.audio_url) == 0)
            have_audio_source = 1;
        else
            fprintf(stderr, "DASH 音频源打开失败，静音播放\n");
    }

    AVFormatContext *format = video_source.format;
    int video_index = video_source.video_index;
    int audio_index = urls.dash
        ? (have_audio_source ? audio_source.audio_index : -1)
        : video_source.audio_index;
    int ret = 0;

    if (video_index < 0) {
        fprintf(stderr, "没有找到视频流\n");
        if (have_audio_source)
            source_close(&audio_source);
        source_close(&video_source);
        curl_global_cleanup();
        return 1;
    }

    AVStream *video_stream = format->streams[video_index];
    printf("视频: %s %dx%d%s\n",
           avcodec_get_name(video_stream->codecpar->codec_id),
           video_stream->codecpar->width,
           video_stream->codecpar->height,
           urls.dash ? "（DASH 独立视频流）" : "");

    int fps = 25;
    AVRational frame_rate = video_stream->avg_frame_rate;
    if (frame_rate.num <= 0 || frame_rate.den <= 0)
        frame_rate = video_stream->r_frame_rate;
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        double value = av_q2d(frame_rate);
        if (value > 1.0 && value < 120.0)
            fps = (int)(value + 0.5);
    }
    printf("帧率: %d fps\n", fps);

    const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter) {
        fprintf(stderr, "找不到 h264_mp4toannexb\n");
        return 1;
    }

    AVBSFContext *bsf = NULL;
    if (av_bsf_alloc(filter, &bsf) < 0) {
        fprintf(stderr, "av_bsf_alloc 失败\n");
        return 1;
    }
    avcodec_parameters_copy(bsf->par_in, video_stream->codecpar);
    bsf->time_base_in = video_stream->time_base;
    if (av_bsf_init(bsf) < 0) {
        fprintf(stderr, "av_bsf_init 失败\n");
        return 1;
    }

    SLVideo_SetLogLevel(k_ESLVideoLogInfo);
    CSLVideoContext *video_context = SLVideo_CreateContext();
    CSLVideoStream *video_stream_sl = NULL;
    if (video_context)
        video_stream_sl = SLVideo_CreateStream(video_context, k_ESLVideoFormatH264, 0);

    if (!video_context || !video_stream_sl) {
        fprintf(stderr, "SLVideo 初始化失败\n");
        return 1;
    }

    SLVideo_SetStreamTargetFramerate(video_stream_sl, (unsigned)fps, 1);
    SLVideo_SetStreamTargetBitrate(video_stream_sl, 1000);

    CSLVideoOverlay *progress = progress_create(video_context);
    double duration = format->duration > 0 ? format->duration / (double)AV_TIME_BASE : 0.0;

    CSLAudioContext *audio_context = NULL;
    AudioThread audio;
    memset(&audio, 0, sizeof(audio));
    if (audio_index >= 0) {
        MediaSource *audio_owner = urls.dash ? &audio_source : &video_source;
        audio_context = SLAudio_CreateContext();
        if (audio_context &&
            audio_thread_start(&audio, audio_owner, urls.dash, audio_context) == 0)
            printf("音频: AAC -> 48kHz/2ch PCM（%s）\n",
                   urls.dash ? "DASH 独立线程自解封装" : "独立线程");
        else
            fprintf(stderr, "音频初始化失败，静音播放\n");
    }
    if (audio_delay_ms >= 0)
        printf("音频启动: 固定延迟 %d ms\n", audio_delay_ms);
    else
        printf("音频启动: 自动（等视频队列停止增长）\n");

    double wall_start = monotonic_seconds();
    double cpu_start = read_cpu_seconds();

    AVPacket *packet = av_packet_alloc();
    AVPacket *output = av_packet_alloc();

    int video_frames = 0;
    int failed = 0;
    int video_started = 0;
    double video_time = 0.0;
    int after_seek = 0;
    int begin_retries = 0;
    unsigned max_video_queue = 0;
    int audio_scheduled = 0;
    uint32_t queue_max = 0;
    int queue_flat_frames = 0;
    double max_submitted_ms = 0.0;
    double next_frame_deadline = monotonic_seconds();

    Controls controls;
    controls_init(&controls);

    while (!controls.quit) {
        controls_poll(&controls);
        if (controls.quit)
            break;
        audio_thread_set_paused(&audio, controls.paused);

        if (controls.seek_delta != 0) {
            double target = video_time + controls.seek_delta;
            if (duration > 0.0) {
                if (target < 0.0)
                    target = 0.0;
                if (target > duration - 1.0)
                    target = duration - 1.0;
            }

            int64_t seek_ts = av_rescale_q((int64_t)(target * AV_TIME_BASE),
                                           AV_TIME_BASE_Q, video_stream->time_base);
            if (av_seek_frame(format, video_index, seek_ts, AVSEEK_FLAG_BACKWARD) < 0) {
                fprintf(stderr, "跳转失败（可能超出缓冲窗口）\n");
            } else {
                avformat_flush(format);
                av_bsf_flush(bsf);
                audio_thread_seek(&audio, target);

                SLVideo_FreeStream(video_stream_sl);
                video_stream_sl = SLVideo_CreateStream(video_context, k_ESLVideoFormatH264, 0);
                if (!video_stream_sl) {
                    fprintf(stderr, "seek 后重建 SLVideo stream 失败\n");
                    failed = 1;
                    break;
                }
                SLVideo_SetStreamTargetFramerate(video_stream_sl, (unsigned)fps, 1);
                SLVideo_SetStreamTargetBitrate(video_stream_sl, 1000);

                video_started = 0;
                after_seek = 1;
                audio_scheduled = 0;
                queue_max = 0;
                queue_flat_frames = 0;
                max_submitted_ms = 0.0;
                video_time = target;
                next_frame_deadline = monotonic_seconds();
                printf("跳转到 %.1f s\n", target);
            }

            controls.seek_delta = 0;
            continue;
        }

        if (controls.paused) {
            usleep(20000);
            continue;
        }

        if (av_read_frame(format, packet) < 0)
            break;

        if (packet->stream_index == video_index) {
            int64_t packet_pts = packet->pts;
            if (packet_pts != AV_NOPTS_VALUE)
                video_time = packet_pts * av_q2d(video_stream->time_base);

            ret = av_bsf_send_packet(bsf, packet);
            if (ret >= 0) {
                while (av_bsf_receive_packet(bsf, output) == 0) {
                    uint32_t queued_ms = SLVideo_GetQueuedVideoMS(video_stream_sl);
                    if (queued_ms > max_video_queue)
                        max_video_queue = queued_ms;
                    video_wait_queue(video_stream_sl, &controls);

                    int retries = 0;
                    while ((ret = SLVideo_BeginFrame(video_stream_sl, output->size)) != 0) {
                        if (++retries > 100) {
                            fprintf(stderr, "BeginFrame 失败: %d\n", ret);
                            failed = 1;
                            break;
                        }
                        usleep(5000);
                    }
                    begin_retries += retries;
                    if (failed) {
                        av_packet_unref(output);
                        break;
                    }

                    if (SLVideo_WriteFrameData(video_stream_sl, output->data, output->size) != 0 ||
                        SLVideo_SubmitFrame(video_stream_sl) != 0) {
                        fprintf(stderr, "视频帧提交失败\n");
                        failed = 1;
                        av_packet_unref(output);
                        break;
                    }

                    if (!video_started) {
                        video_started = 1;
                        if (after_seek) {
                            if (audio.thread_started)
                                audio_thread_schedule_start(&audio, AUDIO_RESEEK_DELAY_MS, 0);
                            audio_scheduled = 1;
                            after_seek = 0;
                            printf("seek 后音频将在 %d ms 后启动\n", AUDIO_RESEEK_DELAY_MS);
                        } else if (audio_delay_ms >= 0) {
                            if (audio.thread_started)
                                audio_thread_schedule_start(&audio, audio_delay_ms, 0);
                            audio_scheduled = 1;
                            printf("首帧已提交，音频固定延迟 %d ms\n", audio_delay_ms);
                        } else {
                            printf("首帧已提交，等视频真正上屏后启动音频\n");
                        }
                    }

                    video_frames++;

                    /* 内容时刻取已提交的最大 PTS，避免 B 帧乱序导致抖动 */
                    double frame_ms = video_time * 1000.0;
                    if (output->pts != AV_NOPTS_VALUE)
                        frame_ms = output->pts * av_q2d(video_stream->time_base) * 1000.0;
                    if (frame_ms > max_submitted_ms)
                        max_submitted_ms = frame_ms;
                    double submitted_ms = max_submitted_ms;
                    uint32_t queued_now = SLVideo_GetQueuedVideoMS(video_stream_sl);

                    if (video_frames <= 120) {
                        printf("帧 %d 内容 %.0f ms 队列 %u ms 呈现估计 %.0f ms\n",
                               video_frames, submitted_ms, queued_now,
                               submitted_ms - (double)queued_now);
                    }

                    if (video_frames > 1 && queued_now > 0) {
                        if (queued_now > queue_max + 5) {
                            queue_max = queued_now;
                            queue_flat_frames = 0;
                        } else {
                            queue_flat_frames++;
                        }

                        if (!audio_scheduled && audio.thread_started &&
                            submitted_ms > 1500.0 && queue_flat_frames >= 5) {
                            double skip_ms = submitted_ms - (double)queued_now;
                            if (skip_ms < 0.0)
                                skip_ms = 0.0;
                            if (skip_ms > 3000.0)
                                skip_ms = 3000.0;
                            audio_thread_schedule_start(&audio, 0, (int)skip_ms);
                            audio_scheduled = 1;
                            printf("视频已上屏: 内容 %.0f ms, 队列 %u ms (峰值 %u), 音频跳过 %.0f ms\n",
                                   submitted_ms, queued_now, queue_max, skip_ms);
                        }
                    }

                    if (!audio_scheduled && audio.thread_started && submitted_ms > 8000.0) {
                        audio_thread_schedule_start(&audio, 0, 0);
                        audio_scheduled = 1;
                        printf("队列迟迟未稳定，音频兜底立即启动\n");
                    }

                    av_packet_unref(output);

                    if (video_frames % 25 == 0) {
                        if (duration > 0.0) {
                            double current_progress = video_time / duration;
                            progress_update(progress, current_progress);
                            printf("进度: %.1f / %.1f s, 视频队列 %u ms (峰值 %u), "
                                   "音频队列 %u (峰值 %u), 音频包 %d, BeginFrame 重试 %d\n",
                                   video_time, duration,
                                   SLVideo_GetQueuedVideoMS(video_stream_sl), max_video_queue,
                                   (unsigned)audio.last_queued_sl, (unsigned)audio.max_queued_sl,
                                   audio.packet_count, begin_retries);
                            if (audio.started_flag) {
                                double audio_content = (double)audio.skip_ms +
                                                       (double)audio.submitted_ms -
                                                       (double)audio.last_queued_sl / 48.0;
                                double video_content = submitted_ms - (double)queued_now;
                                printf("A/V 偏差 %+.0f ms (音频内容 %.0f ms, 视频呈现 %.0f ms)\n",
                                       audio_content - video_content,
                                       audio_content, video_content);
                            }
                        }

                        pthread_mutex_lock(&video_source.buffer.lock);
                        long long downloaded = (long long)video_source.buffer.end_offset;
                        size_t buffered = video_source.buffer.total_bytes;
                        pthread_mutex_unlock(&video_source.buffer.lock);
                        printf("已播放 %d 帧, 已下载 %lld 字节, 窗口内 %zu 字节\n",
                               video_frames, downloaded, buffered);
                    }

                    next_frame_deadline += 1.0 / fps;
                    double now = monotonic_seconds();
                    if (next_frame_deadline > now)
                        usleep((useconds_t)((next_frame_deadline - now) * 1000000.0));
                    else
                        next_frame_deadline = now;
                }
            }
        } else if (packet->stream_index == audio_index && audio.thread_started &&
                   !audio.self_demux) {
            audio_thread_put_packet(&audio, packet);
        }

        av_packet_unref(packet);

        if (failed)
            break;
        if (seconds > 0 && monotonic_seconds() - wall_start >= seconds)
            break;
    }

    av_bsf_send_packet(bsf, NULL);
    while (av_bsf_receive_packet(bsf, output) == 0)
        av_packet_unref(output);

    printf("播放结束: %d 帧\n", video_frames);
    sleep(1);

    double wall_end = monotonic_seconds();
    double cpu_end = read_cpu_seconds();
    long rss_kb = read_status_kb("VmRSS:");
    long peak_rss_kb = read_status_kb("VmHWM:");
    double elapsed = wall_end - wall_start;

    pthread_mutex_lock(&video_source.buffer.lock);
    long long downloaded = (long long)video_source.buffer.end_offset;
    long content_length = video_source.buffer.content_length;
    size_t buffered = video_source.buffer.total_bytes;
    pthread_mutex_unlock(&video_source.buffer.lock);

    printf("下载: %lld 字节 / Content-Length=%ld, 窗口内 %zu 字节\n",
           downloaded, content_length, buffered);
    printf("资源: RSS=%ld kB, 峰值RSS=%ld kB\n", rss_kb, peak_rss_kb);
    if (cpu_start >= 0.0 && cpu_end >= 0.0 && elapsed > 0.0)
        printf("资源: CPU 时间 %.2f s / 墙钟 %.2f s = %.1f%% (单核)\n",
               cpu_end - cpu_start, elapsed, (cpu_end - cpu_start) / elapsed * 100.0);

    if (controls.quit && !failed)
        printf("用户退出\n");
    controls_free(&controls);

    av_packet_free(&packet);
    av_packet_free(&output);

    audio_thread_stop(&audio);
    if (audio_context)
        SLAudio_FreeContext(audio_context);
    SLVideo_FreeStream(video_stream_sl);
    progress_free(progress);
    SLVideo_FreeContext(video_context);
    av_bsf_free(&bsf);

    if (have_audio_source)
        source_close(&audio_source);
    source_close(&video_source);

    curl_global_cleanup();

    printf("结果: %s\n", failed ? "失败" : "成功");
    return failed ? 1 : 0;
}
