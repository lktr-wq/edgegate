#include "edgegate/proxy/proxy_server.h"

// AI-CODE-BEGIN: S5-PROXY-INTEGRATION-TESTS
#include "edgegate/http/request_parser.h"
#include "edgegate/http/response_parser.h"
#include "edgegate/net/unique_fd.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using edgegate::http::ParseStatus;
using edgegate::http::RequestParser;
using edgegate::http::ResponseParser;
using edgegate::net::UniqueFd;
using edgegate::proxy::ProxyConfig;
using edgegate::proxy::ProxyServer;

void send_all(int fd, std::string_view bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t sent = ::send(
            fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("test send failed");
    }
}

UniqueFd make_tcp_listener(std::uint16_t& port)
{
    UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listener) {
        throw std::runtime_error("test listener socket failed");
    }

    const int reuse = 1;
    if (::setsockopt(
            listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ==
        -1) {
        throw std::runtime_error("test listener setsockopt failed");
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    static_cast<void>(::setsockopt(
        listener.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1 ||
        ::listen(listener.get(), 16) == -1) {
        throw std::runtime_error("test listener bind/listen failed");
    }

    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(
            listener.get(),
            reinterpret_cast<sockaddr*>(&bound),
            &length) == -1) {
        throw std::runtime_error("test listener getsockname failed");
    }
    port = ntohs(bound.sin_port);
    return listener;
}

class ScriptedUpstream {
public:
    explicit ScriptedUpstream(std::vector<std::string> responses)
        : listener_(make_tcp_listener(port_)),
          responses_(std::move(responses)),
          thread_([this] { serve(); })
    {
    }

    ~ScriptedUpstream()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept
    {
        return port_;
    }

    void wait()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::vector<std::string> requests() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void serve()
    {
        try {
            for (const std::string& response : responses_) {
                UniqueFd client(::accept4(
                    listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
                if (!client) {
                    throw std::runtime_error("upstream accept failed");
                }

                RequestParser parser;
                std::array<char, 4096> bytes{};
                while (true) {
                    const ssize_t received =
                        ::recv(client.get(), bytes.data(), bytes.size(), 0);
                    if (received <= 0) {
                        throw std::runtime_error("upstream request read failed");
                    }
                    const auto result = parser.consume(std::string_view(
                        bytes.data(), static_cast<std::size_t>(received)));
                    if (result.status == ParseStatus::kError) {
                        throw std::runtime_error("upstream got invalid request");
                    }
                    if (result.status == ParseStatus::kMessageComplete) {
                        break;
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.emplace_back(parser.raw_message());
                }
                if (!response.empty()) {
                    send_all(client.get(), response);
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
        }
    }

    std::uint16_t port_{0};
    UniqueFd listener_;
    std::vector<std::string> responses_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    std::exception_ptr failure_;
    std::thread thread_;
};

class RunningProxy {
public:
    explicit RunningProxy(std::uint16_t upstream_port)
        : server_("127.0.0.1", 0, make_config(upstream_port)),
          thread_([this] { run(); })
    {
    }

    ~RunningProxy()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept
    {
        return server_.port();
    }

    const std::shared_ptr<edgegate::proxy::ProxyStats>& stats() const noexcept
    {
        return server_.stats();
    }

private:
    static ProxyConfig make_config(std::uint16_t upstream_port)
    {
        ProxyConfig config;
        config.upstream_port = upstream_port;
        return config;
    }

    void run()
    {
        while (!stopping_.load()) {
            static_cast<void>(server_.run_once(10));
        }
    }

    ProxyServer server_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

UniqueFd connect_client(std::uint16_t port)
{
    UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!client) {
        throw std::runtime_error("client socket failed");
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    static_cast<void>(::setsockopt(
        client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            client.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        throw std::runtime_error("client connect failed");
    }
    return client;
}

std::size_t count_open_file_descriptors()
{
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
        throw std::runtime_error("opendir /proc/self/fd failed");
    }
    std::size_t count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string_view name(entry->d_name);
        if (name != "." && name != "..") {
            ++count;
        }
    }
    static_cast<void>(::closedir(directory));
    return count;
}

bool wait_for_active_sessions(
    const RunningProxy& proxy,
    std::uint64_t expected,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (proxy.stats()->active_sessions.load() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return proxy.stats()->active_sessions.load() == expected;
}

std::string receive_response(int fd, std::string method = "GET")
{
    ResponseParser parser(std::move(method));
    std::array<char, 4096> bytes{};

    while (true) {
        const ssize_t received = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            const auto result = parser.consume(std::string_view(
                bytes.data(), static_cast<std::size_t>(received)));
            if (result.status == ParseStatus::kError) {
                throw std::runtime_error("client got invalid response");
            }
            if (result.status == ParseStatus::kMessageComplete) {
                return std::string(parser.raw_message());
            }
            continue;
        }
        if (received == 0) {
            const auto result = parser.notify_eof();
            if (result.status == ParseStatus::kMessageComplete) {
                return std::string(parser.raw_message());
            }
            throw std::runtime_error("client got premature EOF");
        }
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error("client receive failed");
    }
}

const std::string kGet =
    "GET /hello HTTP/1.1\r\nHost: example.test\r\n\r\n";

// 测试：代理应把客户端请求原样发给上游，并把 Content-Length 响应完整返回客户端。
TEST(ProxyIntegrationTest, ForwardsContentLengthResponse)
{
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    ScriptedUpstream upstream({response});
    RunningProxy proxy(upstream.port());
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(), kGet);
    EXPECT_EQ(receive_response(client.get()), response);
    upstream.wait();
    ASSERT_EQ(upstream.requests().size(), 1U);
    EXPECT_EQ(upstream.requests()[0], kGet);
    EXPECT_EQ(proxy.stats()->completed_requests.load(), 1U);
}

// 测试：同一客户端的顺序请求应能分别接收 chunked 响应和以上游关闭为边界的响应。
TEST(ProxyIntegrationTest, ForwardsChunkedAndCloseDelimitedResponses)
{
    const std::string chunked =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    const std::string close_delimited =
        "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nbye";
    ScriptedUpstream upstream({chunked, close_delimited});
    RunningProxy proxy(upstream.port());
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(), kGet);
    EXPECT_EQ(receive_response(client.get()), chunked);

    send_all(client.get(), kGet);
    EXPECT_EQ(receive_response(client.get()), close_delimited);
    upstream.wait();
    EXPECT_EQ(proxy.stats()->completed_requests.load(), 2U);
}

// 测试：客户端 Keep-Alive 连接应支持顺序发送两次请求，并在第二个 close 响应后结束连接。
TEST(ProxyIntegrationTest, SupportsSequentialClientKeepAlive)
{
    const std::string first =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none";
    const std::string second =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
        "Connection: close\r\n\r\ntwo";
    ScriptedUpstream upstream({first, second});
    RunningProxy proxy(upstream.port());
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(), kGet);
    EXPECT_EQ(receive_response(client.get()), first);
    send_all(client.get(), kGet);
    EXPECT_EQ(receive_response(client.get()), second);

    char byte = 0;
    EXPECT_EQ(::recv(client.get(), &byte, 1, 0), 0);
    upstream.wait();
}

// 测试：客户端发送缺少 Host 的畸形 HTTP/1.1 请求时，代理应返回 400 并累计客户端错误。
TEST(ProxyIntegrationTest, Returns400ForMalformedRequest)
{
    std::uint16_t unused_port = 0;
    UniqueFd reservation = make_tcp_listener(unused_port);
    RunningProxy proxy(unused_port);
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(), "GET / HTTP/1.1\r\n\r\n");
    const std::string response = receive_response(client.get());
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), 0U);
    EXPECT_EQ(proxy.stats()->client_errors.load(), 1U);
}

