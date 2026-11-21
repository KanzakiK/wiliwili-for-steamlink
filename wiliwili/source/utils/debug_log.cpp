#include "utils/debug_log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace wiliwili {
namespace d27 {
namespace {

std::mutex g_mutex;
std::string g_run;
std::string g_session;
std::string g_content;
std::string g_id;
std::string g_bv;
std::string g_parent;
std::string g_pendingParent;
uint64_t g_sequence = 0;
bool g_controllerSession = false;
bool g_ended = false;
bool g_started = false;
bool g_exitHookInstalled = false;
bool g_identityActive = false;
std::string g_identityContent;
std::string g_identityId;
std::string g_identityBv;
std::string g_identityCid;
std::string g_identityQn;

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03lld",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms.count());
    return buffer;
}

std::string valueOrDash(const std::string& value) {
    return value.empty() ? "-" : value;
}

void emitLog(const std::string& run, const std::string& session,
             const char* source, const std::string& message) {
    if (run.empty()) return;
    std::fprintf(stderr, "[%s][%s][run=%s][session=%s][gen=-][req=-] %s\n",
                 timestamp().c_str(), source, run.c_str(),
                 valueOrDash(session).c_str(), message.c_str());
}

void emitContentEnd(const std::string& run, const std::string& session,
                    const std::string& content, const std::string& id,
                    const std::string& bv, const std::string& reason) {
    emitLog(run, session, "UI",
            "content end reason=" + reason + " content=" + content +
                " id=" + id + " bv=" + valueOrDash(bv));
}

void exitHook() { endSession("app_exit"); }

void installExitHook() {
    if (g_exitHookInstalled) return;
    g_exitHookInstalled = true;
    std::atexit(exitHook);
}

} // namespace

std::string runId() {
    const char* run = std::getenv("SL_DEBUG_RUN_ID");
    return run && *run ? run : "";
}

std::string currentSession() {
    std::unique_lock<std::mutex> lock(g_mutex);
    return g_session;
}

std::string beginSession(const std::string& content, const std::string& id,
                         const std::string& detail, const std::string& bv,
                         const std::string& parent) {
    std::string run = runId();
    if (run.empty()) return "";
    std::string session;
    std::string message;
    std::string previousRun, previousSession, previousContent, previousId, previousBv;
    bool replaceActive = false;
    std::unique_lock<std::mutex> lock(g_mutex);
    installExitHook();
    if (!g_session.empty() && g_started && !g_ended && !g_controllerSession) {
        replaceActive = true;
        previousRun = g_run;
        previousSession = g_session;
        previousContent = g_content;
        previousId = g_id;
        previousBv = g_bv;
    }
    g_run = run;
    g_session = run + "-" + std::to_string(++g_sequence);
    g_content = content;
    g_id = id;
    g_bv = bv;
    g_parent = parent;
    g_controllerSession = false;
    g_ended = false;
    g_started = true;
    session = g_session;
    message = "content start content=" + content + " id=" + id +
              " bv=" + valueOrDash(bv) +
              (parent.empty() ? "" : " parent_session=" + parent) +
              " detail=" + detail;
    lock.unlock();
    if (replaceActive)
        emitContentEnd(previousRun, previousSession, previousContent, previousId,
                       previousBv, "replaced");
    log("UI", message);
    return session;
}

std::string reuseOrBeginSession(const std::string& content, const std::string& id,
                                const std::string& detail, const std::string& bv) {
    std::string run = runId();
    if (run.empty()) return "";
    std::string session;
    std::string message;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_controllerSession && !g_ended) {
        std::string parent = g_pendingParent;
        g_pendingParent.clear();
        g_content = content;
        g_id = id;
        g_bv = bv;
        if (!parent.empty()) g_parent = parent;
        g_controllerSession = false;
        g_started = true;
        session = g_session;
        message = "content start content=" + content + " id=" + id +
                  " bv=" + valueOrDash(bv) +
                  (g_parent.empty() ? "" : " parent_session=" + g_parent) +
                  " detail=" + detail;
        lock.unlock();
        log("UI", message);
        return session;
    }
    lock.unlock();
    std::string parent = consumePendingParent();
    return beginSession(content, id, detail, bv, parent);
}

void beginControllerSession(const std::string& content, const std::string& id,
                            const std::string& detail, const std::string& bv) {
    beginSession(content, id, detail, bv);
    std::unique_lock<std::mutex> lock(g_mutex);
    g_controllerSession = true;
}

void prepareControllerSession(const std::string& content, const std::string& id,
                              const std::string& detail, const std::string& bv) {
    std::string run = runId();
    if (run.empty()) return;
    std::unique_lock<std::mutex> lock(g_mutex);
    g_run = run;
    g_session = run + "-" + std::to_string(++g_sequence);
    g_content = content;
    g_id = id;
    g_bv = bv;
    g_parent.clear();
    g_controllerSession = true;
    g_ended = false;
    g_started = false;
    installExitHook();
}

void endSession(const std::string& reason) {
    std::string message;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_session.empty()) {
        /* 正常启动没有 debug session，但内容身份日志也要成对收尾。 */
        if (!g_identityActive) return;
        g_identityActive = false;
        message = "content end reason=" + reason +
                  " content=" + g_identityContent + " id=" + g_identityId +
                  " bv=" + valueOrDash(g_identityBv) +
                  " cid=" + valueOrDash(g_identityCid) +
                  " qn=" + valueOrDash(g_identityQn);
        lock.unlock();
        emitLog("-", "-", "UI", message);
        return;
    }
    if (g_ended || !g_started) return;
    g_ended = true;
    g_started = false;
    message = "content end reason=" + reason + " content=" + g_content +
              " id=" + g_id + " bv=" + valueOrDash(g_bv);
    lock.unlock();
    log("UI", message);
}

void setPendingParent(const std::string& session) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingParent = session;
}

std::string consumePendingParent() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string result = g_pendingParent;
    g_pendingParent.clear();
    return result;
}

void markContentStarted() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_started = true;
    g_ended = false;
}

void log(const char* source, const std::string& message) {
    std::string run, session;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        run = g_run;
        session = g_session;
    }
    emitLog(run, session, source, message);
}

void logContentIdentity(const std::string& content, const std::string& id,
                        const std::string& detail, const std::string& bv,
                        const std::string& cid, const std::string& qn) {
    // 生产日志不带 run/session，但内容身份必须可追溯；debug run 下由
    // beginSession/reuseOrBeginSession 输出正式 content start，不重复写这里。
    if (!runId().empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_identityActive = true;
        g_identityContent = content;
        g_identityId = id;
        g_identityBv = bv;
        g_identityCid = cid;
        g_identityQn = qn;
    }
    std::string message = "content start content=" + content + " id=" + id +
                          " bv=" + valueOrDash(bv) + " cid=" + valueOrDash(cid) +
                          " qn=" + valueOrDash(qn) + " detail=" + detail;
    emitLog("-", "-", "UI", message);
}

} // namespace d27
} // namespace wiliwili
