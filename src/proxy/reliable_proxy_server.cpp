#include "edgegate/proxy/reliable_proxy_server.h"

// AI-CODE-BEGIN: S7-RELIABLE-PROXY-SERVER-IMPLEMENTATION
#include "edgegate/http/chunked_stream_decoder.h"
#include "edgegate/http/message_rewriter.h"
#include "edgegate/http/request_parser.h"
#include "edgegate/http/response_parser.h"
#include "edgegate/net/byte_buffer.h"
#include "edgegate/net/event_loop.h"
#include "edgegate/net/unique_fd.h"
#include "edgegate/routing/route_table.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace edgegate::proxy {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kIoChunkSize = 16 * 1024;
constexpr int kListenBacklog = 128;
constexpr auto kRuntimeTick = std::chrono::milliseconds(100);

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool header_contains_token(
    std::optional<std::string_view> value,
    std::string_view expected) noexcept
{
    if (!value.has_value()) {
        return false;
    }
    std::string_view remaining = *value;
    while (!remaining.empty()) {
        const std::size_t comma = remaining.find(',');
        std::string_view token = remaining.substr(0, comma);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (equals_ignore_case(token, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

edgegate::net::UniqueFd make_listener(
    const std::string& bind_address,
    std::uint16_t port,
    std::uint16_t& actual_port)
{
    edgegate::net::UniqueFd listener(::socket(
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener) {
        throw system_error_from_errno("socket listener");
    }
    int reuse = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse)) == -1) {
        throw system_error_from_errno("setsockopt SO_REUSEADDR");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("listen address must be numeric IPv4");
    }
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == -1) {
        throw system_error_from_errno("bind listener");
    }
    if (::listen(listener.get(), kListenBacklog) == -1) {
        throw system_error_from_errno("listen");
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound),
                      &length) == -1) {
        throw system_error_from_errno("getsockname");
    }
    actual_port = ntohs(bound.sin_port);
    return listener;
}

std::string peer_address(const sockaddr_in& peer)
{
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::inet_ntop(AF_INET, &peer.sin_addr, text.data(), text.size()) == nullptr) {
        return "0.0.0.0";
    }
    return text.data();
}

class ReliableSession;

struct HealthState {
    edgegate::routing::UpstreamEndpoint endpoint;
    std::uint32_t consecutive_failures{0};
    std::uint32_t consecutive_successes{0};
    Clock::time_point next_check{};
};

enum class ProbePhase { kConnecting, kSending, kReading };

struct HealthProbe {
    std::string key;
    edgegate::net::UniqueFd socket;
    ProbePhase phase{ProbePhase::kConnecting};
    std::string request;
    std::size_t sent{0};
    std::string response_head;
    Clock::time_point deadline{};
};

class ReliableRuntime final : public std::enable_shared_from_this<ReliableRuntime> {
public:
    ReliableRuntime(
        edgegate::config::EdgeGateConfig config,
        std::shared_ptr<ReliableProxyStats> stats)
        : config_(std::move(config)),
          stats_(std::move(stats)),
          routes_(std::make_shared<edgegate::routing::RouteTable>(config_.routes))
    {
        const auto now = Clock::now();
        for (const auto& endpoint : routes_->unique_endpoints()) {
            const std::string key = endpoint_key(endpoint.address, endpoint.port);
            // 服务启动后先等待一个完整 interval，再做第一次探测；避免健康探测
            // 与服务刚启动时的真实请求争抢尚未就绪的后端。
            health_.emplace(key, HealthState{
                endpoint, 0, 0,
                now + std::chrono::milliseconds(config_.health_check.interval_ms)});
        }
    }

    const edgegate::config::EdgeGateConfig& config() const noexcept
    {
        return config_;
    }

    const std::shared_ptr<ReliableProxyStats>& stats() const noexcept
    {
        return stats_;
    }

    const std::shared_ptr<edgegate::routing::RouteTable>& routes() const noexcept
    {
        return routes_;
    }

    void add_session(const std::shared_ptr<ReliableSession>& session)
    {
        sessions_.push_back(session);
    }

    void tick(edgegate::net::EventLoop& loop, Clock::time_point now) noexcept;

    void record_upstream_result(
        const edgegate::routing::UpstreamEndpoint& endpoint,
        bool success) noexcept
    {
        record_result(endpoint_key(endpoint.address, endpoint.port), success);
    }

private:
    static std::string endpoint_key(std::string_view address, std::uint16_t port)
    {
        return std::string(address) + ":" + std::to_string(port);
    }

    void start_probe(const std::string& key, Clock::time_point now) noexcept;
    void advance_probes(Clock::time_point now) noexcept;
    void record_result(const std::string& key, bool success) noexcept;

    edgegate::config::EdgeGateConfig config_;
    std::shared_ptr<ReliableProxyStats> stats_;
    std::shared_ptr<edgegate::routing::RouteTable> routes_;
    std::vector<std::weak_ptr<ReliableSession>> sessions_;
    std::unordered_map<std::string, HealthState> health_;
    std::vector<HealthProbe> probes_;
};

enum class StreamRole { kClient, kUpstream };
enum class StreamState {
    kReadingRequestHead,
    kConnectingUpstream,
    kSendingRequest,
    kReadingResponseHead,
    kStreamingResponse,
    kDrainingResponse,
    kClosed
};

class ReliableEndpoint final : public edgegate::net::EventHandler {
public:
    ReliableEndpoint(
        edgegate::net::UniqueFd socket,
        StreamRole role,
        std::shared_ptr<ReliableSession> session)
        : socket_(std::move(socket)), role_(role), session_(std::move(session))
    {
    }

