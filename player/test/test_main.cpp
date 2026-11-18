/*
 * slplayer_test - SlPlayer 真机验证程序
 *
 * 用法:
 *   slplayer_test <视频URL或本地路径> [--audio <音频URL>]... [--start 秒]
 *                 [--seconds 运行秒数] [--volume 0-100] [--referrer URL]
 *                 [--pause-at 秒 --pause-for 秒] [--seek-at 秒 --seek-to 秒]
 *
 * 控制:
 *   空格 / A  暂停/继续
 *   左右 / 十字键  后退/前进 10 秒
 *   上下      音量 -10/+10
 *   ESC / B   退出
 */

#include "SlPlayer.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

void usage(const char* name) {
    fprintf(stderr,
            "用法: %s <视频URL或本地路径> [--audio <音频URL>]... [--start 秒]\n"
            "          [--seconds 运行秒数] [--volume 0-100] [--referrer URL]\n"
            "          [--pause-at 秒 --pause-for 秒] [--seek-at 秒 --seek-to 秒]\n"
            "          [--no-audio]\n",
            name);
}

const char* stateName(const sl::Player& player) {
    if (!player.isRunning())
        return "stop";
    return player.isPaused() ? "pause" : "play";
}

void printStats(sl::Player& player, double wallSeconds) {
    sl::Stats stats = player.stats();
    printf("[stats] %.1fs wall | %s | pos %.2f / %.2f s | vf %d drop %d retry %d vq %u ms\n",
           wallSeconds, stateName(player), stats.position, stats.duration,
           stats.videoFrames, stats.droppedFrames, stats.beginRetries,
           stats.videoQueueMs);
    printf("[audio] %d Hz started=%d queued=%u delay=%.0f ms submitted=%.2f s "
           "packets=%d | net %lld B win %zu B read %lld base %lld stage %d\n",
           stats.audioRate, stats.audioStarted ? 1 : 0, stats.audioQueuedSamples,
           stats.audioDelayMs, stats.audioSubmittedMs / 1000.0, stats.audioPackets,
           stats.downloadedBytes, stats.bufferedBytes, stats.readPosition,
           stats.bufferBaseOffset, stats.stage);
    /* 第 2 步验证：无锁心跳快照。frameAge = 距最后一帧上屏的毫秒数，
     * watchdog 就是用这个值判断是否卡死（正常播放时应持续很小）。 */
    sl::Heartbeat hb = player.heartbeat();
    std::int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    long long frameAge = hb.lastFrameTimeMs ? (nowMs - hb.lastFrameTimeMs) : -1;
    printf("[hb] frameAge=%lld ms stage=%d playing=%d transition=%d vf=%d "
           "(watchdog 阈值 1500ms)\n",
           frameAge, hb.stage, hb.playing ? 1 : 0, hb.inTransition ? 1 : 0,
           hb.videoFrames);
}

} // namespace

