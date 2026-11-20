#include "SlPlayer.h"

#include "AudioPipeline.h"
#include "H264Sps.h"
#include "HttpSource.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <SLAudio.h>
#include <SLVideo.h>

namespace sl {
namespace {

constexpr unsigned kVideoQueueMaxMs = 4000;
constexpr double kSyncPrerollMs = 50.0;
/* 队列反压期间不喂帧，watchdog 必须挂起；但只给有限时间，
 * 防止 VPU 真死锁时永久卡在 loading。真机 30 秒等待过长，
 * 保留 10 秒：覆盖队列自然回落，同时避免长时间无提示黑屏。 */
constexpr std::int64_t kBackpressureWatchdogSuspendMs = 10000;
/* ---- VPU 鲁棒性参数（第 1 步：事前防御 + Dirty Flag）---- */
/* 脏退出标记：起调前写、正常退出（FreeContext 之后）删；残留 = 上次异常退出 */
constexpr const char* kVpuDirtyFlagPath = "/tmp/slplayer_vpu.lock";
/* 脏标记清洗：建一次 Context 再释放，冲刷残留会话 */
constexpr int kVpuCleanupDelayMs = 200;
/* 起调失败复位：FreeContext → 延时 → CreateContext，有限次重试 */
constexpr int kVpuInitRetryCount = 2;
constexpr int kVpuInitRetryDelayMs = 200;
/* 重建 stream 前给硬件的清理间隔（待真机校准：50 / 100 / 200ms） */
constexpr int kStreamRebuildGapMs = 50;
/* 连续 seek 时的最小重建间隔：50ms 实测会在高 refs 片源上迅速耗尽 SHM 连续块。
 * 只限制“两次重建之间”，单次 seek 不受影响；数值待真机继续校准。 */
constexpr int kSeekRebuildMinGapMs = 400;

constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

/* 正常退出卡住时用于定位阶段；默认关闭，SL_STOP_DEBUG=1 才输出。 */
bool stopDebugEnabled() {
    static const bool enabled = std::getenv("SL_STOP_DEBUG") != nullptr;
    return enabled;
}

void stopLog(const char* stage) {
    if (stopDebugEnabled())
        sl::d27Log("PLAYER", "stop stage=%s", stage);
}

bool syncDebugEnabled() {
    static const bool enabled = std::getenv("SL_SYNC_DEBUG") != nullptr;
    return enabled;
}

void syncLog(const char* fmt, ...) {
    if (!syncDebugEnabled())
        return;
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    sl::d27Log("PLAYER", "sync %s", message);
}

void syncEvent(const SourceSpec& spec, std::uint64_t epoch,
               std::uint64_t requestId, const char* event, const char* fmt, ...) {
    char detail[1024] = {0};
    if (fmt && *fmt) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);
    }
    sl::DebugLogContext context = spec.debug;
    context.requestId = requestId;
    if (detail[0])
        sl::d27LogContext("SYNC", context, "%s epoch=%llu %s", event,
                          (unsigned long long)epoch, detail);
    else
        sl::d27LogContext("SYNC", context, "%s epoch=%llu", event,
                          (unsigned long long)epoch);
}

double meanFps(AVRational rate) {
    double fps = av_q2d(rate);
    if (fps < 1.0 || fps > 120.0)
        return 25.0;
    return fps;
}

/* 从 B 站 DASH URL 里提取 oid/qn，避免把整条签名 URL 写进日志。
 * 典型片段：/upgcxcode/34/04/43684400434/43684400434-1-30064.m4s?... */
std::string sourceTag(const std::string& url) {
    size_t query = url.find('?');
    size_t pathEnd = (query == std::string::npos) ? url.size() : query;
    if (pathEnd == 0)
        return "unknown";
    size_t slash = url.rfind('/', pathEnd - 1);
    if (slash == std::string::npos)
        return "local";
    size_t prev = (slash == 0) ? 0 : url.rfind('/', slash - 1);
    if (prev == std::string::npos)
        return "local";
    std::string oid = url.substr(prev + 1, slash - prev - 1);
    std::string file = url.substr(slash + 1, pathEnd - slash - 1);
    size_t dash = file.rfind('-');
    size_t dot = file.rfind('.');
    if (dash == std::string::npos || dot == std::string::npos || dot <= dash + 1)
        return "oid=" + oid;
    return "oid=" + oid + " qn=" + file.substr(dash + 1, dot - dash - 1);
}

/* steady_clock 单调毫秒（watchdog 心跳用，不受系统时间调整影响） */
std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* VPU 生命周期日志：只在 open/stop/CreateStream/FreeStream 边界输出，
 * 用来对照切清晰度与退出时的驱动调用顺序，不进入逐帧路径。 */
void vpuLog(const char* fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    sl::d27Log("PLAYER", "vpu %lld %s", (long long)steadyNowMs(), message);
}

/* ---- Dirty Flag 辅助（文件 I/O 只做标记，不碰 VPU）---- */
bool dirtyFlagPresent() {
    return std::fopen(kVpuDirtyFlagPath, "r") != nullptr;
}

void writeDirtyFlag() {
    FILE* f = std::fopen(kVpuDirtyFlagPath, "w");
    if (f) {
        std::fprintf(f, "SlPlayer VPU session in progress\n");
        std::fclose(f);
    } else {
        sl::d27Log("PLAYER", "warn dirty flag write failed path=%s",
                   kVpuDirtyFlagPath);
    }
}

void clearDirtyFlag() {
    if (std::remove(kVpuDirtyFlagPath) != 0 && errno != ENOENT) {
        sl::d27Log("PLAYER", "warn dirty flag clear failed path=%s",
                   kVpuDirtyFlagPath);
    }
}

} // namespace

