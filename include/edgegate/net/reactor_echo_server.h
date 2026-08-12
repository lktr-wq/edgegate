#pragma once

// AI-CODE-BEGIN: S4-REACTOR-SERVER-API
#include "edgegate/net/echo_connection.h"
#include "edgegate/net/event_loop.h"

#include <cstdint>
#include <memory>
#include <string>

namespace edgegate::net {

/*
 * 阶段4的可运行验收服务器。端口传 0 时由内核选择空闲端口，便于测试。
 */
class ReactorEchoServer {
public:
    ReactorEchoServer(
        std::string bind_address,
        std::uint16_t port,
        BufferWatermarks watermarks = {});

    int run_once(int timeout_ms);
    void run();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::shared_ptr<ReactorStats>& stats() const noexcept;
    [[nodiscard]] std::size_t handler_count() const noexcept;

private:
    EventLoop loop_;
    std::shared_ptr<ReactorStats> stats_;
    std::uint16_t port_{0};
};

} // namespace edgegate::net
// AI-CODE-END: S4-REACTOR-SERVER-API
