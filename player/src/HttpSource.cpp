#include "HttpSource.h"

#include "DebugLog.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <curl/curl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace sl {
namespace {

constexpr size_t kAvioBufferSize = 64 * 1024;
constexpr size_t kStreamBufferLimit = 8 * 1024 * 1024;
constexpr size_t kStreamChunkSize = 256 * 1024;
constexpr size_t kStreamBackKeep = 512 * 1024;
constexpr int kDownloadMaxRetries = 2;
constexpr int kDownloadRetryBaseDelayMs = 250;

int httpReadTimeoutMs() {
    const char* value = std::getenv("SL_HTTP_READ_TIMEOUT_MS");
    int timeout = value ? atoi(value) : 15000;
    if (timeout < 1000) timeout = 1000;
    if (timeout > 60000) timeout = 60000;
    return timeout;
}

bool isRemoteUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

std::int64_t steadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string localPath(const std::string& url) {
    const std::string prefix = "file://";
    if (url.compare(0, prefix.size(), prefix) == 0)
        return url.substr(prefix.size());
    return url;
}

} // namespace

/* ---------------- 下载窗口 ---------------- */

class HttpSource::Downloader {
public:
    Downloader(std::string url, std::string referrer, std::string userAgent,
               std::atomic<int64_t>* errorClock)
        : url_(std::move(url)), referrer_(std::move(referrer)),
          userAgent_(std::move(userAgent)),
          errorClock_(errorClock),
          debug_(std::getenv("SL_HTTP_DEBUG") != nullptr),
          readTimeoutMs_(httpReadTimeoutMs()) {}

