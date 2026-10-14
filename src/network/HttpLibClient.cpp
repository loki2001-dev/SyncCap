#include "network/HttpLibClient.h"

#include <httplib.h>

namespace {

constexpr double NANOS_PER_MILLI = 1e6;
constexpr int HTTP_SUCCESS_MIN = 200;
constexpr int HTTP_SUCCESS_MAX = 299;

}  // namespace

HttpLibClient::HttpLibClient(bool keepAlive)
    : _keepAlive(keepAlive) {
}

HttpLibClient::~HttpLibClient() = default;

bool HttpLibClient::parseUrl(const std::string& url, parsed_url& out, std::string& error) {
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) {
        error = url.rfind("https://", 0) == 0 ? "https is not enabled in this build" : "url must start with http://";
        return false;
    }
    const size_t hostBegin = scheme.size();
    const size_t pathBegin = url.find('/', hostBegin);
    const std::string hostPort =
        url.substr(hostBegin, pathBegin == std::string::npos ? std::string::npos : pathBegin - hostBegin);
    if (hostPort.empty()) {
        error = "url has no host";
        return false;
    }
    out.schemeHostPort = scheme + hostPort;
    out.path = pathBegin == std::string::npos ? "/" : url.substr(pathBegin);
    return true;
}

template <typename Call>
http_result HttpLibClient::send(const std::string& url, std::chrono::milliseconds timeout, const Call& call) {
    http_result result;
    parsed_url target;
    if (!parseUrl(url, target, result.error)) {
        return result;
    }
    const auto begin = std::chrono::steady_clock::now();
    std::unique_ptr<httplib::Client> oneShot;
    httplib::Client* client = nullptr;
    if (_keepAlive) {
        if (!_connection || _connectedTo != target.schemeHostPort) {
            _connection = std::make_unique<httplib::Client>(target.schemeHostPort);
            _connection->set_keep_alive(true);
            _connectedTo = target.schemeHostPort;
        }
        client = _connection.get();
    } else {
        oneShot = std::make_unique<httplib::Client>(target.schemeHostPort);
        oneShot->set_keep_alive(false);
        client = oneShot.get();
    }
    client->set_connection_timeout(timeout);
    client->set_read_timeout(timeout);
    client->set_write_timeout(timeout);
    const httplib::Result response = call(*client, target.path);
    result.elapsedMs =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count()) /
        NANOS_PER_MILLI;
    if (!response) {
        result.error = httplib::to_string(response.error());
        // a broken kept connection must not poison the next request
        _connection.reset();
        return result;
    }
    result.status = response->status;
    result.body = response->body;
    result.ok = response->status >= HTTP_SUCCESS_MIN && response->status <= HTTP_SUCCESS_MAX;
    if (!result.ok) {
        result.error = "http status " + std::to_string(response->status);
    }
    return result;
}

http_result HttpLibClient::post(const std::string& url, const std::string& body, const std::string& contentType,
                                std::chrono::milliseconds timeout) {
    return send(url, timeout, [&body, &contentType](httplib::Client& client, const std::string& path) {
        return client.Post(path, body, contentType);
    });
}

http_result HttpLibClient::get(const std::string& url, std::chrono::milliseconds timeout) {
    return send(url, timeout, [](httplib::Client& client, const std::string& path) {
        return client.Get(path);
    });
}
