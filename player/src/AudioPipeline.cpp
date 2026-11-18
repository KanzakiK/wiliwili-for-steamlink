#include "AudioPipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <SLAudio.h>

namespace sl {
namespace {

constexpr int kFrameMs = 20;
constexpr int kSinkRateHz = 48000; /* SLAudio.h: native frequency is 48000 Hz */
constexpr size_t kPrerollLimit = 2 * 1024 * 1024;
constexpr int kPacketQueueMax = 96;
constexpr size_t kPacketQueueBytes = 4 * 1024 * 1024;

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::interrupt() {
    abort_.store(true);
    cond_.notify_all();
}

bool AudioPipeline::start(CSLAudioContext* context, AVFormatContext* format,
                          int audioIndex, bool selfDemux) {
    stop();
    if (!context || !format || audioIndex < 0)
        return false;

    context_ = context;
    format_ = format;
    audioIndex_ = audioIndex;
    selfDemux_ = selfDemux;

    AVStream* stream = format->streams[audioIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[SlAudio] 找不到音频解码器\n");
        return false;
    }

    decoder_ = avcodec_alloc_context3(codec);
    if (!decoder_ ||
        avcodec_parameters_to_context(decoder_, stream->codecpar) < 0 ||
        avcodec_open2(decoder_, codec, nullptr) < 0) {
        fprintf(stderr, "[SlAudio] 音频解码器初始化失败\n");
        if (decoder_)
            avcodec_free_context(&decoder_);
        return false;
    }

    int sourceRate = stream->codecpar->sample_rate;
    rate_.store(kSinkRateHz);
    frameBytes_ = kSinkRateHz * kFrameMs * 2 * (int)sizeof(int16_t) / 1000;
    if (frameBytes_ <= 0) {
        fprintf(stderr, "[SlAudio] 帧长计算失败\n");
        avcodec_free_context(&decoder_);
        return false;
    }
    pending_.assign((size_t)frameBytes_, 0);
    pendingSize_ = 0;

    /* 启动时就把 SLAudio stream 建好：设备未就绪时重试，失败则整体视为无音频，
     * 否则解码线程会卡在创建流上，把视频主循环堵在 putPacket。 */
    bool streamReady = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (ensureStream()) {
            streamReady = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!streamReady) {
        fprintf(stderr, "[SlAudio] 音频设备不可用，本次播放静音\n");
        avcodec_free_context(&decoder_);
        return false;
    }

    preroll_.clear();
    started_ = false;
    startedFlag_ = false;
    submittedBytes_ = 0;
    lastQueued_ = 0;
    skipMs_ = 0;
    startAt_ = 0.0;
    warned_ = false;
    abort_ = false;
    paused_ = false;
    seekRequest_ = false;
    seekDone_ = false;

    fprintf(stderr, "[SlAudio] 源 %d Hz -> SLAudio %d Hz / 2ch / %d 字节每帧（%d ms）\n",
            sourceRate, kSinkRateHz, frameBytes_, kFrameMs);

    thread_ = std::thread(&AudioPipeline::threadMain, this);
    return true;
}

void AudioPipeline::stop() {
    if (thread_.joinable()) {
        abort_ = true;
        cond_.notify_all();
        thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(lock_);
        for (AVPacket* packet : packets_)
            av_packet_free(&packet);
        packets_.clear();
        packetBytes_ = 0;
    }

    if (stream_) {
        SLAudio_FreeStream(stream_);
        stream_ = nullptr;
    }
    if (swr_)
        swr_free(&swr_);
    if (decoder_)
        avcodec_free_context(&decoder_);

    context_ = nullptr;
    format_ = nullptr;
    audioIndex_ = -1;
    selfDemux_ = false;
    swrInputRate_ = 0;
    started_ = false;
    startedFlag_ = false;
    submittedBytes_ = 0;
    lastQueued_ = 0;
    pendingSize_ = 0;
    preroll_.clear();
    abort_ = false;
    paused_ = false;
}

void AudioPipeline::putPacket(const AVPacket* packet) {
    if (!thread_.joinable())
        return;
    std::unique_lock<std::mutex> lock(lock_);
    while (!abort_ && !seekRequest_ &&
           (packets_.size() >= (size_t)kPacketQueueMax ||
            packetBytes_ + (size_t)packet->size > kPacketQueueBytes)) {
        cond_.wait(lock);
    }
    if (abort_ || seekRequest_)
        return;

    AVPacket* copy = av_packet_clone(packet);
    if (!copy)
        return;
    packets_.push_back(copy);
    packetBytes_ += (size_t)copy->size;
    cond_.notify_all();
}

void AudioPipeline::setPaused(bool paused) {
    paused_ = paused;
    cond_.notify_all();
}

void AudioPipeline::setVolume(int volume) {
    volume_ = std::min(100, std::max(0, volume));
}

bool AudioPipeline::seek(double targetSeconds) {
    if (!thread_.joinable())
        return false;

    std::unique_lock<std::mutex> lock(lock_);
    seekTarget_ = targetSeconds;
    seekRequest_ = true;
    seekDone_ = false;
    seekOk_ = false;
    cond_.notify_all();
    while (!seekDone_ && !abort_)
        cond_.wait(lock);

    bool ok = seekOk_;
    seekDone_ = false;
    seekRequest_ = false;
    return ok;
}

void AudioPipeline::scheduleStart(int delayMs, int skipMs) {
    skipMs_.store(std::max(0, skipMs));
    startAt_.store(monotonicSeconds() + delayMs / 1000.0);
    cond_.notify_all();
}

double AudioPipeline::submittedMs() const {
    int rate = rate_.load();
    if (rate <= 0)
        return 0.0;
    return (double)submittedBytes_.load() * 1000.0 / (rate * 4.0);
}

double AudioPipeline::delayMs() const {
    int rate = rate_.load();
    if (rate <= 0)
        return 0.0;
    return (double)lastQueued_.load() / 2.0 / rate * 1000.0;
}

int AudioPipeline::queuedPackets() const {
    std::lock_guard<std::mutex> lock(lock_);
    return (int)packets_.size();
}

int AudioPipeline::queueTarget() const {
    /* 与 demo 一致：rate * 0.05 * 4 个交错采样（48k 下 9600，约 100ms） */
    return rate_.load() * 5 * 4 / 100;
}

void AudioPipeline::threadMain() {
    AVFrame* frame = av_frame_alloc();
    if (!frame)
        return;

    if (selfDemux_) {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            av_frame_free(&frame);
            return;
        }

        while (!abort_) {
            bool seek = false;
            double target = 0.0;
            {
                std::lock_guard<std::mutex> lock(lock_);
                if (seekRequest_) {
                    seek = true;
                    target = seekTarget_;
                }
            }
            if (seek) {
                completeSeek(seekInternal(target));
                continue;
            }
            if (paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            int ret = av_read_frame(format_, packet);
            if (ret < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    ret == AVERROR_EOF ? 50 : 10));
                continue;
            }
            if (packet->stream_index == audioIndex_)
                decodePacket(packet, frame);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    } else {
        while (true) {
            std::unique_lock<std::mutex> lock(lock_);
            cond_.wait(lock, [this] {
                return abort_ || seekRequest_ || !packets_.empty();
            });
            if (abort_)
                break;
            if (seekRequest_) {
                double target = seekTarget_;
                lock.unlock();
                completeSeek(seekInternal(target));
                continue;
            }

            AVPacket* packet = packets_.front();
            packets_.pop_front();
            packetBytes_ -= (size_t)packet->size;
            cond_.notify_all();
            lock.unlock();

            decodePacket(packet, frame);
            av_packet_free(&packet);
        }
    }

