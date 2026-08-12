#include "edgegate/net/echo_connection.h"

// AI-CODE-BEGIN: S4-ECHO-CONNECTION-IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::net {

namespace {

constexpr std::size_t kReadChunkSize = 16 * 1024;

} // namespace

EchoConnection::EchoConnection(
    UniqueFd socket,
    std::shared_ptr<ReactorStats> stats,
    BufferWatermarks watermarks)
    : socket_(std::move(socket)),
      stats_(std::move(stats)),
      watermarks_(watermarks),
      input_(kReadChunkSize),
      output_(watermarks.maximum)
{
    if (!socket_ || !stats_) {
        throw std::invalid_argument(
            "EchoConnection requires a socket and shared stats");
    }
    if (watermarks_.low >= watermarks_.high ||
        watermarks_.high > watermarks_.maximum) {
        throw std::invalid_argument("invalid buffer watermarks");
    }

    ++stats_->active_connections;
}

EchoConnection::~EchoConnection()
{
    --stats_->active_connections;
    ++stats_->closed_connections;
}

int EchoConnection::fd() const noexcept
{
    return socket_.get();
}

std::uint32_t EchoConnection::interests() const noexcept
{
    std::uint32_t events = EPOLLRDHUP;
    if (!peer_closed_ && !read_paused_) {
        events |= EPOLLIN;
    }
    if (!output_.empty()) {
        events |= EPOLLOUT;
    }
    return events;
}

void EchoConnection::on_event(
    EventLoop& loop,
    std::uint32_t events) noexcept
{
    if ((events & EPOLLERR) != 0U) {
        ++stats_->socket_errors;
        fatal_error_ = true;
    }

    // 先读完已经到达的数据，再处理 RDHUP，避免丢掉 FIN 前的最后一批字节。
    if (!fatal_error_ && (events & EPOLLIN) != 0U) {
        read_from_peer();
    }

    if ((events & (EPOLLRDHUP | EPOLLHUP)) != 0U) {
        peer_closed_ = true;
    }

    // 新读到的数据可以立刻尝试发送，不必额外等待下一轮 EPOLLOUT。
    if (!fatal_error_ && !output_.empty()) {
        write_to_peer();
    }

    refresh_backpressure_state();

    if (fatal_error_ || (peer_closed_ && output_.empty())) {
        static_cast<void>(loop.remove(fd()));
        return;
    }

    if (!loop.modify(fd(), interests())) {
        ++stats_->registration_errors;
        static_cast<void>(loop.remove(fd()));
    }
}

void EchoConnection::read_from_peer() noexcept
{
    std::array<char, kReadChunkSize> chunk{};

    for (;;) {
        const std::size_t capacity = std::min(
            chunk.size(), output_.writable_capacity());
        if (capacity == 0) {
            read_paused_ = true;
            ++stats_->read_pauses;
            return;
        }

        const ssize_t received = ::recv(
            socket_.get(), chunk.data(), capacity, 0);
        if (received > 0) {
            const std::size_t count = static_cast<std::size_t>(received);
            stats_->bytes_received.fetch_add(count);

            const std::string_view bytes(chunk.data(), count);
            if (!input_.append(bytes) ||
                !output_.append(input_.readable_view()) ||
                !input_.consume(input_.readable_size())) {
                fatal_error_ = true;
                ++stats_->socket_errors;
                return;
            }

            if (output_.readable_size() >= watermarks_.high) {
                read_paused_ = true;
                ++stats_->read_pauses;
                return;
            }
            continue;
        }

        if (received == 0) {
            peer_closed_ = true;
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ++stats_->read_would_block;
            return;
        }

        fatal_error_ = true;
        ++stats_->socket_errors;
        return;
    }
}

void EchoConnection::write_to_peer() noexcept
{
    while (!output_.empty()) {
        const std::string_view pending = output_.readable_view();
        const ssize_t sent = ::send(
            socket_.get(),
            pending.data(),
            pending.size(),
            MSG_NOSIGNAL);

        if (sent > 0) {
            const std::size_t count = static_cast<std::size_t>(sent);
            static_cast<void>(output_.consume(count));
            stats_->bytes_sent.fetch_add(count);
            continue;
        }

        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++stats_->write_would_block;
            return;
        }

        fatal_error_ = true;
        ++stats_->socket_errors;
        return;
    }
}

void EchoConnection::refresh_backpressure_state() noexcept
{
    if (read_paused_ && output_.readable_size() <= watermarks_.low) {
        read_paused_ = false;
    }
}

} // namespace edgegate::net
// AI-CODE-END: S4-ECHO-CONNECTION-IMPLEMENTATION