int main(int argc, char** argv) {
    sl::SourceSpec spec;
    double runSeconds = 0.0;
    int volume = 100;
    bool haveUrl = false;
    double pauseAt = -1.0;
    double pauseFor = 2.0;
    double seekAt = -1.0;
    double seekTo = 0.0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&](const char* option) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s 需要参数\n", option);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--audio") {
            spec.audioUrls.emplace_back(next("--audio"));
        } else if (arg == "--start") {
            spec.startSeconds = atof(next("--start"));
        } else if (arg == "--seconds") {
            runSeconds = atof(next("--seconds"));
        } else if (arg == "--volume") {
            volume = atoi(next("--volume"));
        } else if (arg == "--referrer") {
            spec.referrer = next("--referrer");
        } else if (arg == "--ua") {
            spec.userAgent = next("--ua");
        } else if (arg == "--pause-at") {
            pauseAt = atof(next("--pause-at"));
        } else if (arg == "--pause-for") {
            pauseFor = atof(next("--pause-for"));
        } else if (arg == "--seek-at") {
            seekAt = atof(next("--seek-at"));
        } else if (arg == "--seek-to") {
            seekTo = atof(next("--seek-to"));
        } else if (arg == "--no-audio") {
            spec.disableAudio = true;
        } else if (!arg.empty() && arg[0] != '-') {
            spec.videoUrl = arg;
            haveUrl = true;
        } else {
            fprintf(stderr, "未知参数: %s\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    if (!haveUrl) {
        usage(argv[0]);
        return 1;
    }

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0)
        fprintf(stderr, "SDL 初始化失败（继续用键盘控制）: %s\n", SDL_GetError());

    SDL_GameController* controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) {
                printf("手柄: %s\n", SDL_GameControllerName(controller));
                break;
            }
        }
    }

    sl::Player player;
    player.setVolume(volume);
    player.setEventCallback([&](const sl::Event& event) {
        printf("[event] %-13s pos %.2f / %.2f s%s%s\n",
               sl::eventTypeName(event.type), event.position, event.duration,
               event.message.empty() ? "" : " msg=", event.message.c_str());
        if (event.type == sl::EventType::Opened) {
            sl::VideoInfo info = player.videoInfo();
            printf("[media] %dx%d %.2f fps %.2f s\n", info.width, info.height,
                   info.fps, info.duration);
        }
    });
    /* 第 2 步验证：VPU 致命错误上报（上层在此自行决策，测试程序只打印） */
    player.setFatalCallback([](sl::FatalType type, const std::string& message) {
        printf("[fatal] %s - %s\n", sl::fatalTypeName(type), message.c_str());
    });

    if (!player.open(spec)) {
        fprintf(stderr, "open 失败\n");
        return 1;
    }
    player.play();

    auto start = std::chrono::steady_clock::now();
    auto lastStats = start;
    bool quit = false;
    bool pauseDone = false;
    bool resumeDone = false;
    bool seekDone = false;

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            int action = 0;
            if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_SPACE: action = 1; break;
                case SDLK_LEFT: action = 2; break;
                case SDLK_RIGHT: action = 3; break;
                case SDLK_UP: action = 4; break;
                case SDLK_DOWN: action = 5; break;
                case SDLK_ESCAPE: quit = true; break;
                }
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A: action = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: action = 2; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: action = 3; break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: action = 4; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = 5; break;
                case SDL_CONTROLLER_BUTTON_B: quit = true; break;
                }
            }

            if (action == 1) {
                if (player.isPaused())
                    player.play();
                else
                    player.pause();
            } else if (action == 2 || action == 3) {
                double delta = action == 2 ? -10.0 : 10.0;
                double target = std::max(0.0, player.position() + delta);
                if (player.duration() > 0.0)
                    target = std::min(target, player.duration() - 1.0);
                printf("seek -> %.1f s\n", target);
                player.seek(target);
            } else if (action == 4 || action == 5) {
                volume = std::min(100, std::max(0, volume + (action == 4 ? 10 : -10)));
                player.setVolume(volume);
                printf("volume -> %d\n", volume);
            }
        }

        auto now = std::chrono::steady_clock::now();
        double wall = std::chrono::duration<double>(now - start).count();

        if (pauseAt >= 0.0 && !pauseDone && wall >= pauseAt) {
            printf("自动暂停 @ %.1f s\n", wall);
            player.pause();
            pauseDone = true;
        }
        if (pauseDone && !resumeDone && wall >= pauseAt + pauseFor) {
            printf("自动继续 @ %.1f s\n", wall);
            player.play();
            resumeDone = true;
        }
        if (seekAt >= 0.0 && !seekDone && wall >= seekAt) {
            printf("自动 seek -> %.1f s @ %.1f s\n", seekTo, wall);
            player.seek(seekTo);
            seekDone = true;
        }

        if (now - lastStats >= std::chrono::seconds(1)) {
            lastStats = now;
            printStats(player, wall);
        }

        if (!player.isRunning())
            break;
        if (runSeconds > 0.0 && wall >= runSeconds)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    printf("退出: wall %.1f s\n",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
               .count());
    player.stop();

    if (controller)
        SDL_GameControllerClose(controller);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
    return 0;
}