    int fd() const noexcept override { return socket_.get(); }
    std::uint32_t interests() const noexcept override;
    void on_event(edgegate::net::EventLoop& loop,
                  std::uint32_t events) noexcept override;

private:
    edgegate::net::UniqueFd socket_;
    StreamRole role_;
    std::shared_ptr<ReliableSession> session_;
};

class ReliableSession final : public std::enable_shared_from_this<ReliableSession> {
public:
    ReliableSession(
        int client_fd,
        std::string client_address,
        std::shared_ptr<ReliableRuntime> runtime)
        : client_fd_(client_fd),
          client_address_(std::move(client_address)),
          runtime_(std::move(runtime)),
          to_upstream_(runtime_->config().stream_buffer.capacity),
          to_client_(runtime_->config().stream_buffer.capacity)
    {
        ++runtime_->stats()->active_sessions;
        const auto now = Clock::now();
        request_started_ = now;
        last_io_ = now;
        phase_deadline_ = now + std::chrono::milliseconds(
            runtime_->config().timeouts.client_header_ms);
        request_deadline_ = now + std::chrono::milliseconds(
            runtime_->config().timeouts.request_total_ms);
    }

    ~ReliableSession()
    {
        --runtime_->stats()->active_sessions;
    }

    std::uint32_t interests(StreamRole role) const noexcept;
    void on_event(edgegate::net::EventLoop& loop, StreamRole role,
                  std::uint32_t events) noexcept;
    void check_timeout(edgegate::net::EventLoop& loop,
                       Clock::time_point now) noexcept;

private:
    void read_client(edgegate::net::EventLoop& loop) noexcept;
    bool accept_request_bytes(edgegate::net::EventLoop& loop,
                              std::string_view bytes) noexcept;
    bool finish_request_head(edgegate::net::EventLoop& loop,
                             std::string_view extra_body) noexcept;
    void connect_upstream(edgegate::net::EventLoop& loop) noexcept;
    void finish_connect(edgegate::net::EventLoop& loop) noexcept;
    void write_upstream(edgegate::net::EventLoop& loop) noexcept;
    void read_upstream(edgegate::net::EventLoop& loop) noexcept;
    bool accept_response_bytes(edgegate::net::EventLoop& loop,
                               std::string_view bytes) noexcept;
    bool finish_response_head(edgegate::net::EventLoop& loop,
                              std::string_view extra_body) noexcept;
    bool append_response_body(edgegate::net::EventLoop& loop,
                              std::string_view bytes) noexcept;
    void write_client(edgegate::net::EventLoop& loop) noexcept;
    void complete_response(edgegate::net::EventLoop& loop) noexcept;
    void reset_for_keep_alive() noexcept;

    void upstream_failure(edgegate::net::EventLoop& loop,
                          int final_status, const char* reason,
                          bool timeout) noexcept;
    bool retry_upstream(edgegate::net::EventLoop& loop) noexcept;
    void queue_error(edgegate::net::EventLoop& loop,
                     int status, const char* reason) noexcept;
    void close_upstream(edgegate::net::EventLoop& loop) noexcept;
    void close_session(edgegate::net::EventLoop& loop) noexcept;
    void refresh_interests(edgegate::net::EventLoop& loop) noexcept;
    void update_request_backpressure() noexcept;
    void update_response_backpressure() noexcept;
    void note_io() noexcept { last_io_ = Clock::now(); }

    int client_fd_;
    std::optional<int> upstream_fd_;
    std::string client_address_;
    std::shared_ptr<ReliableRuntime> runtime_;
    StreamState state_{StreamState::kReadingRequestHead};

    std::string request_head_bytes_;
    std::optional<edgegate::http::RequestParser> request_parser_;
    std::string request_method_;
    std::string request_host_;
    std::string request_target_;
    std::string upstream_request_head_;
    std::size_t request_body_expected_{0};
    std::size_t request_body_received_{0};
    bool request_body_complete_{false};
    bool request_paused_{false};
    bool client_read_closed_{false};
    bool close_after_response_{false};
    bool completed_at_least_one_{false};

    std::string response_head_bytes_;
    std::optional<edgegate::http::ResponseParser> response_parser_;
    std::optional<edgegate::http::ChunkedStreamDecoder> chunked_decoder_;
    edgegate::http::ResponseBodyMode response_mode_{
        edgegate::http::ResponseBodyMode::kNoBody};
    std::size_t response_body_expected_{0};
    std::size_t response_body_received_{0};
    std::size_t client_response_bytes_sent_{0};
    bool response_paused_{false};
    bool response_complete_{false};

    edgegate::net::ByteBuffer to_upstream_;
    edgegate::net::ByteBuffer to_client_;
    std::optional<edgegate::routing::UpstreamEndpoint> selected_upstream_;
    std::vector<std::string> attempted_upstream_ids_;

    Clock::time_point request_started_{};
    Clock::time_point request_deadline_{};
    Clock::time_point phase_deadline_{};
    Clock::time_point last_io_{};
};

std::uint32_t ReliableEndpoint::interests() const noexcept
{
    return session_->interests(role_);
}

void ReliableEndpoint::on_event(
    edgegate::net::EventLoop& loop,
    std::uint32_t events) noexcept
{
    session_->on_event(loop, role_, events);
}

