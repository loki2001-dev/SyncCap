#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "network/IHttpClient.h"

namespace httplib {
class Client;
}

struct parsed_url {
    std::string schemeHostPort;  // http://host:port
    std::string path;            // /status
};

// IHttpClient on cpp-httplib (header only, same library as the server)
class HttpLibClient : public IHttpClient {
public:
    // keepAlive: reuse one connection to the last host instead of connecting per request. a new TCP connection
    // costs up to ~20 ms on Windows, too much for a 30 fps poller. the instance is then single-threaded
    explicit HttpLibClient(bool keepAlive = false);
    ~HttpLibClient() override;

    HttpLibClient(const HttpLibClient&) = delete;
    HttpLibClient& operator=(const HttpLibClient&) = delete;

    http_result post(const std::string& url, const std::string& body, const std::string& contentType,
                     std::chrono::milliseconds timeout) override;
    http_result get(const std::string& url, std::chrono::milliseconds timeout) override;

    static bool parseUrl(const std::string& url, parsed_url& out, std::string& error);

private:
    template <typename Call>
    http_result send(const std::string& url, std::chrono::milliseconds timeout, const Call& call);

    const bool _keepAlive;
    std::string _connectedTo;                      // schemeHostPort of _connection
    std::unique_ptr<httplib::Client> _connection;  // keepAlive only
};
