/**

██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██  █  ██ ██ ██      ██ ██  █  ██ ██ ██      ██
██ ███ ██ ██ ██      ██ ██ ███ ██ ██ ██      ██
 ███ ███  ██ ███████ ██  ███ ███  ██ ███████ ██

 Licensed under the GPL-3.0 license
*/

#include <borealis.hpp>

#include "utils/config_helper.hpp"
#include "utils/debug_log.hpp"
#include "utils/activity_helper.hpp"
#include "activity/player_activity.hpp"
#include "view/mpv_core.hpp"
#include "view/video_view.hpp"

#ifdef IOS
#include <SDL2/SDL_main.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace {

std::string d27Arg(const std::string& args, const char* key) {
    size_t pos = 0;
    while (pos <= args.size()) {
        size_t end = args.find(';', pos);
        std::string item = args.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == key)
            return item.substr(eq + 1);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return "";
}

void d27Log(const char* stage, const std::string& scene, const std::string& detail) {
    wiliwili::d27::log(stage, "scene=" + scene + " " + detail);
}

void d27ExpectPlaying(const std::string& scene, int attempts,
                      const std::function<void()>& done) {
    if (MPVCore::instance().isPlaying()) {
        brls::delay(500, done);
        return;
    }
    if (attempts <= 0) {
        d27Log("CMD", scene, "status=timeout waiting=playing");
        return;
    }
    brls::delay(200, [scene, attempts, done]() {
        d27ExpectPlaying(scene, attempts - 1, done);
    });
}

void d27ExpectPaused(const std::string& scene, int attempts,
                     const std::function<void()>& done) {
    if (MPVCore::instance().isPaused()) {
        done();
        return;
    }
    if (attempts <= 0) {
        d27Log("CMD", scene, "status=timeout waiting=paused");
        return;
    }
    brls::delay(200, [scene, attempts, done]() {
        d27ExpectPaused(scene, attempts - 1, done);
    });
}

void d27ExpectPosition(const std::string& scene, double target, int attempts) {
    if (fabs(MPVCore::instance().playback_time - target) <= 2.0) {
        d27Log("CMD", scene, "status=ok");
        return;
    }
    if (attempts <= 0) {
        d27Log("CMD", scene, "status=timeout waiting=position");
        return;
    }
    brls::delay(200, [scene, target, attempts]() {
        d27ExpectPosition(scene, target, attempts - 1);
    });
}

BasePlayerActivity* d27CurrentPlayerActivity() {
    auto stack = brls::Application::getActivitiesStack();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (auto* playerActivity = dynamic_cast<BasePlayerActivity*>(*it))
            return playerActivity;
    }
    return nullptr;
}

