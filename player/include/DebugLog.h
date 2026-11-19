#pragma once

#include <cstdint>
#include <string>

namespace sl {

struct DebugLogContext {
    std::string run;
    std::string session;
    std::string content;
    std::string id;
    std::string bv;
    std::string cid;
    std::string qn;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
};

void setDebugLogContext(const DebugLogContext& context);
void clearDebugLogContext();
DebugLogContext currentDebugLogContext();
void d27Log(const char* source, const char* fmt, ...);
void d27LogReq(const char* source, std::uint64_t requestId, const char* fmt, ...);
void d27LogContext(const char* source, const DebugLogContext& context,
                   const char* fmt, ...);
void d27LogIdentity(const char* source, const char* fmt, ...);
void d27LogIdentityReq(const char* source, std::uint64_t requestId,
                       const char* fmt, ...);
std::string d27Identity();

} // namespace sl