    av_frame_free(&frame);
}

void AudioPipeline::decodePacket(AVPacket* packet, AVFrame* frame) {
    if (!decoder_)
        return;
    if (avcodec_send_packet(decoder_, packet) < 0)
        return;
    while (!abort_ && avcodec_receive_frame(decoder_, frame) >= 0) {
        handleFrame(frame);
        av_frame_unref(frame);
    }
}

void AudioPipeline::handleFrame(AVFrame* frame) {
    int inputRate = frame->sample_rate > 0 ? frame->sample_rate : kSinkRateHz;
    int sinkRate = kSinkRateHz;

    AVChannelLayout inputLayout = frame->ch_layout;
    if (inputLayout.nb_channels <= 0)
        av_channel_layout_default(&inputLayout, 2);

    if (!swr_) {
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
        int ret = swr_alloc_set_opts2(&swr_, &outputLayout, AV_SAMPLE_FMT_S16,
                                      sinkRate, &inputLayout,
                                      (AVSampleFormat)frame->format, inputRate,
                                      0, nullptr);
        if (ret < 0 || swr_init(swr_) < 0) {
            if (!warned_) {
                fprintf(stderr, "[SlAudio] 重采样器初始化失败\n");
                warned_ = true;
            }
            swr_free(&swr_);
            return;
        }
        swrInputRate_ = inputRate;
    }

    if (inputRate != swrInputRate_) {
        /* 采样率变化（极罕见）：重建重采样器 */
        swr_free(&swr_);
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr_, &outputLayout, AV_SAMPLE_FMT_S16, sinkRate,
                                &inputLayout, (AVSampleFormat)frame->format,
                                inputRate, 0, nullptr) < 0 ||
            swr_init(swr_) < 0) {
            swr_free(&swr_);
            return;
        }
        swrInputRate_ = inputRate;
    }

    int maxOutput = (int)av_rescale_rnd(
        swr_get_delay(swr_, inputRate) + frame->nb_samples, sinkRate, inputRate,
        AV_ROUND_UP);
    if (maxOutput <= 0)
        return;

    size_t bytes = (size_t)maxOutput * 2 * sizeof(int16_t);
    if (pcm_.size() < bytes)
        pcm_.resize(bytes);

    uint8_t* output = pcm_.data();
    int converted = swr_convert(swr_, &output, maxOutput,
                                (const uint8_t**)frame->data, frame->nb_samples);
    if (converted > 0)
        submit(pcm_.data(), converted * 2 * (int)sizeof(int16_t));
}

