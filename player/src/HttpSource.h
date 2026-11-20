/*
 * 单条媒体 URL 的下载 + 解封装。
 *
 * 远程 URL：libcurl 后台下载到 8MB 滑动窗口，自定义 AVIO 喂给 FFmpeg，
 * 支持在窗口内 seek；传输中断时从当前窗口末尾 Range 续传；本地文件直接交给 avformat。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct AVFormatContext;
struct AVIOContext;

namespace sl {

class HttpSource {
public:
    HttpSource();
    ~HttpSource();
    HttpSource(const HttpSource&) = delete;
    HttpSource& operator=(const HttpSource&) = delete;

    bool open(const std::string& url, const std::string& referrer,
              const std::string& userAgent);
    void close();

    /* 停止下载并唤醒阻塞中的 av_read_frame；之后仍需 close() 回收线程 */
    void interrupt();

    /* av_seek_frame 前设置媒体时间提示：AVIO 的 SEEK_SET 回调会按
     * 实际文件大小/时长换算字节偏移，避免 FFmpeg 码率估算越界。
     * 必须在 av_seek_frame 返回后调用 endSeek()。 */
    void beginSeek(double seconds);
    void endSeek();

    AVFormatContext* format() const { return format_; }
    int videoIndex() const { return videoIndex_; }
    int audioIndex() const { return audioIndex_; }
    bool remote() const { return remote_; }
    const std::string& error() const { return error_; }

    int64_t downloadedBytes() const;
    size_t bufferedBytes() const;
    int64_t contentLength() const;
    int64_t readPosition() const;
    int64_t baseOffset() const;
    int64_t lastNetworkErrorMs() const { return lastNetworkErrorMs_.load(); }

private:
    class Downloader;
    static int readCallback(void* opaque, uint8_t* data, int size);
    static int64_t seekCallback(void* opaque, int64_t offset, int whence);
    std::shared_ptr<Downloader> downloader() const;

    AVFormatContext* format_ = nullptr;
    AVIOContext* avio_ = nullptr;
    uint8_t* avioBuffer_ = nullptr;
    std::shared_ptr<Downloader> downloader_;
    mutable std::mutex downloaderMutex_;
    mutable std::mutex seekHintMutex_;
    double pendingSeekSeconds_ = -1.0;
    bool seekHintActive_ = false;
    bool remote_ = false;
    std::string error_;
    int videoIndex_ = -1;
    int audioIndex_ = -1;
    std::atomic<int64_t> lastNetworkErrorMs_{0};
};

} // namespace sl
