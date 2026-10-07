#pragma once

#ifdef _WIN32
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace cryget {
struct HttpResult {
    long status = 0;
    uint64_t content_length = 0;
    uint64_t total_length = 0;
    uint64_t received = 0;
};

HttpResult winhttp_get(const std::string& url, const std::atomic<bool>* canceled,
                      const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes);
HttpResult winhttp_get_range(const std::string& url, uint64_t first, uint64_t last, const std::atomic<bool>* canceled,
                            const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes);
HttpResult winhttp_post_json(const std::string& url, const std::string& body,
                            const std::atomic<bool>* canceled,
                            const std::function<void(const char*, size_t, uint64_t, uint64_t)>& on_bytes);
} // namespace cryget
#endif
