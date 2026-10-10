#include "update_checker.h"
#include <windows.h>
#include <winhttp.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <chrono>

namespace NitLink::HolyBear {
bool AutoCheckDue(bool enabled, bool busy, uint64_t now, uint64_t last) {
    return enabled && !busy && now && (!last || (now >= last && now - last >= 86400));
}
const wchar_t* StatusKey(UpdateStatus status) {
    switch (status) {
    case UpdateStatus::Checking: return L"checking";
    case UpdateStatus::Current: return L"current";
    case UpdateStatus::Available: return L"available";
    case UpdateStatus::Offline: return L"offline";
    case UpdateStatus::ApiUnavailable: return L"apiUnavailable";
    case UpdateStatus::Invalid: return L"invalid";
    default: return L"never";
    }
}
UpdateResult ClassifyReply(const HttpReply& reply, std::wstring_view current) {
    if (reply.error == ERROR_WINHTTP_OPERATION_CANCELLED) return {UpdateStatus::Cancelled, {}};
    if (reply.error || !reply.status) return {UpdateStatus::Offline, {}};
    if (reply.status != 200) return {UpdateStatus::ApiUnavailable, {}};
    if (reply.body.empty() || reply.body.size() > MaxResponseBytes || !ParseVersion(current)) return {};
    // Use Windows' JSON implementation, accepting full GitHub objects/arrays but
    // reading only strictly typed release identity fields. No response HTML is used.
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
    struct Apartment {
        bool close;
        ~Apartment() {
            if (close) {
                // This executable has no WinRT DllCanUnloadNow hook. Cached
                // factories must be released before COM can unload their DLLs,
                // including between two checks on short-lived worker threads.
                winrt::clear_factory_cache();
                RoUninitialize();
            }
        }
    } apartment{SUCCEEDED(initialized)};
    try {
        using namespace winrt::Windows::Data::Json;
        JsonObject json;
        if (!JsonObject::TryParse(winrt::to_hstring(reply.body), json)) return {};
        for (const auto key : {L"draft", L"prerelease"}) {
            const auto field = json.GetNamedValue(key);
            if (field.ValueType() != JsonValueType::Boolean || field.GetBoolean()) return {};
        }
        if (json.GetNamedValue(L"tag_name").ValueType() != JsonValueType::String ||
            json.GetNamedValue(L"html_url").ValueType() != JsonValueType::String) return {};
        const std::wstring tag(json.GetNamedString(L"tag_name"));
        const std::wstring url(json.GetNamedString(L"html_url"));
        const auto version = ParseVersion(tag);
        if (!version || !tag.starts_with(L"v") || url != std::wstring(ReleasePrefix) + tag) return {};
        return {*version > *ParseVersion(current) ? UpdateStatus::Available : UpdateStatus::Current, tag};
    } catch (...) { return {}; }
}

namespace {
using Clock = std::chrono::steady_clock;
struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
struct Progress {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = 0, bytes = 0;
    ~Progress() { if (event) CloseHandle(event); if (closed) CloseHandle(closed); }
};
void CALLBACK HttpCallback(HINTERNET, DWORD_PTR context, DWORD status, void* data, DWORD length) {
    auto* progress = reinterpret_cast<Progress*>(context);
    if (!progress) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: SetEvent(progress->closed); return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        progress->error = static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError; break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: progress->bytes = length; break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: break;
    default: return;
    }
    SetEvent(progress->event);
}
// Close only asynchronous handles. HANDLE_CLOSING is the final callback; wait
// for it on the worker before destroying its callback context (never on Present).
struct AsyncRequest {
    HINTERNET value = nullptr;
    Progress& progress;
    bool callbackReady = false;
    ~AsyncRequest() {
        if (value) {
            WinHttpCloseHandle(value);
            if (callbackReady) WaitForSingleObject(progress.closed, INFINITE);
        }
    }
};
HttpReply FetchRelease(std::stop_token stop) {
    HttpReply reply;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    Progress progress;
    if (!progress.event || !progress.closed) { reply.error = ERROR_NOT_ENOUGH_MEMORY; return reply; }
    HttpHandle session{WinHttpOpen(L"NitLink-HolyBear-UpdateCheck/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session.value) { reply.error = GetLastError(); return reply; }
    if (!WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 5000)) { reply.error = GetLastError(); return reply; }
    HttpHandle connection{WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) { reply.error = GetLastError(); return reply; }
    AsyncRequest request{WinHttpOpenRequest(connection.value, L"GET",
        L"/repos/HolyBearTW/nitlink/releases/latest", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), progress};
    if (!request.value) { reply.error = GetLastError(); return reply; }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&progress);
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        reply.error = GetLastError(); return reply;
    }
    if (WinHttpSetStatusCallback(request.value, HttpCallback,
            WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
            WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
            WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        reply.error = GetLastError(); return reply;
    }
    request.callbackReady = true;
    auto await = [&]() {
        while (!stop.stop_requested() && Clock::now() < deadline) {
            if (WaitForSingleObject(progress.event, 50) == WAIT_OBJECT_0) {
                reply.error = progress.error; return !reply.error;
            }
        }
        reply.error = stop.stop_requested() ? ERROR_WINHTTP_OPERATION_CANCELLED : ERROR_WINHTTP_TIMEOUT;
        return false;
    };
    if (stop.stop_requested()) { reply.error = ERROR_WINHTTP_OPERATION_CANCELLED; return reply; }
    if (!WinHttpSendRequest(request.value, L"Accept: application/vnd.github+json\r\n", -1,
            WINHTTP_NO_REQUEST_DATA, 0, 0, context) || !await() ||
        !WinHttpReceiveResponse(request.value, nullptr) || !await()) {
        if (!reply.error) reply.error = GetLastError(); return reply;
    }
    DWORD size = sizeof(reply.status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &reply.status, &size, WINHTTP_NO_HEADER_INDEX)) {
        reply.error = GetLastError(); return reply;
    }
    if (reply.status != 200) return reply;
    char buffer[4096];
    while (!stop.stop_requested()) {
        progress.bytes = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), nullptr) || !await()) {
            if (!reply.error) reply.error = GetLastError(); return reply;
        }
        if (!progress.bytes) return reply;
        if (progress.bytes > MaxResponseBytes - reply.body.size()) {
            // Oversized responses are malformed, not successful releases.
            reply.body.clear(); return reply;
        }
        reply.body.append(buffer, progress.bytes);
    }
    reply.error = ERROR_WINHTTP_OPERATION_CANCELLED;
    return reply;
}
} // namespace