const char* eventTypeName(EventType type) {
    switch (type) {
    case EventType::Opened: return "Opened";
    case EventType::PlaybackStart: return "PlaybackStart";
    case EventType::Progress: return "Progress";
    case EventType::Paused: return "Paused";
    case EventType::Resumed: return "Resumed";
    case EventType::Seeked: return "Seeked";
    case EventType::EndFile: return "EndFile";
    case EventType::Error: return "Error";
    case EventType::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

const char* fatalTypeName(FatalType type) {
    switch (type) {
    case FatalType::VpuInitFailed: return "VpuInitFailed";
    case FatalType::VpuRebuildFailed: return "VpuRebuildFailed";
    case FatalType::FrameSubmitStalled: return "FrameSubmitStalled";
    case FatalType::UnsupportedStream: return "UnsupportedStream";
    }
    return "Unknown";
}

struct Player::Impl {
    Impl() = default;
    ~Impl() { stop(); }

    void setEventCallback(EventCallback cb) {
        std::lock_guard<std::mutex> lock(mutex);
        callback = std::move(cb);
    }

    void setFatalCallback(FatalCallback cb) {
        std::lock_guard<std::mutex> lock(mutex);
        fatalCallback = std::move(cb);
    }

    bool start(const SourceSpec& spec) {
        if (thread.joinable() &&
            std::this_thread::get_id() == thread.get_id()) {
            sl::d27Log("PLAYER", "open rejected from event callback");
            return false;
        }

        vpuLog("open: stop previous player");
        stop();
        vpuLog("open: previous player stopped");
        if (spec.videoUrl.empty())
            return false;

        {
            std::lock_guard<std::mutex> lock(mutex);
            spec_ = spec;
        }

        syncState_.begin(spec.sync.epoch, spec.sync.requestId,
                         spec.sync.desiredPlay);
        {
            std::lock_guard<std::mutex> lock(syncTextMutex);
            syncReason_ = spec.sync.reason;
        }
        pendingAudioSeq_.store(0);
        pauseTargetMs_.store(spec.sync.targetMs >= 0
                                 ? (double)spec.sync.targetMs
                                 : spec.startSeconds * 1000.0);
        pausePending_.store(false);
        lastTimelineRequestId_.store(spec.sync.requestId);
        stop_ = false;
        paused_ = false;
        targetPaused_ = false;
        pendingSeek_ = -1.0;
        pendingSeekId_ = 0;
        latestSeekRequestId_ = 0;
        pauseRequestId_ = 0;
        resumeRequestId_ = 0;
        seeking_ = false;
        pauseFlush_ = false;
        pauseFlushed_ = false;
        resumeSeek_ = false;
        resumeInProgress_ = false;
        fatalMask_ = 0;
        backpressure_ = false;
        lastStreamRebuildMs_ = 0;
        position_ = spec.startSeconds > 0.0 ? spec.startSeconds : 0.0;
        duration_ = spec.endSeconds > 0.0 ? spec.endSeconds : 0.0;
        videoFrames_ = 0;
        droppedFrames_ = 0;
        beginRetries_ = 0;
        videoQueueMs_ = 0;
        audio_.setVolume(volume_.load());
        {
            std::lock_guard<std::mutex> lock(infoMutex);
            info_ = VideoInfo();
        }

        running_ = true;
        /* 线程入口兜底：run() 内若抛出非回调异常（如 bad_alloc），
         * 栈展开时 guard 已清理 VPU，这里捕获避免 std::terminate 拖垮进程。 */
        thread = std::thread([this] {
            try {
                run();
            } catch (const std::exception& e) {
                sl::d27Log("PLAYER", "playback thread exception %s", e.what());
                running_ = false;
            } catch (...) {
                sl::d27Log("PLAYER", "playback thread unknown exception");
                running_ = false;
            }
        });
        return true;
    }

    void stop() {
        if (!thread.joinable()) {
            running_ = false;
            return;
        }
        if (std::this_thread::get_id() == thread.get_id()) {
            stopLog("self-stop: set stop_, interrupt sources");
            stop_ = true;
            audio_.interrupt();
            videoSource_.interrupt();
            audioSource_.interrupt();
            return;
        }

        stopLog("set stop_, interrupt audio/video sources");
        stop_ = true;
        /* 先唤醒音频线程：它可能在 self-demux 或包队列 cond 上等待。 */
        audio_.interrupt();
        videoSource_.interrupt();
        audioSource_.interrupt();
        vpuLog("stop: join player thread");
        stopLog("join player thread");
        thread.join();
        vpuLog("stop: player thread joined");
        stopLog("player thread joined");
        running_ = false;
    }

    void setPaused(bool value, std::uint64_t requestId,
                   const SyncContext& sync) {
        if (syncState_.isStaleCommand(sync.epoch)) {
            syncEvent(spec_, syncState_.epoch.load(), requestId, "SYNC_STALE",
                      "reason=epoch_mismatch event_epoch=%llu",
                      (unsigned long long)sync.epoch);
            return;
        }
        syncState_.begin(sync.epoch ? sync.epoch : syncState_.epoch.load(),
                         requestId, !value);
        {
            std::lock_guard<std::mutex> lock(syncTextMutex);
            syncReason_ = sync.reason;
        }
        pendingAudioSeq_.store(0);
        pausePending_.store(false);
        lastTimelineRequestId_.store(requestId);
        bool previous = targetPaused_.exchange(value);
        if (value)
            pauseRequestId_.store(requestId);
        else
            resumeRequestId_.store(requestId);
        if (previous == value)
            return;

        if (value) {
            /* SLVideo 队列里还有 ~2.4s 画面，必须由播放线程清空才能立即暂停 */
            pauseTargetMs_.store(position_.load() * 1000.0);
            pauseFlush_ = true;
        } else {
            /* 音频要等视频首帧重建并对齐后再恢复，避免声音先跑出去。 */
            if (paused_.load()) {
                resumeTarget_.store(pauseTargetMs_.load() / 1000.0);
                resumeSeek_ = true;
                resumeInProgress_ = true;
            }
        }
        /* 事件必须由播放线程在实际 flush/首帧完成后发出，命令入口只改目标态。 */
    }

    void requestSeek(double seconds, std::uint64_t requestId,
                     const SyncContext& sync) {
        if (syncState_.isStaleCommand(sync.epoch)) {
            syncEvent(spec_, syncState_.epoch.load(), requestId, "SYNC_STALE",
                      "reason=epoch_mismatch event_epoch=%llu",
                      (unsigned long long)sync.epoch);
            return;
        }
        syncState_.begin(sync.epoch ? sync.epoch : syncState_.epoch.load(),
                         requestId, sync.desiredPlay);
        {
            std::lock_guard<std::mutex> lock(syncTextMutex);
            syncReason_ = sync.reason;
        }
        pendingAudioSeq_.store(0);
        pausePending_.store(false);
        lastTimelineRequestId_.store(requestId);
        if (seconds < 0.0)
            seconds = 0.0;
        latestSeekRequestId_.store(requestId);
        pendingSeekId_.store(requestId);
        pendingSeek_ = seconds;
    }

    void setVolume(int percent) {
        int value = std::min(100, std::max(0, percent));
        volume_ = value;
        audio_.setVolume(value);
    }

    Stats stats() const {
        Stats result;
        result.position = position_.load();
        result.duration = duration_.load();
        result.videoFrames = videoFrames_.load();
        result.droppedFrames = droppedFrames_.load();
        result.beginRetries = beginRetries_.load();
        result.videoQueueMs = videoQueueMs_.load();
        result.audioRate = audio_.rate();
        result.audioStarted = audio_.started();
        result.audioQueuedSamples = audio_.queuedSamples();
        result.audioDelayMs = audio_.delayMs();
        result.audioSubmittedMs = audio_.submittedMs();
        result.audioPackets = audio_.queuedPackets();
        result.downloadedBytes = videoSource_.downloadedBytes();
        result.bufferedBytes = videoSource_.bufferedBytes();
        result.readPosition = videoSource_.readPosition();
        result.bufferBaseOffset = videoSource_.baseOffset();
        result.stage = stage_.load();
        return result;
    }

    /*
     * 无锁心跳：只读 atomic，不取任何锁、不碰数据源查询。
     * watchdog 线程高频读它也不会被播放线程拖住（复核见 06 第三节）。
     */
    Heartbeat heartbeat() const {
        Heartbeat hb;
        hb.lastFrameTimeMs = lastFrameTimeMs_.load();
        hb.stage = stage_.load();
        hb.videoFrames = videoFrames_.load();
        hb.presentationMs = position_.load() * 1000.0;
        hb.videoQueueMs = videoQueueMs_.load();
        hb.backpressure = backpressure_.load();
        const std::int64_t videoErrorMs = videoSource_.lastNetworkErrorMs();
        const std::int64_t audioErrorMs = audioSource_.lastNetworkErrorMs();
        const std::int64_t latestErrorMs = (std::max)(videoErrorMs, audioErrorMs);
        hb.networkErrorAgeMs = latestErrorMs > 0
                                   ? (std::max<std::int64_t>(
                                         0, steadyNowMs() - latestErrorMs))
                                   : INT64_MAX;
        hb.workerAlive = running_.load();
        hb.currentPaused = paused_.load();
        hb.targetPaused = targetPaused_.load();
        hb.seeking = seeking_.load() || pendingSeek_.load() >= 0.0;
        if (resumeInProgress_.load())
            hb.currentPaused = false;
        hb.playing = hb.workerAlive && !hb.currentPaused && !hb.seeking;
        /*
         * pause/seek 重建期间队列本来就不增长，watchdog 必须挂起判定。
         * 真机验证发现：仅用 pauseFlush_/pauseFlushed_ 会在暂停静默期失效
         * （清队列完成后 pauseFlushed_ 仍为 true，但恢复 seek 完成会置回 false），
         * 导致长时间暂停时 frameAge 持续增长、transition=0 → watchdog 误判卡死。
         * 因此只要 paused_ 为真就挂起；seek/resume 重建另用标志覆盖。
         */
        hb.inTransition = hb.currentPaused || hb.targetPaused ||
                          pauseFlush_.load() ||
                          pauseFlushed_.load() || resumeSeek_.load() ||
                          resumeInProgress_.load() ||
                          hb.seeking || awaitingFirstFrame_.load() ||
                          (backpressure_.load() &&
                           steadyNowMs() - backpressureSinceMs_.load() <
                               kBackpressureWatchdogSuspendMs);
        return hb;
    }

    void emit(EventType type, std::string message = std::string(),
              std::uint64_t requestId = 0) {
        emitWithPosition(type, position_.load(), std::move(message), requestId);
    }

    void emitWithPosition(EventType type, double position,
                          std::string message = std::string(),
                          std::uint64_t requestId = 0) {
        EventCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = callback;
        }
        if (!cb)
            return;

        Event event;
        event.type = type;
        event.position = position;
        event.duration = duration_.load();
        event.requestId = requestId;
        event.message = std::move(message);
        /* 用户回调抛异常不能穿过播放线程导致 terminate / 跳过 VPU 清理 */
        try {
            cb(event);
        } catch (const std::exception& e) {
            sl::d27Log("PLAYER", "event callback exception %s", e.what());
        } catch (...) {
            sl::d27Log("PLAYER", "event callback unknown exception");
        }
    }

    /* VPU 致命错误上报：与 emit 同样保护，回调异常不穿透播放线程 */
    void emitFatal(FatalType type, const std::string& message) {
        /* 同一类 fatal 在一次播放中只上报一次。BeginFrame 失败后每帧都会
         * 重试 500ms，旧行为每 0.5s 向上报一次，兼容层每次都关闭透明层并
         * 发 END_FILE(ERROR)，用户看到白屏且 watchdog 被停用。 */
        int bit = 1 << (int)type;
        if (fatalMask_.fetch_or(bit) & bit)
            return;
        FatalCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = fatalCallback;
        }
        sl::d27Log("PLAYER", "fatal type=%s message=%s",
                   fatalTypeName(type), message.c_str());
        if (!cb)
            return;
        try {
            cb(type, message);
        } catch (const std::exception& e) {
            sl::d27Log("PLAYER", "fatal callback exception %s", e.what());
        } catch (...) {
            sl::d27Log("PLAYER", "fatal callback unknown exception");
        }
    }

    std::string userAgent() const {
        if (!spec_.userAgent.empty())
            return spec_.userAgent;
        return kDefaultUserAgent;
    }

    void run();

    mutable std::mutex mutex;
    EventCallback callback;
    FatalCallback fatalCallback;
    std::atomic<int> fatalMask_{0};
    std::atomic<bool> backpressure_{false};
    std::atomic<std::int64_t> backpressureSinceMs_{0};
    SourceSpec spec_;

    HttpSource videoSource_;
    HttpSource audioSource_;
    AudioPipeline audio_;

    std::thread thread;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> targetPaused_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> pauseFlush_{false};
    std::atomic<bool> pauseFlushed_{false};
    std::atomic<bool> resumeSeek_{false};
    std::atomic<bool> resumeInProgress_{false};
    std::atomic<bool> awaitingFirstFrame_{false};
    std::atomic<std::int64_t> lastStreamRebuildMs_{0};
    std::atomic<bool> seeking_{false};
    SyncStateTracker syncState_;
    std::atomic<std::uint64_t> pendingAudioSeq_{0};
    std::atomic<std::uint64_t> lastTimelineRequestId_{0};
    std::atomic<double> pauseTargetMs_{0.0};
    std::atomic<bool> pausePending_{false};
    std::mutex syncTextMutex;
    std::string syncReason_;
    std::atomic<double> resumeTarget_{0.0};
    std::atomic<double> pendingSeek_{-1.0};
    std::atomic<std::uint64_t> pendingSeekId_{0};
    std::atomic<std::uint64_t> latestSeekRequestId_{0};
    std::atomic<std::uint64_t> pauseRequestId_{0};
    std::atomic<std::uint64_t> resumeRequestId_{0};
    std::atomic<int> volume_{100};
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<int> videoFrames_{0};
    std::atomic<int> droppedFrames_{0};
    std::atomic<int> beginRetries_{0};
    std::atomic<unsigned> videoQueueMs_{0};
    std::atomic<int> stage_{0};
    /* 第 2 步：最后成功 SubmitFrame 的 steady_clock 毫秒时间戳（watchdog 心跳） */
    std::atomic<std::int64_t> lastFrameTimeMs_{0};

    mutable std::mutex infoMutex;
    VideoInfo info_;
};