void AudioPipeline::submit(const uint8_t* data, int size) {
    if (!ensureStream())
        return;

    if (!started_) {
        auto canStart = [this] {
            return !paused_ && startAt_.load() > 0.0 &&
                   monotonicSeconds() >= startAt_.load();
        };

        if (!canStart()) {
            while (!abort_ && !seekRequest_) {
                if (canStart() ||
                    preroll_.size() + (size_t)size <= kPrerollLimit)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (abort_ || seekRequest_)
                return;
            if (!canStart()) {
                preroll_.insert(preroll_.end(), data, data + size);
                return;
            }
        }

        started_ = true;
        startedFlag_ = true;

        int rate = rate_.load();
        size_t skipBytes = (size_t)skipMs_.load() * rate * 4 / 1000;
        if (skipBytes > preroll_.size())
            skipBytes = preroll_.size();
        if (rate > 0) {
            fprintf(stderr, "[SlAudio] 启动: 预缓冲 %.2f s, 跳过 %.2f s\n",
                    preroll_.size() / (rate * 4.0), skipBytes / (rate * 4.0));
        }
        if (preroll_.size() > skipBytes) {
            submitDirect(preroll_.data() + skipBytes,
                         (int)(preroll_.size() - skipBytes));
        }
        preroll_.clear();
    }

    submitDirect(data, size);
}

void AudioPipeline::submitDirect(const uint8_t* data, int size) {
    while (size > 0) {
        if (abort_ || seekRequest_)
            return;

        int space = frameBytes_ - pendingSize_;
        if (space <= 0)
            space = frameBytes_;
        int take = std::min(size, space);
        memcpy(pending_.data() + pendingSize_, data, (size_t)take);
        pendingSize_ += take;
        data += take;
        size -= take;

        if (pendingSize_ < frameBytes_)
            break;
        drainFrame();
        pendingSize_ = 0;
    }
}

bool AudioPipeline::drainFrame() {
    while (!abort_ && !seekRequest_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        uint32_t queued = SLAudio_GetQueuedAudioSamples(stream_);
        lastQueued_ = queued;
        if ((int)queued <= queueTarget())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (abort_ || seekRequest_)
        return false;

    void* output = SLAudio_BeginFrame(stream_);
    if (!output) {
        if (!warned_) {
            fprintf(stderr, "[SlAudio] SLAudio_BeginFrame 返回空，丢弃一帧\n");
            warned_ = true;
        }
        return true;
    }

    writeFrame(output, pending_.data(), frameBytes_);
    SLAudio_SubmitFrame(stream_);
    submittedBytes_ += (uint64_t)frameBytes_;
    return true;
}

void AudioPipeline::writeFrame(void* output, const uint8_t* data, int size) const {
    int volume = volume_.load();
    if (volume >= 100) {
        memcpy(output, data, (size_t)size);
        return;
    }

    auto* dst = static_cast<int16_t*>(output);
    auto* src = reinterpret_cast<const int16_t*>(data);
    int samples = size / (int)sizeof(int16_t);
    for (int i = 0; i < samples; i++)
        dst[i] = (int16_t)((int)src[i] * volume / 100);
}

bool AudioPipeline::ensureStream() {
    if (stream_)
        return true;

    int rate = rate_.load();
    if (rate <= 0 || frameBytes_ <= 0)
        return false;

    stream_ = SLAudio_CreateStream(context_, rate, 2, frameBytes_, 1);
    if (!stream_) {
        if (!warned_) {
            fprintf(stderr, "[SlAudio] SLAudio_CreateStream 失败\n");
            warned_ = true;
        }
        return false;
    }
    return true;
}

bool AudioPipeline::seekInternal(double targetSeconds) {
    bool ok = true;

    if (selfDemux_ && format_) {
        AVStream* stream = format_->streams[audioIndex_];
        int64_t ts = av_rescale_q((int64_t)(targetSeconds * AV_TIME_BASE),
                                  AV_TIME_BASE_Q, stream->time_base);
        ok = av_seek_frame(format_, audioIndex_, ts, AVSEEK_FLAG_BACKWARD) >= 0;
        if (ok) {
            avformat_flush(format_);
        } else {
            fprintf(stderr, "[SlAudio] 音频流跳转失败（可能超出缓冲窗口）\n");
        }
    }

    if (decoder_)
        avcodec_flush_buffers(decoder_);
    if (swr_)
        swr_free(&swr_);
    swrInputRate_ = 0;

    pendingSize_ = 0;
    preroll_.clear();
    started_ = false;
    startedFlag_ = false;
    submittedBytes_ = 0;
    skipMs_ = 0;
    startAt_ = 0.0;
    lastQueued_ = 0;

    if (stream_) {
        SLAudio_FreeStream(stream_);
        stream_ = nullptr;
    }
    ensureStream();

    return ok;
}

void AudioPipeline::completeSeek(bool ok) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        seekOk_ = ok;
        seekDone_ = true;
    }
    cond_.notify_all();
}

} // namespace sl