std::uint32_t ReliableSession::interests(StreamRole role) const noexcept
{
    if (state_ == StreamState::kClosed) {
        return 0;
    }
    if (role == StreamRole::kClient) {
        std::uint32_t events = EPOLLRDHUP;
        const bool needs_request_input =
            state_ == StreamState::kReadingRequestHead ||
            ((state_ == StreamState::kConnectingUpstream ||
              state_ == StreamState::kSendingRequest) &&
             !request_body_complete_);
        if (needs_request_input && !request_paused_ && !client_read_closed_) {
            events |= EPOLLIN;
        }
        if (!to_client_.empty()) {
            events |= EPOLLOUT;
        }
        return events;
    }

    std::uint32_t events = EPOLLRDHUP;
    if (state_ == StreamState::kConnectingUpstream) {
        events |= EPOLLOUT;
    } else {
        if (!to_upstream_.empty()) {
            events |= EPOLLOUT;
        }
        if ((state_ == StreamState::kReadingResponseHead ||
             state_ == StreamState::kStreamingResponse) &&
            !response_paused_) {
            events |= EPOLLIN;
        }
    }
    return events;
}

void ReliableSession::on_event(
    edgegate::net::EventLoop& loop,
    StreamRole role,
    std::uint32_t events) noexcept
{
    if (state_ == StreamState::kClosed) {
        return;
    }
    if (role == StreamRole::kClient) {
        if ((events & EPOLLERR) != 0U) {
            ++runtime_->stats()->client_errors;
            close_session(loop);
            return;
        }
        if ((events & EPOLLIN) != 0U) {
            read_client(loop);
        }
        if (state_ == StreamState::kClosed) {
            return;
        }
        if ((events & EPOLLRDHUP) != 0U) {
            client_read_closed_ = true;
            close_after_response_ = true;
            if (state_ == StreamState::kReadingRequestHead ||
                !request_body_complete_) {
                ++runtime_->stats()->client_errors;
                close_session(loop);
                return;
            }
        }
        if ((events & EPOLLHUP) != 0U) {
            close_session(loop);
            return;
        }
        if ((events & EPOLLOUT) != 0U && !to_client_.empty()) {
            write_client(loop);
        }
        refresh_interests(loop);
        return;
    }

    if (state_ == StreamState::kConnectingUpstream &&
        (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
        finish_connect(loop);
    } else if ((events & EPOLLERR) != 0U) {
        upstream_failure(loop, 502, "Bad Gateway", false);
    }
    if (state_ == StreamState::kClosed) {
        return;
    }
    if (state_ == StreamState::kSendingRequest &&
        (events & EPOLLOUT) != 0U) {
        write_upstream(loop);
    }
    if ((state_ == StreamState::kReadingResponseHead ||
         state_ == StreamState::kStreamingResponse) &&
        (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0U) {
        read_upstream(loop);
    }
    refresh_interests(loop);
}

void ReliableSession::read_client(edgegate::net::EventLoop& loop) noexcept
{
    std::array<char, kIoChunkSize> bytes{};
    for (;;) {
        std::size_t capacity = bytes.size();
        if (state_ != StreamState::kReadingRequestHead) {
            const std::size_t remaining =
                request_body_expected_ - request_body_received_;
            capacity = std::min({capacity, remaining,
                                 to_upstream_.writable_capacity()});
            if (capacity == 0) {
                update_request_backpressure();
                return;
            }
        }
        const ssize_t received = ::recv(client_fd_, bytes.data(), capacity, 0);
        if (received > 0) {
            note_io();
            if (!accept_request_bytes(
                    loop, std::string_view(bytes.data(),
                        static_cast<std::size_t>(received)))) {
                return;
            }
            if (request_body_complete_ || request_paused_) {
                return;
            }
            continue;
        }
        if (received == 0) {
            client_read_closed_ = true;
            if (!request_body_complete_) {
                ++runtime_->stats()->client_errors;
                close_session(loop);
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        ++runtime_->stats()->client_errors;
        close_session(loop);
        return;
    }
}

bool ReliableSession::accept_request_bytes(
    edgegate::net::EventLoop& loop,
    std::string_view bytes) noexcept
{
    if (state_ == StreamState::kReadingRequestHead) {
        request_head_bytes_.append(bytes.data(), bytes.size());
        const std::size_t end = request_head_bytes_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (request_head_bytes_.size() > runtime_->config().max_header_size) {
                ++runtime_->stats()->client_errors;
                queue_error(loop, 413, "Payload Too Large");
                return false;
            }
            return true;
        }
        const std::size_t header_bytes = end + 4;
        if (header_bytes > runtime_->config().max_header_size) {
            ++runtime_->stats()->client_errors;
            queue_error(loop, 413, "Payload Too Large");
            return false;
        }
        const std::string extra = request_head_bytes_.substr(header_bytes);
        request_head_bytes_.resize(header_bytes);
        return finish_request_head(loop, extra);
    }

    if (bytes.size() > request_body_expected_ - request_body_received_ ||
        !to_upstream_.append(bytes)) {
        ++runtime_->stats()->client_errors;
        queue_error(loop, 400, "Bad Request");
        return false;
    }
    request_body_received_ += bytes.size();
    request_body_complete_ = request_body_received_ == request_body_expected_;
    update_request_backpressure();
    if (request_body_complete_ && to_upstream_.empty()) {
        state_ = StreamState::kReadingResponseHead;
        phase_deadline_ = Clock::now() + std::chrono::milliseconds(
            runtime_->config().timeouts.upstream_header_ms);
    }
    return true;
}

bool ReliableSession::finish_request_head(
    edgegate::net::EventLoop& loop,
    std::string_view extra_body) noexcept
{
    request_parser_.emplace(runtime_->config().max_header_size,
                            runtime_->config().max_request_body_size);
    const auto result = request_parser_->consume(request_head_bytes_);
    if (result.status == edgegate::http::ParseStatus::kError ||
        request_parser_->header_bytes() == 0) {
        ++runtime_->stats()->client_errors;
        const bool too_large =
            result.error == edgegate::http::ParseError::kHeaderTooLarge ||
            result.error == edgegate::http::ParseError::kBodyTooLarge;
        queue_error(loop, too_large ? 413 : 400,
                    too_large ? "Payload Too Large" : "Bad Request");
        return false;
    }

    request_method_ = std::string(request_parser_->method());
    request_target_ = std::string(request_parser_->target());
    request_host_ = std::string(*request_parser_->header_value("Host"));
    request_body_expected_ = request_parser_->content_length();
    if (extra_body.size() > request_body_expected_) {
        ++runtime_->stats()->client_errors;
        queue_error(loop, 400, "Bad Request");
        return false;
    }
    request_body_received_ = extra_body.size();
    request_body_complete_ = request_body_received_ == request_body_expected_;
    close_after_response_ = header_contains_token(
        request_parser_->header_value("Connection"), "close");

    const auto route = runtime_->routes()->lookup(request_host_, request_target_);
    if (route.status == edgegate::routing::RouteLookupStatus::kNoRoute) {
        queue_error(loop, 404, "Not Found");
        return false;
    }
    if (route.status == edgegate::routing::RouteLookupStatus::kNoHealthyUpstream) {
        queue_error(loop, 503, "Service Unavailable");
        return false;
    }
    selected_upstream_ = *route.upstream;
    attempted_upstream_ids_.push_back(selected_upstream_->id);
    upstream_request_head_ =
        edgegate::http::rewrite_request_head_for_upstream(
            *request_parser_, client_address_);
    if (!to_upstream_.append(upstream_request_head_) ||
        (!extra_body.empty() && !to_upstream_.append(extra_body))) {
        ++runtime_->stats()->client_errors;
        queue_error(loop, 413, "Payload Too Large");
        return false;
    }
    update_request_backpressure();
    connect_upstream(loop);
    return state_ != StreamState::kClosed;
}

void ReliableSession::connect_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!selected_upstream_.has_value()) {
        queue_error(loop, 503, "Service Unavailable");
        return;
    }
    edgegate::net::UniqueFd socket(::socket(
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(selected_upstream_->port);
    if (::inet_pton(AF_INET, selected_upstream_->address.c_str(),
                    &address.sin_addr) != 1) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    const int result = ::connect(socket.get(),
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result == 0) {
        state_ = StreamState::kSendingRequest;
    } else if (errno == EINPROGRESS) {
        state_ = StreamState::kConnectingUpstream;
    } else {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }

    const int descriptor = socket.get();
    upstream_fd_ = descriptor;
    try {
        loop.add(std::make_unique<ReliableEndpoint>(
            std::move(socket), StreamRole::kUpstream, shared_from_this()));
    } catch (...) {
        upstream_fd_.reset();
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    phase_deadline_ = Clock::now() + std::chrono::milliseconds(
        runtime_->config().timeouts.upstream_connect_ms);
    if (state_ == StreamState::kSendingRequest) {
        write_upstream(loop);
    }
}

void ReliableSession::finish_connect(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(*upstream_fd_, SOL_SOCKET, SO_ERROR,
                     &socket_error, &length) == -1 || socket_error != 0) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    note_io();
    state_ = StreamState::kSendingRequest;
    write_upstream(loop);
}

void ReliableSession::write_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    while (!to_upstream_.empty()) {
        const std::string_view pending = to_upstream_.readable_view();
        const ssize_t sent = ::send(*upstream_fd_, pending.data(), pending.size(),
                                    MSG_NOSIGNAL);
        if (sent > 0) {
            note_io();
            static_cast<void>(to_upstream_.consume(
                static_cast<std::size_t>(sent)));
            update_request_backpressure();
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    if (request_body_complete_) {
        state_ = StreamState::kReadingResponseHead;
        phase_deadline_ = Clock::now() + std::chrono::milliseconds(
            runtime_->config().timeouts.upstream_header_ms);
    }
}

void ReliableSession::read_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
    std::array<char, kIoChunkSize> bytes{};
    for (;;) {
        std::size_t capacity = bytes.size();
        if (state_ == StreamState::kStreamingResponse) {
            capacity = std::min(capacity, to_client_.writable_capacity());
            if (response_mode_ == edgegate::http::ResponseBodyMode::kContentLength) {
                capacity = std::min(
                    capacity, response_body_expected_ - response_body_received_);
            }
            if (capacity == 0) {
                update_response_backpressure();
                return;
            }
        }
        const ssize_t received = ::recv(*upstream_fd_, bytes.data(), capacity, 0);
        if (received > 0) {
            note_io();
            if (!accept_response_bytes(
                    loop, std::string_view(bytes.data(),
                        static_cast<std::size_t>(received)))) {
                return;
            }
            // complete_response() 会移除本轮上游，并且可能立即把客户端恢复到
            // Keep-Alive 读下一请求。此时绝不能在旧 fd 上再 recv() 一次，
            // 否则旧上游的正常 EOF 会被误算成下一轮的 502。
            if (!upstream_fd_.has_value()) {
                return;
            }
            if (state_ == StreamState::kDrainingResponse || response_paused_) {
                return;
            }
            continue;
        }
        if (received == 0) {
            if (state_ == StreamState::kStreamingResponse &&
                response_mode_ ==
                    edgegate::http::ResponseBodyMode::kCloseDelimited) {
                complete_response(loop);
            } else if (state_ != StreamState::kDrainingResponse) {
                upstream_failure(loop, 502, "Bad Gateway", false);
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        upstream_failure(loop, 502, "Bad Gateway", false);
        return;
    }
}

bool ReliableSession::accept_response_bytes(
    edgegate::net::EventLoop& loop,
    std::string_view bytes) noexcept
{
    if (state_ == StreamState::kReadingResponseHead) {
        response_head_bytes_.append(bytes.data(), bytes.size());
        const std::size_t end = response_head_bytes_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (response_head_bytes_.size() > runtime_->config().max_header_size) {
                upstream_failure(loop, 502, "Bad Gateway", false);
                return false;
            }
            return true;
        }
        const std::size_t header_bytes = end + 4;
        if (header_bytes > runtime_->config().max_header_size) {
            upstream_failure(loop, 502, "Bad Gateway", false);
            return false;
        }
        const std::string extra = response_head_bytes_.substr(header_bytes);
        response_head_bytes_.resize(header_bytes);
        return finish_response_head(loop, extra);
    }
    return append_response_body(loop, bytes);
}

bool ReliableSession::finish_response_head(
    edgegate::net::EventLoop& loop,
    std::string_view extra_body) noexcept
{
    response_parser_.emplace(request_method_, runtime_->config().max_header_size,
                             runtime_->config().max_response_body_size);
    const auto result = response_parser_->consume(response_head_bytes_);
    if (result.status == edgegate::http::ParseStatus::kError ||
        response_parser_->header_bytes() == 0) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return false;
    }
    response_mode_ = response_parser_->body_mode();
    response_body_expected_ = response_mode_ ==
        edgegate::http::ResponseBodyMode::kContentLength
        ? response_parser_->content_length() : 0;
    if (response_mode_ == edgegate::http::ResponseBodyMode::kChunked) {
        chunked_decoder_.emplace(runtime_->config().max_response_body_size);
    }
    if (response_mode_ == edgegate::http::ResponseBodyMode::kCloseDelimited) {
        close_after_response_ = true;
    }

    const std::string rewritten =
        edgegate::http::rewrite_response_head_for_client(
            *response_parser_, close_after_response_ || client_read_closed_);
    if (!to_client_.append(rewritten)) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return false;
    }
    state_ = StreamState::kStreamingResponse;
    update_response_backpressure();

    if (response_mode_ == edgegate::http::ResponseBodyMode::kNoBody ||
        (response_mode_ == edgegate::http::ResponseBodyMode::kContentLength &&
         response_body_expected_ == 0)) {
        if (!extra_body.empty()) {
            upstream_failure(loop, 502, "Bad Gateway", false);
            return false;
        }
        complete_response(loop);
        return true;
    }
    return extra_body.empty() || append_response_body(loop, extra_body);
}

bool ReliableSession::append_response_body(
    edgegate::net::EventLoop& loop,
    std::string_view bytes) noexcept
{
    if (bytes.size() > to_client_.writable_capacity()) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return false;
    }
    if (response_mode_ == edgegate::http::ResponseBodyMode::kContentLength) {
        if (bytes.size() > response_body_expected_ - response_body_received_) {
            upstream_failure(loop, 502, "Bad Gateway", false);
            return false;
        }
        response_body_received_ += bytes.size();
    } else if (response_mode_ == edgegate::http::ResponseBodyMode::kChunked) {
        if (!chunked_decoder_.has_value() ||
            chunked_decoder_->consume(bytes) ==
                edgegate::http::ChunkedStreamStatus::kError) {
            upstream_failure(loop, 502, "Bad Gateway", false);
            return false;
        }
    } else {
        if (bytes.size() > runtime_->config().max_response_body_size ||
            response_body_received_ > runtime_->config().max_response_body_size -
                                      bytes.size()) {
            upstream_failure(loop, 502, "Bad Gateway", false);
            return false;
        }
        response_body_received_ += bytes.size();
    }

    if (!to_client_.append(bytes)) {
        upstream_failure(loop, 502, "Bad Gateway", false);
        return false;
    }
    update_response_backpressure();
    if (response_mode_ == edgegate::http::ResponseBodyMode::kContentLength &&
        response_body_received_ == response_body_expected_) {
        complete_response(loop);
    } else if (response_mode_ == edgegate::http::ResponseBodyMode::kChunked &&
               chunked_decoder_->status() ==
                   edgegate::http::ChunkedStreamStatus::kComplete) {
        complete_response(loop);
    }
    return true;
}

void ReliableSession::write_client(edgegate::net::EventLoop& loop) noexcept
{
    while (!to_client_.empty()) {
        const std::string_view pending = to_client_.readable_view();
        const ssize_t sent = ::send(client_fd_, pending.data(), pending.size(),
                                    MSG_NOSIGNAL);
        if (sent > 0) {
            note_io();
            client_response_bytes_sent_ += static_cast<std::size_t>(sent);
            static_cast<void>(to_client_.consume(static_cast<std::size_t>(sent)));
            update_response_backpressure();
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ++runtime_->stats()->client_errors;
        close_session(loop);
        return;
    }
    if (state_ == StreamState::kDrainingResponse) {
        if (close_after_response_ || client_read_closed_) {
            close_session(loop);
        } else {
            reset_for_keep_alive();
        }
    }
}

void ReliableSession::complete_response(edgegate::net::EventLoop& loop) noexcept
{
    if (response_complete_) {
        return;
    }
    response_complete_ = true;
    if (selected_upstream_.has_value()) {
        runtime_->record_upstream_result(*selected_upstream_, true);
    }
    ++runtime_->stats()->completed_requests;
    completed_at_least_one_ = true;
    close_upstream(loop);
    state_ = StreamState::kDrainingResponse;
    write_client(loop);
}

void ReliableSession::reset_for_keep_alive() noexcept
{
    state_ = StreamState::kReadingRequestHead;
    request_head_bytes_.clear();
    request_parser_.reset();
    request_method_.clear();
    request_host_.clear();
    request_target_.clear();
    upstream_request_head_.clear();
    request_body_expected_ = 0;
    request_body_received_ = 0;
    request_body_complete_ = false;
    request_paused_ = false;
    close_after_response_ = false;
    response_head_bytes_.clear();
    response_parser_.reset();
    chunked_decoder_.reset();
    response_body_expected_ = 0;
    response_body_received_ = 0;
    client_response_bytes_sent_ = 0;
    response_paused_ = false;
    response_complete_ = false;
    selected_upstream_.reset();
    attempted_upstream_ids_.clear();
    to_upstream_.clear();
    const auto now = Clock::now();
    request_started_ = now;
    request_deadline_ = now + std::chrono::milliseconds(
        runtime_->config().timeouts.request_total_ms);
    phase_deadline_ = now + std::chrono::milliseconds(
        runtime_->config().timeouts.keep_alive_idle_ms);
    last_io_ = now;
}

void ReliableSession::upstream_failure(
    edgegate::net::EventLoop& loop,
    int final_status,
    const char* reason,
    bool timeout) noexcept
{
    ++runtime_->stats()->upstream_errors;
    if (timeout) {
        ++runtime_->stats()->upstream_timeouts;
    }
    if (selected_upstream_.has_value()) {
        runtime_->record_upstream_result(*selected_upstream_, false);
    }
    close_upstream(loop);
    if (client_response_bytes_sent_ == 0 && retry_upstream(loop)) {
        return;
    }
    if (client_response_bytes_sent_ != 0) {
        close_session(loop);
        return;
    }
    queue_error(loop, final_status, reason);
}

bool ReliableSession::retry_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!request_body_complete_ || request_body_expected_ != 0 ||
        !(request_method_ == "GET" || request_method_ == "HEAD") ||
        attempted_upstream_ids_.size() >= 2) {
        return false;
    }
    const auto route = runtime_->routes()->lookup(
        request_host_, request_target_, attempted_upstream_ids_);
    if (route.status != edgegate::routing::RouteLookupStatus::kMatched) {
        return false;
    }
    selected_upstream_ = *route.upstream;
    attempted_upstream_ids_.push_back(selected_upstream_->id);
    to_upstream_.clear();
    to_client_.clear();
    response_head_bytes_.clear();
    response_parser_.reset();
    chunked_decoder_.reset();
    response_body_expected_ = 0;
    response_body_received_ = 0;
    response_complete_ = false;
    response_paused_ = false;
    if (!to_upstream_.append(upstream_request_head_)) {
        return false;
    }
    ++runtime_->stats()->retries;
    connect_upstream(loop);
    return state_ != StreamState::kClosed;
}