void Player::Impl::run() {
    SourceSpec spec;
    {
        std::lock_guard<std::mutex> lock(mutex);
        spec = spec_;
    }
    CSLVideoContext* videoContext = nullptr;
    CSLVideoStream* videoStream = nullptr;
    CSLAudioContext* audioContext = nullptr;
    AVBSFContext* bsf = nullptr;
    AVPacket* packet = nullptr;
    AVPacket* filtered = nullptr;
    AVPacket* pendingPacket = nullptr;
    bool havePendingPacket = false;

    AVFormatContext* format = nullptr;
    AVStream* stream = nullptr;
    int videoIndex = -1;
    AVRational frameRate{25, 1};
    double fps = 25.0;
    double duration = 0.0;
    double videoClock = spec.startSeconds;
    double maxSubmittedMs = 0.0;
    /* 音频流开始解码对应的原始媒体时间点：
     * 初始播放为 0；seek/暂停恢复后是实际关键帧 PTS。
     * 计算音频预缓冲 skip 时必须减去该基准，否则会把目标位置之前的内容重复跳过。 */
    double audioBaseMs = 0.0;
    /* 严格 seek 对齐：音频目标用请求时间点，视频实际从关键帧开始解码；
     * 首帧提交后按“已测得的上屏延迟 + 关键帧到目标的时间差”确定音频启动时刻。 */
    double seekDisplayTargetMs = 0.0;
    double firstFramePtsAfterSeekMs = -1.0;
    int measuredVideoLatencyMs = 0;
    bool firstFrameAfterSeekRecorded = false;

    bool audioReady = false;
    bool selfDemux = false;
    bool rewriteSps = false;
    std::vector<uint8_t> patchedExtradata;
    std::vector<uint8_t> rewriteBuffer;
    int spsFrameLogs = 0;
    const char* dumpFramePath = std::getenv("SL_DUMP_FRAME");
    bool frameDumped = false;

    /* SPS 改写默认关闭：用户多轮真机测试确认 refs 7->4 会让不少视频花屏。
     * SL_SPS_REWRITE=1 仅作对照实验；SL_SPS_TARGET 可指定目标。
     * VPU/SHM 压力改由减少重建次数和重建间隔处理。 */
    const char* rewriteEnv = std::getenv("SL_SPS_REWRITE");
    bool enableRewrite = rewriteEnv && std::strcmp(rewriteEnv, "0") != 0;
    bool explicitRewrite = rewriteEnv && std::strcmp(rewriteEnv, "1") == 0;
    int spsTargetRefs = 4;
    /* -1 = 保留片源值：默认只降 refs，不缩短重排/解码缓冲。 */
    int spsTargetReorder = -1;
    int spsTargetBuffering = -1;
    if (const char* target = std::getenv("SL_SPS_TARGET")) {
        int refs = 0;
        int reorder = 0;
        int buffering = 0;
        if (sscanf(target, "%d,%d,%d", &refs, &reorder, &buffering) == 3 &&
            refs > 0 && reorder >= 0 && buffering > 0) {
            spsTargetRefs = refs;
            spsTargetReorder = reorder;
            spsTargetBuffering = buffering;
        }
    }
    sl::d27Log("PLAYER", "sps policy refs<=%d reorder=%s buffering=%s rewrite=%s",
               spsTargetRefs, spsTargetReorder < 0 ? "keep" : "clamp",
               spsTargetBuffering < 0 ? "keep" : "clamp",
               enableRewrite ? "on" : "off");
    bool videoStarted = false;
    bool afterSeek = false;
    bool audioScheduled = false;
    /* 稳定后仍等画面位置走到目标点才启动音频；
     * seek 后喂帧速率可能长时间低于墙钟，固定 delay 会必然失准。 */
    bool audioPendingPosition = false;
    double audioStartPositionMs = 0.0;
    std::uint64_t pendingResumeRequestId = 0;
    bool pendingResumeEvent = false;
    /* D23 验收诊断：seek 入口到视频首帧提交的墙钟间隔，只在 --diag 下输出。 */
    std::int64_t lastSeekStartMs = 0;
    double lastSeekTargetMs = 0.0;
    const bool playDiag = std::getenv("SL_PLAY_DIAG") != nullptr;
    /* 同步失败判据：只看呈现位置是否持续推进，不看墙钟总耗时。
     * 慢但在推进的内容继续等；只有高水位长时间不动才判失败。 */
    constexpr double kSyncProgressStepMs = 100.0;
    int syncNoProgressMs = 5000;
    if (const char* noProgress = std::getenv("SL_SYNC_NO_PROGRESS_MS")) {
        int value = std::atoi(noProgress);
        if (value >= 1000)
            syncNoProgressMs = value;
    }
    bool syncWatchActive = false;
    double syncHighWaterMs = 0.0;
    std::int64_t lastSyncProgressMs = 0;
    bool syncPostStartLogged = false;
    std::int64_t syncStartWallMs = 0;
    double syncStartSkipMs = 0.0;
    uint32_t queueMax = 0;
    int queueFlatFrames = 0;
    int videoFrames = 0;
    auto nextDeadline = std::chrono::steady_clock::now();
    auto lastProgress = nextDeadline;

    /* shutdown 幂等：显式调用与 RAII 兜底可能都触发，保证只释放一次 */
    bool shutdownDone = false;
    auto shutdown = [&] {
        if (shutdownDone)
            return;
        shutdownDone = true;
        stopLog("shutdown: audio_.stop");
        audio_.stop();
        stopLog("shutdown: SLAudio_FreeContext");
        if (audioContext) {
            SLAudio_FreeContext(audioContext);
            audioContext = nullptr;
        }
        stopLog("shutdown: SLVideo_FreeStream");
        if (videoStream) {
            vpuLog("shutdown: FreeStream enter");
            SLVideo_FreeStream(videoStream);
            videoStream = nullptr;
            vpuLog("shutdown: FreeStream done");
        }
        if (bsf)
            av_bsf_free(&bsf);
        if (packet)
            av_packet_free(&packet);
        if (filtered)
            av_packet_free(&filtered);
        if (pendingPacket)
            av_packet_free(&pendingPacket);
        audioSource_.close();
        videoSource_.close();
        stopLog("shutdown: SLVideo_FreeContext");
        if (videoContext) {
            vpuLog("shutdown: FreeContext enter");
            SLVideo_FreeContext(videoContext);
            videoContext = nullptr;
            vpuLog("shutdown: FreeContext done");
        }
        /* 必须在 FreeContext 之后清标记：只有完整走完退出流程才认为 VPU 干净 */
        stopLog("shutdown: clear dirty flag");
        clearDirtyFlag();
        stopLog("shutdown: done");
        running_ = false;
    };

    /* RAII 兜底：emit() 触发的用户回调若抛异常，栈展开时仍会 shutdown，
     * 不会把 VPU stream/context 泄漏在脏状态里（复核见 06 文档第三节）。 */
    struct ShutdownGuard {
        decltype(shutdown)* fn;
        ~ShutdownGuard() { (*fn)(); }
    } guard{&shutdown};

    auto fail = [&](const std::string& message) {
        emit(EventType::Error, message);
        shutdown();
    };

    /*
     * VPU 起调前置清洗（Dirty Flag）：
     * 若标记残留说明上次异常退出（含被 SIGKILL），VPU 可能处于脏状态。
     * SDK 无全局 Deinit，最接近复位的动作是建一次 Context 再释放并延时，
     * 冲刷用户态残留会话。清洗本身不喂任何帧，不会成为新污染源。
     */
    bool wasDirty = dirtyFlagPresent();
    if (wasDirty) {
        sl::d27Log("PLAYER", "dirty flag detected, pre-clean");
        CSLVideoContext* probe = SLVideo_CreateContext();
        if (probe) {
            SLVideo_FreeContext(probe);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kVpuCleanupDelayMs));
            sl::d27Log("PLAYER", "pre-clean done");
        } else {
            sl::d27Log("PLAYER", "pre-clean CreateContext failed, continue");
        }
    }
    writeDirtyFlag();

    SLVideo_SetLogLevel(k_ESLVideoLogError);
    /* 起调失败复位重试：CreateContext 失败无旧 context 可 free，只延时重试 */
    for (int attempt = 0; attempt <= kVpuInitRetryCount; ++attempt) {
        videoContext = SLVideo_CreateContext();
        if (videoContext)
            break;
        if (attempt < kVpuInitRetryCount) {
            sl::d27Log("PLAYER", "CreateContext failed, retry %d/%d after %dms",
                       attempt + 1, kVpuInitRetryCount, kVpuInitRetryDelayMs);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kVpuInitRetryDelayMs));
        }
    }
    if (!videoContext) {
        emitFatal(FatalType::VpuInitFailed, "SLVideo_CreateContext 失败（含复位重试）");
        fail("SLVideo_CreateContext 失败（含复位重试）");
        return;
    }

    if (!videoSource_.open(spec.videoUrl, spec.referrer, userAgent())) {
        fail("视频源打开失败: " + videoSource_.error());
        return;
    }

    format = videoSource_.format();
    videoIndex = videoSource_.videoIndex();
    if (!format || videoIndex < 0) {
        fail("没有找到视频流");
        return;
    }

    stream = format->streams[videoIndex];
    if (stream->codecpar->codec_id != AV_CODEC_ID_H264) {
        fail("仅支持 H.264 视频流");
        return;
    }

    frameRate = stream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0)
        frameRate = stream->r_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0)
        frameRate = AVRational{25, 1};
    fps = meanFps(frameRate);

    duration = format->duration > 0 ? format->duration / (double)AV_TIME_BASE : 0.0;
    if (duration <= 0.0 && stream->duration > 0)
        duration = stream->duration * av_q2d(stream->time_base);
    if (spec.endSeconds > 0.0 && (duration <= 0.0 || duration > spec.endSeconds))
        duration = spec.endSeconds;
    duration_ = duration;
    {
        std::lock_guard<std::mutex> lock(infoMutex);
        info_.width = stream->codecpar->width;
        info_.height = stream->codecpar->height;
        info_.fps = fps;
        info_.duration = duration;
    }
    sl::d27LogIdentity("PLAYER",
                       "media %s audio=%zu duration=%.3f %dx%d fps=%.3f",
                       sourceTag(spec.videoUrl).c_str(), spec.audioUrls.size(),
                       duration, info_.width, info_.height, fps);
    /* 前置 SPS 门禁：只降 refs，默认保留 reorder/buffering。
     * 实测 7->4 refs 可避免高参考帧片源在 seek 后 VDEC SHM 分配失败；
     * 同时压低 reorder/buffering 会截断 B 帧重排，出现花屏/抽动。 */
    int effectiveSpsReorder = spsTargetReorder;
    int effectiveSpsBuffering = spsTargetBuffering;
    bool needSpsRewrite = false;
    if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
        H264SpsInfo sps = parseAvcC(stream->codecpar->extradata,
                                    (size_t)stream->codecpar->extradata_size);
        if (!sps.valid) {
            sl::d27Log("PLAYER", "SPS parse failed, continue");
        } else {
            effectiveSpsReorder = spsTargetReorder < 0
                                      ? sps.reorder
                                      : std::min(sps.reorder, spsTargetReorder);
            effectiveSpsBuffering =
                spsTargetBuffering < 0
                    ? sps.buffering
                    : std::min(sps.buffering, spsTargetBuffering);
            needSpsRewrite = sps.refs > spsTargetRefs ||
                             effectiveSpsReorder < sps.reorder ||
                             effectiveSpsBuffering < sps.buffering;
            sl::d27Log("PLAYER",
                       "SPS refs=%d reorder=%d buffering=%d target=%d/%d/%d "
                       "need=%d rewrite=%d",
                       sps.refs, sps.reorder, sps.buffering,
                       spsTargetRefs, effectiveSpsReorder, effectiveSpsBuffering,
                       needSpsRewrite ? 1 : 0, enableRewrite ? 1 : 0);
            if (!enableRewrite && needSpsRewrite) {
                sl::d27Log("PLAYER", "SPS rewrite disabled by SL_SPS_REWRITE=0");
            }
        }

        if (sps.valid && enableRewrite && needSpsRewrite) {
            patchedExtradata.assign(stream->codecpar->extradata,
                                    stream->codecpar->extradata +
                                        stream->codecpar->extradata_size);
            bool changed = false;
            if (!patchAvcC(patchedExtradata, spsTargetRefs,
                           effectiveSpsReorder, effectiveSpsBuffering,
                           &changed) ||
                !changed) {
                patchedExtradata.clear();
                if (explicitRewrite) {
                    emitFatal(FatalType::UnsupportedStream,
                              "当前清晰度编码档次不受 Steam Link 硬件支持");
                    emit(EventType::Unsupported,
                         "当前清晰度编码档次不受 Steam Link 硬件支持");
                    shutdown();
                    return;
                }
                sl::d27Log("PLAYER", "SPS rewrite failed, use original stream");
            } else {
                rewriteSps = true;
                H264SpsInfo patched = parseAvcC(patchedExtradata.data(),
                                                patchedExtradata.size());
                sl::d27Log("PLAYER",
                           "SPS rewritten refs %d->%d reorder %d->%d buffering %d->%d",
                           sps.refs, patched.refs, sps.reorder, patched.reorder,
                           sps.buffering, patched.buffering);
            }
        }
    } else {
        sl::d27Log("PLAYER", "no extradata, skip SPS gate");
    }

    emit(EventType::Opened);

    const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter || av_bsf_alloc(filter, &bsf) < 0) {
        fail("h264_mp4toannexb 初始化失败");
        return;
    }
    if (avcodec_parameters_copy(bsf->par_in, stream->codecpar) < 0) {
        fail("复制编码参数失败");
        return;
    }
    if (!patchedExtradata.empty()) {
        av_freep(&bsf->par_in->extradata);
        bsf->par_in->extradata = (uint8_t*)av_malloc(
            patchedExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!bsf->par_in->extradata) {
            fail("SPS 缓冲分配失败");
            return;
        }
        memcpy(bsf->par_in->extradata, patchedExtradata.data(),
               patchedExtradata.size());
        memset(bsf->par_in->extradata + patchedExtradata.size(), 0,
               AV_INPUT_BUFFER_PADDING_SIZE);
        bsf->par_in->extradata_size = (int)patchedExtradata.size();
    }
    bsf->time_base_in = stream->time_base;
    if (av_bsf_init(bsf) < 0) {
        fail("av_bsf_init 失败");
        return;
    }

    /*
     * CreateStream 失败复位重试：有旧 context，按 FreeContext → 延时 →
     * CreateContext → CreateStream 的序列重试。重建 context 后 applyStreamParams
     * 仍能正常工作（它只依赖 videoStream 和帧率参数）。
     */
    for (int attempt = 0; attempt <= kVpuInitRetryCount; ++attempt) {
        vpuLog("CreateStream initial enter attempt=%d", attempt);
        videoStream = SLVideo_CreateStream(videoContext, k_ESLVideoFormatH264, 0);
        vpuLog("CreateStream initial result=%p", (void*)videoStream);
        if (videoStream)
            break;
        if (attempt < kVpuInitRetryCount) {
            sl::d27Log("PLAYER", "CreateStream failed, retry %d/%d",
                       attempt + 1, kVpuInitRetryCount);
            if (videoContext) {
                SLVideo_FreeContext(videoContext);
                videoContext = nullptr;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kVpuInitRetryDelayMs));
            videoContext = SLVideo_CreateContext();
            if (!videoContext) {
                sl::d27Log("PLAYER", "CreateContext failed during reset");
                break;
            }
        }
    }
    if (!videoStream) {
        emitFatal(FatalType::VpuInitFailed, "SLVideo_CreateStream 失败（含复位重试）");
        fail("SLVideo_CreateStream 失败（含复位重试）");
        return;
    }
    auto applyStreamParams = [&] {
        if (!videoStream)
            return;
        SLVideo_SetStreamTargetFramerate(videoStream, (unsigned)frameRate.num,
                                         (unsigned)frameRate.den);
        /* Kodi/Moonlight 均不设置目标码率；1000 kbit/s 固定值和按 bw 换算
         * 都已在真机上复现问题，这里保持 SDK 默认值。 */
    };
    applyStreamParams();
    sl::d27Log("PLAYER", "SLVideo stream ready");

    /*
     * 重建 stream 前的清理间隔：seek/pause 都是 FreeStream → CreateStream，
     * 中间留一小段延时给硬件消化 RPC 销毁，降低把 VPU 带入脏状态的风险。
     * 延时值是保守默认（50ms），待真机对照校准（见 06 文档第九节）。
     */
    auto rebuildStream = [&]() -> bool {
        /* watchdog 在 first frame 到达前必须挂起：FreeStream/CreateStream 期间
         * 队列不增长，但此时 transition 已经归零，真机会在 1.5s 后被误杀。 */
        awaitingFirstFrame_.store(true);
        std::int64_t last = lastStreamRebuildMs_.load();
        std::int64_t now = steadyNowMs();
        if (last > 0 && now - last < kSeekRebuildMinGapMs)
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (size_t)(kSeekRebuildMinGapMs - (now - last))));
        if (videoStream) {
            vpuLog("rebuild: FreeStream enter");
            SLVideo_FreeStream(videoStream);
            videoStream = nullptr;
            vpuLog("rebuild: FreeStream done");
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kStreamRebuildGapMs));
        /* 重建也带有限复位重试，避免重建瞬间碰到脏硬件直接失败 */
        for (int attempt = 0; attempt <= kVpuInitRetryCount; ++attempt) {
            vpuLog("rebuild: CreateStream enter attempt=%d", attempt);
            videoStream =
                SLVideo_CreateStream(videoContext, k_ESLVideoFormatH264, 0);
            vpuLog("rebuild: CreateStream result=%p", (void*)videoStream);
            if (videoStream)
                break;
            if (attempt < kVpuInitRetryCount) {
                if (videoContext) {
                    SLVideo_FreeContext(videoContext);
                    videoContext = nullptr;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kVpuInitRetryDelayMs));
                videoContext = SLVideo_CreateContext();
                if (!videoContext)
                    return false;
            }
        }
        if (!videoStream)
            return false;
        applyStreamParams();
        lastStreamRebuildMs_.store(steadyNowMs());
        return true;
    };

    /* 独立音轨（DASH）：主地址失败时顺序尝试备份地址 */
    if (spec.disableAudio) {
        sl::d27Log("AUDIO", "disabled by source spec, skip");
    } else if (!spec.audioUrls.empty()) {
        audioContext = SLAudio_CreateContext();
        bool opened = false;
        for (const std::string& url : spec.audioUrls) {
            sl::d27Log("AUDIO", "open source %s", sourceTag(url).c_str());
            if (audioSource_.open(url, spec.referrer, userAgent())) {
                opened = true;
                break;
            }
            sl::d27Log("AUDIO", "source open failed %s",
                       audioSource_.error().c_str());
        }
        if (audioContext && opened && audioSource_.audioIndex() >= 0) {
            selfDemux = true;
            audioReady = audio_.start(audioContext, audioSource_.format(),
                                      audioSource_.audioIndex(), true,
                                      &audioSource_);
        }
    } else if (videoSource_.audioIndex() >= 0) {
        audioContext = SLAudio_CreateContext();
        if (audioContext) {
            audioReady = audio_.start(audioContext, format,
                                      videoSource_.audioIndex(), false);
        }
    }

    if (!audioReady && audioContext) {
        SLAudio_FreeContext(audioContext);
        audioContext = nullptr;
    }
    if (!audioReady)
        sl::d27Log("AUDIO", "no usable audio, silent playback");
    else
        sl::d27Log("AUDIO", "ready");
    audio_.setVolume(volume_.load());
    sl::d27Log("PLAYER", "enter playback loop");

    /* start() 已创建并查询初始 stream；冷播没有显式 seek ACK。 */
    bool audioResetReady = true;
    bool audioSeekFailed = false;
    std::atomic<bool> syncReadyLogged{false};
    std::atomic<bool> syncStartLogged{false};
    std::uint64_t prepareEpoch = syncState_.epoch.load();

    auto doVideoPrepare = [&](double target, std::uint64_t requestId,
                              bool rebuildVideo) -> bool {
        /* 本次 open 或上一次重建后还没有帧提交时，VPU 队列本来就是空的。 */
        const bool canSkipRebuild = !rebuildVideo || !videoStarted;
        seeking_ = true;
        pauseFlushed_ = false;
        backpressure_ = false;

        int64_t ts = av_rescale_q((int64_t)(target * AV_TIME_BASE),
                                  AV_TIME_BASE_Q, stream->time_base);
        if (videoSource_.remote())
            videoSource_.beginSeek(target);
        bool ok = av_seek_frame(format, videoIndex, ts, AVSEEK_FLAG_BACKWARD) >= 0;
        if (videoSource_.remote())
            videoSource_.endSeek();
        havePendingPacket = false;
        if (ok) {
            avformat_flush(format);
            av_bsf_flush(bsf);
            while (pendingPacket) {
                int readRet = av_read_frame(format, pendingPacket);
                if (readRet < 0)
                    break;
                if (pendingPacket->stream_index == videoIndex) {
                    if (pendingPacket->pts != AV_NOPTS_VALUE)
                        target = pendingPacket->pts * av_q2d(stream->time_base);
                    havePendingPacket = true;
                    break;
                }
                av_packet_unref(pendingPacket);
            }
        } else {
            sl::d27LogReq("PLAYER", requestId,
                          "video seek failed, possible range window miss");
        }
        sl::d27LogReq("PLAYER", requestId,
                      "video seek target=%.3f key=%.3f ok=%d",
                      lastSeekTargetMs / 1000.0, target, ok ? 1 : 0);

        if (!canSkipRebuild && !rebuildStream()) {
            emitFatal(FatalType::VpuRebuildFailed, "seek 后重建 SLVideo stream 失败");
            emit(EventType::Error, "seek 后重建 SLVideo stream 失败");
            seeking_ = false;
            stop_ = true;
            return false;
        }
        return ok;
    };

    auto audioReadyForPlay = [&]() -> bool {
        if (!audioReady)
            return true;
        return audioResetReady && pendingAudioSeq_.load() == 0 &&
               !audioSeekFailed && audio_.prerollMs() >= kSyncPrerollMs;
    };

    auto logSyncReadyIfReady = [&](double presentationMs) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(syncTextMutex);
            reason = syncReason_;
        }
        if (syncState_.has(kSyncReadyVideo) && audioReadyForPlay())
            syncState_.setReady(kSyncReadyAudio);
        if (!syncState_.allReady() || syncReadyLogged.load())
            return;
        syncReadyLogged = true;
        syncState_.phase.store(static_cast<int>(SyncPhase::Running));
        syncEvent(spec, syncState_.epoch.load(),
                  lastTimelineRequestId_.load(), "SYNC_READY",
                  "ready_mask=%u phase=running present=%.1f preroll_ms=%.1f "
                  "reason=%s",
                  syncState_.readyMask.load(), presentationMs,
                  audio_.prerollMs(),
                  reason.empty() ? "-" : reason.c_str());
    };

    auto logSyncStart = [&](double presentationMs, double skipMs,
                            std::uint32_t queuedNow) {
        if (syncStartLogged.load())
            return;
        syncStartLogged = true;
        syncEvent(spec, syncState_.epoch.load(),
                  lastTimelineRequestId_.load(), "SYNC_START",
                  "target_ms=%.0f presentation_estimate_ms=%.1f skip_ms=%.1f "
                  "preroll_ms=%.1f video_queue_ms=%u",
                  audioBaseMs, presentationMs, skipMs, audio_.prerollMs(),
                  queuedNow);
    };

    auto doAudioPrepare = [&](double target, std::uint64_t requestId,
                              bool forPlay) -> std::uint64_t {
        syncState_.clearReady(kSyncReadyAudio);
        if (!audioReady) {
            syncState_.setReady(kSyncReadyAudio);
            audioResetReady = true;
            return 0;
        }
        /* D23：音频 seek 不阻塞视频首帧；D28 S02 提供 seq/result ACK。 */
        std::uint64_t seq = audio_.requestSeek(target);
        pendingAudioSeq_.store(seq);
        audioResetReady = false;
        audioSeekFailed = false;
        /* decodeAllowed 与 audibleOutputAllowed 分离：PAUSE 也允许 reset/seek，
         * 但只有 PLAY 才恢复解码；两者都不调用 scheduleStart 前不会出声。 */
        if (forPlay)
            audio_.setPaused(false);
        sl::d27LogReq("AUDIO", requestId, "prepare queued seq=%llu target=%.3f",
                      (unsigned long long)seq, target);
        return seq;
    };

    auto doSeek = [&](double target, std::uint64_t requestId,
                      bool rebuildVideo = true) -> bool {
        if (duration > 0.0)
            target = std::min(target, std::max(0.0, duration - 0.1));
        if (target < 0.0)
            target = 0.0;
        const double requestedTarget = target;
        lastSeekStartMs = steadyNowMs();
        lastSeekTargetMs = requestedTarget * 1000.0;
        if (playDiag)
            sl::d27LogReq("PLAYER", requestId,
                          "diag seek enter target=%.3f t=%lld",
                          requestedTarget, (long long)lastSeekStartMs);

        syncState_.clearAllReady();
        prepareEpoch = syncState_.epoch.load();
        syncReadyLogged = false;
        syncStartLogged = false;
        bool ok = doVideoPrepare(target, requestId, rebuildVideo);
        std::uint64_t audioSeq = doAudioPrepare(requestedTarget, requestId, true);

        audioBaseMs = requestedTarget * 1000.0;
        seekDisplayTargetMs = requestedTarget * 1000.0;
        firstFramePtsAfterSeekMs = -1.0;
        firstFrameAfterSeekRecorded = false;
        videoStarted = false;
        afterSeek = true;
        audioScheduled = false;
        audioPendingPosition = false;
        audioStartPositionMs = 0.0;
        syncWatchActive = false;
        syncPostStartLogged = false;
        syncStartWallMs = 0;
        syncStartSkipMs = 0.0;
        queueMax = 0;
        queueFlatFrames = 0;
        maxSubmittedMs = requestedTarget * 1000.0;
        videoClock = requestedTarget;
        position_ = requestedTarget;
        nextDeadline = std::chrono::steady_clock::now();
        seeking_ = false;
        if (requestId == 0 || requestId == latestSeekRequestId_.load())
            emit(EventType::Seeked, std::string(), requestId);
        if (playDiag)
            sl::d27LogReq("PLAYER", requestId,
                          "diag seek return elapsed=%lldms audio_pending=%d seq=%llu",
                          (long long)(steadyNowMs() - lastSeekStartMs),
                          audioReady && audio_.seekPending() ? 1 : 0,
                          (unsigned long long)audioSeq);
        return ok;
    };

    packet = av_packet_alloc();
    filtered = av_packet_alloc();
    pendingPacket = av_packet_alloc();
    if (!packet || !filtered || !pendingPacket) {
        fail("AVPacket 分配失败");
        return;
    }

    if (spec.startSeconds > 0.5)
        doSeek(spec.startSeconds, 0, false);

    auto lastDiag = std::chrono::steady_clock::now();
    while (!stop_) {
        std::uint64_t pendingSeq = pendingAudioSeq_.load();
        if (pendingSeq != 0) {
            SeekStatus status = audio_.seekResult(pendingSeq);
            if (status == SeekStatus::Success) {
                pendingAudioSeq_.store(0);
                audioResetReady = true;
            } else if (status == SeekStatus::Failed) {
                pendingAudioSeq_.store(0);
                audioSeekFailed = true;
                syncState_.clearReady(kSyncReadyAudio);
                sl::d27LogReq("AUDIO", lastTimelineRequestId_.load(),
                              "seek failed seq=%llu keep muted, end playback",
                              (unsigned long long)pendingSeq);
                fail("audio seek failed");
                return;
            }
        }
        if (pausePending_.load() && audioResetReady &&
            pendingAudioSeq_.load() == 0 &&
            (!audioReady || audio_.sinkQueueSamples() == 0) &&
            syncState_.has(kSyncReadyVideo)) {
            double at = pauseTargetMs_.load() / 1000.0;
            pausePending_.store(false);
            syncState_.setReady(kSyncReadyAudio);
            syncState_.phase.store(static_cast<int>(SyncPhase::Paused));
            if (!syncReadyLogged.load()) {
                syncReadyLogged = true;
                syncEvent(spec, syncState_.epoch.load(),
                          pauseRequestId_.load(), "SYNC_READY",
                          "ready_mask=%u phase=paused target_ms=%.0f "
                          "sink_queued=%u decode_allowed=%d audible=%d",
                          syncState_.readyMask.load(), at * 1000.0,
                          audio_.sinkQueueSamples(),
                          audio_.decodeAllowed() ? 1 : 0,
                          audio_.audibleOutputAllowed() ? 1 : 0);
            }
            emitWithPosition(EventType::Paused, at, std::string(),
                             pauseRequestId_.load());
            pauseRequestId_.store(0);
        }
        if (playDiag) {
            auto diagNow = std::chrono::steady_clock::now();
            if (diagNow - lastDiag >= std::chrono::seconds(1)) {
                lastDiag = diagNow;
                sl::d27Log("PLAYER",
                           "diag stage=%d pos=%.2f vf=%d drop=%d vq=%u "
                           "downloaded=%lld buffered=%lld read=%lld base=%lld "
                           "paused=%d after_seek=%d awaiting=%d bp=%d",
                           stage_.load(), position_.load(), videoFrames,
                           droppedFrames_.load(), videoQueueMs_.load(),
                           (long long)videoSource_.downloadedBytes(),
                           (long long)videoSource_.bufferedBytes(),
                           (long long)videoSource_.readPosition(),
                           videoSource_.baseOffset(), paused_.load() ? 1 : 0,
                           afterSeek ? 1 : 0,
                           awaitingFirstFrame_.load() ? 1 : 0,
                           backpressure_.load() ? 1 : 0);
            }
        }
        if (pauseFlush_.exchange(false)) {
            syncState_.begin(syncState_.epoch.load(), pauseRequestId_.load(),
                             false);
            prepareEpoch = syncState_.epoch.load();
            syncReadyLogged = false;
            syncStartLogged = false;
            /* 暂停点在命令入口冻结，不用播放线程稍后读到的推进位置。 */
            double at = pauseTargetMs_.load() / 1000.0;
            if (!rebuildStream()) {
                emitFatal(FatalType::VpuRebuildFailed, "暂停时重建 SLVideo stream 失败");
                fail("暂停时重建 SLVideo stream 失败");
                return;
            }
            videoStarted = false;
            afterSeek = true;
            audioScheduled = false;
            audioPendingPosition = false;
            audioStartPositionMs = 0.0;
            syncWatchActive = false;
            syncPostStartLogged = false;
            syncStartWallMs = 0;
            syncStartSkipMs = 0.0;
            queueMax = 0;
            queueFlatFrames = 0;
            maxSubmittedMs = at * 1000.0;
            audioBaseMs = at * 1000.0;
            lastSeekTargetMs = at * 1000.0;
            seekDisplayTargetMs = at * 1000.0;
            videoClock = at;
            resumeTarget_ = at;
            pauseFlushed_ = true;
            /* 与 Kodi FlushBuffers 一致：视频队列清空后才停音频。 */
            audio_.setPaused(true);
            /* pause 需要 reset/stream/SDK queue ACK，但不需要 preroll。 */
            doAudioPrepare(at, pauseRequestId_.load(), false);
            sl::d27Log("PLAYER", "pause flush video queue at=%.2f", at);
            resumeInProgress_ = false;
            backpressure_ = false;
            if (targetPaused_.load()) {
                paused_ = true;
                syncState_.setReady(kSyncReadyVideo);
                pausePending_.store(true);
                /* SYNC_READY/SL_EVT_PAUSED 等音频 ACK 在上面统一发出。 */
            } else {
                /* pause/resume 在 flush 期间连续到达：以最新目标为准，直接补恢复。 */
                resumeTarget_ = at;
                resumeSeek_ = true;
            }
        }

        double target = pendingSeek_.exchange(-1.0);
        std::uint64_t seekRequestId = pendingSeekId_.exchange(0);
        if (target >= 0.0) {
            doSeek(target, seekRequestId);
        } else if (resumeSeek_.exchange(false)) {
            syncState_.begin(syncState_.epoch.load(), resumeRequestId_.load(),
                             true);
            prepareEpoch = syncState_.epoch.load();
            audioResetReady = true;
            audioSeekFailed = false;
            syncReadyLogged = false;
            syncStartLogged = false;
            std::uint64_t resumeRequestId = resumeRequestId_.load();
            if (paused_.load()) {
                pendingResumeRequestId = resumeRequestId;
                pendingResumeEvent = true;
            }
            resumeRequestId_ = 0;
            /* resume 沿用 pause epoch：只恢复视频读取和音频解码/预缓冲，
             * 不调用 doAudioPrepare，所以 seekSequence 不增加。 */
            double at = resumeTarget_.load();
            pausePending_.store(false);
            if (!doVideoPrepare(at, resumeRequestId, false)) {
                fail("resume video prepare failed");
                return;
            }
            audio_.setPaused(false); /* decodeAllowed=true，尚未 scheduleStart */
            pendingAudioSeq_.store(0);
            audioResetReady = true;
            audioSeekFailed = false;
            videoStarted = false;
            afterSeek = true;
            audioScheduled = false;
            audioPendingPosition = false;
            audioStartPositionMs = 0.0;
            syncWatchActive = false;
            syncPostStartLogged = false;
            syncStartWallMs = 0;
            syncStartSkipMs = 0.0;
            queueMax = 0;
            queueFlatFrames = 0;
            maxSubmittedMs = at * 1000.0;
            audioBaseMs = at * 1000.0;
            lastSeekTargetMs = at * 1000.0;
            seekDisplayTargetMs = at * 1000.0;
            videoClock = at;
            position_ = at;
            nextDeadline = std::chrono::steady_clock::now();
            seeking_ = false;
            syncEvent(spec, syncState_.epoch.load(), resumeRequestId,
                      "SYNC_START_PREPARE",
                      "target_ms=%.0f epoch_reused=%d decode_allowed=%d "
                      "audible=%d",
                      at * 1000.0, syncState_.epoch.load() != 0 ? 1 : 0,
                      audio_.decodeAllowed() ? 1 : 0,
                      audio_.audibleOutputAllowed() ? 1 : 0);
            /* 允许播放线程提交恢复后的首帧；Resumed 事件仍等首帧完成再发。 */
            if (pendingResumeEvent)
                paused_ = false;
        }

        if (stop_)
            break;

        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        stage_ = 1;
        int ret;
        if (havePendingPacket) {
            av_packet_move_ref(packet, pendingPacket);
            havePendingPacket = false;
            ret = 0;
        } else {
            ret = av_read_frame(format, packet);
        }
        stage_ = 2;
        if (ret < 0) {
            if (stop_)
                break;
            if (ret == AVERROR_EOF) {
                emit(EventType::EndFile);
                break;
            }
            char text[128];
            av_strerror(ret, text, sizeof(text));
            if (playDiag)
                sl::d27Log("PLAYER", "diag read failed ret=%d %s stage=%d",
                           ret, text, stage_.load());
            fail(std::string("读取失败: ") + text);
            return;
        }

        if (packet->stream_index == videoIndex) {
            if (packet->pts != AV_NOPTS_VALUE)
                videoClock = packet->pts * av_q2d(stream->time_base);

            stage_ = 20;
            ret = av_bsf_send_packet(bsf, packet);
            stage_ = 21;
            if (ret >= 0) {
                while (av_bsf_receive_packet(bsf, filtered) == 0) {
                    stage_ = 22;
                    if (filtered->size <= 0) {
                        av_packet_unref(filtered);
                        continue;
                    }

                    const uint8_t* frameData = filtered->data;
                    int frameSize = filtered->size;
                    if (rewriteSps && annexBContainsSps(frameData, (size_t)frameSize)) {
                        int patched = 0;
                        if (patchAnnexBSps(frameData, (size_t)frameSize, rewriteBuffer,
                                           spsTargetRefs, effectiveSpsReorder,
                                           effectiveSpsBuffering, &patched) &&
                            patched > 0) {
                            frameData = rewriteBuffer.data();
                            frameSize = (int)rewriteBuffer.size();
                            if (spsFrameLogs < 3) {
                                H264SpsInfo frameSps =
                                    parseFirstSpsAnnexB(frameData, (size_t)frameSize);
                                sl::d27Log("PLAYER",
                                           "frame SPS refs=%d reorder=%d buffering=%d",
                                           frameSps.refs, frameSps.reorder,
                                           frameSps.buffering);
                                spsFrameLogs++;
                            }
                        } else if (spsFrameLogs < 3) {
                            sl::d27Log("PLAYER", "frame SPS rewrite failed");
                            spsFrameLogs++;
                        }
                    }

                    if (dumpFramePath && !frameDumped) {
                        frameDumped = true;
                        FILE* dump = fopen(dumpFramePath, "wb");
                        if (dump) {
                            fwrite(frameData, 1, (size_t)frameSize, dump);
                            fclose(dump);
                            sl::d27Log("PLAYER", "first frame exported bytes=%d",
                                       frameSize);
                        }
                    }

                    stage_ = 30;
                    uint32_t queued = SLVideo_GetQueuedVideoMS(videoStream);
                    stage_ = 31;
                    videoQueueMs_ = queued;
                    int waited = 0;
                    while (SLVideo_GetQueuedVideoMS(videoStream) > kVideoQueueMaxMs) {
                        if (stop_ || waited >= 500)
                            break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        waited += 5;
                    }
                    uint32_t queuedAfterWait = SLVideo_GetQueuedVideoMS(videoStream);
                    videoQueueMs_ = queuedAfterWait;
                    if (!stop_ && queuedAfterWait > kVideoQueueMaxMs) {
                        /* VPU 队列在 500ms 内没有消化，不能再喂新帧，否则队列
                         * 会越积越长，最终 BeginFrame 报 out of space。 */
                        if (!backpressure_.exchange(true))
                            backpressureSinceMs_.store(steadyNowMs());
                        droppedFrames_++;
                        av_packet_unref(filtered);
                        continue;
                    }
                    stage_ = 32;

                    int beginResult = 0;
                    int retries = 0;
                    stage_ = 3;
                    while ((beginResult = SLVideo_BeginFrame(videoStream,
                                                             frameSize)) != 0) {
                        if (++retries > 100 || stop_)
                            break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    beginRetries_ += retries;

                    /* 第 2 步：BeginFrame 重试耗尽（非主动 stop）= 帧提交停滞，主动上报。
                     * 注意仍走原来的丢帧继续逻辑，不在这里改播放行为；上报只是让上层
                     * watchdog 能提前知道硬件异常，心跳停滞判定作为另一道保险。 */
                    if (beginResult != 0 && !stop_) {
                        emitFatal(FatalType::FrameSubmitStalled,
                                  "SLVideo_BeginFrame 连续重试耗尽");
                    }

                    bool submitted = false;
                    if (beginResult == 0 && !stop_) {
                        if (SLVideo_WriteFrameData(videoStream, (void*)frameData,
                                                   frameSize) == 0 &&
                            SLVideo_SubmitFrame(videoStream) == 0) {
                            submitted = true;
                            backpressure_ = false;
                        } else {
                            sl::d27Log("PLAYER", "frame submit failed, drop");
                        }
                    }
                    if (!submitted) {
                        droppedFrames_++;
                        av_packet_unref(filtered);
                        continue;
                    }

                    stage_ = 4;
                    videoFrames++;
                    videoFrames_ = videoFrames;
                    /* 心跳：每次成功 SubmitFrame 更新单调时间戳 */
                    lastFrameTimeMs_ = steadyNowMs();
                    awaitingFirstFrame_.store(false);

                    double frameMs = videoClock * 1000.0;
                    if (filtered->pts != AV_NOPTS_VALUE)
                        frameMs = filtered->pts * av_q2d(stream->time_base) * 1000.0;
                    if (frameMs > maxSubmittedMs)
                        maxSubmittedMs = frameMs;
                    double submittedMs = maxSubmittedMs;
                    stage_ = 40;
                    uint32_t queuedNow = SLVideo_GetQueuedVideoMS(videoStream);
                    stage_ = 41;
                    videoQueueMs_ = queuedNow;
                    double presentationMs = submittedMs - (double)queuedNow;
                    if (presentationMs < 0.0)
                        presentationMs = 0.0;
                    position_ = presentationMs / 1000.0;

                    if (!videoStarted) {
                        videoStarted = true;
                        if (playDiag)
                            sl::d27Log("PLAYER",
                                       "diag first submit pts=%.1f queued=%u "
                                       "vf=%d stage=%d",
                                       frameMs, queuedNow, videoFrames,
                                       stage_.load());
                        if (playDiag && lastSeekStartMs > 0)
                            sl::d27Log("PLAYER",
                                       "diag first submit after seek age=%lldms "
                                       "audio_pending=%d",
                                       (long long)(steadyNowMs() - lastSeekStartMs),
                                       audioReady && audio_.seekPending() ? 1 : 0);
                        emit(EventType::PlaybackStart);
                        if (syncState_.epoch.load() == prepareEpoch) {
                            syncState_.setReady(kSyncReadyVideo);
                            logSyncReadyIfReady(presentationMs);
                        }
                    }

                    /* seek/暂停恢复：这里只记录首帧 PTS，不在此安排音频。
                     * 新建 stream 的首帧 queued 往往是 0，队列还要数秒才爬到稳态；
                     * 若按墙钟立刻 scheduleStart，音频会在画面队列追上前先到目标点。
                     * 真正的启动点放到下面的队列稳定分支，统一按呈现位置计算。 */
                    if (afterSeek && !firstFrameAfterSeekRecorded) {
                        firstFrameAfterSeekRecorded = true;
                        firstFramePtsAfterSeekMs = frameMs;
                        syncLog("seek first frame: target=%.0f first=%.0f queued=%u",
                                seekDisplayTargetMs, frameMs, queuedNow);
                        if (playDiag)
                            sl::d27Log("PLAYER",
                                       "diag seek first frame recorded target=%.0f "
                                       "first=%.0f queued=%u",
                                       seekDisplayTargetMs, frameMs, queuedNow);
                    }

                    /* 恢复完成的定义：视频首帧已重建、音频启动已安排；
                     * 音频不可用时不受该条件阻塞，避免永远不发 Resumed。 */
                    if (pendingResumeEvent && firstFrameAfterSeekRecorded &&
                        (!audioReady || audioScheduled)) {
                        if (!targetPaused_.load()) {
                            emit(EventType::Resumed, std::string(),
                                 pendingResumeRequestId);
                        }
                        pendingResumeRequestId = 0;
                        pendingResumeEvent = false;
                        resumeInProgress_ = false;
                    }

                    if (audioReady && !audioScheduled && !audioPendingPosition &&
                        videoFrames > 1 &&
                        queuedNow > 0) {
                        if (queuedNow > queueMax + 5) {
                            queueMax = queuedNow;
                            queueFlatFrames = 0;
                        } else {
                            queueFlatFrames++;
                        }

                        /* 初始播放与 seek/暂停恢复统一按“已呈现内容”对齐；
                         * 固定 300ms 会在高参考帧/慢上屏时留下固定音画偏移。 */
                        if ((afterSeek || submittedMs > 1500.0) &&
                            queueFlatFrames >= 5) {
                            double offsetMs = presentationMs - audioBaseMs;
                            double skipMs = std::max(0.0, std::min(offsetMs, 3000.0));
                            /* 带 start= 打开时第一帧也处于 afterSeek，旧条件会让
                             * measuredVideoLatencyMs 永远是 0，后续 seek 全部走
                             * 兜底分支而不是首帧对齐，表现为快进后音画不同步。 */
                            if (measuredVideoLatencyMs <= 0)
                                measuredVideoLatencyMs = (int)queuedNow;
                            audio_.setPaused(false);
                                logSyncReadyIfReady(presentationMs);
                            if (offsetMs <= 0.0) {
                                /* 画面还没走到目标点：只登记目标位置，不按墙钟启动。
                                 * 后面的逐帧循环会在 presentationMs 到达目标时启动音频。 */
                                audioPendingPosition = true;
                                audioStartPositionMs = audioBaseMs;
                                syncLog("stable, wait position: target=%.0f first=%.0f "
                                        "present=%.0f latency=%d wait=%.0f",
                                        audioBaseMs, firstFramePtsAfterSeekMs,
                                        presentationMs, measuredVideoLatencyMs,
                                        -offsetMs);
                                sl::d27LogIdentity("AUDIO",
                                                   "queue stable, wait position: "
                                                   "present=%.0f target=%.0f wait=%.0f queue=%u",
                                                   presentationMs, audioBaseMs,
                                                   -offsetMs, queuedNow);
                            } else {
                                logSyncReadyIfReady(presentationMs);
                                logSyncStart(presentationMs, skipMs, queuedNow);
                                audio_.scheduleStart(0, skipMs);
                                audioScheduled = true;
                                afterSeek = false;
                                syncStartWallMs = steadyNowMs();
                                syncStartSkipMs = skipMs;
                                syncLog("stable start: target=%.0f first=%.0f "
                                        "present=%.2f latency=%d delay=0 skip=%.2f "
                                        "frame=%.2f audio_submit=%.2f queue=%u",
                                        audioBaseMs, firstFramePtsAfterSeekMs,
                                        presentationMs, measuredVideoLatencyMs, skipMs,
                                        frameMs, audio_.submittedMs(), queuedNow);
                                sl::d27LogIdentity("AUDIO",
                                                   "queue stable, audio start: "
                                                   "present=%.2f target=%.0f skip=%.2f",
                                                   presentationMs, audioBaseMs,
                                                   skipMs);
                            }
                        }
                    }

                    if (audioReady && audioPendingPosition &&
                        presentationMs >= audioStartPositionMs) {
                        double skipMs = std::max(
                            0.0, std::min(presentationMs - audioStartPositionMs, 3000.0));
                        audio_.setPaused(false);
                        logSyncReadyIfReady(presentationMs);
                        logSyncStart(presentationMs, skipMs, queuedNow);
                        audio_.scheduleStart(0, skipMs);
                        audioScheduled = true;
                        audioPendingPosition = false;
                        afterSeek = false;
                        syncStartWallMs = steadyNowMs();
                        syncStartSkipMs = skipMs;
                        syncLog("position start: target=%.2f present=%.2f skip=%.2f "
                                "frame=%.2f audio_submit=%.2f queue=%u",
                                audioStartPositionMs, presentationMs, skipMs,
                                frameMs, audio_.submittedMs(), queuedNow);
                        sl::d27LogIdentity("AUDIO",
                                           "position reached, audio start: "
                                           "target=%.2f present=%.2f skip=%.2f",
                                           audioStartPositionMs, presentationMs,
                                           skipMs);
                    }

                    /* 同步等待只允许失败，不允许绕过对齐强行出声。
                     * 判据是呈现位置高水位是否推进：慢但在走就继续等，
                     * 完全停住超过阈值才报 Error 并结束播放。 */
                    if (audioScheduled && !syncPostStartLogged && syncStartWallMs > 0 &&
                        steadyNowMs() - syncStartWallMs >= 5000) {
                        syncPostStartLogged = true;
                        const double audioContentMs =
                            audioBaseMs + syncStartSkipMs + audio_.submittedMs();
                        syncEvent(spec, syncState_.epoch.load(),
                                  lastTimelineRequestId_.load(), "SYNC_DRIFT",
                                  "elapsed_ms=5000 drift_estimate_ms=%.2f "
                                  "video_queue_ms=%u audio_queue_samples=%u",
                                  presentationMs - audioContentMs, queuedNow,
                                  audio_.sinkQueueSamples());
                        syncLog("post start 5s: target=%.2f frame=%.2f present=%.2f "
                                "audio_content=%.2f drift=%.2f skip=%.2f queue=%u",
                                audioBaseMs, frameMs, presentationMs, audioContentMs,
                                presentationMs - audioContentMs, syncStartSkipMs, queuedNow);
                    }

                    const bool syncWaiting =
                        audioReady && !audioScheduled && videoStarted && !paused_.load();
                    if (!syncWaiting) {
                        syncWatchActive = false;
                    } else {
                        const std::int64_t syncNow = steadyNowMs();
                        if (!syncWatchActive) {
                            syncWatchActive = true;
                            syncHighWaterMs = presentationMs;
                            lastSyncProgressMs = syncNow;
                        } else if (presentationMs >= syncHighWaterMs + kSyncProgressStepMs) {
                            syncHighWaterMs = presentationMs;
                            lastSyncProgressMs = syncNow;
                        } else if (syncNow - lastSyncProgressMs >= syncNoProgressMs) {
                            char detail[256];
                            std::snprintf(detail, sizeof(detail),
                                          "音视频对齐失败: present=%.0f target=%.0f "
                                          "high=%.0f queue=%u frames=%d stage=%d "
                                          "no_progress=%dms",
                                          presentationMs, audioBaseMs, syncHighWaterMs,
                                          queuedNow, videoFrames, stage_.load(),
                                          syncNoProgressMs);
                            sl::d27Log("PLAYER", "end playback reason=%s", detail);
                            fail(detail);
                            return;
                        }
                    }

                    stage_ = 42;
                    av_packet_unref(filtered);

                    auto now = std::chrono::steady_clock::now();
                    if (!seeking_.load() &&
                        now - lastProgress >= std::chrono::milliseconds(250)) {
                        lastProgress = now;
                        emit(EventType::Progress);
                    }
                    stage_ = 43;

                    nextDeadline += std::chrono::microseconds(
                        (int64_t)(1000000.0 / fps));
                    stage_ = 5;
                    if (nextDeadline > now)
                        std::this_thread::sleep_until(nextDeadline);
                    else
                        nextDeadline = now;
                    stage_ = 6;

                    if (spec.endSeconds > 0.0 &&
                        position_.load() >= spec.endSeconds) {
                        emit(EventType::EndFile);
                        stop_ = true;
                    }
                    if (stop_)
                        break;
                }
            }
        } else if (audioReady && !selfDemux &&
                   packet->stream_index == videoSource_.audioIndex()) {
            audio_.putPacket(packet);
        }

        av_packet_unref(packet);
    }

    shutdown();
}

