#pragma once

// AI-CODE-BEGIN: S5-PROXY-SERVER-API
#include "edgegate/net/event_loop.h"
#include "edgegate/proxy/proxy_session.h"

#include <cstdint>
#include <memory>
#include <string>

namespace edgegate::proxy {

class ProxyServer {
public:
    ProxyServer(
        std::string bind_address,
        std::uint16_t listen_port,
        ProxyConfig config);

    int run_once(int timeout_ms);
    void run();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::shared_ptr<ProxyStats>& stats() const noexcept;

private:
    edgegate::net::EventLoop loop_;
    std::shared_ptr<ProxyStats> stats_;
    std::uint16_t port_{0};
};

} // namespace edgegate::proxy
// AI-CODE-END: S5-PROXY-SERVER-API