void ReliableSession::queue_error(
    edgegate::net::EventLoop& loop,
    int status,
    const char* reason) noexcept
{
    close_upstream(loop);
    to_upstream_.clear();
    to_client_.clear();
    const std::string body = std::to_string(status) + " " + reason + "\n";
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    if (!to_client_.append(response)) {
        close_session(loop);
        return;
    }
    close_after_response_ = true;
    response_complete_ = true;
    state_ = StreamState::kDrainingResponse;
    write_client(loop);
}

void ReliableSession::close_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (upstream_fd_.has_value()) {
        static_cast<void>(loop.remove(*upstream_fd_));
        upstream_fd_.reset();
    }
}

void ReliableSession::close_session(edgegate::net::EventLoop& loop) noexcept
{
    if (state_ == StreamState::kClosed) {
        return;
    }
    state_ = StreamState::kClosed;
    close_upstream(loop);
    static_cast<void>(loop.remove(client_fd_));
}

void ReliableSession::refresh_interests(edgegate::net::EventLoop& loop) noexcept
{
    if (state_ == StreamState::kClosed) {
        return;
    }
    if (!loop.modify(client_fd_, interests(StreamRole::kClient))) {
        close_session(loop);
        return;
    }
    if (upstream_fd_.has_value() &&
        !loop.modify(*upstream_fd_, interests(StreamRole::kUpstream))) {
        upstream_failure(loop, 502, "Bad Gateway", false);
    }
}

