#include "AudioPipeline.h"

#include "DebugLog.h"
#include "HttpSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <SLAudio.h>

namespace sl {
namespace {

constexpr int kFrameMs = 50;
constexpr int kSinkRateHz = 48000; /* SLAudio.h: native frequency is 48000 Hz */
constexpr std::size_t kPcmDumpLimitBytes = 8u * 1024u * 1024u;
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
                          int audioIndex, bool selfDemux, HttpSource* source) {
    stop();
    if (!context || !format || audioIndex < 0)
        return false;

    context_ = context;
    format_ = format;
    source_ = source;
    audioIndex_ = audioIndex;
    selfDemux_ = selfDemux;
    startOffsetSec_ = 0.0;
    if (const char* offset = std::getenv("SL_SYNC_AUDIO_OFFSET_MS"))
        startOffsetSec_ = std::atof(offset) / 1000.0;

    AVStream* stream = format->streams[audioIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        sl::d27Log("AUDIO", "decoder not found");
        return false;
    }

    decoder_ = avcodec_alloc_context3(codec);
    if (!decoder_ ||
        avcodec_parameters_to_context(decoder_, stream->codecpar) < 0 ||
        avcodec_open2(decoder_, codec, nullptr) < 0) {
        sl::d27Log("AUDIO", "decoder init failed");
        if (decoder_)
            avcodec_free_context(&decoder_);
        return false;
    }

    int sourceRate = stream->codecpar->sample_rate;
    rate_.store(kSinkRateHz);
    frameBytes_ = kSinkRateHz * kFrameMs * 2 * (int)sizeof(int16_t) / 1000;
    if (frameBytes_ <= 0) {
        sl::d27Log("AUDIO", "frame size calculation failed");
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
        sl::d27Log("AUDIO", "device unavailable, silent playback");
        avcodec_free_context(&decoder_);
        return false;
    }

    preroll_.clear();
    prerollBytes_.store(0);
    started_ = false;
    startedFlag_ = false;
    submittedBytes_ = 0;
    lastQueued_ = 0;
    sinkQueueSamples_.store(0);
    skipMs_ = 0;
    startAt_ = 0.0;
    warned_ = false;
    abort_ = false;
    paused_ = false;
    seekRequest_ = false;
    seekActive_ = false;
    ack_.reset();
    seekTarget_ = 0.0;
    startPending_ = false;
    pendingStartAt_ = 0.0;
    pendingStartSequence_ = 0;

    sl::d27Log("AUDIO",
               "format source=%dHz sink=%dHz channels=2 frame_bytes=%d "
               "frame_ms=%d",
               sourceRate, kSinkRateHz, frameBytes_, kFrameMs);

    if (const char* dumpPath = std::getenv("SL_AUDIO_DUMP_PCM")) {
        if (*dumpPath && !pcmDump_) {
            pcmDump_ = std::fopen(dumpPath, "wb");
            pcmDumpBytes_ = 0;
            sl::d27Log("AUDIO", "pcm dump enabled path=%s limit=%u",
                       dumpPath, (unsigned)kPcmDumpLimitBytes);
        }
    }
    if (const char* dumpPath = std::getenv("SL_AUDIO_SUBMIT_DUMP_PCM")) {
        if (*dumpPath && !submitDump_) {
            submitDump_ = std::fopen(dumpPath, "wb");
            submitDumpBytes_ = 0;
            sl::d27Log("AUDIO", "submit dump enabled path=%s limit=%u",
                       dumpPath, (unsigned)kPcmDumpLimitBytes);
        }
    }

    thread_ = std::thread(&AudioPipeline::threadMain, this);
    if (stream_)
        sinkQueueSamples_.store(SLAudio_GetQueuedAudioSamples(stream_));
    return true;
}

void AudioPipeline::stop() {
    if (thread_.joinable()) {
        abort_ = true;
        cond_.notify_all();
        thread_.join();
    }
    if (pcmDump_) {
        std::fclose(pcmDump_);
        pcmDump_ = nullptr;
        sl::d27Log("AUDIO", "pcm dump closed bytes=%u",
                   (unsigned)pcmDumpBytes_);
    }
    if (submitDump_) {
        std::fclose(submitDump_);
        submitDump_ = nullptr;
        sl::d27Log("AUDIO", "submit dump closed bytes=%u",
                   (unsigned)submitDumpBytes_);
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
    source_ = nullptr;
    audioIndex_ = -1;
    selfDemux_ = false;
    swrInputRate_ = 0;
    started_ = false;
    startedFlag_ = false;
    submittedBytes_ = 0;
    lastQueued_ = 0;
    sinkQueueSamples_.store(0);
    pendingSize_ = 0;
    preroll_.clear();
    prerollBytes_.store(0);
    abort_ = false;
    paused_ = false;
    seekRequest_ = false;
    seekActive_ = false;
    ack_.reset();
    startPending_ = false;
}

void AudioPipeline::putPacket(const AVPacket* packet) {
    if (!thread_.joinable())
        return;
    std::unique_lock<std::mutex> lock(lock_);
    while (!abort_ && !seekActive_ &&
           (packets_.size() >= (size_t)kPacketQueueMax ||
            packetBytes_ + (size_t)packet->size > kPacketQueueBytes)) {
        cond_.wait(lock);
    }
    if (abort_ || seekActive_)
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

std::uint64_t AudioPipeline::requestSeek(double targetSeconds) {
    if (!thread_.joinable())
        return 0;

    std::uint64_t sequence;
    {
        std::lock_guard<std::mutex> lock(lock_);
        sequence = ack_.begin();
        seekTarget_ = targetSeconds;
        seekRequest_.store(true);
        seekActive_.store(true);
        /* 请求当帧就阻断旧音频：writeFrame 输出静音，旧启动点失效。
         * 已排队的压缩帧直接丢弃，解码线程持有的 PCM 在 seekInternal 中清理。 */
        startPending_ = false;
        startAt_.store(0.0);
        skipMs_.store(0);
        for (AVPacket* packet : packets_)
            av_packet_free(&packet);
        packets_.clear();
        packetBytes_ = 0;
    }
    sl::d27Log("AUDIO", "seek queued seq=%llu target=%.3f",
               (unsigned long long)sequence, targetSeconds);
    cond_.notify_all();
    return sequence;
}

void AudioPipeline::scheduleStart(double delayMs, double skipMs) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (seekActive_.load()) {
            /* 视频首帧可能早于音频 stream 重建完成；保留对齐参数，
             * 让音频线程在新 stream 就绪后原样应用。这里必须保存绝对时刻：
             * 若等 seek 完成后才用 delayMs 重新计时，会把等待 seek 的时间
             * 再次加到音频延迟上，表现为 seek 后声音滞后。 */
            startPending_ = true;
            pendingStartAt_ = monotonicSeconds() + std::max(0.0, delayMs) / 1000.0;
            pendingStartDelayMs_ = delayMs;
            pendingStartSkipMs_ = skipMs;
            pendingStartSequence_ = ack_.sequence.load();
        } else {
            startPending_ = false;
            applyStartLocked(delayMs, skipMs);
        }
    }
    cond_.notify_all();
}

void AudioPipeline::applyStartLocked(double delayMs, double skipMs) {
    applyStartAtLocked(monotonicSeconds() + std::max(0.0, delayMs) / 1000.0, skipMs);
}

void AudioPipeline::applyStartAtLocked(double atSeconds, double skipMs) {
    skipMs_.store(std::max(0.0, skipMs));
    /* 正值让音频整体晚启动，负值让音频提前；所有启动路径只在此加一次。 */
    startAt_.store(atSeconds + startOffsetSec_);
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

double AudioPipeline::prerollMs() const {
    int rate = rate_.load();
    if (rate <= 0)
        return 0.0;
    return (double)prerollBytes_.load() * 1000.0 / (rate * 4.0);
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
            std::uint64_t sequence = 0;
            {
                std::lock_guard<std::mutex> lock(lock_);
                if (seekRequest_) {
                    seek = true;
                    target = seekTarget_;
                    sequence = ack_.sequence.load();
                }
            }
            if (seek) {
                finishSeek(sequence, seekInternal(target));
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
                std::uint64_t sequence = ack_.sequence.load();
                lock.unlock();
                finishSeek(sequence, seekInternal(target));
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

    bool needSwrInit = !swr_ || inputRate != swrInputRate_ ||
                       (int)frame->format != swrInputFormat_ ||
                       inputLayout.nb_channels != swrInputChannels_;
    if (needSwrInit) {
        if (swr_)
            swr_free(&swr_);
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr_, &outputLayout, AV_SAMPLE_FMT_S16,
                                sinkRate, &inputLayout,
                                (AVSampleFormat)frame->format, inputRate, 0,
                                nullptr) < 0 ||
            swr_init(swr_) < 0) {
            if (!warned_) {
                sl::d27Log("AUDIO",
                           "resampler init failed in=%dHz fmt=%d ch=%d "
                           "out=%dHz ch=2",
                           inputRate, frame->format, inputLayout.nb_channels,
                           sinkRate);
                warned_ = true;
            }
            swr_free(&swr_);
            return;
        }
        swrInputRate_ = inputRate;
        swrInputFormat_ = (int)frame->format;
        swrInputChannels_ = inputLayout.nb_channels;
        sl::d27Log("AUDIO",
                   "resampler init in=%dHz fmt=%d ch=%d out=%dHz fmt=S16 ch=2",
                   inputRate, frame->format, inputLayout.nb_channels, sinkRate);
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
    if (converted > 0) {
        if (pcmDump_ && pcmDumpBytes_ < kPcmDumpLimitBytes) {
            std::size_t want = (std::size_t)converted * 2 * sizeof(int16_t);
            if (want > kPcmDumpLimitBytes - pcmDumpBytes_)
                want = kPcmDumpLimitBytes - pcmDumpBytes_;
            std::fwrite(pcm_.data(), 1, want, pcmDump_);
            pcmDumpBytes_ += want;
        }
        submit(pcm_.data(), converted * 2 * (int)sizeof(int16_t));
    }
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
                prerollBytes_.fetch_add((size_t)size);
                return;
            }
        }

