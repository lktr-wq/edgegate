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
        if (candidate.healthy) {
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
                !all_decimal_digits(host.substr(bracket + 2))) {
                return {};
            }
        }
        host = host.substr(0, bracket + 1);
    } else {
        const std::size_t colon = host.rfind(':');
        if (colon != std::string_view::npos) {
            if (host.find(':') != colon ||
                !all_decimal_digits(host.substr(colon + 1))) {
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
