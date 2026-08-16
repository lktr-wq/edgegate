#pragma once

// AI-CODE-BEGIN: S8-MANAGEMENT-SERVER-API
#include "edgegate/net/event_loop.h"

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace edgegate::runtime {

using ManagementCommandHandler = std::function<nlohmann::json(
    edgegate::net::EventLoop&,
    const nlohmann::json&)>;

/*
 * 创建一个注册到 EventLoop 的 Unix Socket Listener。每条连接只接收一行
 * JSON 请求、返回一行 JSON 响应，然后关闭，正好对应 edgegatectl 的
 * “一次执行一条命令”。
 */
[[nodiscard]] std::unique_ptr<edgegate::net::EventHandler>
make_management_listener(
    const std::string& socket_path,
    ManagementCommandHandler handler);

} // namespace edgegate::runtime
// AI-CODE-END: S8-MANAGEMENT-SERVER-API
