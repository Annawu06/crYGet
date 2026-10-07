#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include "winhttp_download.hpp"
#include <array>
#include <chrono>
#include <stdexcept>

namespace cryget {
namespace {
struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

std::wstring wide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid UTF-8 URL");
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length))
        throw std::runtime_error("Cannot decode URL");
    return result;
}

void check_cancel(const std::atomic<bool>* canceled) {
    if (canceled && canceled->load()) throw std::runtime_error("Download canceled");
}

[[noreturn]] void fail(const char* stage) {
    throw std::runtime_error(std::string(stage) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
} // namespace

HttpResult winhttp_request(const std::string& url, const std::string* body, bool range,
                           uint64_t range_first, uint64_t range_last, const std::atomic<bool>* canceled,
                           const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes) {
    check_cancel(canceled);
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("The media address is missing or is not HTTPS");
    const auto address = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts)) fail("Cannot parse HTTPS URL");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength) throw std::runtime_error("Invalid HTTPS URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HttpHandle session(WinHttpOpen(L"crYGet/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) fail("Cannot initialize Windows HTTP");
    if (!WinHttpSetTimeouts(session.value, 10000, 10000, 10000, 30000)) fail("Cannot set HTTP timeouts");
    DWORD redirects = 5;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects)))
        fail("Cannot limit HTTP redirects");
    HttpHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    if (!connection.value) fail("Cannot connect to media server");
    HttpHandle request(WinHttpOpenRequest(connection.value, body ? L"POST" : L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) fail("Cannot create HTTP request");
    std::wstring headers = body ? L"Accept-Encoding: identity\r\nContent-Type: application/json\r\n" :
                                  L"Accept-Encoding: identity\r\n";
    if (range) headers += L"Range: bytes=" + std::to_wstring(range_first) + L"-" +
                          std::to_wstring(range_last) + L"\r\n";
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1),
                            body ? const_cast<char*>(body->data()) : WINHTTP_NO_REQUEST_DATA,
                            body ? static_cast<DWORD>(body->size()) : 0,
                            body ? static_cast<DWORD>(body->size()) : 0, 0)) fail("Cannot send HTTP request");
    check_cancel(canceled);
    if (!WinHttpReceiveResponse(request.value, nullptr)) fail("Cannot receive HTTP response");
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        fail("Cannot read HTTP status");
    if (status != 200 && !(range && status == 206))
        throw std::runtime_error("HTTP server returned " + std::to_string(status));
    if (range && status == 200 && range_first != 0)
        throw std::runtime_error("Media server ignored a later byte range");

    HttpResult result;
    result.status = status;
    wchar_t length_text[32]{};
    DWORD length_size = sizeof(length_text);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            length_text, &length_size, WINHTTP_NO_HEADER_INDEX)) {
        try { result.content_length = std::stoull(length_text); } catch (const std::exception&) {}
    }
    if (range && status == 206) {
        wchar_t range_text[128]{};
        DWORD range_size = sizeof(range_text);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
                                 range_text, &range_size, WINHTTP_NO_HEADER_INDEX))
            throw std::runtime_error("Media response is missing Content-Range");
        const std::wstring value(range_text);
        const auto dash = value.find(L'-');
        const auto slash = value.find(L'/');
        try {
            if (value.rfind(L"bytes ", 0) != 0 || dash == std::wstring::npos || slash == std::wstring::npos)
                throw std::runtime_error("Invalid Content-Range");
            const auto first = std::stoull(value.substr(6, dash - 6));
            const auto last = std::stoull(value.substr(dash + 1, slash - dash - 1));
            result.total_length = std::stoull(value.substr(slash + 1));
            if (first != range_first || last < first || last > range_last || result.total_length <= last)
                throw std::runtime_error("Unexpected media byte range");
            result.content_length = last - first + 1;
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid media Content-Range");
        }
    }
    std::array<char, 64 * 1024> buffer{};
    auto speed_start = std::chrono::steady_clock::now();
    uint64_t speed_bytes = 0;
    for (;;) {
        check_cancel(canceled);
        DWORD count = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count))
            fail("Cannot read HTTP response");
        if (!count) break;
        result.received += count;
        on_bytes(buffer.data(), count, result.received, result.content_length);
        const auto now = std::chrono::steady_clock::now();
        if (now - speed_start >= std::chrono::seconds(30)) {
            if (result.received - speed_bytes < 30 * 1024) throw std::runtime_error("HTTP transfer is too slow");
            speed_start = now;
            speed_bytes = result.received;
        }
    }
    check_cancel(canceled);
    if (range && status == 206 && result.received != result.content_length)
        throw std::runtime_error("Incomplete media byte range");
    return result;
}

HttpResult winhttp_get(const std::string& url, const std::atomic<bool>* canceled,
                      const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes) {
    return winhttp_request(url, nullptr, false, 0, 0, canceled, on_bytes);
}

HttpResult winhttp_get_range(const std::string& url, uint64_t first, uint64_t last, const std::atomic<bool>* canceled,
                            const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes) {
    return winhttp_request(url, nullptr, true, first, last, canceled, on_bytes);
}

HttpResult winhttp_post_json(const std::string& url, const std::string& body,
                            const std::atomic<bool>* canceled,
                            const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes) {
    return winhttp_request(url, &body, false, 0, 0, canceled, on_bytes);
}
} // namespace cryget
#endif