void ReliableSession::update_request_backpressure() noexcept
{
    const auto& limits = runtime_->config().stream_buffer;
    if (!request_paused_ && to_upstream_.readable_size() >= limits.high_watermark) {
        request_paused_ = true;
        ++runtime_->stats()->backpressure_pauses;
    } else if (request_paused_ &&
               to_upstream_.readable_size() <= limits.low_watermark) {
        request_paused_ = false;
        ++runtime_->stats()->backpressure_resumes;
    }
}

void ReliableSession::update_response_backpressure() noexcept
{
    const auto& limits = runtime_->config().stream_buffer;
    if (!response_paused_ && to_client_.readable_size() >= limits.high_watermark) {
        response_paused_ = true;
        ++runtime_->stats()->backpressure_pauses;
    } else if (response_paused_ &&
               to_client_.readable_size() <= limits.low_watermark) {
        response_paused_ = false;
        ++runtime_->stats()->backpressure_resumes;
    }
}

void ReliableSession::check_timeout(
    edgegate::net::EventLoop& loop,
    Clock::time_point now) noexcept
{
    if (state_ == StreamState::kClosed) {
        return;
    }
    if (state_ == StreamState::kReadingRequestHead && now >= phase_deadline_) {
        close_session(loop);
        return;
    }
    if (now >= request_deadline_) {
        if (client_response_bytes_sent_ == 0 && upstream_fd_.has_value()) {
            upstream_failure(loop, 504, "Gateway Timeout", true);
        } else {
            close_session(loop);
        }
        return;
    }
    if ((state_ == StreamState::kConnectingUpstream ||
         state_ == StreamState::kReadingResponseHead) &&
        now >= phase_deadline_) {
        upstream_failure(loop, 504, "Gateway Timeout", true);
        return;
    }
    if ((state_ == StreamState::kSendingRequest ||
         state_ == StreamState::kStreamingResponse ||
         state_ == StreamState::kDrainingResponse) &&
        now - last_io_ >= std::chrono::milliseconds(
            runtime_->config().timeouts.io_idle_ms)) {
        if (state_ == StreamState::kSendingRequest && !request_body_complete_) {
            ++runtime_->stats()->client_errors;
            close_session(loop);
        } else if (client_response_bytes_sent_ == 0) {
            upstream_failure(loop, 504, "Gateway Timeout", true);
        } else {
            close_session(loop);
        }
    }
}

