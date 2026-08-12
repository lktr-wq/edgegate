#include "edgegate/net/reactor_echo_server.h"

// AI-CODE-BEGIN: S4-REACTOR-INTEGRATION-TESTS
#include "edgegate/net/unique_fd.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <future>
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

using edgegate::net::BufferWatermarks;
using edgegate::net::ReactorEchoServer;
using edgegate::net::UniqueFd;

class RunningServer {
public:
    explicit RunningServer(BufferWatermarks watermarks = {})
        : server_("127.0.0.1", 0, watermarks),
          thread_([this] { run(); })
    {
    }

    ~RunningServer()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    std::uint16_t port() const noexcept
    {
        return server_.port();
    }

    const std::shared_ptr<edgegate::net::ReactorStats>& stats() const noexcept
    {
        return server_.stats();
    }

private:
    void run()
    {
        try {
            while (!stopping_.load()) {
                static_cast<void>(server_.run_once(10));
            }
        } catch (...) {
            failure_ = std::current_exception();
        }
    }

    ReactorEchoServer server_;
    std::atomic<bool> stopping_{false};
    std::exception_ptr failure_;
    std::thread thread_;
};

UniqueFd connect_client(std::uint16_t port)
{
    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket) {
        throw std::runtime_error("socket client failed");
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    if (::setsockopt(
            socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
        -1) {
        throw std::runtime_error("setsockopt receive timeout failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        throw std::runtime_error("connect client failed");
    }
    return socket;
}

void send_all(int fd, std::string_view bytes)
{
    std::size_t sent_total = 0;
    while (sent_total < bytes.size()) {
        const ssize_t sent = ::send(
            fd,
            bytes.data() + sent_total,
            bytes.size() - sent_total,
            MSG_NOSIGNAL);
        if (sent > 0) {
            sent_total += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("send_all failed");
    }
}

std::string receive_exact(int fd, std::size_t expected)
{
    std::string received;
    received.reserve(expected);
    std::array<char, 4096> chunk{};

    while (received.size() < expected) {
        const std::size_t capacity = std::min(
            chunk.size(), expected - received.size());
        const ssize_t count = ::recv(fd, chunk.data(), capacity, 0);
        if (count > 0) {
            received.append(chunk.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("receive_exact failed");
    }
    return received;
}

bool wait_for_active_connections(
    const RunningServer& server,
    std::uint64_t expected,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.stats()->active_connections.load() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return server.stats()->active_connections.load() == expected;
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

TEST(ReactorEchoIntegrationTest, EchoesMultipleSequentialMessages)
{
    RunningServer server;
    UniqueFd client = connect_client(server.port());

    for (const std::string message : {"hello", " reactor ", "world"}) {
        send_all(client.get(), message);
        EXPECT_EQ(receive_exact(client.get(), message.size()), message);
    }

    client.reset();
    EXPECT_TRUE(wait_for_active_connections(
        server, 0, std::chrono::milliseconds(500)));
    EXPECT_EQ(server.stats()->closed_connections.load(), 1U);
}

TEST(ReactorEchoIntegrationTest, ServesConcurrentClients)
{
    RunningServer server;
    constexpr int client_count = 24;
    std::vector<std::future<std::string>> clients;

    for (int index = 0; index < client_count; ++index) {
        clients.push_back(std::async(
            std::launch::async,
            [&server, index] {
                UniqueFd client = connect_client(server.port());
                const std::string message =
                    "client-" + std::to_string(index) +
                    std::string(4096, static_cast<char>('a' + index % 26));
                send_all(client.get(), message);
                return receive_exact(client.get(), message.size());
            }));
    }

    for (int index = 0; index < client_count; ++index) {
        const std::string expected =
            "client-" + std::to_string(index) +
            std::string(4096, static_cast<char>('a' + index % 26));
        EXPECT_EQ(clients[static_cast<std::size_t>(index)].get(), expected);
    }
    EXPECT_EQ(server.stats()->accepted_connections.load(), client_count);
    EXPECT_TRUE(wait_for_active_connections(
        server, 0, std::chrono::milliseconds(500)));
    EXPECT_EQ(server.stats()->closed_connections.load(), client_count);
}

TEST(ReactorEchoIntegrationTest, SlowClientDoesNotBlockFastClient)
{
    RunningServer server;
    UniqueFd slow = connect_client(server.port());

    // slow 只发一半且保持连接；阻塞式“读到完整消息再处理”会卡在这里。
    send_all(slow.get(), "unfinished-slow-client");

    auto fast_result = std::async(std::launch::async, [&server] {
        UniqueFd fast = connect_client(server.port());
        const std::string message = "fast-client-still-progresses";
        send_all(fast.get(), message);
        return receive_exact(fast.get(), message.size());
    });

    EXPECT_EQ(
        fast_result.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    EXPECT_EQ(fast_result.get(), "fast-client-still-progresses");
}

TEST(ReactorEchoIntegrationTest, PreservesLargePayloadUnderBackpressure)
{
    const BufferWatermarks watermarks{
        64 * 1024,
        48 * 1024,
        16 * 1024};
    RunningServer server(watermarks);
    UniqueFd client = connect_client(server.port());

    std::string payload;
    payload.resize(512 * 1024);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>('A' + index % 26);
    }

    auto sender = std::async(std::launch::async, [&client, &payload] {
        send_all(client.get(), payload);
    });

    // 暂停读取，让服务端输出缓冲先达到高水位，然后再开始排空。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::string echoed = receive_exact(client.get(), payload.size());
    sender.get();

    EXPECT_EQ(echoed, payload);
    EXPECT_GE(server.stats()->read_pauses.load(), 1U);
    EXPECT_EQ(server.stats()->socket_errors.load(), 0U);
}

TEST(ReactorEchoIntegrationTest, ReleasesDescriptorsAfterServerDestruction)
{
    const std::size_t descriptors_before = count_open_file_descriptors();

    {
        RunningServer server;
        for (int index = 0; index < 50; ++index) {
            UniqueFd client = connect_client(server.port());
            const std::string message = "fd-cycle-" + std::to_string(index);
            send_all(client.get(), message);
            EXPECT_EQ(receive_exact(client.get(), message.size()), message);
        }

        EXPECT_TRUE(wait_for_active_connections(
            server, 0, std::chrono::milliseconds(1000)));
        EXPECT_EQ(server.stats()->accepted_connections.load(), 50U);
        EXPECT_EQ(server.stats()->closed_connections.load(), 50U);
    }

    EXPECT_EQ(count_open_file_descriptors(), descriptors_before);
}

} // namespace
// AI-CODE-END: S4-REACTOR-INTEGRATION-TESTS
