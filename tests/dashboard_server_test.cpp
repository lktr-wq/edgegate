#include "edgegate/net/event_loop.h"
#include "edgegate/net/unique_fd.h"
#include "edgegate/runtime/dashboard_server.h"

// AI-CODE-BEGIN: S9-DASHBOARD-SERVER-TESTS
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using edgegate::net::UniqueFd;

void send_all(int fd, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent == -1 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("dashboard test send failed");
        }
    }
}

std::string request(std::uint16_t port, std::string_view wire)
{
    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!client) throw std::runtime_error("dashboard test socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        throw std::runtime_error("dashboard test address failed");
    }
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        throw std::runtime_error("dashboard test connect failed");
    }
    send_all(client.get(), wire);
    std::string response;
    char bytes[2048];
    for (;;) {
        const ssize_t received = ::recv(client.get(), bytes, sizeof(bytes), 0);
        if (received > 0) {
            response.append(bytes, static_cast<std::size_t>(received));
        } else if (received == 0) {
            return response;
        } else if (errno != EINTR) {
            throw std::runtime_error("dashboard test receive failed");
        }
    }
}

class RunningDashboard {
public:
    RunningDashboard()
    {
        loop_.add(edgegate::runtime::make_dashboard_listener(
            "127.0.0.1", 0,
            [] { return nlohmann::json{{"service", "edgegate"}, {"requests", 7}}; },
            port_));
        thread_ = std::thread([this] {
            while (!stopping_.load()) {
                static_cast<void>(loop_.run_once(10));
            }
        });
    }

    ~RunningDashboard()
    {
        stopping_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    edgegate::net::EventLoop loop_;
    std::uint16_t port_{0};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// 测试：根页面和 JSON 使用独立只读 HTTP 接口，并附带禁止缓存与安全响应头。
TEST(DashboardServerTest, ServesPageAndJsonSnapshot)
{
    RunningDashboard dashboard;
    const std::string page = request(
        dashboard.port(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(page.find("HTTP/1.1 200 OK\r\n"), 0U);
    EXPECT_NE(page.find("EdgeGate Dashboard"), std::string::npos);
    EXPECT_NE(page.find("Content-Security-Policy:"), std::string::npos);

    const std::string api = request(
        dashboard.port(),
        "GET /api/dashboard HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(api.find("HTTP/1.1 200 OK\r\n"), 0U);
    EXPECT_NE(api.find("Cache-Control: no-store\r\n"), std::string::npos);
    EXPECT_NE(api.find("\"requests\":7"), std::string::npos);
}

// 测试：页面没有写操作；非 GET/HEAD 方法拒绝，未知路径稳定返回 404。
TEST(DashboardServerTest, RejectsWritesAndUnknownPaths)
{
    RunningDashboard dashboard;
    const std::string head = request(
        dashboard.port(), "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::size_t head_body = head.find("\r\n\r\n");
    ASSERT_NE(head_body, std::string::npos);
    EXPECT_EQ(head.size(), head_body + 4);
    EXPECT_NE(head.find("Content-Length: "), std::string::npos);
    EXPECT_EQ(request(
        dashboard.port(), "POST /api/dashboard HTTP/1.1\r\nHost: x\r\n\r\n")
        .find("HTTP/1.1 405 Method Not Allowed\r\n"), 0U);
    EXPECT_EQ(request(
        dashboard.port(), "GET /reload HTTP/1.1\r\nHost: x\r\n\r\n")
        .find("HTTP/1.1 404 Not Found\r\n"), 0U);

    const std::string oversized =
        "GET / HTTP/1.1\r\nHost: x\r\nX-Fill: " + std::string(8200, 'x') +
        "\r\n\r\n";
    EXPECT_EQ(request(dashboard.port(), oversized)
                  .find("HTTP/1.1 413 Payload Too Large\r\n"),
              0U);
}

} // namespace
// AI-CODE-END: S9-DASHBOARD-SERVER-TESTS
