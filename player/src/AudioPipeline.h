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

class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();

    bool start(CSLAudioContext* context, AVFormatContext* format, int audioIndex,
               bool selfDemux);
    /* 停止前先唤醒音频线程，避免播放线程等待包队列时 join 卡死。 */
    void interrupt();
    void stop();

    void putPacket(const AVPacket* packet);
    void setPaused(bool paused);
    bool seek(double targetSeconds);
    void scheduleStart(int delayMs, int skipMs);
    void setVolume(int volume);

    bool started() const { return startedFlag_; }
    int rate() const { return rate_; }
    double submittedMs() const;
    double delayMs() const;
    uint32_t queuedSamples() const { return lastQueued_; }
    int queuedPackets() const;

private:
    void threadMain();
    void decodePacket(AVPacket* packet, AVFrame* frame);
    void handleFrame(AVFrame* frame);
    void submit(const uint8_t* data, int size);
    void submitDirect(const uint8_t* data, int size);
    bool drainFrame();
    bool seekInternal(double targetSeconds);
    void completeSeek(bool ok);
    bool ensureStream();
    void writeFrame(void* output, const uint8_t* data, int size) const;
    int queueTarget() const;

    CSLAudioContext* context_ = nullptr;
    AVFormatContext* format_ = nullptr;
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

    std::atomic<int> skipMs_{0};
    std::atomic<double> startAt_{0.0};
    bool started_ = false;
    std::atomic<uint64_t> submittedBytes_{0};
    std::atomic<bool> startedFlag_{false};
    std::atomic<uint32_t> lastQueued_{0};
    std::atomic<int> volume_{100};
    bool warned_ = false;

    std::thread thread_;
    mutable std::mutex lock_;

    std::condition_variable cond_;
    std::deque<AVPacket*> packets_;
    size_t packetBytes_ = 0;
    std::atomic<bool> seekRequest_{false};
    bool seekDone_ = false;
    bool seekOk_ = false;
    double seekTarget_ = 0.0;

    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
};

} // namespace sl
