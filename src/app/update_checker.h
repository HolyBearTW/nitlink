#pragma once

#include "holybear_version.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace NitLink::HolyBear {
inline constexpr size_t MaxResponseBytes = 256 * 1024;
bool AutoCheckDue(bool enabled, bool busy, uint64_t nowUtc, uint64_t lastCheckUtc);
enum class UpdateStatus { Never, Checking, Current, Available, Offline, RateLimited, NotFound,
    ServerUnavailable, ApiUnavailable, Invalid, Cancelled };
const wchar_t* StatusKey(UpdateStatus status);
struct HttpReply {
    uint32_t status = 0, error = 0;
    std::string body;
    std::optional<uint32_t> rateLimitRemaining;
};
std::optional<uint32_t> ParseRateLimitRemaining(std::string_view value);
struct UpdateResult { UpdateStatus status = UpdateStatus::Invalid; std::wstring tag; };
UpdateResult ClassifyReply(const HttpReply& reply, std::wstring_view current = Version);

// Worker owns all HTTP/JSON work. UI/render thread only reads a completion flag
// and consumes a bounded result; it never waits for a network operation.
class UpdateChecker {
public:
    using Transport = std::function<HttpReply(std::stop_token)>;
    explicit UpdateChecker(Transport transport = {});
    ~UpdateChecker();
    bool Start();
    void Cancel();
    bool Busy() const { return m_busy.load(); }
    std::optional<UpdateResult> TakeResult();
private:
    Transport m_transport;
    std::jthread m_worker;
    std::atomic<bool> m_busy{false}, m_ready{false};
    std::mutex m_mutex;
    UpdateResult m_result;
};
} // namespace NitLink::HolyBear
