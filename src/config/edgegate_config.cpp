#include "edgegate/config/edgegate_config.h"

// AI-CODE-BEGIN: S6-YAML-CONFIG-IMPLEMENTATION
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <arpa/inet.h>
#include <yaml-cpp/yaml.h>

namespace edgegate::config {

namespace {

[[noreturn]] void fail(
    const std::string& path,
    const YAML::Node& node,
    const std::string& message)
{
    std::ostringstream output;
    output << path;
    if (node.IsDefined() && !node.Mark().is_null()) {
        output << ':' << node.Mark().line + 1 << ':' << node.Mark().column + 1;
    }
    output << ": " << message;
    throw std::runtime_error(output.str());
}

void require_map(
    const std::string& path,
    const YAML::Node& node,
    const char* name)
{
    if (!node || !node.IsMap()) {
        fail(path, node, std::string(name) + " must be a map");
    }
}

void require_sequence(
    const std::string& path,
    const YAML::Node& node,
    const char* name)
{
    if (!node || !node.IsSequence()) {
        fail(path, node, std::string(name) + " must be a sequence");
    }
}

void reject_unknown_keys(
    const std::string& path,
    const YAML::Node& node,
    std::initializer_list<const char*> allowed)
{
    const std::unordered_set<std::string> allowed_keys(
        allowed.begin(), allowed.end());
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            fail(path, entry.first, "configuration key must be text");
        }
        const std::string key = entry.first.Scalar();
        if (allowed_keys.count(key) == 0U) {
            fail(path, entry.first, "unknown configuration key '" + key + "'");
        }
    }
}

template <typename Value>
Value required_scalar(
    const std::string& path,
    const YAML::Node& parent,
    const char* key)
{
    const YAML::Node node = parent[key];
    if (!node || !node.IsScalar()) {
        fail(path, node, std::string("missing or invalid '") + key + "'");
    }
    try {
        return node.as<Value>();
    } catch (const YAML::Exception&) {
        fail(path, node, std::string("invalid value for '") + key + "'");
    }
}

std::size_t positive_size(
    const std::string& path,
    const YAML::Node& parent,
    const char* key,
    std::size_t default_value)
{
    const YAML::Node node = parent[key];
    if (!node) {
        return default_value;
    }
    if (!node.IsScalar()) {
        fail(path, node, std::string("'") + key + "' must be an integer");
    }
    try {
        const auto value = node.as<unsigned long long>();
        if (value == 0 || value > std::numeric_limits<std::size_t>::max()) {
            fail(path, node, std::string("'") + key + "' is out of range");
        }
        return static_cast<std::size_t>(value);
    } catch (const YAML::Exception&) {
        fail(path, node, std::string("'") + key + "' must be a positive integer");
    }
}

// AI-CODE-BEGIN: S7-RELIABILITY-CONFIG-PARSING
std::uint32_t positive_u32(
    const std::string& path,
    const YAML::Node& parent,
    const char* key,
    std::uint32_t default_value)
{
    const std::size_t value = positive_size(path, parent, key, default_value);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        fail(path, parent[key], std::string("'") + key + "' is out of range");
    }
    return static_cast<std::uint32_t>(value);
}

// AI-CODE-BEGIN: S8-MANAGEMENT-AND-LOGGING-CONFIG-PARSING
bool optional_bool(
    const std::string& path,
    const YAML::Node& parent,
    const char* key,
    bool default_value)
{
    const YAML::Node node = parent[key];
    if (!node) {
        return default_value;
    }
    if (!node.IsScalar()) {
        fail(path, node, std::string("'") + key + "' must be true or false");
    }
    try {
        return node.as<bool>();
    } catch (const YAML::Exception&) {
        fail(path, node, std::string("'") + key + "' must be true or false");
    }
}

std::string optional_string(
    const std::string& path,
    const YAML::Node& parent,
    const char* key,
    std::string default_value)
{
    const YAML::Node node = parent[key];
    if (!node) {
        return default_value;
    }
    if (!node.IsScalar()) {
        fail(path, node, std::string("'") + key + "' must be text");
    }
    try {
        return node.as<std::string>();
    } catch (const YAML::Exception&) {
        fail(path, node, std::string("'") + key + "' must be text");
    }
}
// AI-CODE-END: S8-MANAGEMENT-AND-LOGGING-CONFIG-PARSING
// AI-CODE-END: S7-RELIABILITY-CONFIG-PARSING

