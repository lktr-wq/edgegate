#pragma once

// AI-CODE-BEGIN: S5-PROXY-SESSION-API
#include "edgegate/http/request_parser.h"
#include "edgegate/http/response_parser.h"
#include "edgegate/net/byte_buffer.h"
#include "edgegate/net/event_loop.h"
#include "edgegate/net/unique_fd.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace edgegate::proxy {

enum class EndpointRole {
    kClient,
    kUpstream
};

enum class ProxyState {
    kReadingRequest,
    kConnectingUpstream,
    kSendingRequest,
    kReadingResponse,
    kSendingResponse,
    kClosed
};

struct ProxyConfig {
    std::string upstream_address{"127.0.0.1"};
    std::uint16_t upstream_port{19080};
    std::size_t max_header_size{8192};
    std::size_t max_request_body_size{1024 * 1024};
    std::size_t max_response_body_size{8 * 1024 * 1024};
};

struct ProxyStats {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> completed_requests{0};
    std::atomic<std::uint64_t> client_errors{0};
    std::atomic<std::uint64_t> upstream_errors{0};
    std::atomic<std::uint64_t> active_sessions{0};
};

class ProxySession;

class ProxyEndpoint final : public edgegate::net::EventHandler {
public:
    ProxyEndpoint(
        edgegate::net::UniqueFd socket,
        EndpointRole role,
        std::shared_ptr<ProxySession> session);

    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::uint32_t interests() const noexcept override;
    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override;

private:
    edgegate::net::UniqueFd socket_;
    EndpointRole role_;
    std::shared_ptr<ProxySession> session_;
};

class ProxySession final : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(
        int client_fd,
        ProxyConfig config,
        std::shared_ptr<ProxyStats> stats);
    ~ProxySession();

    [[nodiscard]] std::uint32_t interests(EndpointRole role) const noexcept;
    void on_event(
        edgegate::net::EventLoop& loop,
        EndpointRole role,
        std::uint32_t events) noexcept;

    [[nodiscard]] ProxyState state() const noexcept;

private:
    void read_client(edgegate::net::EventLoop& loop) noexcept;
    void connect_upstream(edgegate::net::EventLoop& loop) noexcept;
    void finish_upstream_connect(edgegate::net::EventLoop& loop) noexcept;
    void write_upstream(edgegate::net::EventLoop& loop) noexcept;
    void read_upstream(edgegate::net::EventLoop& loop) noexcept;
    void write_client(edgegate::net::EventLoop& loop) noexcept;

    void finish_response(edgegate::net::EventLoop& loop) noexcept;
    void queue_error_response(
        edgegate::net::EventLoop& loop,
        int status,
        const char* reason) noexcept;
    void close_upstream(edgegate::net::EventLoop& loop) noexcept;
    void close_session(edgegate::net::EventLoop& loop) noexcept;
    void refresh_interests(edgegate::net::EventLoop& loop) noexcept;

    [[nodiscard]] bool request_wants_close() const noexcept;
    [[nodiscard]] bool response_requires_close() const noexcept;

    int client_fd_;
    std::optional<int> upstream_fd_;
    ProxyConfig config_;
    std::shared_ptr<ProxyStats> stats_;
    ProxyState state_{ProxyState::kReadingRequest};

    edgegate::http::RequestParser request_parser_;
    std::optional<edgegate::http::ResponseParser> response_parser_;
    edgegate::net::ByteBuffer to_upstream_;
    edgegate::net::ByteBuffer to_client_;
    bool close_after_response_{false};
    bool client_read_closed_{false};
};

} // namespace edgegate::proxy
// AI-CODE-END: S5-PROXY-SESSION-API
