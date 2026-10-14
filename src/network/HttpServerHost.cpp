#include "network/HttpServerHost.h"

#include <httplib.h>

HttpServerHost::HttpServerHost()
    : _server(std::make_unique<httplib::Server>()) {
}

HttpServerHost::~HttpServerHost() {
    stop();
}

httplib::Server& HttpServerHost::get_server() {
    return *_server;
}

int HttpServerHost::bind(const std::string& host, int port) {
    if (port == 0) {
        const int bound = _server->bind_to_any_port(host);
        return bound > 0 ? bound : 0;
    }
    return _server->bind_to_port(host, port) ? port : 0;
}

void HttpServerHost::listen() {
    _thread = std::thread([this]() {
        _server->listen_after_bind();
    });
}

void HttpServerHost::stop() {
    _server->stop();
    if (_thread.joinable()) {
        _thread.join();
    }
}
