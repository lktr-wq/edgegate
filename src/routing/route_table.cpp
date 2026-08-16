#include "edgegate/routing/route_table.h"

// AI-CODE-BEGIN: S6-ROUTE-TABLE-IMPLEMENTATION
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace edgegate::routing {

namespace {

bool all_decimal_digits(std::string_view value) noexcept
{
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return character >= '0' && character <= '9';
           });
}

bool valid_port(std::string_view value) noexcept
{
    if (!all_decimal_digits(value)) {
        return false;
    }
    std::uint32_t port = 0;
    for (char character : value) {
        port = port * 10U + static_cast<std::uint32_t>(character - '0');
        if (port > 65535U) {
            return false;
        }
    }
    return true;
}

bool valid_normalized_host(std::string_view host) noexcept
{
    if (host.empty()) {
        return false;
    }

    if (host.front() == '[') {
        if (host.size() < 3 || host.back() != ']') {
            return false;
        }
        const std::string_view address = host.substr(1, host.size() - 2);
        return !address.empty() &&
               std::all_of(address.begin(), address.end(), [](char character) {
                   const auto value = static_cast<unsigned char>(character);
                   return std::isxdigit(value) != 0 ||
                          character == ':' || character == '.';
               });
    }

    if (host.size() > 253) {
        return false;
    }
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const std::size_t dot = host.find('.', label_start);
        const std::size_t label_end =
            dot == std::string_view::npos ? host.size() : dot;
        const std::string_view label =
            host.substr(label_start, label_end - label_start);
        if (label.empty() || label.size() > 63 ||
            label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (char character : label) {
            const auto value = static_cast<unsigned char>(character);
            if (std::isalnum(value) == 0 && character != '-') {
                return false;
            }
        }
        if (dot == std::string_view::npos) {
            break;
        }
        label_start = dot + 1;
    }
    return true;
}

} // namespace

RouteTable::RouteTable(std::vector<RouteDefinition> routes)
{
    std::unordered_set<std::string> route_ids;
    std::unordered_set<std::string> match_keys;

    states_.reserve(routes.size());
    for (RouteDefinition& route : routes) {
        if (route.id.empty() || !route_ids.insert(route.id).second) {
            throw std::invalid_argument("route id must be non-empty and unique");
        }
        if (route.path_prefix.empty() || route.path_prefix.front() != '/') {
            throw std::invalid_argument("route path prefix must start with /");
        }
        if (route.upstreams.empty()) {
            throw std::invalid_argument("route must contain at least one upstream");
        }

        bool wildcard = false;
        std::string host_pattern = route.host_pattern;
        if (host_pattern.rfind("*.", 0) == 0) {
            wildcard = true;
            host_pattern.erase(0, 2);
        }
        if (host_pattern.find('*') != std::string::npos) {
            throw std::invalid_argument("wildcard is only allowed as *. prefix");
        }

        const std::string normalized = normalize_host(host_pattern);
        if (normalized.empty()) {
            throw std::invalid_argument("route host must be non-empty");
        }

        const std::string match_key =
            (wildcard ? "*." : "=") + normalized + "\n" + route.path_prefix;
        if (!match_keys.insert(match_key).second) {
            throw std::invalid_argument("duplicate host and path route");
        }

        std::unordered_set<std::string> upstream_ids;
        for (const UpstreamEndpoint& upstream : route.upstreams) {
            if (upstream.id.empty() ||
                !upstream_ids.insert(upstream.id).second ||
                upstream.address.empty() || upstream.port == 0) {
                throw std::invalid_argument(
                    "upstream id/address/port must be valid and ids unique");
            }
        }

        states_.push_back(RouteState{
            std::move(route), normalized, wildcard, 0});
    }

    public_routes_.reserve(states_.size());
    for (const RouteState& state : states_) {
        public_routes_.push_back(state.definition);
    }
}

RouteLookupResult RouteTable::lookup(
    std::string_view host_header,
    std::string_view request_target)
{
    return lookup(host_header, request_target, {});
}

// AI-CODE-BEGIN: S7-RETRY-AND-HEALTH-ROUTING
RouteLookupResult RouteTable::lookup(
    std::string_view host_header,
    std::string_view request_target,
    const std::vector<std::string>& excluded_upstream_ids)
{
    const std::string host = normalize_host(host_header);
    const std::string_view path = target_path(request_target);
    if (host.empty() || path.empty()) {
        return {};
    }

    RouteState* best = nullptr;
    for (RouteState& candidate : states_) {
        if (!host_matches(candidate, host) ||
            path.rfind(candidate.definition.path_prefix, 0) != 0) {
            continue;
        }

        if (best == nullptr ||
            (best->wildcard && !candidate.wildcard) ||
            (best->wildcard == candidate.wildcard &&
             candidate.normalized_host.size() > best->normalized_host.size()) ||
            (best->wildcard == candidate.wildcard &&
             candidate.normalized_host.size() == best->normalized_host.size() &&
             candidate.definition.path_prefix.size() >
                 best->definition.path_prefix.size())) {
            best = &candidate;
        }
    }

    if (best == nullptr) {
        return {};
    }

    const std::size_t count = best->definition.upstreams.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index =
            (best->round_robin_cursor + offset) % count;
        const UpstreamEndpoint& candidate =
            best->definition.upstreams[index];
        const bool excluded = std::find(
            excluded_upstream_ids.begin(),
            excluded_upstream_ids.end(),
            candidate.id) != excluded_upstream_ids.end();
        if (candidate.healthy && !excluded) {
            best->round_robin_cursor = (index + 1) % count;
            return {
                RouteLookupStatus::kMatched,
                best->definition.id,
                candidate};
        }
    }

    return {
        RouteLookupStatus::kNoHealthyUpstream,
        best->definition.id,
        std::nullopt};
}