std::uint16_t port_value(
    const std::string& path,
    const YAML::Node& node,
    const char* field)
{
    if (!node || !node.IsScalar()) {
        fail(path, node, std::string("missing or invalid '") + field + "'");
    }
    try {
        const auto value = node.as<unsigned int>();
        if (value == 0 || value > 65535U) {
            fail(path, node, std::string("'") + field + "' must be 1..65535");
        }
        return static_cast<std::uint16_t>(value);
    } catch (const YAML::Exception&) {
        fail(path, node, std::string("'") + field + "' must be 1..65535");
    }
}

void require_ipv4(
    const std::string& path,
    const YAML::Node& node,
    const std::string& address,
    const char* field)
{
    in_addr parsed{};
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        fail(path, node, std::string("'") + field +
            "' must be a numeric IPv4 address; DNS names are not supported");
    }
}

} // namespace

EdgeGateConfig load_edgegate_config(const std::string& path)
{
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& error) {
        throw std::runtime_error(path + ": " + error.what());
    }

    require_map(path, root, "configuration root");
    reject_unknown_keys(path, root, {
        "listen", "limits", "stream_buffer", "timeouts", "health_check",
        "management", "logging", "dashboard", "upstream_pools", "routes"});

    EdgeGateConfig config;

    const YAML::Node listen = root["listen"];
    require_map(path, listen, "listen");
    reject_unknown_keys(path, listen, {"address", "port"});
    config.listen_address = required_scalar<std::string>(path, listen, "address");
    require_ipv4(path, listen["address"], config.listen_address, "listen.address");
    config.listen_port = port_value(path, listen["port"], "listen.port");

    const YAML::Node limits = root["limits"];
    if (limits) {
        require_map(path, limits, "limits");
        reject_unknown_keys(path, limits, {
            "max_header_size",
            "max_request_body_size",
            "max_response_body_size"});
        config.max_header_size = positive_size(
            path, limits, "max_header_size", config.max_header_size);
        config.max_request_body_size = positive_size(
            path, limits, "max_request_body_size", config.max_request_body_size);
        config.max_response_body_size = positive_size(
            path, limits, "max_response_body_size", config.max_response_body_size);
    }

    // AI-CODE-BEGIN: S7-RELIABILITY-CONFIG-PARSING
    const YAML::Node stream_buffer = root["stream_buffer"];
    if (stream_buffer) {
        require_map(path, stream_buffer, "stream_buffer");
        reject_unknown_keys(path, stream_buffer, {
            "capacity", "high_watermark", "low_watermark"});
        config.stream_buffer.capacity = positive_size(
            path, stream_buffer, "capacity", config.stream_buffer.capacity);
        config.stream_buffer.high_watermark = positive_size(
            path, stream_buffer, "high_watermark",
            config.stream_buffer.high_watermark);
        config.stream_buffer.low_watermark = positive_size(
            path, stream_buffer, "low_watermark",
            config.stream_buffer.low_watermark);
        if (config.stream_buffer.low_watermark >=
                config.stream_buffer.high_watermark ||
            config.stream_buffer.high_watermark >=
                config.stream_buffer.capacity) {
            fail(path, stream_buffer,
                "stream watermarks must satisfy low < high < capacity");
        }
    }
    constexpr std::size_t stream_overhead = 17 * 1024;
    if (config.max_header_size >
            std::numeric_limits<std::size_t>::max() - stream_overhead ||
        config.stream_buffer.capacity <
            config.max_header_size + stream_overhead) {
        fail(path, stream_buffer,
            "stream_buffer.capacity must leave 17 KiB beyond max_header_size "
            "for rewritten headers and the first streamed body chunk");
    }

    const YAML::Node timeouts = root["timeouts"];
    if (timeouts) {
        require_map(path, timeouts, "timeouts");
        reject_unknown_keys(path, timeouts, {
            "client_header_ms", "upstream_connect_ms", "upstream_header_ms",
            "io_idle_ms", "request_total_ms", "keep_alive_idle_ms"});
        config.timeouts.client_header_ms = positive_u32(
            path, timeouts, "client_header_ms", config.timeouts.client_header_ms);
        config.timeouts.upstream_connect_ms = positive_u32(
            path, timeouts, "upstream_connect_ms",
            config.timeouts.upstream_connect_ms);
        config.timeouts.upstream_header_ms = positive_u32(
            path, timeouts, "upstream_header_ms",
            config.timeouts.upstream_header_ms);
        config.timeouts.io_idle_ms = positive_u32(
            path, timeouts, "io_idle_ms", config.timeouts.io_idle_ms);
        config.timeouts.request_total_ms = positive_u32(
            path, timeouts, "request_total_ms", config.timeouts.request_total_ms);
        config.timeouts.keep_alive_idle_ms = positive_u32(
            path, timeouts, "keep_alive_idle_ms",
            config.timeouts.keep_alive_idle_ms);
    }

    const YAML::Node health = root["health_check"];
    if (health) {
        require_map(path, health, "health_check");
        reject_unknown_keys(path, health, {
            "interval_ms", "timeout_ms", "failure_threshold",
            "success_threshold", "path"});
        config.health_check.interval_ms = positive_u32(
            path, health, "interval_ms", config.health_check.interval_ms);
        config.health_check.timeout_ms = positive_u32(
            path, health, "timeout_ms", config.health_check.timeout_ms);
        config.health_check.failure_threshold = positive_u32(
            path, health, "failure_threshold",
            config.health_check.failure_threshold);
        config.health_check.success_threshold = positive_u32(
            path, health, "success_threshold",
            config.health_check.success_threshold);
        if (health["path"]) {
            config.health_check.path =
                required_scalar<std::string>(path, health, "path");
        }
        if (config.health_check.path.empty() ||
            config.health_check.path.front() != '/' ||
            config.health_check.path.find_first_of("\r\n") != std::string::npos) {
            fail(path, health["path"],
                "health_check.path must be a safe origin-form path");
        }
    }
    // AI-CODE-END: S7-RELIABILITY-CONFIG-PARSING

    // AI-CODE-BEGIN: S8-MANAGEMENT-AND-LOGGING-CONFIG-PARSING
    const YAML::Node management = root["management"];
    if (management) {
        require_map(path, management, "management");
        reject_unknown_keys(path, management, {
            "enabled", "socket_path", "drain_timeout_ms"});
        config.management.enabled = optional_bool(
            path, management, "enabled", config.management.enabled);
        config.management.socket_path = optional_string(
            path, management, "socket_path", config.management.socket_path);
        config.management.drain_timeout_ms = positive_u32(
            path, management, "drain_timeout_ms",
            config.management.drain_timeout_ms);
        if (config.management.socket_path.empty() ||
            config.management.socket_path.front() != '/' ||
            config.management.socket_path.size() >= 100 ||
            config.management.socket_path.find_first_of("\r\n") !=
                std::string::npos) {
            fail(path, management["socket_path"],
                "management.socket_path must be an absolute Unix path "
                "shorter than 100 bytes");
        }
    }

    const YAML::Node logging = root["logging"];
    if (logging) {
        require_map(path, logging, "logging");
        reject_unknown_keys(path, logging, {
            "enabled", "directory", "level", "max_file_size", "max_files"});
        config.logging.enabled = optional_bool(
            path, logging, "enabled", config.logging.enabled);
        config.logging.directory = optional_string(
            path, logging, "directory", config.logging.directory);
        config.logging.level = optional_string(
            path, logging, "level", config.logging.level);
        config.logging.max_file_size = positive_size(
            path, logging, "max_file_size", config.logging.max_file_size);
        config.logging.max_files = positive_size(
            path, logging, "max_files", config.logging.max_files);
        const std::unordered_set<std::string> valid_levels{
            "trace", "debug", "info", "warn", "error", "critical", "off"};
        if (config.logging.directory.empty() ||
            config.logging.directory.front() != '/') {
            fail(path, logging["directory"],
                "logging.directory must be an absolute path");
        }
        if (valid_levels.count(config.logging.level) == 0U) {
            fail(path, logging["level"],
                "logging.level must be trace/debug/info/warn/error/critical/off");
        }
        if (config.logging.max_files > 100) {
            fail(path, logging["max_files"], "logging.max_files must be <= 100");
        }
    }
    // AI-CODE-END: S8-MANAGEMENT-AND-LOGGING-CONFIG-PARSING

    // AI-CODE-BEGIN: S9-DASHBOARD-CONFIG-PARSING
    const YAML::Node dashboard = root["dashboard"];
    if (dashboard) {
        require_map(path, dashboard, "dashboard");
        reject_unknown_keys(path, dashboard, {
            "enabled", "address", "port", "refresh_interval_ms",
            "recent_error_limit", "slow_request_threshold_ms",
            "slow_request_limit"});
        config.dashboard.enabled = optional_bool(
            path, dashboard, "enabled", config.dashboard.enabled);
        config.dashboard.address = optional_string(
            path, dashboard, "address", config.dashboard.address);
        if (dashboard["port"]) {
            config.dashboard.port = port_value(
                path, dashboard["port"], "dashboard.port");
        }
        config.dashboard.refresh_interval_ms = positive_u32(
            path, dashboard, "refresh_interval_ms",
            config.dashboard.refresh_interval_ms);
        config.dashboard.recent_error_limit = positive_size(
            path, dashboard, "recent_error_limit",
            config.dashboard.recent_error_limit);
        config.dashboard.slow_request_threshold_ms = positive_u32(
            path, dashboard, "slow_request_threshold_ms",
            config.dashboard.slow_request_threshold_ms);
        config.dashboard.slow_request_limit = positive_size(
            path, dashboard, "slow_request_limit",
            config.dashboard.slow_request_limit);

        require_ipv4(
            path, dashboard["address"], config.dashboard.address,
            "dashboard.address");
        if (config.dashboard.enabled &&
            config.dashboard.address != "127.0.0.1") {
            fail(path, dashboard["address"],
                "dashboard.address must be 127.0.0.1 in v1");
        }
        if (config.dashboard.refresh_interval_ms < 250 ||
            config.dashboard.refresh_interval_ms > 60000) {
            fail(path, dashboard["refresh_interval_ms"],
                "dashboard.refresh_interval_ms must be 250..60000");
        }
        if (config.dashboard.recent_error_limit > 500) {
            fail(path, dashboard["recent_error_limit"],
                "dashboard.recent_error_limit must be <= 500");
        }
        if (config.dashboard.slow_request_threshold_ms > 3600000) {
            fail(path, dashboard["slow_request_threshold_ms"],
                "dashboard.slow_request_threshold_ms must be <= 3600000");
        }
        if (config.dashboard.slow_request_limit > 200) {
            fail(path, dashboard["slow_request_limit"],
                "dashboard.slow_request_limit must be <= 200");
        }
    }
    // AI-CODE-END: S9-DASHBOARD-CONFIG-PARSING

    const YAML::Node pools = root["upstream_pools"];
    require_sequence(path, pools, "upstream_pools");
    if (pools.size() == 0U) {
        fail(path, pools, "upstream_pools must not be empty");
    }

    std::unordered_map<std::string, std::vector<edgegate::routing::UpstreamEndpoint>>
        upstream_pools;
    for (const YAML::Node& pool : pools) {
        require_map(path, pool, "upstream pool");
        reject_unknown_keys(path, pool, {"id", "endpoints"});
        const std::string pool_id = required_scalar<std::string>(path, pool, "id");
        if (pool_id.empty() || upstream_pools.count(pool_id) != 0U) {
            fail(path, pool["id"], "upstream pool id must be non-empty and unique");
        }

        const YAML::Node endpoints = pool["endpoints"];
        require_sequence(path, endpoints, "upstream pool endpoints");
        if (endpoints.size() == 0U) {
            fail(path, endpoints, "upstream pool must contain at least one endpoint");
        }

        std::vector<edgegate::routing::UpstreamEndpoint> parsed_endpoints;
        std::unordered_set<std::string> endpoint_ids;
        for (const YAML::Node& endpoint : endpoints) {
            require_map(path, endpoint, "upstream endpoint");
            reject_unknown_keys(path, endpoint, {"id", "address", "port"});
            const std::string id = required_scalar<std::string>(path, endpoint, "id");
            const std::string address =
                required_scalar<std::string>(path, endpoint, "address");
            if (id.empty() || !endpoint_ids.insert(id).second) {
                fail(path, endpoint["id"],
                    "endpoint id must be non-empty and unique within its pool");
            }
            require_ipv4(path, endpoint["address"], address, "endpoint.address");
            parsed_endpoints.push_back(edgegate::routing::UpstreamEndpoint{
                id, address, port_value(path, endpoint["port"], "endpoint.port"), true});
        }
        upstream_pools.emplace(pool_id, std::move(parsed_endpoints));
    }

    const YAML::Node routes = root["routes"];
    require_sequence(path, routes, "routes");
    if (routes.size() == 0U) {
        fail(path, routes, "routes must not be empty");
    }
    config.routes.reserve(routes.size());
    for (const YAML::Node& route : routes) {
        require_map(path, route, "route");
        reject_unknown_keys(path, route, {"id", "host", "path_prefix", "upstream_pool"});
        const std::string pool_id =
            required_scalar<std::string>(path, route, "upstream_pool");
        const auto pool = upstream_pools.find(pool_id);
        if (pool == upstream_pools.end()) {
            fail(path, route["upstream_pool"],
                "route references unknown upstream pool '" + pool_id + "'");
        }
        config.routes.push_back(edgegate::routing::RouteDefinition{
            required_scalar<std::string>(path, route, "id"),
            required_scalar<std::string>(path, route, "host"),
            required_scalar<std::string>(path, route, "path_prefix"),
            pool->second});
    }

    try {
        static_cast<void>(edgegate::routing::RouteTable(config.routes));
    } catch (const std::invalid_argument& error) {
        fail(path, routes, error.what());
    }
    return config;
}

} // namespace edgegate::config
// AI-CODE-END: S6-YAML-CONFIG-IMPLEMENTATION
