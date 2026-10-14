#pragma once

#include <chrono>
#include <string>

#include "network/IHttpClient.h"

struct parsed_url {
    std::string schemeHostPort;  // http://host:port
    std::string path;            // /status
};

// IHttpClient on cpp-httplib (header only, same library as the server)
class HttpLibClient : public IHttpClient {
public:
    http_result post(const std::string& url, const std::string& body, const std::string& contentType,
                     std::chrono::milliseconds timeout) override;
    http_result get(const std::string& url, std::chrono::milliseconds timeout) override;

    static bool parseUrl(const std::string& url, parsed_url& out, std::string& error);

private:
    template <typename Call>
    http_result send(const std::string& url, std::chrono::milliseconds timeout, const Call& call);
};