void d27ExpectQuality(const std::string& scene, int qn,
                      const std::string& oldSession, int attempts,
                      const std::function<void()>& done) {
    auto* playerActivity = d27CurrentPlayerActivity();
    std::string session = wiliwili::d27::currentSession();
    if (playerActivity && session != oldSession &&
        playerActivity->getQualityCodeForDebug() == qn &&
        MPVCore::instance().isPlaying()) {
        char detail[192];
        snprintf(detail, sizeof(detail),
                 "qn=%d old_session=%s new_session=%s status=ok", qn,
                 oldSession.c_str(), session.c_str());
        d27Log("CMD", scene, detail);
        done();
        return;
    }
    if (attempts <= 0) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "qn=%d status=timeout waiting=new_session_and_playing", qn);
        d27Log("CMD", scene, detail);
        return;
    }
    brls::delay(200, [scene, qn, oldSession, attempts, done]() {
        d27ExpectQuality(scene, qn, oldSession, attempts - 1, done);
    });
}

} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::TERMINAL = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "wiliwili.log";
            brls::Logger::setLogOutput(std::fopen(path, "w+"));
        }
    }

    // Load cookies and settings
    ProgramConfig::instance().init();

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }

    // Return directly to the desktop when closing the application (only for NX)
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow("wiliwili");
    brls::Logger::info("createWindow done");

    // Register custom view\theme\style
    Register::initCustomView();
    Register::initCustomTheme();
    Register::initCustomStyle();

    brls::Application::getPlatform()->disableScreenDimming(false);

    if (brls::Application::getPlatform()->isApplicationMode()) {
        Intent::openMain();
        const char* debugRun = getenv("SL_DEBUG_RUN_ID");
        const char* debugContent = getenv("SL_DEBUG_CONTENT");
        const char* debugScene = getenv("SL_DEBUG_SCENE");
        if (debugRun && *debugRun && debugContent && *debugContent &&
            debugScene && *debugScene) {
            std::string content = debugContent;
            std::string scene = debugScene;
            std::string args = getenv("SL_DEBUG_ARGS") ? getenv("SL_DEBUG_ARGS") : "";
            int timeout = atoi(d27Arg(args, "timeout").c_str());
            if (timeout <= 0) timeout = 30;

            std::string debugContentType = "page";
            std::string debugContentId = content;
            std::string debugContentBv;
            std::string debugContentDetail = "target=" + content;
            if (content.rfind("video:", 0) == 0) {
                std::string value = content.substr(6);
                size_t cidPos = value.find(':');
                debugContentType = "video";
                debugContentId = cidPos == std::string::npos ? value : value.substr(0, cidPos);
                debugContentBv = debugContentId;
                debugContentDetail = cidPos == std::string::npos
                                         ? "bv=" + debugContentBv
                                         : "bv=" + debugContentBv +
                                               " cid=" + value.substr(cidPos + 1);
            } else if (content.rfind("season:ep:", 0) == 0) {
                debugContentType = "season";
                debugContentId = content.substr(10);
                debugContentDetail = "ep_id=" + debugContentId;
            } else if (content.rfind("season:", 0) == 0) {
                debugContentType = "season";
                debugContentId = content.substr(7);
                debugContentDetail = "season_id=" + debugContentId;
            } else if (content.rfind("live:", 0) == 0) {
                debugContentType = "live";
                debugContentId = content.substr(5);
                debugContentDetail = "room_id=" + debugContentId;
            } else if (content.rfind("dynamic:", 0) == 0) {
                debugContentType = "dynamic";
                debugContentId = content.substr(8);
                debugContentDetail = "id=" + debugContentId;
            }
            wiliwili::d27::prepareControllerSession(debugContentType, debugContentId,
                                                    debugContentDetail, debugContentBv);
            if (debugContentType == "page") {
                wiliwili::d27::reuseOrBeginSession("page", debugContentId,
                                                   debugContentDetail);
            }
            if (content.rfind("video:", 0) == 0)
                Intent::openBV(debugContentId);
            else if (content.rfind("season:ep:", 0) == 0)
                Intent::openSeasonByEpId(strtoull(content.substr(10).c_str(), nullptr, 10));
            else if (content.rfind("season:", 0) == 0)
                Intent::openSeasonBySeasonId(strtoull(content.substr(7).c_str(), nullptr, 10));
            else if (content.rfind("live:", 0) == 0)
                Intent::openLive(atoi(content.substr(5).c_str()));
            else if (content.rfind("dynamic:", 0) == 0)
                Intent::openActivity(content.substr(8));
            else if (content == "page:setting")
                Intent::openSetting();
            else if (content.rfind("page:search:", 0) == 0)
                Intent::openSearch(content.substr(12));
            else if (content.rfind("page:collection:", 0) == 0)
                Intent::openCollection(content.substr(16));
            d27Log("CMD", scene, "status=start content=" + content);

            if (scene == "page_nav" || scene == "none") {
                brls::delay(500, [scene]() {
                    d27Log("CMD", scene, "status=ok");
                    wiliwili::d27::endSession("user_stop");
                });
            } else if (scene == "play") {
                d27ExpectPlaying(scene, timeout * 5,
                                 [scene]() { d27Log("CMD", scene, "status=ok"); });
            } else if (scene == "seek_forward" || scene == "seek_backward") {
                std::string targetText = d27Arg(args, "target");
                std::string deltaText = d27Arg(args, "delta");
                std::string targetMsText = d27Arg(args, "target_ms");
                std::string deltaMsText = d27Arg(args, "delta_ms");
                if (targetText.empty() && deltaText.empty() &&
                    targetMsText.empty() && deltaMsText.empty()) {
                    d27Log("CMD", scene,
                           "status=error missing_target_or_delta_ms");
                    return EXIT_FAILURE;
                }
                /* S05：毫秒参数优先；旧秒参数仍可由旧脚本使用。 */
                bool useTarget = !targetText.empty() || !targetMsText.empty();
                bool useTargetMs = !targetMsText.empty();
                bool useDeltaMs = !deltaMsText.empty();
                double target = useTargetMs
                                    ? (double)strtoll(targetMsText.c_str(), nullptr, 10) / 1000.0
                                    : atof(targetText.c_str());
                double delta = useDeltaMs
                                   ? (double)strtoll(deltaMsText.c_str(), nullptr, 10) / 1000.0
                                   : atof(deltaText.c_str());
                long long targetMs = strtoll(targetMsText.c_str(), nullptr, 10);
                long long deltaMs = strtoll(deltaMsText.c_str(), nullptr, 10);
                d27ExpectPlaying(scene, timeout * 5,
                                 [scene, target, delta, useTarget, useTargetMs,
                                  useDeltaMs, targetMs, deltaMs, timeout]() {
                    double expected = target;
                    if (useTarget) {
                        char detail[192];
                        if (useTargetMs)
                            snprintf(detail, sizeof(detail),
                                     "mode=target target_ms=%lld status=sent",
                                     targetMs);
                        else
                            snprintf(detail, sizeof(detail),
                                     "mode=target target=%.3f status=sent", target);
                        d27Log("CMD", scene, detail);
                        MPVCore::instance().command_async(
                            "seek", fmt::format("{:.3f}", target), "absolute");
                    } else {
                        double current = MPVCore::instance().playback_time;
                        double signedDelta =
                            scene == "seek_forward" ? delta : -delta;
                        long long signedDeltaMs =
                            useDeltaMs
                                ? (scene == "seek_forward" ? deltaMs : -deltaMs)
                                : (long long)(signedDelta * 1000.0);
                        expected = current + signedDelta;
                        if (expected < 0) expected = 0;
                        char detail[256];
                        if (useDeltaMs)
                            snprintf(detail, sizeof(detail),
                                     "mode=delta delta_ms=%lld current=%.3f "
                                     "expected=%.3f status=sent",
                                     signedDeltaMs, current, expected);
                        else
                            snprintf(detail, sizeof(detail),
                                     "mode=delta delta=%.3f current=%.3f "
                                     "expected=%.3f status=sent",
                                     delta, current, expected);
                        d27Log("CMD", scene, detail);
                        MPVCore::instance().command_async(
                            "seek", fmt::format("{:.3f}", signedDelta),
                            "relative");
                    }
                    d27ExpectPosition(scene, expected, timeout * 5);
                });
            } else if (scene == "pause_resume") {
                int after = atoi(d27Arg(args, "after").c_str());
                if (after <= 0) after = 2;
                d27ExpectPlaying(scene, timeout * 5, [scene, after]() {
                    MPVCore::instance().pause();
                    d27ExpectPaused(scene, 150, [scene, after]() {
                        brls::delay(after * 1000, [scene]() {
                            MPVCore::instance().resume();
                            d27ExpectPlaying(scene, 150,
                                             [scene]() { d27Log("CMD", scene, "status=ok"); });
                        });
                    });
                });
            } else if (scene == "quality_change") {
                int qn = atoi(d27Arg(args, "qn").c_str());
                if (qn <= 0) {
                    d27Log("CMD", scene, "status=error missing_qn");
                    return EXIT_FAILURE;
                }
                d27ExpectPlaying(scene, timeout * 5, [scene, qn, timeout]() {
                    auto* playerActivity = d27CurrentPlayerActivity();
                    if (!playerActivity) {
                        d27Log("CMD", scene, "status=error no_player_activity");
                        return;
                    }
                    std::string oldSession = wiliwili::d27::currentSession();
                    int oldQn = playerActivity->getQualityCodeForDebug();
                    char detail[192];
                    snprintf(detail, sizeof(detail),
                             "qn=%d old_qn=%d old_session=%s status=sent", qn, oldQn,
                             oldSession.c_str());
                    d27Log("CMD", scene, detail);
                    if (!playerActivity->setQualityCodeForDebug(qn)) {
                        d27Log("CMD", scene, "status=error invalid_qn");
                        return;
                    }
                    d27ExpectQuality(scene, qn, oldSession, timeout * 5, []() {});
                });
            } else {
                d27Log("CMD", scene, "status=error unknown_scene");
            }
        }
        // Uncomment these lines to debug activities
        //        Intent::openBV("BV1Da411Y7U4");  // 弹幕防遮挡 (横屏)
        //        Intent::openBV("BV1iN4y1m7J3");  // 弹幕防遮挡 (竖屏)
        //        Intent::openBV("BV1kT4y1s7od");  // 高级弹幕 测试0
        //        Intent::openBV("BV1eN4y147bC");  // 高级弹幕 测试1
        //        Intent::openBV("BV16x411D7NK");  // 高级弹幕 测试2
        //        Intent::openBV("BV1uW411e7gt");  // bas 弹幕 (Bilibili Animation Script)
        //        Intent::openBV("BV1zb4y1j7vz");  // flv 模式报错：HTTP 424
        //        Intent::openBV("BV1jL41167ZG");  // 充电视频
        //        Intent::openBV("BV1dx411c7Av");  // flv拼接视频
        //        Intent::openBV("BV15z4y1Z734");  // 4K HDR 视频
        //        Intent::openBV("BV1qM4y1w716");  // 8K
        //        Intent::openBV("BV1PN4y1G7u2");  // up主视频自动跳转番剧
        //        Intent::openBV("BV1sK411s7zq");  // 多P视频测试
        //        Intent::openBV("BV1Cg411j76F");  // 多字幕测试
        //        Intent::openBV("BV1A44y1u7PF");  // 测试FFMPEG在switch上的bug（加载时间过长）
        //        Intent::openBV("BV1eD4y1b7Jv");  // 测试 MPV 在switch上的bug（长时间播放崩溃）
        //        Intent::openBV("BV1U3411c7Qx");  // 测试长标题
        //        Intent::openBV("BV1fG411W7Px");  // 测试弹幕
        //        Intent::openSeasonByEpId(323434);// 测试电影
        //        Intent::openLive(1942240);       // 测试直播
        //        Intent::openSearch("harry");     // 测试搜索影片
        //        Intent::openTVSearch();          // 测试TV搜索模式
        //        Intent::openHint();              // 应用开启教程页面
        //        Intent::openCollection("2511565362"); // 测试打开收藏夹
        //        Intent::openPgcFilter("/page/home/pgc/more?type=2&index_type=2&area=2&order=2&season_status=-1&season_status=3,6"); // 影片分类索引
        //        Intent::openSetting();  //  设置页面
    } else {
        Intent::openHint();
    }

    GA("open_app", {{"version", APPVersion::instance().getVersionStr()},
                    {"language", brls::Application::getLocale()},
                    {"window", fmt::format("{}x{}", brls::Application::windowWidth, brls::Application::windowHeight)}})
    APPVersion::instance().checkUpdate();

    // Run the app
    // brls::Application::setLimitedFPS(60);
    while (brls::Application::mainLoop()) {
    }

    brls::Logger::info("mainLoop done");

    // Cleanup curl and Check whether restart is required
    ProgramConfig::instance().exit(argv);

    // Exit
    return EXIT_SUCCESS;
}

#ifdef __WINRT__
#include <borealis/core/main.hpp>
#endif
