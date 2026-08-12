#pragma once

// AI-CODE-BEGIN: S6-ROUTE-TABLE-API
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edgegate::routing {

struct UpstreamEndpoint {
    std::string id;
    std::string address;
    std::uint16_t port{0};
    bool healthy{true};
};

struct RouteDefinition {
    std::string id;
    std::string host_pattern;
    std::string path_prefix;
    std::vector<UpstreamEndpoint> upstreams;
};

enum class RouteLookupStatus {
    kMatched,
    kNoRoute,
    kNoHealthyUpstream
};

struct RouteLookupResult {
    RouteLookupStatus status{RouteLookupStatus::kNoRoute};
    std::string route_id;
    std::optional<UpstreamEndpoint> upstream;
};

/*
 * Host 先匹配：精确 Host 优先于通配 Host；通配写成 *.example.com，
 * 多个通配同时命中时后缀更长者优先。确定 Host 后，选择最长路径前缀。
 * 同一路由只在 healthy=true 的上游中轮询。
 */
class RouteTable {
public:
    explicit RouteTable(std::vector<RouteDefinition> routes);

    RouteLookupResult lookup(
        std::string_view host_header,
        std::string_view request_target);

    [[nodiscard]] bool set_upstream_health(
        std::string_view route_id,
        std::string_view upstream_id,
        bool healthy) noexcept;

    [[nodiscard]] const std::vector<RouteDefinition>& routes() const noexcept;

private:
    struct RouteState {
        RouteDefinition definition;
        std::string normalized_host;
        bool wildcard{false};
        std::size_t round_robin_cursor{0};
    };

    [[nodiscard]] static std::string normalize_host(std::string_view host);
    [[nodiscard]] static std::string_view target_path(
        std::string_view target) noexcept;
    [[nodiscard]] static bool host_matches(
        const RouteState& route,
        std::string_view normalized_host) noexcept;

    std::vector<RouteState> states_;
    std::vector<RouteDefinition> public_routes_;
};

} // namespace edgegate::routing
// AI-CODE-END: S6-ROUTE-TABLE-API