void ReliableRuntime::tick(
    edgegate::net::EventLoop& loop,
    Clock::time_point now) noexcept
{
    auto output = sessions_.begin();
    for (auto input = sessions_.begin(); input != sessions_.end(); ++input) {
        if (auto session = input->lock()) {
            session->check_timeout(loop, now);
            *output++ = *input;
        }
    }
    sessions_.erase(output, sessions_.end());

    advance_probes(now);
    for (auto& [key, state] : health_) {
        const bool active = std::any_of(
            probes_.begin(), probes_.end(),
            [&key](const HealthProbe& probe) { return probe.key == key; });
        if (!active && now >= state.next_check) {
            start_probe(key, now);
            state.next_check = now + std::chrono::milliseconds(
                config_.health_check.interval_ms);
        }
    }
}

void ReliableRuntime::start_probe(
    const std::string& key,
    Clock::time_point now) noexcept
{
    const auto found = health_.find(key);
    if (found == health_.end()) {
        return;
    }
    edgegate::net::UniqueFd socket(::socket(
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket) {
        record_result(key, false);
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(found->second.endpoint.port);
    if (::inet_pton(AF_INET, found->second.endpoint.address.c_str(),
                    &address.sin_addr) != 1) {
        record_result(key, false);
        return;
    }
    const int result = ::connect(socket.get(),
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result == -1 && errno != EINPROGRESS) {
        record_result(key, false);
        return;
    }
    const std::string request =
        "GET " + config_.health_check.path + " HTTP/1.1\r\nHost: " +
        found->second.endpoint.address + "\r\nConnection: close\r\n\r\n";
    probes_.push_back(HealthProbe{
        key, std::move(socket),
        result == 0 ? ProbePhase::kSending : ProbePhase::kConnecting,
        request, 0, {},
        now + std::chrono::milliseconds(config_.health_check.timeout_ms)});
}

void ReliableRuntime::advance_probes(Clock::time_point now) noexcept
{
    std::size_t index = 0;
    while (index < probes_.size()) {
        HealthProbe& probe = probes_[index];
        bool finished = false;
        bool success = false;
        if (now >= probe.deadline) {
            finished = true;
        } else if (probe.phase == ProbePhase::kConnecting) {
            pollfd descriptor{probe.socket.get(), POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, 0);
            if (ready > 0) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (::getsockopt(probe.socket.get(), SOL_SOCKET, SO_ERROR,
                                 &socket_error, &length) == 0 &&
                    socket_error == 0) {
                    probe.phase = ProbePhase::kSending;
                } else {
                    finished = true;
                }
            } else if (ready == -1 && errno != EINTR) {
                finished = true;
            }
        }

        if (!finished && probe.phase == ProbePhase::kSending) {
            const ssize_t sent = ::send(
                probe.socket.get(), probe.request.data() + probe.sent,
                probe.request.size() - probe.sent, MSG_NOSIGNAL);
            if (sent > 0) {
                probe.sent += static_cast<std::size_t>(sent);
                if (probe.sent == probe.request.size()) {
                    probe.phase = ProbePhase::kReading;
                }
            } else if (sent == -1 && errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
                finished = true;
            }
        }

        if (!finished && probe.phase == ProbePhase::kReading) {
            std::array<char, 1024> bytes{};
            const ssize_t received = ::recv(
                probe.socket.get(), bytes.data(), bytes.size(), 0);
            if (received > 0) {
                probe.response_head.append(
                    bytes.data(), static_cast<std::size_t>(received));
                const std::size_t line_end = probe.response_head.find("\r\n");
                if (line_end != std::string::npos) {
                    const std::string_view line(
                        probe.response_head.data(), line_end);
                    const std::size_t first_space = line.find(' ');
                    const std::size_t second_space = first_space == std::string::npos
                        ? std::string::npos : line.find(' ', first_space + 1);
                    if (first_space != std::string::npos &&
                        second_space != std::string::npos &&
                        second_space - first_space == 4) {
                        const int status = std::atoi(
                            std::string(line.substr(first_space + 1, 3)).c_str());
                        success = status >= 200 && status < 300;
                    }
                    finished = true;
                } else if (probe.response_head.size() >
                           config_.max_header_size) {
                    finished = true;
                }
            } else if (received == 0) {
                finished = true;
            } else if (errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
                finished = true;
            }
        }

        if (finished) {
            const std::string key = probe.key;
            probes_.erase(probes_.begin() + static_cast<std::ptrdiff_t>(index));
            record_result(key, success);
        } else {
            ++index;
        }
    }
}

void ReliableRuntime::record_result(
    const std::string& key,
    bool success) noexcept
{
    const auto found = health_.find(key);
    if (found == health_.end()) {
        return;
    }
    HealthState& state = found->second;
    if (success) {
        state.consecutive_failures = 0;
        ++state.consecutive_successes;
        if (!state.endpoint.healthy &&
            state.consecutive_successes >= config_.health_check.success_threshold) {
            state.endpoint.healthy = true;
            static_cast<void>(routes_->set_endpoint_health(
                state.endpoint.address, state.endpoint.port, true));
            ++stats_->health_transitions;
        }
    } else {
        ++stats_->health_failures;
        state.consecutive_successes = 0;
        ++state.consecutive_failures;
        if (state.endpoint.healthy &&
            state.consecutive_failures >= config_.health_check.failure_threshold) {
            state.endpoint.healthy = false;
            static_cast<void>(routes_->set_endpoint_health(
                state.endpoint.address, state.endpoint.port, false));
            ++stats_->health_transitions;
        }
    }
}

class ReliableListener final : public edgegate::net::EventHandler {
public:
    ReliableListener(edgegate::net::UniqueFd listener,
                     std::shared_ptr<ReliableRuntime> runtime)
        : listener_(std::move(listener)), runtime_(std::move(runtime))
    {
    }

    int fd() const noexcept override { return listener_.get(); }
    std::uint32_t interests() const noexcept override { return EPOLLIN; }

    void on_event(edgegate::net::EventLoop& loop,
                  std::uint32_t events) noexcept override
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
            loop.stop();
            return;
        }
        for (;;) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            edgegate::net::UniqueFd client(::accept4(
                listener_.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!client) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                return;
            }
            try {
                const int descriptor = client.get();
                auto session = std::make_shared<ReliableSession>(
                    descriptor, peer_address(peer), runtime_);
                runtime_->add_session(session);
                loop.add(std::make_unique<ReliableEndpoint>(
                    std::move(client), StreamRole::kClient, session));
                ++runtime_->stats()->accepted;
            } catch (...) {
                ++runtime_->stats()->client_errors;
            }
        }
    }

