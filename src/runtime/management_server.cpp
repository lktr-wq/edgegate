#include "edgegate/runtime/management_server.h"

// AI-CODE-BEGIN: S8-MANAGEMENT-SERVER-IMPLEMENTATION
#include "edgegate/net/unique_fd.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace edgegate::runtime {

namespace {

constexpr std::size_t kMaximumCommandBytes = 4096;
constexpr int kManagementBacklog = 16;

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

void remove_owned_stale_socket(const std::string& path)
{
    struct stat information {};
    if (::lstat(path.c_str(), &information) == -1) {
        if (errno == ENOENT) {
            return;
        }
        throw system_error_from_errno("lstat management socket");
    }
    if (!S_ISSOCK(information.st_mode) || information.st_uid != ::geteuid()) {
        throw std::runtime_error(
            "management path exists but is not an owned Unix socket: " + path);
    }
    if (::unlink(path.c_str()) == -1) {
        throw system_error_from_errno("unlink stale management socket");
    }
}

class ManagementConnection final : public edgegate::net::EventHandler {
public:
    ManagementConnection(
        edgegate::net::UniqueFd socket,
        ManagementCommandHandler handler)
        : socket_(std::move(socket)), handler_(std::move(handler))
    {
    }

    int fd() const noexcept override { return socket_.get(); }

    std::uint32_t interests() const noexcept override
    {
        return response_.empty() ? EPOLLIN : EPOLLOUT;
    }

    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override
    {
        if ((events & EPOLLERR) != 0U) {
            static_cast<void>(loop.remove(fd()));
            return;
        }
        if (response_.empty() && (events & EPOLLIN) != 0U) {
            read_command(loop);
        }
        if (!response_.empty() && (events & EPOLLOUT) != 0U) {
            write_response(loop);
            return;
        }
        // 对端可能“发完命令后关闭写方向”，此时 epoll 会同时给出
        // EPOLLIN/EPOLLHUP。必须先读取 EPOLLIN，不能把完整命令误丢掉。
        if ((events & EPOLLHUP) != 0U && response_.empty()) {
            static_cast<void>(loop.remove(fd()));
        }
    }

private:
    void queue_response(
        edgegate::net::EventLoop& loop,
        nlohmann::json response) noexcept
    {
        try {
            response_ = response.dump();
            response_ += '\n';
            static_cast<void>(loop.modify(fd(), EPOLLOUT));
        } catch (...) {
            static_cast<void>(loop.remove(fd()));
        }
    }

    void read_command(edgegate::net::EventLoop& loop) noexcept
    {
        std::array<char, 1024> bytes{};
        for (;;) {
            const ssize_t received = ::recv(
                fd(), bytes.data(), bytes.size(), 0);
            if (received > 0) {
                request_.append(bytes.data(), static_cast<std::size_t>(received));
                const std::size_t newline = request_.find('\n');
                if (newline != std::string::npos) {
                    const nlohmann::json request = nlohmann::json::parse(
                        request_.begin(),
                        request_.begin() + static_cast<std::ptrdiff_t>(newline),
                        nullptr,
                        false);
                    if (request.is_discarded() || !request.is_object()) {
                        queue_response(loop, {
                            {"ok", false}, {"error", "invalid JSON command"}});
                        return;
                    }
                    try {
                        queue_response(loop, handler_(loop, request));
                    } catch (const std::exception& error) {
                        queue_response(loop, {
                            {"ok", false}, {"error", error.what()}});
                    } catch (...) {
                        queue_response(loop, {
                            {"ok", false}, {"error", "internal command error"}});
                    }
                    return;
                }
                if (request_.size() > kMaximumCommandBytes) {
                    queue_response(loop, {
                        {"ok", false}, {"error", "command exceeds 4096 bytes"}});
                    return;
                }
                continue;
            }
            if (received == 0) {
                static_cast<void>(loop.remove(fd()));
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            static_cast<void>(loop.remove(fd()));
            return;
        }
    }

    void write_response(edgegate::net::EventLoop& loop) noexcept
    {
        while (response_offset_ < response_.size()) {
            const ssize_t sent = ::send(
                fd(), response_.data() + response_offset_,
                response_.size() - response_offset_, MSG_NOSIGNAL);
            if (sent > 0) {
                response_offset_ += static_cast<std::size_t>(sent);
            } else if (sent == -1 && errno == EINTR) {
                continue;
            } else if (sent == -1 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            } else {
                static_cast<void>(loop.remove(fd()));
                return;
            }
        }
        static_cast<void>(loop.remove(fd()));
    }

    edgegate::net::UniqueFd socket_;
    ManagementCommandHandler handler_;
    std::string request_;
    std::string response_;
    std::size_t response_offset_{0};
};

class ManagementListener final : public edgegate::net::EventHandler {
public:
    ManagementListener(
        edgegate::net::UniqueFd socket,
        std::string path,
        ManagementCommandHandler handler)
        : socket_(std::move(socket)),
          path_(std::move(path)),
          handler_(std::move(handler))
    {
    }

    ~ManagementListener() override
    {
        struct stat information {};
        if (::lstat(path_.c_str(), &information) == 0 &&
            S_ISSOCK(information.st_mode) &&
            information.st_uid == ::geteuid()) {
            static_cast<void>(::unlink(path_.c_str()));
        }
    }

    int fd() const noexcept override { return socket_.get(); }
    std::uint32_t interests() const noexcept override { return EPOLLIN; }

    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
            loop.stop();
            return;
        }
        for (;;) {
            edgegate::net::UniqueFd connection(::accept4(
                socket_.get(), nullptr, nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!connection) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            try {
                loop.add(std::make_unique<ManagementConnection>(
                    std::move(connection), handler_));
            } catch (...) {
            }
        }
    }

private:
    edgegate::net::UniqueFd socket_;
    std::string path_;
    ManagementCommandHandler handler_;
};

} // namespace

std::unique_ptr<edgegate::net::EventHandler> make_management_listener(
    const std::string& socket_path,
    ManagementCommandHandler handler)
{
    if (!handler) {
        throw std::invalid_argument("management command handler is required");
    }
    remove_owned_stale_socket(socket_path);
    edgegate::net::UniqueFd socket(::socket(
        AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket) {
        throw system_error_from_errno("socket management");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("management socket path is too long");
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == -1) {
        throw system_error_from_errno("bind management socket");
    }
    if (::chmod(socket_path.c_str(), S_IRUSR | S_IWUSR) == -1) {
        static_cast<void>(::unlink(socket_path.c_str()));
        throw system_error_from_errno("chmod management socket");
    }
    if (::listen(socket.get(), kManagementBacklog) == -1) {
        static_cast<void>(::unlink(socket_path.c_str()));
        throw system_error_from_errno("listen management socket");
    }
    return std::make_unique<ManagementListener>(
        std::move(socket), socket_path, std::move(handler));
}

} // namespace edgegate::runtime
// AI-CODE-END: S8-MANAGEMENT-SERVER-IMPLEMENTATION
