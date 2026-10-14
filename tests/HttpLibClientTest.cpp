#include "catch_amalgamated.hpp"

#include <chrono>
#include <string>

#include <httplib.h>

#include "network/HttpLibClient.h"
#include "network/HttpServerHost.h"

namespace {

constexpr std::chrono::milliseconds TIMEOUT{2000};

// answers with the client's source port, so a test can tell whether two requests shared a connection
class PortEchoServer {
public:
    explicit PortEchoServer(int keepAliveMaxCount = 100) {
        _host.get_server().set_keep_alive_max_count(static_cast<size_t>(keepAliveMaxCount));
        _host.get_server().Get("/port", [](const httplib::Request& request, httplib::Response& response) {
            response.set_content(std::to_string(request.remote_port), "text/plain");
        });
        _port = _host.bind("127.0.0.1", 0);
        _host.listen();
    }

    ~PortEchoServer() {
        _host.stop();
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(_port) + "/port";
    }

    int port() const {
        return _port;
    }

private:
    HttpServerHost _host;
    int _port = 0;
};

}  // namespace

TEST_CASE("HttpLibClient with keep-alive reuses one connection") {
    PortEchoServer server;
    REQUIRE(server.port() > 0);
    HttpLibClient client(true);
    const http_result first = client.get(server.url(), TIMEOUT);
    const http_result second = client.get(server.url(), TIMEOUT);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    REQUIRE(first.body == second.body);
}

TEST_CASE("HttpLibClient without keep-alive connects per request") {
    PortEchoServer server;
    REQUIRE(server.port() > 0);
    HttpLibClient client;
    const http_result first = client.get(server.url(), TIMEOUT);
    const http_result second = client.get(server.url(), TIMEOUT);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    REQUIRE(first.body != second.body);
}

TEST_CASE("HttpLibClient with keep-alive reconnects when the server closes the connection") {
    PortEchoServer server(1);
    REQUIRE(server.port() > 0);
    HttpLibClient client(true);
    for (int request = 0; request < 3; ++request) {
        REQUIRE(client.get(server.url(), TIMEOUT).ok);
    }
}
