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
    reject_unknown_keys(path, root, {"listen", "limits", "upstream_pools", "routes"});

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