private:
    edgegate::net::UniqueFd listener_;
    std::shared_ptr<ReliableRuntime> runtime_;
};

class RuntimeTimer final : public edgegate::net::EventHandler {
public:
    RuntimeTimer(edgegate::net::UniqueFd timer,
                 std::shared_ptr<ReliableRuntime> runtime)
        : timer_(std::move(timer)), runtime_(std::move(runtime))
    {
    }

    int fd() const noexcept override { return timer_.get(); }
    std::uint32_t interests() const noexcept override { return EPOLLIN; }

    void on_event(edgegate::net::EventLoop& loop,
                  std::uint32_t events) noexcept override
    {
        if ((events & EPOLLIN) == 0U) {
            return;
        }
        std::uint64_t expirations = 0;
        while (::read(timer_.get(), &expirations, sizeof(expirations)) == -1 &&
               errno == EINTR) {
        }
        runtime_->tick(loop, Clock::now());
    }

private:
    edgegate::net::UniqueFd timer_;
    std::shared_ptr<ReliableRuntime> runtime_;
};

edgegate::net::UniqueFd make_runtime_timer()
{
    edgegate::net::UniqueFd timer(::timerfd_create(
        CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!timer) {
        throw system_error_from_errno("timerfd_create");
    }
    itimerspec specification{};
    specification.it_value.tv_sec = 0;
    specification.it_value.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(kRuntimeTick).count();
    specification.it_interval = specification.it_value;
    if (::timerfd_settime(timer.get(), 0, &specification, nullptr) == -1) {
        throw system_error_from_errno("timerfd_settime");
    }
    return timer;
}

} // namespace

