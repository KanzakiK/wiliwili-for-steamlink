/*
 * SlPlayer - Steam Link 硬解播放引擎
 *
 * 视频走 SLVideo 硬件视频层（固定全屏），UI 由调用方在 GL 层叠加；
 * 音频走 SLAudio，按源采样率透传。
 *
 * 事件回调可能来自播放线程或控制线程，调用方需要自己保证线程安全，
 * 并且不要在回调里调用 open()/stop()。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sl {

struct SourceSpec {
    std::string videoUrl;
    /* 独立音轨（DASH）：第一条为主，其余为备份；为空表示音视频同源 */
    std::vector<std::string> audioUrls;
    std::string referrer = "https://www.bilibili.com/";
    std::string userAgent;
    /* 诊断用：忽略音频，避免音频设备异常影响视频验证 */
    bool disableAudio = false;
    double startSeconds = 0.0;
    /* 0 表示播放到文件结尾 */
    double endSeconds = 0.0;
};

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;
};

enum class EventType {
    Opened,        /* 解封装完成，可读取 duration/videoInfo */
    PlaybackStart, /* 首帧已提交到 SLVideo */
    Progress,      /* 约每 250ms 一次 */
    Paused,
    Resumed,
    Seeked,
    EndFile,
    Error,
    /* 码流参数超出 Steam Link 硬解能力，且无法安全改写 */
    Unsupported,
};

struct Event {
    EventType type = EventType::Error;
    double position = 0.0;
    double duration = 0.0;
    /* 触发本次状态变化的命令序号；状态通知之外的事件为 0。 */
    std::uint64_t requestId = 0;
    std::string message;
};

/*
 * VPU 致命错误类型（第 2 步）：只上报引擎能自己检测到的、与硬件/码流
 * 强相关的致命错误。普通错误（网络读失败、源打开失败）仍走 EventType::Error。
 * 上层收到后自行决策（提示用户/重启播放器/退回主界面），引擎不 _exit()。
 */
enum class FatalType {
    VpuInitFailed,      /* CreateContext/CreateStream 复位重试后仍失败 */
    VpuRebuildFailed,   /* seek/pause 重建 stream 失败 */
    FrameSubmitStalled, /* BeginFrame 连续重试耗尽仍失败（帧提交停滞） */
    UnsupportedStream,  /* 码流超出硬件能力且无法安全改写 */
};

/*
 * 无锁心跳快照（第 2 步）：供上层 watchdog 判断播放线程是否卡死。
 * 只由 atomic 字段构成，读它不会取任何锁、不会碰数据源查询，
 * 因此 watchdog 线程读它不会被卡死的播放线程拖住。禁止改用 stats()。
 */
struct Heartbeat {
    /* steady_clock 的毫秒值（相对 epoch），最后一次 SubmitFrame 成功的时间 */
    std::int64_t lastFrameTimeMs = 0;
    int stage = 0;              /* 当前内部阶段，便于记录卡死现场 */
    bool playing = false;       /* 是否在正常播放（非暂停/非停止） */
    bool inTransition = false;  /* 是否处于合法 pause/seek 窗口（watchdog 应挂起判定） */
    /* P0：目标态与当前态分开上报；watchdog 不再把目标态当成已完成的当前态。 */
    bool currentPaused = false;
    bool targetPaused = false;
    bool seeking = false;
    /* 播放工作线程仍在运行；IPC 主循环存活不等于这一项为 true。 */
    bool workerAlive = false;
    int videoFrames = 0;
};

struct Stats {
    double position = 0.0;
    double duration = 0.0;
    int videoFrames = 0;
    int droppedFrames = 0;
    int beginRetries = 0;
    unsigned videoQueueMs = 0;
    int audioRate = 0;
    bool audioStarted = false;
    unsigned audioQueuedSamples = 0;
    double audioDelayMs = 0.0;
    double audioSubmittedMs = 0.0;
    int audioPackets = 0;
    long long downloadedBytes = 0;
    size_t bufferedBytes = 0;
    long long readPosition = 0;
    long long bufferBaseOffset = 0;
    int stage = 0;
};

class Player {
public:
    using EventCallback = std::function<void(const Event&)>;
    using FatalCallback = std::function<void(FatalType type, const std::string& message)>;

    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void setEventCallback(EventCallback callback);
    /* VPU 致命错误上报（第 2 步）：回调在播放线程触发，上层自行决策 */
    void setFatalCallback(FatalCallback callback);
    void setVolume(int volume); /* 0 - 100 */

    /* 异步：立即返回，Opened/Error 通过事件通知 */
    bool open(const SourceSpec& spec);
    /* requestId 由上层分配，对应状态完成事件原样回传；0 表示无请求方。 */
    void play(std::uint64_t requestId = 0);
    void pause(std::uint64_t requestId = 0);
    bool isPaused() const;
    bool isRunning() const;
    /* 同步：停止播放线程并释放 SLVideo/SLAudio */
    void stop();
    /* 异步跳转（秒）；同一文件的旧请求完成时不再上报。 */
    void seek(double seconds, std::uint64_t requestId = 0);

    double position() const;
    double duration() const;
    VideoInfo videoInfo() const;
    Stats stats() const;
    /* 无锁心跳（第 2 步）：watchdog 专用，只读 atomic，不取锁 */
    Heartbeat heartbeat() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* eventTypeName(EventType type);
const char* fatalTypeName(FatalType type);

} // namespace sl
