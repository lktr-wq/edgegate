#pragma once

// AI-CODE-BEGIN: S6-YAML-CONFIG-API
#include "edgegate/routing/route_table.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace edgegate::config {

// AI-CODE-BEGIN: S7-RELIABILITY-CONFIG-API
struct StreamBufferConfig {
    std::size_t capacity{256 * 1024};
    std::size_t high_watermark{192 * 1024};
    std::size_t low_watermark{64 * 1024};
};

struct TimeoutConfig {
    std::uint32_t client_header_ms{10000};
    std::uint32_t upstream_connect_ms{2000};
    std::uint32_t upstream_header_ms{5000};
    std::uint32_t io_idle_ms{15000};
    std::uint32_t request_total_ms{60000};
    std::uint32_t keep_alive_idle_ms{30000};
};

struct HealthCheckConfig {
    std::uint32_t interval_ms{5000};
    std::uint32_t timeout_ms{1000};
    std::uint32_t failure_threshold{3};
    std::uint32_t success_threshold{2};
    std::string path{"/health"};
};
// AI-CODE-END: S7-RELIABILITY-CONFIG-API

/*
 * 阶段 6 只把已经实现的能力放进配置文件：监听地址、容量限制、
 * 上游池和路由。超时、健康检查和日志会在对应阶段新增配置项。
 */
struct EdgeGateConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t listen_port{18080};
    std::size_t max_header_size{8192};
    std::size_t max_request_body_size{1024 * 1024};
    std::size_t max_response_body_size{8 * 1024 * 1024};
    // AI-CODE-BEGIN: S7-RELIABILITY-CONFIG-FIELDS
    StreamBufferConfig stream_buffer;
    TimeoutConfig timeouts;
    HealthCheckConfig health_check;
    // AI-CODE-END: S7-RELIABILITY-CONFIG-FIELDS
    std::vector<edgegate::routing::RouteDefinition> routes;
};

/*
 * 读取并完整校验 YAML。失败时抛出的异常包含文件名、行列和原因，
 * 调用者不能拿到“只解析了一半”的配置对象。
 */
[[nodiscard]] EdgeGateConfig load_edgegate_config(const std::string& path);

} // namespace edgegate::config
// AI-CODE-END: S6-YAML-CONFIG-API
