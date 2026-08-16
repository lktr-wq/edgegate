#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// AI-CODE-BEGIN: S6-TEST-BACKEND
namespace {

class Socket {
public:
    explicit Socket(int fd = -1) noexcept : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) { ::close(fd_); } }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

std::uint16_t parse_port(const char* text)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > 65535) {
        throw std::invalid_argument("port must be 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

// AI-CODE-BEGIN: S9-TEST-BACKEND-DELAY
std::uint32_t parse_delay(const char* text)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 60000) {
        throw std::invalid_argument("delay_ms must be 0..60000");
    }
    return static_cast<std::uint32_t>(value);
}
// AI-CODE-END: S9-TEST-BACKEND-DELAY

void send_all(int fd, const std::string& response)
{
    std::size_t offset = 0;
    while (offset < response.size()) {
        const ssize_t sent = ::send(
            fd,
            response.data() + offset,
            response.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
        } else if (sent == -1 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("send response failed");
        }
    }
}

Socket create_listener(std::uint16_t port)
{
    Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listener) {
        throw std::runtime_error("socket failed");
    }
    const int reuse = 1;
    if (::setsockopt(
            listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        throw std::runtime_error("setsockopt failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1 ||
        ::listen(listener.get(), 32) == -1) {
        throw std::runtime_error("bind/listen failed");
    }
    return listener;
}

void serve(
    const std::string& name,
    std::uint16_t port,
    std::uint32_t delay_ms)
{
    Socket listener = create_listener(port);
    std::cout << name << " listening on 127.0.0.1:" << port << std::endl;

    for (;;) {
        Socket client;
        do {
            client = Socket(::accept4(
                listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
        } while (!client && errno == EINTR);
        if (!client) {
            throw std::runtime_error("accept failed");
        }

        std::array<char, 4096> buffer{};
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 8192) {
            const ssize_t received =
                ::recv(client.get(), buffer.data(), buffer.size(), 0);
            if (received > 0) {
                request.append(buffer.data(), static_cast<std::size_t>(received));
            } else if (received == -1 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }

        // AI-CODE-BEGIN: S9-TEST-BACKEND-DELAY-BEHAVIOR
        // 阶段9验收用可控延迟制造慢请求；默认0保持阶段6原有行为。
        if (delay_ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        // AI-CODE-END: S9-TEST-BACKEND-DELAY-BEHAVIOR
        const std::string body = name + "\n";
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        send_all(client.get(), response);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: edgegate_test_backend <name> <port> [delay_ms]\n";
        return 2;
    }
    try {
        serve(
            argv[1],
            parse_port(argv[2]),
            argc == 4 ? parse_delay(argv[3]) : 0);
    } catch (const std::exception& error) {
        std::cerr << "test backend failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
// AI-CODE-END: S6-TEST-BACKEND
