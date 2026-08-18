#include "edgegate/http/response_parser.h"
#include "edgegate/net/unique_fd.h"
#include "edgegate/proxy/reliable_proxy_server.h"

// AI-CODE-BEGIN: S7-RELIABLE-PROXY-INTEGRATION-TESTS
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

using edgegate::http::ParseStatus;
using edgegate::http::ResponseParser;
using edgegate::net::UniqueFd;
using edgegate::proxy::ReliableProxyServer;
using edgegate::proxy::ReliableProxyStats;

void send_all(int fd, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent == -1 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

UniqueFd make_listener(std::uint16_t& port)
{
    UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    int reuse = 1;
    static_cast<void>(::setsockopt(
        listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!listener) {
        throw std::runtime_error("test socket failed");
    }
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == -1 || ::listen(listener.get(), 32) == -1) {
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

class FlexibleBackend {
public:
    using Responder = std::function<std::string(std::string_view)>;

    explicit FlexibleBackend(Responder responder)
        : listener_(make_listener(port_)), responder_(std::move(responder)),
          thread_([this] { serve(); })
    {
    }

    ~FlexibleBackend()
    {
        stopping_.store(true);
        ::shutdown(listener_.get(), SHUT_RDWR);
        listener_.reset();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }

private:
    void serve()
    {
        while (!stopping_.load()) {
            UniqueFd client(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!client) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            std::string request;
            std::array<char, 4096> bytes{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(
                    client.get(), bytes.data(), bytes.size(), 0);
                if (received <= 0) {
                    break;
                }
                request.append(bytes.data(), static_cast<std::size_t>(received));
            }
            const std::size_t header_end = request.find("\r\n\r\n");
            std::size_t content_length = 0;
            const std::size_t length_field = request.find("Content-Length:");
            if (length_field != std::string::npos &&
                length_field < header_end) {
                const std::size_t value_start = length_field + 15;
                const std::size_t value_end = request.find("\r\n", value_start);
                content_length = static_cast<std::size_t>(std::stoull(
                    request.substr(value_start, value_end - value_start)));
            }
            while (header_end != std::string::npos &&
                   request.size() < header_end + 4 + content_length) {
                const ssize_t received = ::recv(
                    client.get(), bytes.data(), bytes.size(), 0);
                if (received <= 0) {
                    break;
                }
                request.append(bytes.data(), static_cast<std::size_t>(received));
            }
            const std::string response = responder_(request);
            send_all(client.get(), response);
        }
    }

    std::uint16_t port_{0};
    UniqueFd listener_;
    Responder responder_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

edgegate::config::EdgeGateConfig config_for(
    std::vector<edgegate::routing::UpstreamEndpoint> endpoints)
{
    edgegate::config::EdgeGateConfig config;
    config.listen_port = 0;
    config.routes = {{"api", "api.test", "/", std::move(endpoints)}};
    config.health_check.interval_ms = 60000;
    config.timeouts.io_idle_ms = 5000;
    return config;
}

class RunningReliableProxy {
public:
    explicit RunningReliableProxy(edgegate::config::EdgeGateConfig config)
        : server_(std::move(config)), thread_([this] {
              while (!stopping_.load()) {
                  static_cast<void>(server_.run_once(10));
              }
          })
    {
    }

    ~RunningReliableProxy()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return server_.port(); }
    const std::shared_ptr<ReliableProxyStats>& stats() const noexcept
    {
        return server_.stats();
    }

private:
    ReliableProxyServer server_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

UniqueFd connect_client(std::uint16_t port, bool tiny_receive_buffer = false)
{
    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    timeval timeout{5, 0};
    static_cast<void>(::setsockopt(
        client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    if (tiny_receive_buffer) {
        int size = 4096;
        static_cast<void>(::setsockopt(
            client.get(), SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        throw std::runtime_error("test client connect failed");
    }
    return client;
}

std::string receive_response(int fd, std::string method = "GET")
{
    ResponseParser parser(std::move(method), 8192, 10 * 1024 * 1024);
    std::string observed_wire;
    std::array<char, 16384> bytes{};
    for (;;) {
        const ssize_t received = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            observed_wire.append(bytes.data(), static_cast<std::size_t>(received));
            const auto result = parser.consume(std::string_view(
                bytes.data(), static_cast<std::size_t>(received)));
            if (result.status == ParseStatus::kMessageComplete) {
                return std::string(parser.raw_message());
            }
            if (result.status == ParseStatus::kError) {
                throw std::runtime_error("invalid proxied response");
            }
        } else if (received == 0) {
            const auto result = parser.notify_eof();
            if (result.status == ParseStatus::kMessageComplete) {
                return std::string(parser.raw_message());
            }
            throw std::runtime_error(
                "premature response EOF after: " + observed_wire);
        } else if (errno != EINTR) {
            throw std::runtime_error("response receive failed");
        }
    }
}

bool wait_until(const std::function<bool()>& condition,
                std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// 测试：4 MiB 响应大于32 KiB应用缓冲区，慢客户端会触发暂停和恢复，但字节不丢失。
TEST(ReliableProxyIntegrationTest, StreamsLargeResponseWithObservableBackpressure)
{
    const std::string body(4 * 1024 * 1024, 'x');
    FlexibleBackend backend([&body](std::string_view) {
        return "HTTP/1.1 200 OK\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    });
    auto config = config_for({{"one", "127.0.0.1", backend.port(), true}});
    config.stream_buffer = {32768, 24576, 8192};
    RunningReliableProxy proxy(std::move(config));
    UniqueFd client = connect_client(proxy.port(), true);
    send_all(client.get(), "GET /large HTTP/1.1\r\nHost: api.test\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const std::string response = receive_response(client.get());
    EXPECT_EQ(response.substr(response.size() - body.size()), body);
    EXPECT_GT(proxy.stats()->backpressure_pauses.load(), 0U);
    EXPECT_GT(proxy.stats()->backpressure_resumes.load(), 0U);
}

// 测试：512 KiB POST 大于32 KiB方向缓冲，证明请求体也是边读边发而非整包塞入缓冲。
TEST(ReliableProxyIntegrationTest, StreamsRequestBodyLargerThanBuffer)
{
    constexpr std::size_t body_size = 512 * 1024;
    FlexibleBackend backend([](std::string_view request) {
        const std::size_t body = request.find("\r\n\r\n") + 4;
        const std::string count = std::to_string(request.size() - body);
        return "HTTP/1.1 200 OK\r\nContent-Length: " +
               std::to_string(count.size()) + "\r\n\r\n" + count;
    });
    auto config = config_for({{"one", "127.0.0.1", backend.port(), true}});
    config.stream_buffer = {32768, 24576, 8192};
    RunningReliableProxy proxy(std::move(config));
    UniqueFd client = connect_client(proxy.port());
    const std::string body(body_size, 'p');
    const std::string head =
        "POST /upload HTTP/1.1\r\nHost: api.test\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n";
    send_all(client.get(), head);
    send_all(client.get(), body);
    EXPECT_NE(receive_response(client.get()).find(
                  "\r\n\r\n" + std::to_string(body_size)),
              std::string::npos);
}

// 测试：GET 在第一个节点连接失败且尚未向客户端发出响应时，只重试一次不同节点。
TEST(ReliableProxyIntegrationTest, SafelyRetriesGetBeforeResponseStarts)
{
    std::uint16_t refused_port = 0;
    { UniqueFd reservation = make_listener(refused_port); }
    FlexibleBackend healthy([](std::string_view) {
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    });
    RunningReliableProxy proxy(config_for({
        {"broken", "127.0.0.1", refused_port, true},
        {"healthy", "127.0.0.1", healthy.port(), true}}));
    UniqueFd client = connect_client(proxy.port());
    send_all(client.get(), "GET / HTTP/1.1\r\nHost: api.test\r\n\r\n");
    EXPECT_NE(receive_response(client.get()).find("\r\n\r\nok"),
              std::string::npos);
    EXPECT_EQ(proxy.stats()->retries.load(), 1U);
}

// 测试：带请求体语义的 POST 即使正文长度为0也不自动换节点，避免重复副作用。
TEST(ReliableProxyIntegrationTest, DoesNotRetryPostRequest)
{
    std::uint16_t refused_port = 0;
    { UniqueFd reservation = make_listener(refused_port); }
    FlexibleBackend healthy([](std::string_view) {
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    });
    RunningReliableProxy proxy(config_for({
        {"broken", "127.0.0.1", refused_port, true},
        {"healthy", "127.0.0.1", healthy.port(), true}}));
    UniqueFd client = connect_client(proxy.port());
    send_all(client.get(),
        "POST / HTTP/1.1\r\nHost: api.test\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(receive_response(client.get()).find(
                  "HTTP/1.1 502 Bad Gateway\r\n"), 0U);
    EXPECT_EQ(proxy.stats()->retries.load(), 0U);
}

// 测试：新代理保留 chunk framing；关闭定界响应则强制客户端 Connection: close。
TEST(ReliableProxyIntegrationTest, StreamsChunkedAndCloseDelimitedResponses)
{
    FlexibleBackend backend([](std::string_view request) {
        if (request.find("GET /chunked ") != std::string_view::npos) {
            return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "5\r\nhello\r\n0\r\n\r\n";
        }
        return "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nbye";
    });
    RunningReliableProxy proxy(config_for({
        {"node", "127.0.0.1", backend.port(), true}}));
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(),
        "GET /chunked HTTP/1.1\r\nHost: api.test\r\n\r\n");
    const std::string chunked = receive_response(client.get());
    EXPECT_NE(chunked.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
    EXPECT_NE(chunked.find("5\r\nhello\r\n0\r\n\r\n"), std::string::npos);

    send_all(client.get(),
        "GET /close HTTP/1.1\r\nHost: api.test\r\n\r\n");
    const std::string closed = receive_response(client.get());
    EXPECT_NE(closed.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(closed.substr(closed.size() - 3), "bye");
}

// 测试：上游在2 MiB声明正文中只发1 MiB后断开；客户端已收到部分200时不能再拼502。
TEST(ReliableProxyIntegrationTest, ClosesInsteadOfAppendingErrorAfterPartialResponse)
{
    const std::string partial(1024 * 1024, 'z');
    FlexibleBackend backend([&partial](std::string_view) {
        return "HTTP/1.1 200 OK\r\nContent-Length: 2097152\r\n\r\n" + partial;
    });
    auto config = config_for({{"one", "127.0.0.1", backend.port(), true}});
    config.stream_buffer = {32768, 24576, 8192};
    RunningReliableProxy proxy(std::move(config));
    UniqueFd client = connect_client(proxy.port(), true);
    send_all(client.get(), "GET /partial HTTP/1.1\r\nHost: api.test\r\n\r\n");

    std::string observed;
    std::array<char, 16384> bytes{};
    for (;;) {
        const ssize_t received = ::recv(client.get(), bytes.data(), bytes.size(), 0);
        if (received > 0) {
            observed.append(bytes.data(), static_cast<std::size_t>(received));
        } else if (received == 0) {
            break;
        } else if (errno != EINTR) {
            throw std::runtime_error("partial response receive failed");
        }
    }
    EXPECT_EQ(observed.find("HTTP/1.1 200 OK\r\n"), 0U);
    EXPECT_EQ(observed.find("502 Bad Gateway"), std::string::npos);
    // 故障后代理立即关闭，因此应用缓冲中尚未 send() 的尾部允许丢弃；
    // 这里证明客户端确实已收到“部分响应”，不要求把代理尚未发出的尾部冲完。
    EXPECT_GT(observed.size(), 64 * 1024U);
}

// 测试：上游在响应头超时前没有返回任何字节时，客户端得到明确的504。
TEST(ReliableProxyIntegrationTest, Returns504ForUpstreamHeaderTimeout)
{
    FlexibleBackend slow([](std::string_view) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    });
    auto config = config_for({{"slow", "127.0.0.1", slow.port(), true}});
    config.timeouts.upstream_header_ms = 100;
    RunningReliableProxy proxy(std::move(config));
    UniqueFd client = connect_client(proxy.port());
    send_all(client.get(), "POST / HTTP/1.1\r\nHost: api.test\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(receive_response(client.get()).find(
                  "HTTP/1.1 504 Gateway Timeout\r\n"), 0U);
    EXPECT_EQ(proxy.stats()->upstream_timeouts.load(), 1U);
}

// 测试：主动 /health 连续失败会摘除节点并返回503；恢复为2xx后节点重新加入路由。
TEST(ReliableProxyIntegrationTest, ActiveHealthCheckExcludesAndRecoversEndpoint)
{
    std::atomic<bool> healthy{false};
    FlexibleBackend backend([&healthy](std::string_view request) {
        if (request.find("GET /health ") != std::string_view::npos) {
            return healthy.load()
                ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"
                : "HTTP/1.1 500 Down\r\nContent-Length: 0\r\n\r\n";
        }
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    });
    auto config = config_for({{"node", "127.0.0.1", backend.port(), true}});
    config.health_check.interval_ms = 100;
    config.health_check.timeout_ms = 300;
    config.health_check.failure_threshold = 1;
    config.health_check.success_threshold = 1;
    RunningReliableProxy proxy(std::move(config));

    ASSERT_TRUE(wait_until([&proxy] {
        return proxy.stats()->health_transitions.load() >= 1;
    }, std::chrono::seconds(2)));
    {
        UniqueFd client = connect_client(proxy.port());
        send_all(client.get(), "GET / HTTP/1.1\r\nHost: api.test\r\n\r\n");
        EXPECT_EQ(receive_response(client.get()).find(
                      "HTTP/1.1 503 Service Unavailable\r\n"), 0U);
    }

    healthy.store(true);
    ASSERT_TRUE(wait_until([&proxy] {
        return proxy.stats()->health_transitions.load() >= 2;
    }, std::chrono::seconds(2)));
    UniqueFd client = connect_client(proxy.port());
    send_all(client.get(), "GET / HTTP/1.1\r\nHost: api.test\r\n\r\n");
    EXPECT_NE(receive_response(client.get()).find("\r\n\r\nok"),
              std::string::npos);
}

// AI-CODE-BEGIN: S11-NORMAL-KEEPALIVE-CLOSE-REGRESSION
// 测试：客户端完成 Keep-Alive 请求后正常 close，连接应被回收但不能误报 client_errors。
TEST(ReliableProxyIntegrationTest, NormalKeepAliveCloseIsNotClientError)
{
    FlexibleBackend backend([](std::string_view) {
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    });
    RunningReliableProxy proxy(config_for({
        {"node", "127.0.0.1", backend.port(), true}}));
    {
        UniqueFd client = connect_client(proxy.port());
        send_all(client.get(),
            "GET / HTTP/1.1\r\nHost: api.test\r\nConnection: keep-alive\r\n\r\n");
        EXPECT_NE(receive_response(client.get()).find("\r\n\r\nok"),
                  std::string::npos);
        ASSERT_TRUE(wait_until([&proxy] {
            return proxy.stats()->completed_requests.load() == 1;
        }, std::chrono::seconds(1)));
    }

    ASSERT_TRUE(wait_until([&proxy] {
        return proxy.stats()->active_sessions.load() == 0;
    }, std::chrono::seconds(1)));
    EXPECT_EQ(proxy.stats()->client_errors.load(), 0U);
}
// AI-CODE-END: S11-NORMAL-KEEPALIVE-CLOSE-REGRESSION

} // namespace
// AI-CODE-END: S7-RELIABLE-PROXY-INTEGRATION-TESTS
