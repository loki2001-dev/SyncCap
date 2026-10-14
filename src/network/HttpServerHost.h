#pragma once

#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

/**
 * @brief an HTTP server and the thread that runs it (transport, P2)
 *
 * bind() is synchronous so a port conflict is known at startup; listen() serves on the server's own
 * threads; stop() returns after the listener thread has ended
 */
class HttpServerHost {
public:
    HttpServerHost();
    ~HttpServerHost();

    HttpServerHost(const HttpServerHost&) = delete;
    HttpServerHost& operator=(const HttpServerHost&) = delete;

    // routes are registered here before bind()
    httplib::Server& get_server();
    // port 0 picks a free port. returns the bound port, 0 on failure
    int bind(const std::string& host, int port);
    void listen();
    void stop();

private:
    std::unique_ptr<httplib::Server> _server;
    std::thread _thread;
};