    ~Downloader() { join(); }

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable())
            return;
        stop_ = false;
        thread_ = std::thread(&Downloader::run, this);
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cond_.notify_all();
    }

    /* 窗口外 seek：停掉旧请求，从目标字节重新发起 Range 请求。
     * seek 由 FFmpeg 的 AVIO 回调（播放线程）调用，join 必须发生在锁外。 */
    void restartAt(int64_t offset) {
        requestStop();
        join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chunks_.clear();
            baseOffset_ = offset;
            endOffset_ = offset;
            readPos_ = offset;
            totalBytes_ = 0;
            eof_ = false;
            error_ = false;
            stop_ = false;
            errorMessage_.clear();
            rangeStart_ = offset;
            sl::d27Log("HTTP", "range relocate offset=%lld", (long long)offset);
            cond_.notify_all();
        }
        start();
    }

    void join() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            thread = std::move(thread_);
        }
        if (thread.joinable())
            thread.join();
    }

    bool failed() const { return error_; }

    int64_t downloaded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return endOffset_;
    }

    size_t buffered() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalBytes_;
    }

    int64_t readPosition() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return readPos_;
    }

    int64_t baseOffset() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return baseOffset_;
    }

    long contentLength() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return contentLength_;
    }

    int read(uint8_t* data, int size) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (endOffset_ < readPos_ + size && debug_)
            sl::d27Log("HTTP", "read wait size=%d pos=%lld end=%lld base=%lld",
                       size, (long long)readPos_, (long long)endOffset_,
                       (long long)baseOffset_);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(readTimeoutMs_);
        while (!stop_ && !error_ && !eof_ && endOffset_ < readPos_ + size) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                sl::d27Log("HTTP",
                           "read timeout size=%d pos=%lld end=%lld waited_ms=%d",
                           size, (long long)readPos_, (long long)endOffset_,
                           readTimeoutMs_);
                if (errorClock_)
                    errorClock_->store(steadyNowMs(), std::memory_order_relaxed);
                error_ = true;
                errorMessage_ = "read timeout";
                eof_ = true;
                cond_.notify_all();
                return AVERROR(EIO);
            }
            cond_.wait_for(lock, std::chrono::milliseconds(
                std::min<int64_t>(remaining.count(), 1000)));
        }

        if (error_ || stop_)
            return AVERROR_EXIT;
        if (readPos_ < baseOffset_) {
            sl::d27Log("HTTP", "read position evicted pos=%lld base=%lld",
                       (long long)readPos_, (long long)baseOffset_);
            return AVERROR(EIO);
        }

        int copied = 0;
        int64_t position = readPos_;
        while (copied < size) {
            Chunk* chunk = findChunkLocked(position);
            if (!chunk)
                break;
            size_t offsetInChunk = (size_t)(position - chunk->offset);
            size_t available = chunk->used - offsetInChunk;
            size_t remaining = (size_t)(size - copied);
            size_t bytes = std::min(available, remaining);
            memcpy(data + copied, chunk->data.data() + offsetInChunk, bytes);
            copied += (int)bytes;
            position += (int64_t)bytes;
        }

        readPos_ = position;
        cond_.notify_all();

        if (copied > 0)
            return copied;
        if (eof_ && readPos_ >= endOffset_)
            return AVERROR_EOF;
        return AVERROR(EIO);
    }

    int64_t seek(int64_t offset, int whence) {
        if (whence == AVSEEK_SIZE) {
            std::lock_guard<std::mutex> lock(mutex_);
            return contentLength_ > 0 ? (int64_t)contentLength_ : -1;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (debug_)
            sl::d27Log("HTTP", "seek whence=%d offset=%lld pos=%lld end=%lld",
                       whence, (long long)offset, (long long)readPos_,
                       (long long)endOffset_);
        int64_t target;
        if (whence == SEEK_SET) {
            target = offset;
        } else if (whence == SEEK_CUR) {
            target = readPos_ + offset;
        } else if (whence == SEEK_END) {
            if (contentLength_ <= 0)
                return -1;
            target = (int64_t)contentLength_ + offset;
        } else {
            return -1;
        }

        if (target < 0)
            return -1;
        if (contentLength_ > 0 && target >= contentLength_)
            target = contentLength_ - 1;
        bool needRestart = target < baseOffset_;
        if (!needRestart && target > endOffset_ + (int64_t)kStreamChunkSize &&
            (contentLength_ <= 0 || target < contentLength_)) {
            /* 大步快进：等待顺序下载追上可能要几十秒，直接 Range 重定位。 */
            needRestart = true;
        }
        if (needRestart) {
            lock.unlock();
            restartAt(target);
            return target;
        }
        readPos_ = target;
        cond_.notify_all();
        return target;
    }

private:
    struct Chunk {
        std::vector<uint8_t> data;
        size_t used = 0;
        int64_t offset = 0;
    };

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* opaque) {
        auto* self = static_cast<Downloader*>(opaque);
        size_t bytes = size * nmemb;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->rejectRangeBody_)
                return 0;
        }
        return self->append((const uint8_t*)ptr, bytes) == 0 ? bytes : 0;
    }

    static size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* opaque) {
        auto* self = static_cast<Downloader*>(opaque);
        size_t bytes = size * nmemb;
        std::string line(ptr, bytes);
        if (line.compare(0, 5, "HTTP/") == 0) {
            size_t space = line.find(' ');
            int status = space == std::string::npos ? 0 : atoi(line.c_str() + space + 1);
            if (status >= 200) {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->expectRangeStatus_ && status != 206) {
                    self->rejectRangeBody_ = true;
                    sl::d27Log("HTTP", "range rejected http=%d", status);
                }
            }
        }
        if (bytes > 15 && strncasecmp(ptr, "Content-Length:", 15) == 0) {
            /* Range 响应的 Content-Length 是剩余长度，不是整片长度。 */
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->rangeStart_ <= 0) {
                int64_t length = strtoll(ptr + 15, nullptr, 10);
                if (length > 0)
                    self->contentLength_ = length;
            }
        } else if (bytes > 14 && strncasecmp(ptr, "Content-Range:", 14) == 0) {
            /* bytes 12345-99999/100000 -> 总长度取 '/' 后面的数字。 */
            const char* slash = strrchr(ptr, '/');
            int64_t total = slash ? strtoll(slash + 1, nullptr, 10) : 0;
            if (total > 0) {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->contentLength_ = total;
            }
        }
        return bytes;
    }

    static int progressCallback(void* opaque, double, double, double, double) {
        auto* self = static_cast<Downloader*>(opaque);
        std::lock_guard<std::mutex> lock(self->mutex_);
        return self->stop_ ? 1 : 0;
    }

    void run() {
        for (int attempt = 0; attempt <= kDownloadMaxRetries; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (errorClock_)
                    errorClock_->store(steadyNowMs(), std::memory_order_relaxed);
                error_ = true;
                errorMessage_ = "curl init failed";
                eof_ = true;
                cond_.notify_all();
                return;
            }

            curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            if (!userAgent_.empty())
                curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
            if (!referrer_.empty())
                curl_easy_setopt(curl, CURLOPT_REFERER, referrer_.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressCallback);
            curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, this);

            std::string rangeHeader;
            bool rangeRequest = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                int64_t resumeFrom = attempt == 0 ? rangeStart_ : endOffset_;
                if (resumeFrom > 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%lld-", (long long)resumeFrom);
                    rangeHeader = buf;
                    rangeRequest = true;
                }
                expectRangeStatus_ = rangeRequest;
                rejectRangeBody_ = false;
                error_ = false;
                eof_ = false;
            }
            if (!rangeHeader.empty())
                curl_easy_setopt(curl, CURLOPT_RANGE, rangeHeader.c_str());

            CURLcode result = curl_easy_perform(curl);
            long httpStatus = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
            curl_easy_cleanup(curl);

            bool shouldRetry = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                expectRangeStatus_ = false;
                rejectRangeBody_ = false;
                if (stop_)
                    break;
                bool rangeRejected = rangeRequest && httpStatus != 206;
                if (result == CURLE_OK && !rangeRejected && httpStatus < 400) {
                    eof_ = true;
                    cond_.notify_all();
                    return;
                }
                if (attempt < kDownloadMaxRetries) {
                    /* 已下载的 chunk 保留；下次从窗口末尾续传，已排队读取不受影响。 */
                    sl::d27Log("HTTP",
                               "transfer interrupted error=%s http=%ld "
                               "resume=%lld retry=%d/%d",
                               curl_easy_strerror(result), httpStatus,
                               (long long)endOffset_, attempt + 1,
                               kDownloadMaxRetries);
                    if (errorClock_)
                        errorClock_->store(steadyNowMs(), std::memory_order_relaxed);
                    error_ = false;
                    eof_ = false;
                    cond_.notify_all();
                    shouldRetry = true;
                } else {
                    if (errorClock_)
                        errorClock_->store(steadyNowMs(), std::memory_order_relaxed);
                    if (result != CURLE_OK) {
                        sl::d27Log("HTTP", "download failed error=%s",
                                   curl_easy_strerror(result));
                        errorMessage_ = curl_easy_strerror(result);
                    } else if (httpStatus >= 400) {
                        sl::d27Log("HTTP", "http error status=%ld", httpStatus);
                        errorMessage_ = "HTTP error";
                    } else {
                        sl::d27Log("HTTP", "range request not 206 status=%ld",
                                   httpStatus);
                        errorMessage_ = "Range unsupported";
                    }
                    error_ = true;
                    eof_ = true;
                    cond_.notify_all();
                    return;
                }
            }
            if (shouldRetry)
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    kDownloadRetryBaseDelayMs * (attempt + 1)));
        }
    }

    int append(const uint8_t* data, size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (bytes > 0) {
            while (!stop_ && totalBytes_ + bytes > kStreamBufferLimit) {
                evictLocked(false);
                if (totalBytes_ + bytes <= kStreamBufferLimit)
                    break;
                evictLocked(true);
                if (totalBytes_ + bytes <= kStreamBufferLimit)
                    break;
                if (debug_)
                    sl::d27Log("HTTP", "append wait total=%zu pos=%lld base=%lld",
                               totalBytes_, (long long)readPos_,
                               (long long)baseOffset_);
                cond_.wait(lock);
            }
            if (stop_ || error_)
                return -1;

            if (chunks_.empty() || chunks_.back().used == kStreamChunkSize) {
                Chunk chunk;
                chunk.data.resize(kStreamChunkSize);
                chunk.offset = endOffset_;
                chunks_.push_back(std::move(chunk));
            }
            Chunk& chunk = chunks_.back();
            size_t space = kStreamChunkSize - chunk.used;
            size_t appendBytes = std::min(bytes, space);
            memcpy(chunk.data.data() + chunk.used, data, appendBytes);
            chunk.used += appendBytes;
            endOffset_ += (int64_t)appendBytes;
            totalBytes_ += appendBytes;
            data += appendBytes;
            bytes -= appendBytes;
        }
        cond_.notify_all();
        return 0;
    }

    void evictLocked(bool aggressive) {
        int64_t threshold = aggressive
            ? readPos_
            : (readPos_ > (int64_t)kStreamBackKeep ? readPos_ - (int64_t)kStreamBackKeep : 0);

        while (!chunks_.empty()) {
            Chunk& chunk = chunks_.front();
            int64_t end = chunk.offset + (int64_t)chunk.used;
            if (end > threshold)
                break;
            totalBytes_ -= chunk.used;
            chunks_.pop_front();
        }

        baseOffset_ = chunks_.empty() ? endOffset_ : chunks_.front().offset;
    }

    Chunk* findChunkLocked(int64_t position) {
        for (auto& chunk : chunks_) {
            if (position >= chunk.offset &&
                position < chunk.offset + (int64_t)chunk.used)
                return &chunk;
        }
        return nullptr;
    }

    std::string url_;
    std::string referrer_;
    std::string userAgent_;
    std::string errorMessage_;
    std::atomic<int64_t>* errorClock_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::thread thread_;

    std::deque<Chunk> chunks_;
    int64_t baseOffset_ = 0;
    int64_t endOffset_ = 0;
    int64_t readPos_ = 0;
    size_t totalBytes_ = 0;
    int64_t contentLength_ = 0;
    int64_t rangeStart_ = 0; /* >0 表示当前下载是 Range 请求，Content-Length 不能当总长 */
    bool eof_ = false;
    bool error_ = false;
    bool stop_ = false;
    bool expectRangeStatus_ = false;
    bool rejectRangeBody_ = false;
    bool debug_ = false;
    int readTimeoutMs_ = 15000;
};

