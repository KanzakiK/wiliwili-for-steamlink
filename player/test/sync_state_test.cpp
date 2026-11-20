/*
 * D28 S03：SlPlayer 本地四状态与双 ready bit 契约测试。
 * 不链接 SDK，只验证状态跟踪器的转换和 epoch 过期判据。
 */
#include "SlPlayer.h"

#include <cstdio>

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main() {
    sl::SyncStateTracker state;
    CHECK(state.phase.load() == static_cast<int>(sl::SyncPhase::Idle));
    CHECK(!state.allReady());

    state.begin(7, 11, true);
    CHECK(state.phase.load() == static_cast<int>(sl::SyncPhase::Preparing));
    CHECK(state.matches(7));
    CHECK(state.matches(0)); /* 旧调用者不带 epoch 时仍兼容 */
    CHECK(!state.matches(6));
    CHECK(state.desiredPlay.load() == 1);

    state.setReady(sl::kSyncReadyVideo);
    CHECK(state.has(sl::kSyncReadyVideo));
    CHECK(!state.allReady());
    state.setReady(sl::kSyncReadyAudio);
    CHECK(state.allReady());
    state.clearReady(sl::kSyncReadyAudio);
    CHECK(!state.allReady());

    state.begin(8, 12, false);
    CHECK(!state.matches(7));
    CHECK(state.desiredPlay.load() == 0);
    CHECK(!state.allReady());

    state.phase.store(static_cast<int>(sl::SyncPhase::Paused));
    CHECK(state.phase.load() == static_cast<int>(sl::SyncPhase::Paused));

    std::printf("sync state contract ok\n");
    return 0;
}
