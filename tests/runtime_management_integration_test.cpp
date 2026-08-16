#include "edgegate/config/edgegate_config.h"
#include "edgegate/net/unique_fd.h"
#include "edgegate/proxy/reliable_proxy_server.h"
#include "edgegate/runtime/runtime_logger.h"

// AI-CODE-BEGIN: S8-RUNTIME-MANAGEMENT-INTEGRATION-TESTS
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using edgegate::net::UniqueFd;
using edgegate::proxy::ReliableProxyServer;

void send_all(int fd, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent == -1 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("test send failed");
        }
    }
}

bool wait_until(
    const std::function<bool()>& condition,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

UniqueFd make_tcp_listener(std::uint16_t& port)
{
    UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listener) throw std::runtime_error("test socket failed");
    int reuse = 1;
    static_cast<void>(::setsockopt(
        listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == -1 || ::listen(listener.get(), 16) == -1) {
        throw std::runtime_error("test listener setup failed");
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound),
                      &length) == -1) {
        throw std::runtime_error("test getsockname failed");
    }
    port = ntohs(bound.sin_port);
    return listener;
}

std::uint16_t reserve_free_port()
{
    std::uint16_t port = 0;
    UniqueFd temporary = make_tcp_listener(port);
    return port;
}

class TextBackend {
public:
    explicit TextBackend(std::string body)
        : body_(std::move(body)),
          listener_(make_tcp_listener(port_)),
          thread_([this] { serve(); })
    {
    }