/* ---------------- HttpSource ---------------- */

HttpSource::HttpSource() = default;

HttpSource::~HttpSource() { close(); }

bool HttpSource::open(const std::string& url, const std::string& referrer,
                      const std::string& userAgent) {
    close();
    error_.clear();
    lastNetworkErrorMs_.store(0, std::memory_order_relaxed);
    videoIndex_ = -1;
    audioIndex_ = -1;
    remote_ = isRemoteUrl(url);

    static std::once_flag curlOnce;
    std::call_once(curlOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    int ret;
    if (remote_) {
        auto downloader = std::make_shared<Downloader>(url, referrer, userAgent,
                                                       &lastNetworkErrorMs_);
        downloader->start();
        {
            std::lock_guard<std::mutex> lock(downloaderMutex_);
            downloader_ = downloader;
        }

        avioBuffer_ = (uint8_t*)av_malloc(kAvioBufferSize);
        if (!avioBuffer_) {
            error_ = "AVIO 缓冲分配失败";
            close();
            return false;
        }
        avio_ = avio_alloc_context(avioBuffer_, (int)kAvioBufferSize, 0, this,
                                   &HttpSource::readCallback, nullptr,
                                   &HttpSource::seekCallback);
        if (!avio_) {
            error_ = "avio_alloc_context 失败";
            close();
            return false;
        }

        format_ = avformat_alloc_context();
        if (!format_) {
            error_ = "avformat_alloc_context 失败";
            close();
            return false;
        }
        format_->pb = avio_;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;

        ret = avformat_open_input(&format_, "slplayer", nullptr, nullptr);
    } else {
        ret = avformat_open_input(&format_, localPath(url).c_str(), nullptr, nullptr);
    }

    if (ret < 0) {
        char text[128];
        av_strerror(ret, text, sizeof(text));
        error_ = std::string("avformat_open_input: ") + text;
        close();
        return false;
    }

    ret = avformat_find_stream_info(format_, nullptr);
    if (ret < 0) {
        char text[128];
        av_strerror(ret, text, sizeof(text));
        error_ = std::string("avformat_find_stream_info: ") + text;
        close();
        return false;
    }

    videoIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return true;
}

void HttpSource::close() {
    endSeek();
    std::shared_ptr<Downloader> downloader;
    {
        std::lock_guard<std::mutex> lock(downloaderMutex_);
        downloader = downloader_;
        downloader_.reset();
    }
    if (downloader)
        downloader->requestStop();

    if (format_)
        avformat_close_input(&format_);

    if (avio_)
        avio_context_free(&avio_);
    else if (avioBuffer_)
        av_freep(&avioBuffer_);
    avioBuffer_ = nullptr;

    if (downloader)
        downloader->join();

    videoIndex_ = -1;
    audioIndex_ = -1;
    remote_ = false;
}

void HttpSource::interrupt() {
    auto downloader = this->downloader();
    if (downloader)
        downloader->requestStop();
}

void HttpSource::beginSeek(double seconds) {
    std::lock_guard<std::mutex> lock(seekHintMutex_);
    pendingSeekSeconds_ = seconds < 0.0 ? 0.0 : seconds;
    seekHintActive_ = true;
}

void HttpSource::endSeek() {
    std::lock_guard<std::mutex> lock(seekHintMutex_);
    seekHintActive_ = false;
    pendingSeekSeconds_ = -1.0;
}

std::shared_ptr<HttpSource::Downloader> HttpSource::downloader() const {
    std::lock_guard<std::mutex> lock(downloaderMutex_);
    return downloader_;
}

int64_t HttpSource::downloadedBytes() const {
    auto downloader = this->downloader();
    return downloader ? downloader->downloaded() : 0;
}

size_t HttpSource::bufferedBytes() const {
    auto downloader = this->downloader();
    return downloader ? downloader->buffered() : 0;
}

int64_t HttpSource::contentLength() const {
    auto downloader = this->downloader();
    return downloader ? downloader->contentLength() : 0;
}

int64_t HttpSource::readPosition() const {
    auto downloader = this->downloader();
    return downloader ? downloader->readPosition() : 0;
}

int64_t HttpSource::baseOffset() const {
    auto downloader = this->downloader();
    return downloader ? downloader->baseOffset() : 0;
}

int HttpSource::readCallback(void* opaque, uint8_t* data, int size) {
    auto* self = static_cast<HttpSource*>(opaque);
    auto downloader = self->downloader();
    return downloader ? downloader->read(data, size) : AVERROR_EXIT;
}

int64_t HttpSource::seekCallback(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<HttpSource*>(opaque);
    auto downloader = self->downloader();
    if (!downloader) return -1;
    if (whence == SEEK_SET) {
        double seconds = -1.0;
        {
            std::lock_guard<std::mutex> lock(self->seekHintMutex_);
            if (self->seekHintActive_) seconds = self->pendingSeekSeconds_;
        }
        int64_t length = downloader->contentLength();
        double duration = self->format_ && self->format_->duration > 0
                              ? (double)self->format_->duration / AV_TIME_BASE
                              : 0.0;
        /* 只在 FFmpeg 给出的字节偏移越界时用时间提示纠偏；
         * 正常 seek（包括窗口外回跳）仍使用 FFmpeg 的精确偏移 + Range 重定位。 */
        if (seconds >= 0.0 && length > 0 && duration > 0.0 &&
            (offset < 0 || offset >= length)) {
            double ratio = seconds / duration;
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            int64_t mapped = (int64_t)((double)length * ratio);
            sl::d27Log("HTTP", "seek time hint %.3f/%.3fs -> %lld/%lld",
                       seconds, duration, (long long)mapped, (long long)length);
            return downloader->seek(mapped, whence);
        }
    }
    return downloader->seek(offset, whence);
}

} // namespace sl
