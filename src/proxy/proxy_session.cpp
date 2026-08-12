#include "edgegate/proxy/proxy_session.h"

// AI-CODE-BEGIN: S5-PROXY-SESSION-IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::proxy {

namespace {

constexpr std::size_t kIoChunkSize = 16 * 1024;

std::size_t checked_capacity(
    std::size_t header_size,
    std::size_t body_size)
{
    if (body_size > std::numeric_limits<std::size_t>::max() - header_size) {
        throw std::invalid_argument("proxy buffer capacity overflow");
    }
    return header_size + body_size;
}

char ascii_lower(char character) noexcept
{
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character + ('a' - 'A'));
    }
    return character;
}

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
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
        while (!token.empty() &&
               (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() &&
               (token.back() == ' ' || token.back() == '\t')) {
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

} // namespace

ProxyEndpoint::ProxyEndpoint(
    edgegate::net::UniqueFd socket,
    EndpointRole role,
    std::shared_ptr<ProxySession> session)
    : socket_(std::move(socket)),
      role_(role),
      session_(std::move(session))
{
    if (!socket_ || !session_) {
        throw std::invalid_argument("ProxyEndpoint requires socket and session");
    }
}

int ProxyEndpoint::fd() const noexcept
{
    return socket_.get();
}

std::uint32_t ProxyEndpoint::interests() const noexcept
{
    return session_->interests(role_);
}

void ProxyEndpoint::on_event(
    edgegate::net::EventLoop& loop,
    std::uint32_t events) noexcept
{
    session_->on_event(loop, role_, events);
}

ProxySession::ProxySession(
    int client_fd,
    ProxyConfig config,
    std::shared_ptr<ProxyStats> stats)
    : client_fd_(client_fd),
      config_(std::move(config)),
      stats_(std::move(stats)),
      request_parser_(
          config_.max_header_size,
          config_.max_request_body_size),
      to_upstream_(checked_capacity(
          config_.max_header_size,
          config_.max_request_body_size)),
      to_client_(checked_capacity(
          config_.max_header_size,
          config_.max_response_body_size))
{
    if (client_fd_ < 0 || !stats_) {
        throw std::invalid_argument("ProxySession requires client fd and stats");
    }
    ++stats_->active_sessions;
}

ProxySession::~ProxySession()
{
    --stats_->active_sessions;
}

std::uint32_t ProxySession::interests(EndpointRole role) const noexcept
{
    if (state_ == ProxyState::kClosed) {
        return 0;
    }

    std::uint32_t events = 0;
    if (role == EndpointRole::kClient) {
        if (!client_read_closed_) {
            events |= EPOLLRDHUP;
        }
        if (state_ == ProxyState::kReadingRequest && !client_read_closed_) {
            events |= EPOLLIN;
        }
        if (!to_client_.empty()) {
            events |= EPOLLOUT;
        }
        return events;
    }

    events |= EPOLLRDHUP;
    if (state_ == ProxyState::kConnectingUpstream ||
        state_ == ProxyState::kSendingRequest) {
        events |= EPOLLOUT;
    }
    if (state_ == ProxyState::kReadingResponse) {
        events |= EPOLLIN;
    }
    return events;
}

void ProxySession::on_event(
    edgegate::net::EventLoop& loop,
    EndpointRole role,
    std::uint32_t events) noexcept
{
    if (state_ == ProxyState::kClosed) {
        return;
    }

    if (role == EndpointRole::kClient) {
        if ((events & EPOLLERR) != 0U) {
            ++stats_->client_errors;
            close_session(loop);
            return;
        }

        if ((events & EPOLLIN) != 0U &&
            state_ == ProxyState::kReadingRequest) {
            read_client(loop);
        }
        if (state_ == ProxyState::kClosed) {
            return;
        }

        if ((events & EPOLLRDHUP) != 0U) {
            client_read_closed_ = true;
            close_after_response_ = true;
            if (state_ == ProxyState::kReadingRequest) {
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

    if (state_ == ProxyState::kConnectingUpstream &&
        (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
        finish_upstream_connect(loop);
    } else if ((events & EPOLLERR) != 0U) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
    }

    if (state_ == ProxyState::kSendingRequest &&
        (events & EPOLLOUT) != 0U) {
        write_upstream(loop);
    }

    if (state_ == ProxyState::kReadingResponse &&
        (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0U) {
        read_upstream(loop);
    }
    refresh_interests(loop);
}

ProxyState ProxySession::state() const noexcept
{
    return state_;
}

void ProxySession::read_client(edgegate::net::EventLoop& loop) noexcept
{
    std::array<char, kIoChunkSize> bytes{};
    for (;;) {
        const ssize_t received =
            ::recv(client_fd_, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            const auto result = request_parser_.consume(std::string_view(
                bytes.data(), static_cast<std::size_t>(received)));
            if (result.status == edgegate::http::ParseStatus::kError) {
                ++stats_->client_errors;
                const int status =
                    result.error == edgegate::http::ParseError::kBodyTooLarge ||
                    result.error == edgegate::http::ParseError::kHeaderTooLarge
                    ? 413
                    : 400;
                queue_error_response(
                    loop,
                    status,
                    status == 413 ? "Payload Too Large" : "Bad Request");
                return;
            }
            if (result.status ==
                edgegate::http::ParseStatus::kMessageComplete) {
                close_after_response_ = request_wants_close();
                if (!to_upstream_.append(request_parser_.raw_message())) {
                    ++stats_->client_errors;
                    queue_error_response(loop, 413, "Payload Too Large");
                    return;
                }
                connect_upstream(loop);
                return;
            }
            continue;
        }

        if (received == 0) {
            client_read_closed_ = true;
            close_session(loop);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        ++stats_->client_errors;
        close_session(loop);
        return;
    }
}

void ProxySession::connect_upstream(edgegate::net::EventLoop& loop) noexcept
{
    edgegate::net::UniqueFd socket(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0));
    if (!socket) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.upstream_port);
    if (::inet_pton(
            AF_INET,
            config_.upstream_address.c_str(),
            &address.sin_addr) != 1) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    const int result = ::connect(
        socket.get(),
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
    if (result == 0) {
        state_ = ProxyState::kSendingRequest;
    } else if (errno == EINPROGRESS) {
        state_ = ProxyState::kConnectingUpstream;
    } else {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    const int descriptor = socket.get();
    upstream_fd_ = descriptor;
    try {
        loop.add(std::make_unique<ProxyEndpoint>(
            std::move(socket),
            EndpointRole::kUpstream,
            shared_from_this()));
    } catch (...) {
        upstream_fd_.reset();
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    if (state_ == ProxyState::kSendingRequest) {
        write_upstream(loop);
    }
}

void ProxySession::finish_upstream_connect(
    edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(
            *upstream_fd_,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &length) == -1 ||
        socket_error != 0) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    state_ = ProxyState::kSendingRequest;
    write_upstream(loop);
}

void ProxySession::write_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    while (!to_upstream_.empty()) {
        const std::string_view pending = to_upstream_.readable_view();
        const ssize_t sent = ::send(
            *upstream_fd_,
            pending.data(),
            pending.size(),
            MSG_NOSIGNAL);
        if (sent > 0) {
            static_cast<void>(
                to_upstream_.consume(static_cast<std::size_t>(sent)));
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    response_parser_.emplace(
        std::string(request_parser_.method()),
        config_.max_header_size,
        config_.max_response_body_size);
    state_ = ProxyState::kReadingResponse;
}

void ProxySession::read_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value() || !response_parser_.has_value()) {
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    std::array<char, kIoChunkSize> bytes{};
    for (;;) {
        const ssize_t received =
            ::recv(*upstream_fd_, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            const auto result = response_parser_->consume(std::string_view(
                bytes.data(), static_cast<std::size_t>(received)));
            if (result.status == edgegate::http::ParseStatus::kError) {
                ++stats_->upstream_errors;
                queue_error_response(loop, 502, "Bad Gateway");
                return;
            }
            if (result.status ==
                edgegate::http::ParseStatus::kMessageComplete) {
                if (response_parser_->remaining_bytes() != 0) {
                    ++stats_->upstream_errors;
                    queue_error_response(loop, 502, "Bad Gateway");
                    return;
                }
                finish_response(loop);
                return;
            }
            continue;
        }

        if (received == 0) {
            const auto result = response_parser_->notify_eof();
            if (result.status ==
                edgegate::http::ParseStatus::kMessageComplete) {
                finish_response(loop);
            } else {
                ++stats_->upstream_errors;
                queue_error_response(loop, 502, "Bad Gateway");
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }
}

void ProxySession::write_client(edgegate::net::EventLoop& loop) noexcept
{
    while (!to_client_.empty()) {
        const std::string_view pending = to_client_.readable_view();
        const ssize_t sent = ::send(
            client_fd_, pending.data(), pending.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            static_cast<void>(
                to_client_.consume(static_cast<std::size_t>(sent)));
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ++stats_->client_errors;
        close_session(loop);
        return;
    }

    if (state_ != ProxyState::kSendingResponse) {
        return;
    }
    if (close_after_response_ || client_read_closed_) {
        close_session(loop);
        return;
    }

    request_parser_ = edgegate::http::RequestParser(
        config_.max_header_size,
        config_.max_request_body_size);
    response_parser_.reset();
    close_after_response_ = false;
    state_ = ProxyState::kReadingRequest;
}

void ProxySession::finish_response(edgegate::net::EventLoop& loop) noexcept
{
    close_after_response_ =
        close_after_response_ || response_requires_close();

    if (!to_client_.append(response_parser_->raw_message())) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    ++stats_->completed_requests;
    close_upstream(loop);
    state_ = ProxyState::kSendingResponse;
    write_client(loop);
}

void ProxySession::queue_error_response(
    edgegate::net::EventLoop& loop,
    int status,
    const char* reason) noexcept
{
    close_upstream(loop);
    to_upstream_.clear();
    to_client_.clear();

    const std::string body =
        std::to_string(status) + " " + reason + "\n";
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
    state_ = ProxyState::kSendingResponse;
    write_client(loop);
}

void ProxySession::close_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (upstream_fd_.has_value()) {
        static_cast<void>(loop.remove(*upstream_fd_));
        upstream_fd_.reset();
    }
}

void ProxySession::close_session(edgegate::net::EventLoop& loop) noexcept
{
    if (state_ == ProxyState::kClosed) {
        return;
    }
    state_ = ProxyState::kClosed;
    close_upstream(loop);
    static_cast<void>(loop.remove(client_fd_));
}

void ProxySession::refresh_interests(
    edgegate::net::EventLoop& loop) noexcept
{
    if (state_ == ProxyState::kClosed) {
        return;
    }
    if (!loop.modify(client_fd_, interests(EndpointRole::kClient))) {
        close_session(loop);
        return;
    }
    if (upstream_fd_.has_value() &&
        !loop.modify(*upstream_fd_, interests(EndpointRole::kUpstream))) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
    }
}

bool ProxySession::request_wants_close() const noexcept
{
    return header_contains_token(
        request_parser_.header_value("Connection"), "close");
}

bool ProxySession::response_requires_close() const noexcept
{
    if (response_parser_->body_mode() ==
        edgegate::http::ResponseBodyMode::kCloseDelimited) {
        return true;
    }
    const auto connection = response_parser_->header_value("Connection");
    if (header_contains_token(connection, "close")) {
        return true;
    }
    if (response_parser_->version() == "HTTP/1.0") {
        return !header_contains_token(connection, "keep-alive");
    }
    return false;
}

} // namespace edgegate::proxy
// AI-CODE-END: S5-PROXY-SESSION-IMPLEMENTATION
