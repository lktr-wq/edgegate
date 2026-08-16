#pragma once

// AI-CODE-BEGIN: S8-RUNTIME-LOGGER-API
#include "edgegate/config/edgegate_config.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace spdlog { class logger; }

namespace edgegate::runtime {

/*
 * 三个 logger 共用 spdlog 的有界异步队列和后台线程。网络事件循环只把
 * 一条已经整理好的记录放入队列，不在代理回调中等待磁盘完成写入。
 */
class RuntimeLogger {
public:
    explicit RuntimeLogger(const edgegate::config::LoggingConfig& config);

    RuntimeLogger(const RuntimeLogger&) = delete;
    RuntimeLogger& operator=(const RuntimeLogger&) = delete;

    void access(
        std::string_view method,
        std::string_view host,
        std::string_view target,
        int status,
        std::size_t response_bytes,
        long long latency_ms,
        std::string_view upstream) noexcept;
    void error(std::string_view event, std::string_view detail) noexcept;
    void management(std::string_view event, std::string_view detail) noexcept;
    void flush() noexcept;

    [[nodiscard]] bool enabled() const noexcept;

private:
    bool enabled_{false};
    std::shared_ptr<spdlog::logger> access_logger_;
    std::shared_ptr<spdlog::logger> error_logger_;
    std::shared_ptr<spdlog::logger> management_logger_;
};

} // namespace edgegate::runtime
// AI-CODE-END: S8-RUNTIME-LOGGER-API
