#pragma once

// AI-CODE-BEGIN: S9-DASHBOARD-SERVER-API
#include "edgegate/net/event_loop.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace edgegate::runtime {

using DashboardSnapshotHandler = std::function<nlohmann::json()>;

/*
 * 创建只读 Dashboard 的 TCP Listener。port=0 仅供测试申请临时端口，
 * actual_port 返回内核最终绑定的端口。
 */
[[nodiscard]] std::unique_ptr<edgegate::net::EventHandler>
make_dashboard_listener(
    const std::string& address,
    std::uint16_t port,
    DashboardSnapshotHandler handler,
    std::uint16_t& actual_port);

} // namespace edgegate::runtime
// AI-CODE-END: S9-DASHBOARD-SERVER-API
