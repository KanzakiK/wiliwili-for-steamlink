#include "DebugLog.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace sl {
namespace {

std::mutex g_debugLogMutex;
DebugLogContext g_debugLogContext;

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

std::string d27IdentityFor(const DebugLogContext& context) {
    return "content=" + valueOrDash(context.content) +
           " id=" + valueOrDash(context.id) +
           " bv=" + valueOrDash(context.bv) +
           " cid=" + valueOrDash(context.cid) +
           " qn=" + valueOrDash(context.qn);
}

void logV(const char* source, const DebugLogContext& context, bool withIdentity,
          const char* fmt, va_list args) {
    char ids[128];
    std::snprintf(ids, sizeof(ids), "[run=%s][session=%s][gen=%llu][req=%llu]",
                  valueOrDash(context.run).c_str(),
                  valueOrDash(context.session).c_str(),
                  (unsigned long long)context.generation,
                  (unsigned long long)context.requestId);
    std::fprintf(stderr, "[%s][%s]%s ", timestamp().c_str(), source, ids);
    std::vfprintf(stderr, fmt, args);
    if (withIdentity)
        std::fprintf(stderr, " %s", d27IdentityFor(context).c_str());
    std::fputc('\n', stderr);
}

} // namespace

void setDebugLogContext(const DebugLogContext& context) {
    std::lock_guard<std::mutex> lock(g_debugLogMutex);
    g_debugLogContext = context;
}

void clearDebugLogContext() {
    std::lock_guard<std::mutex> lock(g_debugLogMutex);
    g_debugLogContext = DebugLogContext{};
}

DebugLogContext currentDebugLogContext() {
    std::lock_guard<std::mutex> lock(g_debugLogMutex);
    return g_debugLogContext;
}

void d27Log(const char* source, const char* fmt, ...) {
    DebugLogContext context;
    {
        std::lock_guard<std::mutex> lock(g_debugLogMutex);
        context = g_debugLogContext;
    }
    va_list args;
    va_start(args, fmt);
    logV(source, context, false, fmt, args);
    va_end(args);
}

void d27LogReq(const char* source, std::uint64_t requestId, const char* fmt, ...) {
    DebugLogContext context = currentDebugLogContext();
    context.requestId = requestId;
    va_list args;
    va_start(args, fmt);
    logV(source, context, false, fmt, args);
    va_end(args);
}

void d27LogContext(const char* source, const DebugLogContext& context,
                   const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(source, context, false, fmt, args);
    va_end(args);
}

std::string d27Identity() { return d27IdentityFor(currentDebugLogContext()); }

void d27LogIdentity(const char* source, const char* fmt, ...) {
    DebugLogContext context = currentDebugLogContext();
    va_list args;
    va_start(args, fmt);
    logV(source, context, true, fmt, args);
    va_end(args);
}

void d27LogIdentityReq(const char* source, std::uint64_t requestId,
                       const char* fmt, ...) {
    DebugLogContext context = currentDebugLogContext();
    context.requestId = requestId;
    va_list args;
    va_start(args, fmt);
    logV(source, context, true, fmt, args);
    va_end(args);
}

} // namespace sl