        started_ = true;
        startedFlag_ = true;

        int rate = rate_.load();
        /* preroll 是 stereo S16 PCM，必须以 4 字节 PCM frame 对齐；
         * 否则会把 16-bit 采样错位，表现为白噪音。 */
        size_t skipBytes = pcmSkipBytes(skipMs_.load(), rate);
        if (skipBytes > preroll_.size())
            skipBytes = preroll_.size();
        if (rate > 0) {
            sl::d27Log("AUDIO",
                       "start preroll=%.3fs skip=%.3fs offset=%.1fms",
                       preroll_.size() / (rate * 4.0),
                       skipBytes / (rate * 4.0), startOffsetSec_ * 1000.0);
        }
        if (preroll_.size() > skipBytes) {
            submitDirect(preroll_.data() + skipBytes,
                         (int)(preroll_.size() - skipBytes));
        }
        preroll_.clear();
        prerollBytes_.store(0);
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
        sinkQueueSamples_.store(queued);
        if ((int)queued <= queueTarget())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (abort_ || seekRequest_)
        return false;

    void* output = SLAudio_BeginFrame(stream_);
    if (!output) {
        if (!warned_) {
            sl::d27Log("AUDIO", "BeginFrame returned empty, drop frame");
            warned_ = true;
        }
        return true;
    }

