#pragma once

// AI-CODE-BEGIN: S9-OBSERVABILITY-API
#include "edgegate/config/edgegate_config.h"
#include "edgegate/routing/route_table.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace edgegate::runtime {

/*
 * 保存 Dashboard 所需的有界运行统计。该对象只由 EventLoop 线程访问，
 * 因而不需要给每次请求的热路径增加互斥锁。
 */
class ObservabilityStore {
public:
    explicit ObservabilityStore(
        const edgegate::config::DashboardConfig& config);

    void reconfigure(const edgegate::config::DashboardConfig& config) noexcept;

    void record_request(
        std::string_view method,
        std::string_view path,
        std::string_view route_id,
        const edgegate::routing::UpstreamEndpoint* upstream,
        int status,
        std::uint64_t response_bytes,
        std::uint64_t latency_ms,
        std::size_t attempts);

    void record_upstream_result(
        const edgegate::routing::UpstreamEndpoint& upstream,
        bool success,
        bool timeout) noexcept;

    void record_error(
        std::string_view category,
        std::string_view detail);

    [[nodiscard]] nlohmann::json snapshot() const;

private:
    struct RouteCounters {
        std::uint64_t requests{0};
        std::uint64_t errors{0};
    };

    struct UpstreamCounters {
        std::string id;
        std::string address;
        std::uint16_t port{0};
        std::uint64_t attempts{0};
        std::uint64_t successes{0};
        std::uint64_t failures{0};
        std::uint64_t timeouts{0};
    };

    static std::string timestamp_utc();
    static std::string upstream_key(
        std::string_view address,
        std::uint16_t port);
    [[nodiscard]] std::uint64_t percentile(double ratio) const noexcept;
    void trim_bounded_records() noexcept;

    edgegate::config::DashboardConfig config_;
    std::uint64_t request_count_{0};
    std::uint64_t response_bytes_{0};
    std::uint64_t latency_sum_ms_{0};
    std::uint64_t latency_min_ms_{0};
    std::uint64_t latency_max_ms_{0};
    std::vector<std::uint64_t> latency_buckets_;
    std::unordered_map<int, std::uint64_t> status_codes_;
    std::unordered_map<std::string, RouteCounters> routes_;
    std::unordered_map<std::string, UpstreamCounters> upstreams_;
    std::deque<nlohmann::json> recent_errors_;
    std::deque<nlohmann::json> slow_requests_;
    std::uint64_t next_error_sequence_{1};
    std::uint64_t next_slow_sequence_{1};
};

} // namespace edgegate::runtime
// AI-CODE-END: S9-OBSERVABILITY-API
