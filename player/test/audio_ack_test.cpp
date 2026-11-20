/*
 * D28 S02：seek ACK 状态契约测试。
 * 只依赖 AudioPipeline.h 中的轻量状态结构，不链接 FFmpeg/SLAudio。
 */
#include "AudioPipeline.h"

#include <cstdio>

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main() {
    sl::SeekAckState ack;
    CHECK(ack.result(0) == sl::SeekStatus::None);
    CHECK(ack.result(1) == sl::SeekStatus::Stale);
    CHECK(!ack.pending());
    CHECK(!ack.isMuted());

    std::uint64_t first = ack.begin();
    CHECK(first == 1);
    CHECK(ack.pending());
    CHECK(ack.result(first) == sl::SeekStatus::Pending);
    CHECK(ack.isMuted());

    std::uint64_t second = ack.begin();
    CHECK(second == 2);
    CHECK(ack.pending());
    CHECK(ack.result(first) == sl::SeekStatus::Stale);
    CHECK(ack.result(second) == sl::SeekStatus::Pending);

    /* 旧序列完成不能改变当前序列结果。 */
    ack.finish(first, true);
    CHECK(ack.result(first) == sl::SeekStatus::Stale);
    CHECK(ack.result(second) == sl::SeekStatus::Pending);

    /* 当前序列失败要留下 failed，供 SlPlayer 保持静音、不置 AUDIO_READY。 */
    ack.finish(second, false);
    CHECK(ack.result(second) == sl::SeekStatus::Failed);
    CHECK(!ack.pending());
    CHECK(ack.isMuted());

    ack.reset();
    CHECK(ack.result(0) == sl::SeekStatus::None);
    CHECK(!ack.isMuted());
    std::uint64_t third = ack.begin();
    CHECK(third == 1);
    ack.finish(third, true);
    CHECK(ack.result(third) == sl::SeekStatus::Success);
    CHECK(!ack.isMuted());
    CHECK(ack.result(0) == sl::SeekStatus::Stale);

    /* 成功 ACK 的四个条件：demux（如适用）、新 stream、SDK 队列为空。 */
    CHECK(sl::seekAckReady(true, true, 0));
    CHECK(!sl::seekAckReady(false, true, 0));
    CHECK(!sl::seekAckReady(true, false, 0));
    CHECK(!sl::seekAckReady(true, true, 960));

    std::printf("audio ack contract ok\n");
    return 0;
}