    writeFrame(output, pending_.data(), frameBytes_);
    if (submitDump_ && submitDumpBytes_ < kPcmDumpLimitBytes) {
        std::size_t want = (std::size_t)frameBytes_;
        if (want > kPcmDumpLimitBytes - submitDumpBytes_)
            want = kPcmDumpLimitBytes - submitDumpBytes_;
        std::fwrite(output, 1, want, submitDump_);
        submitDumpBytes_ += want;
    }
    SLAudio_SubmitFrame(stream_);
    submittedBytes_ += (uint64_t)frameBytes_;
    return true;
}

void AudioPipeline::writeFrame(void* output, const uint8_t* data, int size) const {
    /* 请求 seek 后到新 stream 重建完成前，连已解码的旧 PCM 也不能出声。
     * SLAudio 没有 flush/pause 接口，静音是请求入口能立即生效的唯一手段；
     * 硬件队列里已提交的少量帧仍由 seekInternal 的 FreeStream 清掉。 */
    if (ack_.isMuted()) {
        memset(output, 0, (size_t)size);
        return;
    }

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
            sl::d27Log("AUDIO", "CreateStream failed");
            warned_ = true;
        }
        return false;
    }
    return true;
}

bool AudioPipeline::seekInternal(double targetSeconds) {
    bool ok = true;

    if (selfDemux_ && format_) {
        /* DASH 音轨由本线程解封装：HTTP Range 窗口提示必须在 av_seek_frame
         * 外围设置，不能再由播放线程在同步 seek 外层代劳。 */
        const bool hasSeekHint = source_ && source_->remote();
        if (hasSeekHint)
            source_->beginSeek(targetSeconds);
        {
            AVStream* stream = format_->streams[audioIndex_];
            int64_t ts = av_rescale_q((int64_t)(targetSeconds * AV_TIME_BASE),
                                      AV_TIME_BASE_Q, stream->time_base);
            ok = av_seek_frame(format_, audioIndex_, ts, AVSEEK_FLAG_BACKWARD) >= 0;
        }
        if (hasSeekHint)
            source_->endSeek();
        if (ok) {
            avformat_flush(format_);
        } else {
            sl::d27Log("AUDIO", "audio seek failed, possible range window miss");
        }
    }

    if (decoder_)
        avcodec_flush_buffers(decoder_);
    if (swr_)
        swr_free(&swr_);
    swrInputRate_ = 0;
    swrInputFormat_ = 0;
    swrInputChannels_ = 0;

    pendingSize_ = 0;
    preroll_.clear();
    prerollBytes_.store(0);
    started_ = false;
    startedFlag_ = false;
    submittedBytes_ = 0;
    skipMs_ = 0;
    startAt_ = 0.0;
    lastQueued_ = 0; /* 软件计数清零，不作为 SDK 队列 ACK。 */

    if (stream_) {
        SLAudio_FreeStream(stream_);
        stream_ = nullptr;
    }
    if (!ensureStream()) {
        sl::d27Log("AUDIO", "seek stream recreate failed");
        sinkQueueSamples_.store(0);
        return false;
    }
    uint32_t queued = SLAudio_GetQueuedAudioSamples(stream_);
    sinkQueueSamples_.store(queued);
    bool ready = seekAckReady(ok, true, queued);
    if (!ready && queued != 0)
        sl::d27Log("AUDIO", "seek queue not empty queued=%u", queued);
    return ready;
}

