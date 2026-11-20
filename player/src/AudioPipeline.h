/*
 * 音频管线：解码 -> swr 仅做格式/声道 -> S16 PCM -> SLAudio。
 *
 * SLAudio 官方约定 native frequency 为 48kHz；源采样率统一重采样到 48k。
 * 所有 bytes<->ms 换算都基于提交给 SLAudio 的实际 48k rate。
 * DASH 模式由音频线程自己解封装；单文件模式由播放线程投递 AVPacket。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct CSLAudioContext;
struct CSLAudioStream;

namespace sl {

class HttpSource;

enum class SeekStatus { None, Pending, Success, Failed, Stale };

/* D28 S02：只保存当前最新序列和结果，不建立历史注册表。 */
struct SeekAckState {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<int> status{0};
    std::atomic<bool> muted{false};

    std::uint64_t begin() {
        std::uint64_t seq = sequence.fetch_add(1) + 1;
        status.store(static_cast<int>(SeekStatus::Pending));
        muted.store(true);
        return seq;
    }

    void finish(std::uint64_t seq, bool ok) {
        if (seq != sequence.load())
            return;
        status.store(static_cast<int>(ok ? SeekStatus::Success
                                         : SeekStatus::Failed));
        if (ok)
            muted.store(false);
    }

    void reset() {
        sequence.store(0);
        status.store(static_cast<int>(SeekStatus::None));
        muted.store(false);
    }

    bool pending() const {
        return status.load() == static_cast<int>(SeekStatus::Pending);
    }

    bool isMuted() const { return muted.load(); }

    SeekStatus result(std::uint64_t seq) const {
        std::uint64_t current = sequence.load();
        if (seq == 0)
            return current == 0 ? SeekStatus::None : SeekStatus::Stale;
        if (seq != current)
            return SeekStatus::Stale;
        return static_cast<SeekStatus>(status.load());
    }
};

/* seekInternal 的成功判据：self-demux seek（如适用）、新 stream 创建、
 * SDK 实际队列为空三者缺一不可。软件 lastQueued_ 不参与判断。 */
inline bool seekAckReady(bool demuxOk, bool streamOk, std::uint32_t sinkQueued) {
    return demuxOk && streamOk && sinkQueued == 0;
}

class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();

    bool start(CSLAudioContext* context, AVFormatContext* format, int audioIndex,
               bool selfDemux, HttpSource* source = nullptr);
    /* 停止前先唤醒音频线程，避免播放线程等待包队列时 join 卡死。 */
    void interrupt();
    void stop();

    void putPacket(const AVPacket* packet);
    void setPaused(bool paused);
    /* 非阻塞请求：立即阻断旧音频提交，实际 demux seek / flush / 重建由音频线程完成。 */
    std::uint64_t requestSeek(double targetSeconds);
    void scheduleStart(double delayMs, double skipMs);
    void setVolume(int volume);

    bool started() const { return startedFlag_; }
    bool seekPending() const { return seekRequest_.load() || seekActive_.load(); }
    SeekStatus seekResult(std::uint64_t seq) const { return ack_.result(seq); }
    int rate() const { return rate_; }
    double submittedMs() const;
    double delayMs() const;
    double prerollMs() const;
    std::uint32_t sinkQueueSamples() const { return sinkQueueSamples_.load(); }
    uint32_t queuedSamples() const { return lastQueued_; }
    bool decodeAllowed() const { return !paused_.load(); }
    bool audibleOutputAllowed() const {
        return decodeAllowed() && startAt_.load() > 0.0 && !ack_.isMuted();
    }
    int queuedPackets() const;

private:
    void threadMain();
    void decodePacket(AVPacket* packet, AVFrame* frame);
    void handleFrame(AVFrame* frame);
    void submit(const uint8_t* data, int size);
    void submitDirect(const uint8_t* data, int size);
    bool drainFrame();
    bool seekInternal(double targetSeconds);
    void finishSeek(std::uint64_t sequence, bool ok);
    bool ensureStream();
    void writeFrame(void* output, const uint8_t* data, int size) const;
    int queueTarget() const;
    void applyStartLocked(double delayMs, double skipMs);
    void applyStartAtLocked(double atSeconds, double skipMs);

    CSLAudioContext* context_ = nullptr;
    AVFormatContext* format_ = nullptr;
    HttpSource* source_ = nullptr;
    int audioIndex_ = -1;
    bool selfDemux_ = false;

    AVCodecContext* decoder_ = nullptr;
    SwrContext* swr_ = nullptr;
    CSLAudioStream* stream_ = nullptr;
    int swrInputRate_ = 0;

    std::atomic<int> rate_{0};
    int frameBytes_ = 0;
    std::vector<uint8_t> pending_;
    int pendingSize_ = 0;
    std::vector<uint8_t> preroll_;
    std::vector<uint8_t> pcm_;

    std::atomic<double> skipMs_{0.0};
    std::atomic<double> startAt_{0.0};
    double startOffsetSec_ = 0.0;
    bool started_ = false;
    std::atomic<uint64_t> submittedBytes_{0};
    std::atomic<bool> startedFlag_{false};
    std::atomic<uint32_t> lastQueued_{0};
    std::atomic<uint32_t> sinkQueueSamples_{0};
    std::atomic<size_t> prerollBytes_{0};
    std::atomic<int> volume_{100};
    bool warned_ = false;

    std::thread thread_;
    mutable std::mutex lock_;

    std::condition_variable cond_;
    std::deque<AVPacket*> packets_;
    size_t packetBytes_ = 0;
    /* seek 请求序列：音频线程只应用当前序列的完成，旧完成不得解除静音或启动点。 */
    std::atomic<bool> seekRequest_{false};
    std::atomic<bool> seekActive_{false};
    SeekAckState ack_;
    double seekTarget_ = 0.0; /* lock_ 保护 */
    bool startPending_ = false;
    double pendingStartAt_ = 0.0;
    double pendingStartDelayMs_ = 0.0;
    double pendingStartSkipMs_ = 0.0;
    std::uint64_t pendingStartSequence_ = 0;

    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
};

} // namespace sl
