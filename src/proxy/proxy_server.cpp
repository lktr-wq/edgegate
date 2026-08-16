#include "edgegate/proxy/proxy_server.h"

// AI-CODE-BEGIN: S5-PROXY-SERVER-IMPLEMENTATION
#include "edgegate/net/unique_fd.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::proxy {

namespace {

constexpr int kListenBacklog = 128;

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

class ProxyListener final : public edgegate::net::EventHandler {
public:
    ProxyListener(
        edgegate::net::UniqueFd listener,
        ProxyConfig config,
        std::shared_ptr<ProxyStats> stats)
        : listener_(std::move(listener)),
          config_(std::move(config)),
          stats_(std::move(stats))
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

    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
            loop.stop();
            return;
        }

        for (;;) {
            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            const int accepted_fd = ::accept4(
                listener_.get(),
                reinterpret_cast<sockaddr*>(&peer),
                &peer_length,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (accepted_fd >= 0) {
                edgegate::net::UniqueFd client(accepted_fd);
                try {
                    char peer_text[INET_ADDRSTRLEN]{};
                    if (::inet_ntop(
                            AF_INET,
                            &peer.sin_addr,
                            peer_text,
                            sizeof(peer_text)) == nullptr) {
                        throw std::runtime_error("failed to format client IPv4 address");
                    }
                    auto session = std::make_shared<ProxySession>(
                        accepted_fd, peer_text, config_, stats_);
                    loop.add(std::make_unique<ProxyEndpoint>(
                        std::move(client), EndpointRole::kClient, session));
                    ++stats_->accepted;
                } catch (...) {
                    ++stats_->client_errors;
                }
                continue;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            ++stats_->client_errors;
            return;
        }
    }

private:
    edgegate::net::UniqueFd listener_;
    ProxyConfig config_;
    std::shared_ptr<ProxyStats> stats_;
};

edgegate::net::UniqueFd create_listener(
    const std::string& bind_address,
    std::uint16_t requested_port,
    std::uint16_t& actual_port)
{
    edgegate::net::UniqueFd listener(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0));
    if (!listener) {
        throw system_error_from_errno("socket proxy listener");
    }

    const int reuse_address = 1;
    if (::setsockopt(
            listener.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1) {
        throw system_error_from_errno("setsockopt proxy listener");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(requested_port);
    if (::inet_pton(
            AF_INET,
            bind_address.c_str(),
            &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid proxy bind address");
    }
    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        throw system_error_from_errno("bind proxy listener");
    }
    if (::listen(listener.get(), kListenBacklog) == -1) {
        throw system_error_from_errno("listen proxy");
    }

    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(
            listener.get(),
            reinterpret_cast<sockaddr*>(&bound),
            &length) == -1) {
        throw system_error_from_errno("getsockname proxy listener");
    }
    actual_port = ntohs(bound.sin_port);
    return listener;
}

} // namespace

ProxyServer::ProxyServer(
    std::string bind_address,
    std::uint16_t listen_port,
    ProxyConfig config)
    : stats_(std::make_shared<ProxyStats>())
{
    auto listener = create_listener(bind_address, listen_port, port_);
    loop_.add(std::make_unique<ProxyListener>(
        std::move(listener), std::move(config), stats_));
}

int ProxyServer::run_once(int timeout_ms)
{
    return loop_.run_once(timeout_ms);
}

void ProxyServer::run()
{
    loop_.run();
}

void ProxyServer::stop() noexcept
{
    loop_.stop();
}

std::uint16_t ProxyServer::port() const noexcept
{
    return port_;
}

const std::shared_ptr<ProxyStats>& ProxyServer::stats() const noexcept
{
    return stats_;
}

} // namespace edgegate::proxy
// AI-CODE-END: S5-PROXY-SERVER-IMPLEMENTATION