class ReliableProxyServer::Implementation {
public:
    explicit Implementation(edgegate::config::EdgeGateConfig config)
        : stats_(std::make_shared<ReliableProxyStats>()),
          runtime_(std::make_shared<ReliableRuntime>(config, stats_))
    {
        edgegate::net::UniqueFd listener = make_listener(
            config.listen_address, config.listen_port, port_);
        loop_.add(std::make_unique<ReliableListener>(
            std::move(listener), runtime_));
        loop_.add(std::make_unique<RuntimeTimer>(
            make_runtime_timer(), runtime_));
    }

    edgegate::net::EventLoop loop_;
    std::shared_ptr<ReliableProxyStats> stats_;
    std::shared_ptr<ReliableRuntime> runtime_;
    std::uint16_t port_{0};
};

ReliableProxyServer::ReliableProxyServer(edgegate::config::EdgeGateConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config)))
{
}

ReliableProxyServer::~ReliableProxyServer() = default;

int ReliableProxyServer::run_once(int timeout_ms)
{
    return implementation_->loop_.run_once(timeout_ms);
}

void ReliableProxyServer::run()
{
    implementation_->loop_.run();
}

void ReliableProxyServer::stop() noexcept
{
    implementation_->loop_.stop();
}

std::uint16_t ReliableProxyServer::port() const noexcept
{
    return implementation_->port_;
}

const std::shared_ptr<ReliableProxyStats>& ReliableProxyServer::stats() const noexcept
{
    return implementation_->stats_;
}

} // namespace edgegate::proxy
// AI-CODE-END: S7-RELIABLE-PROXY-SERVER-IMPLEMENTATION
