// AI-CODE-BEGIN: S11-LOAD-GENERATOR
/*
 * EdgeGate 阶段11客户端压测器。
 *
 * 输入：目标地址、总请求数和并发客户端数。
 * 处理：多个线程各自保持一条 HTTP/1.1 连接，顺序发送并校验响应。
 * 输出：成功/失败数量、状态码、QPS 和客户端实测 P50/P95/P99 JSON。
 *
 * 注意：这里的线程属于“压测客户端”，不是 EdgeGate 服务线程。
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kReadChunkSize = 8192;
constexpr std::size_t kMaxResponseHeaderSize = 65536;
constexpr std::size_t kFailureExampleLimit = 20;

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int descriptor) noexcept : descriptor_(descriptor) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : descriptor_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return descriptor_ >= 0;
    }

    void reset(int replacement = -1) noexcept
    {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = replacement;
    }

    [[nodiscard]] int release() noexcept
    {
        const int result = descriptor_;
        descriptor_ = -1;
        return result;
    }

private:
    int descriptor_{-1};
};

struct Options {
    std::string address{"127.0.0.1"};
    std::uint16_t port{18080};
    std::string host{"api.edgegate.test"};
    std::string path{"/api/benchmark"};
    std::uint64_t requests{100000};
    std::size_t concurrency{100};
    std::uint32_t timeout_ms{5000};
    std::string output_path;
};

struct HttpResponse {
    int status{0};
    std::string body;
    bool close_connection{false};
};

struct SharedResults {
    explicit SharedResults(std::size_t request_count)
        : latency_us(request_count, 0)
    {
    }

    std::atomic<std::uint64_t> next_request{0};
    std::atomic<std::uint64_t> successes{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> transport_failures{0};
    std::atomic<std::uint64_t> invalid_bodies{0};
    std::atomic<std::uint64_t> reconnects{0};
    std::vector<std::uint64_t> latency_us;
    std::mutex details_mutex;
    std::map<int, std::uint64_t> status_codes;
    std::vector<std::string> failure_examples;
};

[[noreturn]] void usage_error(std::string_view detail)
{
    throw std::invalid_argument(
        std::string(detail) +
        "\nUsage: edgegate_loadgen [--address IPv4] [--port PORT]"
        " [--host HOST] [--path PATH] [--requests N]"
        " [--concurrency N] [--timeout-ms N] [--output FILE]");
}

std::uint64_t parse_unsigned(
    std::string_view text,
    std::uint64_t minimum,
    std::uint64_t maximum,
    std::string_view name)
{
    if (text.empty()) {
        usage_error(std::string(name) + " must not be empty");
    }
    std::uint64_t result = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            usage_error(std::string(name) + " must be an integer");
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (result > (maximum - digit) / 10) {
            usage_error(std::string(name) + " is out of range");
        }
        result = result * 10 + digit;
    }
    if (result < minimum || result > maximum) {
        usage_error(std::string(name) + " is out of range");
    }
    return result;
}

Options parse_options(int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view name(argv[index]);
        if (index + 1 >= argc) {
            usage_error(std::string(name) + " requires a value");
        }
        const std::string value(argv[++index]);
        if (name == "--address") {
            options.address = value;
        } else if (name == "--port") {
            options.port = static_cast<std::uint16_t>(
                parse_unsigned(value, 1, 65535, "port"));
        } else if (name == "--host") {
            options.host = value;
        } else if (name == "--path") {
            options.path = value;
        } else if (name == "--requests") {
            options.requests = parse_unsigned(
                value, 1, 100000000, "requests");
        } else if (name == "--concurrency") {
            options.concurrency = static_cast<std::size_t>(
                parse_unsigned(value, 1, 10000, "concurrency"));
        } else if (name == "--timeout-ms") {
            options.timeout_ms = static_cast<std::uint32_t>(
                parse_unsigned(value, 1, 600000, "timeout-ms"));
        } else if (name == "--output") {
            options.output_path = value;
        } else {
            usage_error("unknown option: " + std::string(name));
        }
    }
    if (options.host.empty() || options.path.empty() || options.path.front() != '/') {
        usage_error("host must be non-empty and path must start with '/'");
    }
    if (options.concurrency > options.requests) {
        options.concurrency = static_cast<std::size_t>(options.requests);
    }
    return options;
}

std::string lowercase(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character + ('a' - 'A'));
        }
        return character;
    });
    return result;
}

std::string trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

Socket connect_server(const Options& options)
{
    Socket socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket) {
        throw std::runtime_error("socket failed: " + std::to_string(errno));
    }

    const timeval timeout{
        static_cast<time_t>(options.timeout_ms / 1000),
        static_cast<suseconds_t>((options.timeout_ms % 1000) * 1000)};
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout)) == -1 ||
        ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout)) == -1) {
        throw std::runtime_error("setsockopt timeout failed");
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.address.c_str(), &endpoint.sin_addr) != 1) {
        throw std::runtime_error("address must be numeric IPv4");
    }
    while (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint),
                     sizeof(endpoint)) == -1) {
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error("connect failed: " + std::to_string(errno));
    }
    return socket;
}

void send_all(int descriptor, std::string_view message)
{
    std::size_t offset = 0;
    while (offset < message.size()) {
        const ssize_t sent = ::send(
            descriptor,
            message.data() + offset,
            message.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
        } else if (sent == -1 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("send failed: " + std::to_string(errno));
        }
    }
}

void receive_more(int descriptor, std::string& pending)
{
    std::array<char, kReadChunkSize> buffer{};
    ssize_t received = -1;
    do {
        received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
    } while (received == -1 && errno == EINTR);
    if (received > 0) {
        pending.append(buffer.data(), static_cast<std::size_t>(received));
        return;
    }
    if (received == 0) {
        throw std::runtime_error("peer closed before complete response");
    }
    throw std::runtime_error("recv failed: " + std::to_string(errno));
}

HttpResponse read_response(int descriptor, std::string& pending)
{
    std::size_t header_end = pending.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        if (pending.size() >= kMaxResponseHeaderSize) {
            throw std::runtime_error("response header exceeded limit");
        }
        receive_more(descriptor, pending);
        header_end = pending.find("\r\n\r\n");
    }

    const std::string_view header(pending.data(), header_end + 2);
    const std::size_t status_end = header.find("\r\n");
    if (status_end == std::string_view::npos) {
        throw std::runtime_error("malformed response status line");
    }
    const std::string status_line(header.substr(0, status_end));
    std::istringstream status_stream(status_line);
    std::string version;
    int status = 0;
    if (!(status_stream >> version >> status) || version.rfind("HTTP/", 0) != 0) {
        throw std::runtime_error("malformed response status line");
    }

    std::optional<std::size_t> content_length;
    bool close_connection = false;
    std::size_t line_start = status_end + 2;
    while (line_start < header.size()) {
        const std::size_t line_end = header.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end == line_start) {
            break;
        }
        const std::string_view line = header.substr(line_start, line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            throw std::runtime_error("malformed response header");
        }
        const std::string name = lowercase(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "content-length") {
            const std::uint64_t parsed = parse_unsigned(
                value, 0, 64 * 1024 * 1024, "response Content-Length");
            if (content_length.has_value() && *content_length != parsed) {
                throw std::runtime_error("conflicting response Content-Length");
            }
            content_length = static_cast<std::size_t>(parsed);
        } else if (name == "connection" && lowercase(value).find("close") != std::string::npos) {
            close_connection = true;
        } else if (name == "transfer-encoding") {
            throw std::runtime_error("load generator benchmark expects Content-Length responses");
        }
        line_start = line_end + 2;
    }
    if (!content_length.has_value()) {
        throw std::runtime_error("response has no Content-Length");
    }

    const std::size_t message_size = header_end + 4 + *content_length;
    while (pending.size() < message_size) {
        receive_more(descriptor, pending);
    }
    HttpResponse response{
        status,
        pending.substr(header_end + 4, *content_length),
        close_connection};
    pending.erase(0, message_size);
    return response;
}

bool valid_backend_body(std::string_view body)
{
    return body == "backend-a\n" || body == "backend-b\n" || body == "backend-c\n";
}

void add_failure(SharedResults& results, std::uint64_t request_id, std::string detail)
{
    ++results.failures;
    std::lock_guard<std::mutex> lock(results.details_mutex);
    if (results.failure_examples.size() < kFailureExampleLimit) {
        results.failure_examples.push_back(
            "request=" + std::to_string(request_id) + " " + std::move(detail));
    }
}

void worker(const Options& options, SharedResults& results)
{
    Socket socket;
    std::string pending;

    for (;;) {
        const std::uint64_t request_id = results.next_request.fetch_add(1);
        if (request_id >= options.requests) {
            return;
        }
        const auto started = Clock::now();
        try {
            if (!socket) {
                socket = connect_server(options);
                ++results.reconnects;
            }
            const std::string request =
                "GET " + options.path + "?request=" + std::to_string(request_id) +
                " HTTP/1.1\r\nHost: " + options.host +
                "\r\nUser-Agent: edgegate-loadgen/1\r\n"
                "Accept: text/plain\r\nConnection: keep-alive\r\n\r\n";
            send_all(socket.get(), request);
            const HttpResponse response = read_response(socket.get(), pending);
            results.latency_us[static_cast<std::size_t>(request_id)] =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - started).count());
            {
                std::lock_guard<std::mutex> lock(results.details_mutex);
                ++results.status_codes[response.status];
            }
            if (response.status != 200) {
                add_failure(results, request_id,
                            "unexpected status=" + std::to_string(response.status));
            } else if (!valid_backend_body(response.body)) {
                ++results.invalid_bodies;
                add_failure(results, request_id, "invalid backend response body");
            } else {
                ++results.successes;
            }
            if (response.close_connection) {
                socket.reset();
                pending.clear();
            }
        } catch (const std::exception& error) {
            ++results.transport_failures;
            add_failure(results, request_id, error.what());
            socket.reset();
            pending.clear();
        }
    }
}

double percentile_ms(const std::vector<std::uint64_t>& sorted, double ratio)
{
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(ratio * static_cast<double>(sorted.size()))) - 1;
    return static_cast<double>(sorted[std::min(index, sorted.size() - 1)]) / 1000.0;
}

nlohmann::json make_report(
    const Options& options,
    const SharedResults& results,
    std::chrono::microseconds elapsed)
{
    std::vector<std::uint64_t> latencies;
    latencies.reserve(results.latency_us.size());
    for (const std::uint64_t latency : results.latency_us) {
        if (latency != 0) {
            latencies.push_back(latency);
        }
    }
    std::sort(latencies.begin(), latencies.end());
    const double elapsed_seconds = static_cast<double>(elapsed.count()) / 1000000.0;

    nlohmann::json status_codes = nlohmann::json::object();
    for (const auto& [status, count] : results.status_codes) {
        status_codes[std::to_string(status)] = count;
    }
    const double average_ms = latencies.empty() ? 0.0 :
        static_cast<double>(std::accumulate(
            latencies.begin(), latencies.end(), std::uint64_t{0})) /
        static_cast<double>(latencies.size()) / 1000.0;

    return {
        {"schema_version", 1},
        {"target", {
            {"address", options.address}, {"port", options.port},
            {"host", options.host}, {"path", options.path}}},
        {"load", {
            {"requests", options.requests}, {"concurrency", options.concurrency},
            {"timeout_ms", options.timeout_ms}}},
        {"results", {
            {"successes", results.successes.load()},
            {"failures", results.failures.load()},
            {"transport_failures", results.transport_failures.load()},
            {"invalid_bodies", results.invalid_bodies.load()},
            {"connections_opened", results.reconnects.load()},
            {"status_codes", std::move(status_codes)},
            {"failure_examples", results.failure_examples}}},
        {"timing", {
            {"elapsed_seconds", elapsed_seconds},
            {"qps", elapsed_seconds == 0.0 ? 0.0 :
                static_cast<double>(results.successes.load()) / elapsed_seconds},
            {"latency_samples", latencies.size()},
            {"latency_ms", {
                {"min", latencies.empty() ? 0.0 : static_cast<double>(latencies.front()) / 1000.0},
                {"average", average_ms},
                {"p50", percentile_ms(latencies, 0.50)},
                {"p95", percentile_ms(latencies, 0.95)},
                {"p99", percentile_ms(latencies, 0.99)},
                {"max", latencies.empty() ? 0.0 : static_cast<double>(latencies.back()) / 1000.0}}}}}
    };
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const Options options = parse_options(argc, argv);
        SharedResults results(static_cast<std::size_t>(options.requests));
        std::vector<std::thread> workers;
        workers.reserve(options.concurrency);

        const auto started = Clock::now();
        for (std::size_t index = 0; index < options.concurrency; ++index) {
            workers.emplace_back(worker, std::cref(options), std::ref(results));
        }
        for (std::thread& thread : workers) {
            thread.join();
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - started);
        const nlohmann::json report = make_report(options, results, elapsed);
        const std::string serialized = report.dump(2) + '\n';
        std::cout << serialized;
        if (!options.output_path.empty()) {
            std::ofstream output(options.output_path);
            if (!output) {
                throw std::runtime_error("cannot open output file: " + options.output_path);
            }
            output << serialized;
        }
        return results.failures.load() == 0 &&
               results.successes.load() == options.requests ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "edgegate_loadgen: " << error.what() << '\n';
        return 2;
    }
}
// AI-CODE-END: S11-LOAD-GENERATOR