    ~TextBackend()
    {
        stopping_.store(true);
        ::shutdown(listener_.get(), SHUT_RDWR);
        listener_.reset();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void serve()
    {
        while (!stopping_.load()) {
            UniqueFd client(::accept4(
                listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!client) {
                if (errno == EINTR) continue;
                return;
            }
            std::string request;
            std::array<char, 1024> bytes{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(
                    client.get(), bytes.data(), bytes.size(), 0);
                if (received <= 0) break;
                request.append(bytes.data(), static_cast<std::size_t>(received));
            }
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(body_.size()) +
                "\r\nConnection: close\r\n\r\n" + body_;
            try {
                send_all(client.get(), response);
            } catch (...) {
            }
        }
    }

    std::string body_;
    std::uint16_t port_{0};
    UniqueFd listener_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

class HoldingBackend {
public:
    HoldingBackend()
        : listener_(make_tcp_listener(port_)),
          thread_([this] { serve(); })
    {
    }

    ~HoldingBackend()
    {
        stopping_.store(true);
        ::shutdown(listener_.get(), SHUT_RDWR);
        listener_.reset();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void serve()
    {
        UniqueFd client(::accept4(
            listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!client) return;
        std::array<char, 1024> bytes{};
        static_cast<void>(::recv(client.get(), bytes.data(), bytes.size(), 0));
        while (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::uint16_t port_{0};
    UniqueFd listener_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

class TemporaryRuntimeFiles {
public:
    TemporaryRuntimeFiles()
        : prefix_("/tmp/edgegate-stage8-" + std::to_string(::getpid()) + "-" +
                  std::to_string(++sequence_)),
          yaml_path_(prefix_ + ".yaml"),
          socket_path_(prefix_ + ".sock"),
          log_directory_(prefix_ + "-logs")
    {
    }

    ~TemporaryRuntimeFiles()
    {
        static_cast<void>(::unlink(yaml_path_.c_str()));
        static_cast<void>(::unlink(socket_path_.c_str()));
        std::error_code ignored;
        std::filesystem::remove_all(log_directory_, ignored);
    }

    void write(
        std::uint16_t listen_port,
        std::uint16_t upstream_port,
        bool valid = true,
        bool logging_enabled = false,
        std::uint16_t dashboard_port = 0)
    {
        std::ofstream output(yaml_path_, std::ios::trunc);
        if (!valid) {
            output << "listen: [this is invalid for EdgeGate]\n";
            return;
        }
        output
            << "listen:\n"
            << "  address: 127.0.0.1\n"
            << "  port: " << listen_port << "\n"
            << "management:\n"
            << "  enabled: true\n"
            << "  socket_path: " << socket_path_ << "\n"
            << "  drain_timeout_ms: 500\n"
            << "logging:\n"
            << "  enabled: " << (logging_enabled ? "true" : "false") << "\n"
            << "  directory: " << log_directory_ << "\n"
            << "  level: info\n"
            << "  max_file_size: 1024\n"
            << "  max_files: 2\n";
        if (dashboard_port != 0) {
            output
                << "dashboard:\n"
                << "  enabled: true\n"
                << "  address: 127.0.0.1\n"
                << "  port: " << dashboard_port << "\n"
                << "  refresh_interval_ms: 250\n"
                << "  recent_error_limit: 5\n"
                << "  slow_request_threshold_ms: 1\n"
                << "  slow_request_limit: 5\n";
        }
        output
            << "health_check:\n"
            << "  interval_ms: 60000\n"
            << "  timeout_ms: 1000\n"
            << "  failure_threshold: 3\n"
            << "  success_threshold: 2\n"
            << "  path: /health\n"
            << "upstream_pools:\n"
            << "  - id: pool\n"
            << "    endpoints:\n"
            << "      - id: backend\n"
            << "        address: 127.0.0.1\n"
            << "        port: " << upstream_port << "\n"
            << "routes:\n"
            << "  - id: route\n"
            << "    host: api.test\n"
            << "    path_prefix: /\n"
            << "    upstream_pool: pool\n";
        if (!output) throw std::runtime_error("failed to write test YAML");
    }

    [[nodiscard]] const std::string& yaml_path() const noexcept
    {
        return yaml_path_;
    }
    [[nodiscard]] const std::string& socket_path() const noexcept
    {
        return socket_path_;
    }
    [[nodiscard]] const std::string& log_directory() const noexcept
    {
        return log_directory_;
    }

private:
    static int sequence_;
    std::string prefix_;
    std::string yaml_path_;
    std::string socket_path_;
    std::string log_directory_;
};

int TemporaryRuntimeFiles::sequence_ = 0;

nlohmann::json management_command(
    const std::string& socket_path,
    std::string_view command)
{
    UniqueFd client(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!client) throw std::runtime_error("management client socket failed");
    timeval timeout{2, 0};
    static_cast<void>(::setsockopt(
        client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error("test management path too long");
    }
    std::copy(socket_path.begin(), socket_path.end(), address.sun_path);
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        throw std::runtime_error("management connect failed");
    }
    send_all(client.get(), nlohmann::json{{"command", command}}.dump() + "\n");
    std::string response;
    std::array<char, 1024> bytes{};
    while (response.find('\n') == std::string::npos) {
        const ssize_t received = ::recv(
            client.get(), bytes.data(), bytes.size(), 0);
        if (received > 0) {
            response.append(bytes.data(), static_cast<std::size_t>(received));
        } else if (received == -1 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("management response failed");
        }
    }
    response.resize(response.find('\n'));
    return nlohmann::json::parse(response);
}

class RunningManagedProxy {
public:
    RunningManagedProxy(
        edgegate::config::EdgeGateConfig config,
        std::string config_path,
        std::string socket_path)
        : server_(std::move(config), std::move(config_path)),
          socket_path_(std::move(socket_path)),
          thread_([this] { server_.run(); })
    {
        if (!wait_until(
                [this] { return ::access(socket_path_.c_str(), F_OK) == 0; },
                std::chrono::seconds(2))) {
            throw std::runtime_error("management socket did not appear");
        }
    }

    ~RunningManagedProxy()
    {
        if (thread_.joinable()) {
            try {
                static_cast<void>(management_command(socket_path_, "stop"));
            } catch (...) {
            }
            thread_.join();
        }
    }

    void stop_and_join()
    {
        const auto response = management_command(socket_path_, "stop");
        if (!response.value("ok", false)) {
            throw std::runtime_error("stop command failed");
        }
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return server_.port(); }

private:
    ReliableProxyServer server_;
    std::string socket_path_;
    std::thread thread_;
};

std::string proxy_get(
    std::uint16_t port,
    std::string_view host = "api.test")
{
    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    timeval timeout{2, 0};
    static_cast<void>(::setsockopt(
        client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        throw std::runtime_error("proxy connect failed");
    }
    send_all(client.get(),
        "GET / HTTP/1.1\r\nHost: " + std::string(host) +
        "\r\nConnection: close\r\n\r\n");
    std::string response;
    std::array<char, 1024> bytes{};
    for (;;) {
        const ssize_t received = ::recv(
            client.get(), bytes.data(), bytes.size(), 0);
        if (received > 0) {
            response.append(bytes.data(), static_cast<std::size_t>(received));
        } else if (received == 0) {
            return response;
        } else if (errno != EINTR) {
            throw std::runtime_error("proxy response failed");
        }
    }
}

// AI-CODE-BEGIN: S9-DASHBOARD-PROXY-TEST-HELPER
std::string dashboard_get(std::uint16_t port, std::string_view path)
{
    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!client) throw std::runtime_error("dashboard client socket failed");
    timeval timeout{2, 0};
    static_cast<void>(::setsockopt(
        client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        throw std::runtime_error("dashboard connect failed");
    }
    send_all(client.get(),
        "GET " + std::string(path) +
        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    std::string response;
    std::array<char, 4096> bytes{};
    for (;;) {
        const ssize_t received = ::recv(
            client.get(), bytes.data(), bytes.size(), 0);
        if (received > 0) {
            response.append(bytes.data(), static_cast<std::size_t>(received));
        } else if (received == 0) {
            return response;
        } else if (errno != EINTR) {
            throw std::runtime_error("dashboard response failed");
        }
    }
}
// AI-CODE-END: S9-DASHBOARD-PROXY-TEST-HELPER

bool tcp_connect_fails(std::uint16_t port)
{
    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == -1;
}

// 测试：管理 Socket 权限为0600，查询命令返回确定结构；drain停止接收新客户端，
// 但管理通道继续存在，最后stop让EventLoop退出。
TEST(RuntimeManagementIntegrationTest, QueriesDrainAndStop)
{
    TextBackend backend("managed");
    TemporaryRuntimeFiles files;
    const std::uint16_t listen_port = reserve_free_port();
    files.write(listen_port, backend.port());
    auto config = edgegate::config::load_edgegate_config(files.yaml_path());
    RunningManagedProxy proxy(
        std::move(config), files.yaml_path(), files.socket_path());

    struct stat information {};
    ASSERT_EQ(::stat(files.socket_path().c_str(), &information), 0);
    EXPECT_EQ(information.st_mode & 0777, 0600);

    const auto status = management_command(files.socket_path(), "status");
    ASSERT_TRUE(status.value("ok", false));
    EXPECT_EQ(status["data"]["state"], "running");
    EXPECT_EQ(management_command(files.socket_path(), "routes")["data"].size(), 1U);
    EXPECT_EQ(management_command(files.socket_path(), "upstreams")["data"].size(), 1U);
    EXPECT_TRUE(management_command(files.socket_path(), "stats")["data"]
                    .contains("completed_requests"));

    EXPECT_TRUE(management_command(files.socket_path(), "drain").value("ok", false));
    EXPECT_TRUE(management_command(files.socket_path(), "drain").value("ok", false));
    EXPECT_TRUE(wait_until(
        [listen_port] { return tcp_connect_fails(listen_port); },
        std::chrono::seconds(1)));
    EXPECT_TRUE(wait_until(
        [&files] {
            return management_command(files.socket_path(), "status")
                       ["data"]["state"] == "drained";
        },
        std::chrono::seconds(2)));
    proxy.stop_and_join();
}

// 测试：合法reload让下一条请求切到新上游；无效YAML和不可热变更的监听端口
// 均被拒绝，并继续使用最后一份成功配置。
TEST(RuntimeManagementIntegrationTest, ReloadIsAtomicAndRejectsImmutableChanges)
{
    TextBackend first("first");
    TextBackend second("second");
    TemporaryRuntimeFiles files;
    const std::uint16_t listen_port = reserve_free_port();
    files.write(listen_port, first.port(), true, true);
    auto config = edgegate::config::load_edgegate_config(files.yaml_path());
    RunningManagedProxy proxy(
        std::move(config), files.yaml_path(), files.socket_path());

    EXPECT_NE(proxy_get(proxy.port()).find("first"), std::string::npos);
    files.write(listen_port, second.port(), true, true);
    const auto successful = management_command(files.socket_path(), "reload");
    ASSERT_TRUE(successful.value("ok", false));
    EXPECT_EQ(successful["data"]["config_generation"], 2);
    EXPECT_NE(proxy_get(proxy.port()).find("second"), std::string::npos);

    files.write(listen_port, second.port(), false, true);
    EXPECT_FALSE(management_command(files.socket_path(), "reload")
                     .value("ok", true));
    EXPECT_NE(proxy_get(proxy.port()).find("second"), std::string::npos);

    const std::uint16_t different_port = reserve_free_port();
    files.write(different_port, first.port(), true, true);
    EXPECT_FALSE(management_command(files.socket_path(), "reload")
                     .value("ok", true));
    EXPECT_NE(proxy_get(proxy.port()).find("second"), std::string::npos);

    proxy.stop_and_join();
    EXPECT_TRUE(std::filesystem::exists(
        std::filesystem::path(files.log_directory()) / "access.log"));
    EXPECT_TRUE(std::filesystem::exists(
        std::filesystem::path(files.log_directory()) / "management.log"));
}

// 测试：上游一直不返回时，drain不会永远等待；到达500ms期限后必须强制
// 回收在途会话，同时保留管理Socket供随后stop使用。
TEST(RuntimeManagementIntegrationTest, DrainDeadlineForceClosesStuckSession)
{
    HoldingBackend backend;
    TemporaryRuntimeFiles files;
    const std::uint16_t listen_port = reserve_free_port();
    files.write(listen_port, backend.port());
    auto config = edgegate::config::load_edgegate_config(files.yaml_path());
    RunningManagedProxy proxy(
        std::move(config), files.yaml_path(), files.socket_path());

    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    timeval timeout{2, 0};
    static_cast<void>(::setsockopt(
        client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(proxy.port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address)), 0);
    send_all(client.get(), "GET / HTTP/1.1\r\nHost: api.test\r\n\r\n");
    ASSERT_TRUE(wait_until(
        [&files] {
            return management_command(files.socket_path(), "stats")
                       ["data"]["active_sessions"].get<unsigned long long>() > 0;
        },
        std::chrono::seconds(1)));

    ASSERT_TRUE(management_command(files.socket_path(), "drain")
                    .value("ok", false));
    char byte = 0;
    const ssize_t received = ::recv(client.get(), &byte, 1, 0);
    EXPECT_LE(received, 0);
    EXPECT_TRUE(wait_until(
        [&files] {
            return management_command(files.socket_path(), "status")
                       ["data"]["state"] == "drained";
        },
        std::chrono::seconds(2)));
    proxy.stop_and_join();
}

// AI-CODE-BEGIN: S9-DASHBOARD-PROXY-INTEGRATION-TEST
// 测试：真实代理请求完成后，独立 Dashboard 端口应同时给出页面、状态码、
// 路由/上游统计和最近错误；Dashboard 自己的访问不能计入代理请求数。
TEST(RuntimeManagementIntegrationTest, DashboardReflectsLiveProxyTraffic)
{
    TextBackend backend("dashboard-ok");
    TemporaryRuntimeFiles files;
    const std::uint16_t listen_port = reserve_free_port();
    const std::uint16_t dashboard_port = reserve_free_port();
    files.write(listen_port, backend.port(), true, false, dashboard_port);
    auto config = edgegate::config::load_edgegate_config(files.yaml_path());
    RunningManagedProxy proxy(
        std::move(config), files.yaml_path(), files.socket_path());

    EXPECT_NE(proxy_get(proxy.port()).find("dashboard-ok"), std::string::npos);
    EXPECT_EQ(proxy_get(proxy.port(), "missing.test").find(
                  "HTTP/1.1 404 Not Found\r\n"),
              0U);

    const std::string page = dashboard_get(dashboard_port, "/");
    EXPECT_EQ(page.find("HTTP/1.1 200 OK\r\n"), 0U);
    EXPECT_NE(page.find("EDGEGATE / OBSERVABILITY"), std::string::npos);

    const std::string api = dashboard_get(dashboard_port, "/api/dashboard");
    const std::size_t body_start = api.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    const auto snapshot = nlohmann::json::parse(api.substr(body_start + 4));
    EXPECT_EQ(snapshot["schema_version"], 1);
    EXPECT_EQ(snapshot["metrics"]["requests"]["total"], 2);
    EXPECT_EQ(snapshot["metrics"]["requests"]["status_codes"]["200"], 1);
    EXPECT_EQ(snapshot["metrics"]["requests"]["status_codes"]["404"], 1);
    EXPECT_FALSE(snapshot["metrics"]["recent_errors"].empty());
    ASSERT_EQ(snapshot["routes"].size(), 1U);
    ASSERT_EQ(snapshot["upstreams"].size(), 1U);
    ASSERT_EQ(snapshot["metrics"]["upstreams"].size(), 1U);
    EXPECT_EQ(snapshot["metrics"]["upstreams"][0]["successes"], 1);
    proxy.stop_and_join();
}
// AI-CODE-END: S9-DASHBOARD-PROXY-INTEGRATION-TEST

// 测试：单个日志超过配置容量后应保留当前文件和编号备份，证明使用了轮转
// sink，而不是让日志无限增长。
TEST(RuntimeManagementIntegrationTest, RotatesRuntimeLogs)
{
    TemporaryRuntimeFiles files;
    edgegate::config::LoggingConfig config;
    config.enabled = true;
    config.directory = files.log_directory();
    config.max_file_size = 256;
    config.max_files = 2;

    {
        edgegate::runtime::RuntimeLogger logger(config);
        const std::string detail(180, 'x');
        for (int index = 0; index < 100; ++index) {
            logger.error("rotation_test", detail);
        }
        logger.flush();
        EXPECT_TRUE(wait_until(
            [&files] {
                return std::filesystem::exists(
                    std::filesystem::path(files.log_directory()) /
                    "error.1.log");
            },
            std::chrono::seconds(2)));
    }
}

} // namespace
// AI-CODE-END: S8-RUNTIME-MANAGEMENT-INTEGRATION-TESTS
