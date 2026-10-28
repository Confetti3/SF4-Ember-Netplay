#pragma once

// A TCP server on 127.0.0.1 for the tests that need a real peer for WinHTTP.
// It takes one connection at a time and passes each to `serve`, which reads
// and answers as the test needs; the connection closes when `serve` returns.
// Include before <windows.h> and call WSAStartup before the first server.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

#include "test_support.hxx"

namespace sf4e_test {
class LoopbackServer {
public:
    explicit LoopbackServer(std::function<void(SOCKET)> serve) : serve_(std::move(serve)) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(listener_ != INVALID_SOCKET && bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        int length = sizeof(address);
        CHECK(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0 && listen(listener_, 4) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Accept(); });
    }
    ~LoopbackServer() {
        closesocket(listener_);
        thread_.join();
    }
    int Port() const { return port_; }
    int Connections() const { return connections_.load(); }
private:
    void Accept() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            ++connections_;
            // A test that goes wrong ends in a failed check, not a hang.
            DWORD timeout = 10000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            serve_(client);
            closesocket(client);
        }
    }
    std::function<void(SOCKET)> serve_;
    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<int> connections_{0};
    std::thread thread_;
};
}