// 测试：上游拒绝连接或在响应完成前关闭时，代理都应向客户端返回 502 Bad Gateway。
TEST(ProxyIntegrationTest, Returns502WhenUpstreamRefusesOrClosesEarly)
{
    std::uint16_t refused_port = 0;
    {
        UniqueFd reservation = make_tcp_listener(refused_port);
    }

    RunningProxy refused_proxy(refused_port);
    UniqueFd refused_client = connect_client(refused_proxy.port());
    send_all(refused_client.get(), kGet);
    EXPECT_EQ(
        receive_response(refused_client.get()).find(
            "HTTP/1.1 502 Bad Gateway\r\n"),
        0U);

    ScriptedUpstream closing_upstream({""});
    RunningProxy closing_proxy(closing_upstream.port());
    UniqueFd closing_client = connect_client(closing_proxy.port());
    send_all(closing_client.get(), kGet);
    EXPECT_EQ(
        receive_response(closing_client.get()).find(
            "HTTP/1.1 502 Bad Gateway\r\n"),
        0U);
    closing_upstream.wait();
}

// 测试：代理应完整转发 Content-Length 请求体，并正确识别 HEAD 响应没有实际正文。
TEST(ProxyIntegrationTest, ForwardsContentLengthRequestBodyAndHeadResponse)
{
    const std::string post_response =
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
    const std::string head_response =
        "HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n";
    ScriptedUpstream upstream({post_response, head_response});
    RunningProxy proxy(upstream.port());
    UniqueFd client = connect_client(proxy.port());

    const std::string post =
        "POST /data HTTP/1.1\r\nHost: example.test\r\n"
        "Content-Length: 4\r\n\r\ndata";
    send_all(client.get(), post);
    EXPECT_EQ(receive_response(client.get()), post_response);

    const std::string head =
        "HEAD /data HTTP/1.1\r\nHost: example.test\r\n\r\n";
    send_all(client.get(), head);
    EXPECT_EQ(receive_response(client.get(), "HEAD"), head_response);

    upstream.wait();
    ASSERT_EQ(upstream.requests().size(), 2U);
    EXPECT_EQ(upstream.requests()[0], post);
    EXPECT_EQ(upstream.requests()[1], head);
}

