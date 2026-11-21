#pragma once

#include <string>

namespace wiliwili {
namespace d27 {

std::string runId();
std::string currentSession();

std::string beginSession(const std::string& content, const std::string& id,
                         const std::string& detail, const std::string& bv = "",
                         const std::string& parent = "");
std::string reuseOrBeginSession(const std::string& content, const std::string& id,
                                const std::string& detail, const std::string& bv = "");
void beginControllerSession(const std::string& content, const std::string& id,
                            const std::string& detail, const std::string& bv = "");
void prepareControllerSession(const std::string& content, const std::string& id,
                              const std::string& detail, const std::string& bv = "");
void endSession(const std::string& reason);

void setPendingParent(const std::string& session);
std::string consumePendingParent();
void markContentStarted();

void log(const char* source, const std::string& message);

// 正常启动也写一行内容身份；run/session 为 "-" 时不伪造 debug session。
void logContentIdentity(const std::string& content, const std::string& id,
                        const std::string& detail, const std::string& bv,
                        const std::string& cid, const std::string& qn);

} // namespace d27
} // namespace wiliwili
