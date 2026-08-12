#include "edgegate/net/reactor_echo_server.h"

// AI-CODE-BEGIN: S4-REACTOR-SERVER-IMPLEMENTATION
#include "edgegate/net/unique_fd.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::net {

namespace {

constexpr int kListenBacklog = 128;

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

class ListenerHandler final : public EventHandler {
public:
    ListenerHandler(
        UniqueFd listener,
        std::shared_ptr<ReactorStats> stats,
        BufferWatermarks watermarks)
        : listener_(std::move(listener)),
          stats_(std::move(stats)),
          watermarks_(watermarks)
    {
    }

    int fd() const noexcept override
    {
        return listener_.get();
    }

    std::uint32_t interests() const noexcept override
    {
        return EPOLLIN;
    }

    void on_event(EventLoop& loop, std::uint32_t events) noexcept override
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
            ++stats_->socket_errors;
            loop.stop();
            return;
        }

        for (;;) {
            const int accepted_fd = ::accept4(
                listener_.get(),
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (accepted_fd >= 0) {
                UniqueFd accepted(accepted_fd);
                try {
                    loop.add(std::make_unique<EchoConnection>(
                        std::move(accepted), stats_, watermarks_));
                    ++stats_->accepted_connections;
                } catch (...) {
                    ++stats_->registration_errors;
                }
                continue;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            ++stats_->socket_errors;
            return;
        }
    }

private:
    UniqueFd listener_;
    std::shared_ptr<ReactorStats> stats_;
    BufferWatermarks watermarks_;
};

UniqueFd create_listener(
    const std::string& bind_address,
    std::uint16_t requested_port,
    std::uint16_t& actual_port)
{
    UniqueFd listener(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0));
    if (!listener) {
        throw system_error_from_errno("socket listener");
    }

    const int reuse_address = 1;
    if (::setsockopt(
            listener.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1) {
        throw system_error_from_errno("setsockopt SO_REUSEADDR");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(requested_port);
    if (::inet_pton(
            AF_INET,
            bind_address.c_str(),
            &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 bind address");
    }

    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        throw system_error_from_errno("bind listener");
    }
    if (::listen(listener.get(), kListenBacklog) == -1) {
        throw system_error_from_errno("listen");
    }

    sockaddr_in bound_address{};
    socklen_t bound_size = sizeof(bound_address);
    if (::getsockname(
            listener.get(),
            reinterpret_cast<sockaddr*>(&bound_address),
            &bound_size) == -1) {
        throw system_error_from_errno("getsockname");
    }
    actual_port = ntohs(bound_address.sin_port);
    return listener;
}

} // namespace

ReactorEchoServer::ReactorEchoServer(
    std::string bind_address,
    std::uint16_t port,
    BufferWatermarks watermarks)
    : stats_(std::make_shared<ReactorStats>())
{
    UniqueFd listener = create_listener(bind_address, port, port_);
    loop_.add(std::make_unique<ListenerHandler>(
        std::move(listener), stats_, watermarks));
}

int ReactorEchoServer::run_once(int timeout_ms)
{
    return loop_.run_once(timeout_ms);
}

void ReactorEchoServer::run()
{
    loop_.run();
}

void ReactorEchoServer::stop() noexcept
{
    loop_.stop();
}

std::uint16_t ReactorEchoServer::port() const noexcept
{
    return port_;
}

const std::shared_ptr<ReactorStats>& ReactorEchoServer::stats() const noexcept
{
    return stats_;
}

std::size_t ReactorEchoServer::handler_count() const noexcept
{
    return loop_.handler_count();
}

} // namespace edgegate::net
// AI-CODE-END: S4-REACTOR-SERVER-IMPLEMENTATION