// 测试：客户端关闭发送方向但仍保留接收方向时，代理应继续完成响应并正常关闭会话。
TEST(ProxyIntegrationTest, CompletesResponseAfterClientHalfClose)
{
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    ScriptedUpstream upstream({response});
    RunningProxy proxy(upstream.port());
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(), kGet);
    ASSERT_EQ(::shutdown(client.get(), SHUT_WR), 0);
    EXPECT_EQ(receive_response(client.get()), response);

    char byte = 0;
    EXPECT_EQ(::recv(client.get(), &byte, 1, 0), 0);
    upstream.wait();
}

// 测试：上游返回无法解析的非 HTTP 数据时，代理应返回 502 并累计上游错误。
TEST(ProxyIntegrationTest, RejectsMalformedUpstreamResponseWith502)
{
    ScriptedUpstream upstream({"NOT-HTTP\r\n\r\n"});
    RunningProxy proxy(upstream.port());
    UniqueFd client = connect_client(proxy.port());

    send_all(client.get(), kGet);
    EXPECT_EQ(
        receive_response(client.get()).find(
            "HTTP/1.1 502 Bad Gateway\r\n"),
        0U);
    upstream.wait();
    EXPECT_EQ(proxy.stats()->upstream_errors.load(), 1U);
}

// 测试：16 个客户端同时建立代理会话时，每个客户端都应独立收到正确响应。
TEST(ProxyIntegrationTest, ServesConcurrentProxySessions)
{
    constexpr int client_count = 16;
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Connection: close\r\n\r\nok";
    ScriptedUpstream upstream(
        std::vector<std::string>(client_count, response));
    RunningProxy proxy(upstream.port());
    std::vector<std::future<std::string>> clients;

    for (int index = 0; index < client_count; ++index) {
        clients.push_back(std::async(
            std::launch::async,
            [&proxy, index] {
                UniqueFd client = connect_client(proxy.port());
                const std::string request =
                    "GET /" + std::to_string(index) +
                    " HTTP/1.1\r\nHost: example.test\r\n"
                    "Connection: close\r\n\r\n";
                send_all(client.get(), request);
                return receive_response(client.get());
            }));
    }

    for (auto& client : clients) {
        EXPECT_EQ(client.get(), response);
    }
    upstream.wait();
    EXPECT_EQ(proxy.stats()->completed_requests.load(), client_count);
}

// 测试：连续完成 40 次代理会话并销毁服务后，打开的 fd 数量应恢复到测试前水平。
TEST(ProxyIntegrationTest, ReleasesAllDescriptorsAfterRepeatedSessions)
{
    const std::size_t descriptors_before = count_open_file_descriptors();

    {
        constexpr int session_count = 40;
        const std::string response =
            "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        ScriptedUpstream upstream(
            std::vector<std::string>(session_count, response));
        {
            RunningProxy proxy(upstream.port());
            for (int index = 0; index < session_count; ++index) {
                UniqueFd client = connect_client(proxy.port());
                send_all(
                    client.get(),
                    "GET / HTTP/1.1\r\nHost: example.test\r\n"
                    "Connection: close\r\n\r\n");
                EXPECT_EQ(receive_response(client.get()), response);
            }
            upstream.wait();
            EXPECT_TRUE(wait_for_active_sessions(
                proxy, 0, std::chrono::milliseconds(1000)));
        }
    }

    EXPECT_EQ(count_open_file_descriptors(), descriptors_before);
}

} // namespace
// AI-CODE-END: S5-PROXY-INTEGRATION-TESTS
