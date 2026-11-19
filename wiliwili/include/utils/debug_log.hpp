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

} // namespace d27
} // namespace wiliwili
