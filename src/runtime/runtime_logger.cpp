#include "edgegate/runtime/runtime_logger.h"

// AI-CODE-BEGIN: S8-RUNTIME-LOGGER-IMPLEMENTATION
#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace edgegate::runtime {

namespace {

std::once_flag thread_pool_once;
std::atomic<unsigned long long> logger_sequence{0};

spdlog::level::level_enum parse_level(std::string_view level)
{
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "info") return spdlog::level::info;
    if (level == "warn") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    if (level == "off") return spdlog::level::off;
    throw std::invalid_argument("invalid logging level");
}

std::shared_ptr<spdlog::logger> make_rotating_logger(
    const std::filesystem::path& path,
    std::size_t max_file_size,
    std::size_t max_files,
    spdlog::level::level_enum level,
    std::string_view purpose)
{
    const auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path.string(), max_file_size, max_files);
    const std::string name = "edgegate-" + std::string(purpose) + "-" +
        std::to_string(++logger_sequence);
    auto logger = std::make_shared<spdlog::async_logger>(
        name,
        sink,
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(level);
    logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z [%l] %v");
    logger->flush_on(spdlog::level::info);
    return logger;
}

} // namespace

RuntimeLogger::RuntimeLogger(const edgegate::config::LoggingConfig& config)
    : enabled_(config.enabled)
{
    if (!enabled_) {
        return;
    }
    std::call_once(thread_pool_once, [] {
        // 队列满时覆盖最旧记录，避免磁盘过慢反向阻塞整个 Reactor。
        spdlog::init_thread_pool(8192, 1);
    });
    std::filesystem::create_directories(config.directory);
    const auto level = parse_level(config.level);
    const std::filesystem::path directory(config.directory);
    access_logger_ = make_rotating_logger(
        directory / "access.log", config.max_file_size,
        config.max_files, level, "access");
    error_logger_ = make_rotating_logger(
        directory / "error.log", config.max_file_size,
        config.max_files, level, "error");
    management_logger_ = make_rotating_logger(
        directory / "management.log", config.max_file_size,
        config.max_files, level, "management");
}

void RuntimeLogger::access(
    std::string_view method,
    std::string_view host,
    std::string_view target,
    int status,
    std::size_t response_bytes,
    long long latency_ms,
    std::string_view upstream) noexcept
{
    if (!access_logger_) return;
    try {
        access_logger_->info(
            "method={} host={} target={} status={} bytes={} latency_ms={} upstream={}",
            method, host, target, status, response_bytes, latency_ms, upstream);
    } catch (...) {
    }
}

void RuntimeLogger::error(
    std::string_view event,
    std::string_view detail) noexcept
{
    if (!error_logger_) return;
    try {
        error_logger_->error("event={} detail={}", event, detail);
    } catch (...) {
    }
}

void RuntimeLogger::management(
    std::string_view event,
    std::string_view detail) noexcept
{
    if (!management_logger_) return;
    try {
        management_logger_->info("event={} detail={}", event, detail);
    } catch (...) {
    }
}

void RuntimeLogger::flush() noexcept
{
    try {
        if (access_logger_) access_logger_->flush();
        if (error_logger_) error_logger_->flush();
        if (management_logger_) management_logger_->flush();
    } catch (...) {
    }
}

bool RuntimeLogger::enabled() const noexcept
{
    return enabled_;
}

} // namespace edgegate::runtime
// AI-CODE-END: S8-RUNTIME-LOGGER-IMPLEMENTATION