/* ---------------- Player 公共接口 ---------------- */

Player::Player() : impl_(new Impl()) {}

Player::~Player() = default;

void Player::setEventCallback(EventCallback callback) {
    impl_->setEventCallback(std::move(callback));
}

void Player::setFatalCallback(FatalCallback callback) {
    impl_->setFatalCallback(std::move(callback));
}

void Player::setVolume(int volume) { impl_->setVolume(volume); }

bool Player::open(const SourceSpec& spec) { return impl_->start(spec); }

void Player::play(std::uint64_t requestId, const SyncContext& sync) {
    impl_->setPaused(false, requestId, sync);
}

void Player::pause(std::uint64_t requestId, const SyncContext& sync) {
    impl_->setPaused(true, requestId, sync);
}

bool Player::isPaused() const { return impl_->paused_; }

bool Player::isRunning() const { return impl_->running_; }

void Player::stop() { impl_->stop(); }

void Player::seek(double seconds, std::uint64_t requestId,
                  const SyncContext& sync) {
    impl_->requestSeek(seconds, requestId, sync);
}

double Player::position() const { return impl_->position_.load(); }

double Player::duration() const { return impl_->duration_.load(); }

VideoInfo Player::videoInfo() const {
    std::lock_guard<std::mutex> lock(impl_->infoMutex);
    return impl_->info_;
}

Stats Player::stats() const { return impl_->stats(); }

Heartbeat Player::heartbeat() const { return impl_->heartbeat(); }

} // namespace sl
