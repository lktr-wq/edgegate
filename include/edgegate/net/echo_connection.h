#pragma once

// AI-CODE-BEGIN: S4-ECHO-CONNECTION-API
#include "edgegate/net/byte_buffer.h"
#include "edgegate/net/event_loop.h"
#include "edgegate/net/unique_fd.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace edgegate::net {

struct ReactorStats {
    std::atomic<std::uint64_t> accepted_connections{0};
    std::atomic<std::uint64_t> active_connections{0};
    std::atomic<std::uint64_t> closed_connections{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> read_would_block{0};
    std::atomic<std::uint64_t> write_would_block{0};
    std::atomic<std::uint64_t> read_pauses{0};
    std::atomic<std::uint64_t> socket_errors{0};
    std::atomic<std::uint64_t> registration_errors{0};
};

struct BufferWatermarks {
    std::size_t maximum{256 * 1024};
    std::size_t high{192 * 1024};
    std::size_t low{64 * 1024};
};

/*
 * 阶段4的练习连接：收到什么字节就原样回送什么字节。
 *
 * Echo 不是最终反向代理协议，只用于单独验证非阻塞读写、EAGAIN、
 * 部分写、背压和连接回收；阶段5会复用同一事件循环组织代理状态机。
 */
class EchoConnection final : public EventHandler {
public:
    EchoConnection(
        UniqueFd socket,
        std::shared_ptr<ReactorStats> stats,
        BufferWatermarks watermarks = {});
    ~EchoConnection() override;

    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::uint32_t interests() const noexcept override;
    void on_event(EventLoop& loop, std::uint32_t events) noexcept override;

private:
    void read_from_peer() noexcept;
    void write_to_peer() noexcept;
    void refresh_backpressure_state() noexcept;

    UniqueFd socket_;
    std::shared_ptr<ReactorStats> stats_;
    BufferWatermarks watermarks_;
    ByteBuffer input_;
    ByteBuffer output_;
    bool peer_closed_{false};
    bool fatal_error_{false};
    bool read_paused_{false};
};

} // namespace edgegate::net
// AI-CODE-END: S4-ECHO-CONNECTION-API