static const char* seekStatusName(SeekStatus status) {
    switch (status) {
        case SeekStatus::None: return "none";
        case SeekStatus::Pending: return "pending";
        case SeekStatus::Success: return "success";
        case SeekStatus::Failed: return "failed";
        case SeekStatus::Stale: return "stale";
    }
    return "unknown";
}

void AudioPipeline::finishSeek(std::uint64_t sequence, bool ok) {
    bool latest = false;
    {
        std::lock_guard<std::mutex> lock(lock_);
        latest = sequence == ack_.sequence.load();
        if (!latest) {
            sl::d27Log("AUDIO", "drop stale seek seq=%llu current=%llu",
                       (unsigned long long)sequence,
                       (unsigned long long)ack_.sequence.load());
        } else {
            seekRequest_.store(false);
            if (ok && startPending_ && pendingStartSequence_ == sequence) {
                /* 首帧对齐可能在音频 stream 重建完成前到达；成功时原样应用。 */
                applyStartAtLocked(pendingStartAt_, pendingStartSkipMs_);
            }
            startPending_ = false;
            ack_.finish(sequence, ok);
            seekActive_.store(false);
            /* 失败时 SeekAckState::finish 保留 mute，绝不输出旧 PCM。 */
        }
    }
    cond_.notify_all();
    if (latest) {
        sl::d27Log("AUDIO", "seek done seq=%llu ok=%d result=%s muted=%d "
                            "sink_queued=%u preroll_ms=%.1f",
                   (unsigned long long)sequence, ok ? 1 : 0,
                   seekStatusName(seekResult(sequence)),
                   ack_.isMuted() ? 1 : 0, sinkQueueSamples_.load(),
                   prerollMs());
    }
}

} // namespace sl