std::size_t RouteTable::set_endpoint_health(
    std::string_view address,
    std::uint16_t port,
    bool healthy) noexcept
{
    std::size_t changed = 0;
    for (RouteState& route : states_) {
        for (UpstreamEndpoint& upstream : route.definition.upstreams) {
            if (upstream.address == address && upstream.port == port) {
                if (upstream.healthy != healthy) {
                    ++changed;
                }
                upstream.healthy = healthy;
            }
        }
    }
    for (RouteDefinition& route : public_routes_) {
        for (UpstreamEndpoint& upstream : route.upstreams) {
            if (upstream.address == address && upstream.port == port) {
                upstream.healthy = healthy;
            }
        }
    }
    return changed;
}

std::vector<UpstreamEndpoint> RouteTable::unique_endpoints() const
{
    std::vector<UpstreamEndpoint> result;
    std::unordered_set<std::string> keys;
    for (const RouteState& route : states_) {
        for (const UpstreamEndpoint& upstream : route.definition.upstreams) {
            const std::string key = upstream.address + ":" +
                                    std::to_string(upstream.port);
            if (keys.insert(key).second) {
                result.push_back(upstream);
            }
        }
    }
    return result;
}
// AI-CODE-END: S7-RETRY-AND-HEALTH-ROUTING

bool RouteTable::set_upstream_health(
    std::string_view route_id,
    std::string_view upstream_id,
    bool healthy) noexcept
{
    for (RouteState& route : states_) {
        if (route.definition.id != route_id) {
            continue;
        }
        for (UpstreamEndpoint& upstream : route.definition.upstreams) {
            if (upstream.id == upstream_id) {
                upstream.healthy = healthy;
                for (RouteDefinition& public_route : public_routes_) {
                    if (public_route.id == route_id) {
                        for (UpstreamEndpoint& public_upstream :
                             public_route.upstreams) {
                            if (public_upstream.id == upstream_id) {
                                public_upstream.healthy = healthy;
                            }
                        }
                    }
                }
                return true;
            }
        }
    }
    return false;
}

const std::vector<RouteDefinition>& RouteTable::routes() const noexcept
{
    return public_routes_;
}

std::string RouteTable::normalize_host(std::string_view host)
{
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) {
        host.remove_prefix(1);
    }
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) {
        host.remove_suffix(1);
    }

    if (host.empty()) {
        return {};
    }

    if (host.front() == '[') {
        const std::size_t bracket = host.find(']');
        if (bracket == std::string_view::npos) {
            return {};
        }
        if (bracket + 1 < host.size()) {
            if (host[bracket + 1] != ':' ||
                !valid_port(host.substr(bracket + 2))) {
                return {};
            }
        }
        host = host.substr(0, bracket + 1);
    } else {
        const std::size_t colon = host.rfind(':');
        if (colon != std::string_view::npos) {
            if (host.find(':') != colon ||
                !valid_port(host.substr(colon + 1))) {
                return {};
            }
            host = host.substr(0, colon);
        }
    }

    if (!host.empty() && host.back() == '.') {
        host.remove_suffix(1);
    }
    if (host.empty()) {
        return {};
    }

    std::string normalized(host);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (!valid_normalized_host(normalized)) {
        return {};
    }
    return normalized;
}

std::string_view RouteTable::target_path(std::string_view target) noexcept
{
    if (target.empty() || target.front() != '/') {
        return {};
    }
    return target.substr(0, target.find('?'));
}

bool RouteTable::host_matches(
    const RouteState& route,
    std::string_view normalized_host) noexcept
{
    if (!route.wildcard) {
        return normalized_host == route.normalized_host;
    }
    if (normalized_host.size() <= route.normalized_host.size()) {
        return false;
    }
    const std::size_t suffix_start =
        normalized_host.size() - route.normalized_host.size();
    return normalized_host[suffix_start - 1] == '.' &&
           normalized_host.substr(suffix_start) == route.normalized_host;
}

} // namespace edgegate::routing
// AI-CODE-END: S6-ROUTE-TABLE-IMPLEMENTATION
