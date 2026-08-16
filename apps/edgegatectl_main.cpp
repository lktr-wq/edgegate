// AI-CODE-BEGIN: S8-EDGEGATECTL
// 阶段 8：一次启动只执行一条管理命令的命令行客户端。

#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr std::string_view kDefaultSocketPath = "/tmp/edgegate.sock";
constexpr std::size_t kMaximumResponseSize = 1024U * 1024U;

struct CommandLine {
    std::string socket_path{kDefaultSocketPath};
    std::string command;
    bool json_output{false};
};

void print_usage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " [--socket PATH] status|routes|upstreams|stats [--json]\n"
        << "       " << program
        << " [--socket PATH] reload|drain|stop\n";
}

bool is_known_command(std::string_view command) {
    return command == "status" || command == "routes" ||
           command == "upstreams" || command == "stats" ||
           command == "reload" || command == "drain" || command == "stop";
}

bool parse_command_line(int argc, char* argv[], CommandLine& result) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);

        if (argument == "--socket") {
            if (++index >= argc) {
                return false;
            }
            result.socket_path = argv[index];
        } else if (argument == "--json") {
            result.json_output = true;
        } else if (result.command.empty()) {
            result.command = argument;
        } else {
            return false;
        }
    }

    if (!is_known_command(result.command)) {
        return false;
    }

    // v1 只为 stats 提供机器可读输出，避免不同命令出现含义不一致的参数。
    return !result.json_output || result.command == "stats";
}

class ScopedFileDescriptor {
public:
    explicit ScopedFileDescriptor(int fd) noexcept : fd_(fd) {}

    ~ScopedFileDescriptor() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_{-1};
};

bool send_all(int fd, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t result = send(
            fd,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL);

        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool receive_json_line(int fd, std::string& response) {
    char buffer[4096]{};

    while (response.size() <= kMaximumResponseSize) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            response.append(buffer, static_cast<std::size_t>(received));
            const std::size_t line_end = response.find('\n');
            if (line_end != std::string::npos) {
                response.resize(line_end);
                return true;
            }
            continue;
        }
        if (received == 0) {
            return !response.empty();
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return false;
}

int connect_management_socket(const std::string& path) {
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return -1;
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);

    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

void print_status(const nlohmann::json& data) {
    std::cout << "State:              " << data.value("state", "unknown") << '\n'
              << "Listen:             " << data.value("listen", "unknown") << '\n'
              << "Uptime (ms):        " << data.value("uptime_ms", 0ULL) << '\n'
              << "Config generation: " << data.value("config_generation", 0ULL) << '\n'
              << "Active sessions:   " << data.value("active_sessions", 0ULL) << '\n'
              << "Completed requests:" << data.value("completed_requests", 0ULL) << '\n';
}

void print_routes(const nlohmann::json& data) {
    std::cout << std::left << std::setw(20) << "ID"
              << std::setw(28) << "HOST" << "PATH\n";
    for (const auto& route : data) {
        std::cout << std::left << std::setw(20) << route.value("id", "")
                  << std::setw(28) << route.value("host", "")
                  << route.value("path_prefix", "") << '\n';
    }
}

void print_upstreams(const nlohmann::json& data) {
    std::cout << std::left << std::setw(20) << "ID"
              << std::setw(24) << "ADDRESS"
              << std::setw(10) << "HEALTH"
              << std::setw(12) << "FAILURES" << "SUCCESSES\n";
    for (const auto& upstream : data) {
        const std::string address = upstream.value("address", "") + ":" +
                                    std::to_string(upstream.value("port", 0));
        std::cout << std::left << std::setw(20) << upstream.value("id", "")
                  << std::setw(24) << address
                  << std::setw(10)
                  << (upstream.value("healthy", false) ? "healthy" : "down")
                  << std::setw(12) << upstream.value("consecutive_failures", 0ULL)
                  << upstream.value("consecutive_successes", 0ULL) << '\n';
    }
}

void print_stats(const nlohmann::json& data) {
    for (auto iterator = data.begin(); iterator != data.end(); ++iterator) {
        std::cout << iterator.key() << ": " << iterator.value() << '\n';
    }
}

void print_human_response(const std::string& command, const nlohmann::json& data) {
    if (command == "status") {
        print_status(data);
    } else if (command == "routes") {
        print_routes(data);
    } else if (command == "upstreams") {
        print_upstreams(data);
    } else if (command == "stats") {
        print_stats(data);
    } else {
        std::cout << data.value("message", "ok") << '\n';
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    CommandLine options;
    if (!parse_command_line(argc, argv, options)) {
        print_usage(argv[0]);
        return 2;
    }

    const int connected_fd = connect_management_socket(options.socket_path);
    if (connected_fd == -1) {
        std::cerr << "edgegatectl: cannot connect to " << options.socket_path
                  << ": " << std::strerror(errno) << '\n';
        return 3;
    }
    const ScopedFileDescriptor socket_fd(connected_fd);

    const std::string request =
        nlohmann::json{{"command", options.command}}.dump() + "\n";
    if (!send_all(socket_fd.get(), request)) {
        std::cerr << "edgegatectl: failed to send command: "
                  << std::strerror(errno) << '\n';
        return 3;
    }

    std::string response_line;
    if (!receive_json_line(socket_fd.get(), response_line)) {
        std::cerr << "edgegatectl: failed to receive a complete response\n";
        return 3;
    }

    nlohmann::json response;
    try {
        response = nlohmann::json::parse(response_line);
    } catch (const nlohmann::json::exception& error) {
        std::cerr << "edgegatectl: invalid JSON response: " << error.what() << '\n';
        return 3;
    }

    if (!response.value("ok", false)) {
        std::cerr << "edgegatectl: " << response.value("error", "command failed") << '\n';
        return 1;
    }

    const nlohmann::json data = response.value("data", nlohmann::json::object());
    if (options.json_output) {
        std::cout << data.dump() << '\n';
    } else {
        print_human_response(options.command, data);
    }
    return 0;
}

// AI-CODE-END: S8-EDGEGATECTL
