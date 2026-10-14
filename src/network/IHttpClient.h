#pragma once

#include <chrono>
#include <string>

struct http_result {
    bool ok = false;  // transport succeeded and status is 2xx
    int status = 0;
    std::string error;
    std::string body;
    double elapsedMs = 0.0;
};

/**
 * @brief blocking HTTP client. callers run it on a dedicated thread (P2)
 *
 * every call has a timeout covering connect, write and read
 */
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    virtual http_result post(const std::string& url, const std::string& body, const std::string& contentType,
                             std::chrono::milliseconds timeout) = 0;
    virtual http_result get(const std::string& url, std::chrono::milliseconds timeout) = 0;
};
