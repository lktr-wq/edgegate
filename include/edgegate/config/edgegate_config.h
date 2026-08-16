#pragma once

// AI-CODE-BEGIN: S6-YAML-CONFIG-API
#include "edgegate/routing/route_table.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace edgegate::config {

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
    std::vector<edgegate::routing::RouteDefinition> routes;
};

/*
 * 读取并完整校验 YAML。失败时抛出的异常包含文件名、行列和原因，
 * 调用者不能拿到“只解析了一半”的配置对象。
 */
[[nodiscard]] EdgeGateConfig load_edgegate_config(const std::string& path);

} // namespace edgegate::config
// AI-CODE-END: S6-YAML-CONFIG-API