UpdateChecker::UpdateChecker(Transport transport) : m_transport(transport ? std::move(transport) : FetchRelease) {}
UpdateChecker::~UpdateChecker() { Cancel(); if (m_worker.joinable()) m_worker.join(); }
bool UpdateChecker::Start() {
    if (m_busy.exchange(true)) return false;
    if (m_worker.joinable()) m_worker.join(); // Previous worker is already finished.
    m_ready = false;
    try {
        m_worker = std::jthread([this](std::stop_token stop) {
            UpdateResult result;
            try { result = ClassifyReply(m_transport(stop)); } catch (...) { result.status = UpdateStatus::Offline; }
            if (stop.stop_requested()) result = {UpdateStatus::Cancelled, {}};
            { std::lock_guard lock(m_mutex); m_result = std::move(result); }
            m_ready = true;
            m_busy = false;
        });
    } catch (...) { m_busy = false; return false; }
    return true;
}
void UpdateChecker::Cancel() { if (m_worker.joinable()) m_worker.request_stop(); }
std::optional<UpdateResult> UpdateChecker::TakeResult() {
    if (!m_ready.load(std::memory_order_relaxed) || !m_ready.exchange(false)) return {};
    std::lock_guard lock(m_mutex);
    return m_result;
}
} // namespace NitLink::HolyBear
