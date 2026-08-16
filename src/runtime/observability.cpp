#include "edgegate/runtime/observability.h"

// AI-CODE-BEGIN: S9-OBSERVABILITY-IMPLEMENTATION
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace edgegate::runtime {

namespace {

/*
 * 每个数是一个延迟区间的上界，最后一个桶保存所有更慢请求。
 * 固定桶只占固定内存，同时足以估算 P50/P95/P99。
 */
constexpr std::array<std::uint64_t, 13> kLatencyUpperBoundsMs{
    1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000,
    std::numeric_limits<std::uint64_t>::max()};

} // namespace

ObservabilityStore::ObservabilityStore(
    const edgegate::config::DashboardConfig& config)
    : config_(config), latency_buckets_(kLatencyUpperBoundsMs.size(), 0)
{
}

void ObservabilityStore::reconfigure(
    const edgegate::config::DashboardConfig& config) noexcept
{
    config_.refresh_interval_ms = config.refresh_interval_ms;
    config_.recent_error_limit = config.recent_error_limit;
    config_.slow_request_threshold_ms = config.slow_request_threshold_ms;
    config_.slow_request_limit = config.slow_request_limit;
    trim_bounded_records();
}

void ObservabilityStore::record_request(
    std::string_view method,
    std::string_view path,
    std::string_view route_id,
    const edgegate::routing::UpstreamEndpoint* upstream,
    int status,
    std::uint64_t response_bytes,
    std::uint64_t latency_ms,
    std::size_t attempts)
{
    ++request_count_;
    response_bytes_ += response_bytes;
    latency_sum_ms_ += latency_ms;
    latency_min_ms_ = request_count_ == 1
        ? latency_ms : std::min(latency_min_ms_, latency_ms);
    latency_max_ms_ = std::max(latency_max_ms_, latency_ms);
    ++status_codes_[status];

    const auto bucket = std::lower_bound(
        kLatencyUpperBoundsMs.begin(), kLatencyUpperBoundsMs.end(), latency_ms);
    const std::size_t bucket_index = static_cast<std::size_t>(
        std::distance(kLatencyUpperBoundsMs.begin(), bucket));
    ++latency_buckets_[bucket_index];

    const std::string route = route_id.empty() ? "unmatched" : std::string(route_id);
    RouteCounters& route_counters = routes_[route];
    ++route_counters.requests;
    if (status >= 400) {
        ++route_counters.errors;
    }

    if (latency_ms < config_.slow_request_threshold_ms) {
        return;
    }

    // 即使以后出现新的调用者，也在指标存储边界再次剥离查询参数，
    // 防止令牌、账号等内容进入内存快照和浏览器。
    std::string_view safe_path = path;
    safe_path = safe_path.substr(0, safe_path.find('?'));
    nlohmann::json record{
        {"sequence", next_slow_sequence_++},
        {"time", timestamp_utc()},
        {"method", method.empty() ? "unknown" : std::string(method)},
        {"path", safe_path.empty() ? "/" : std::string(safe_path)},
        {"route", route},
        {"status", status},
        {"latency_ms", latency_ms},
        {"attempts", attempts}};
    if (upstream != nullptr) {
        record["upstream"] = {
            {"id", upstream->id},
            {"address", upstream->address},
            {"port", upstream->port}};
    } else {
        record["upstream"] = nullptr;
    }
    slow_requests_.push_back(std::move(record));
    trim_bounded_records();
}

void ObservabilityStore::record_upstream_result(
    const edgegate::routing::UpstreamEndpoint& upstream,
    bool success,
    bool timeout) noexcept
{
    try {
        UpstreamCounters& counters = upstreams_[
            upstream_key(upstream.address, upstream.port)];
        counters.id = upstream.id;
        counters.address = upstream.address;
        counters.port = upstream.port;
        ++counters.attempts;
        if (success) {
            ++counters.successes;
        } else {
            ++counters.failures;
        }
        if (timeout) {
            ++counters.timeouts;
        }
    } catch (...) {
        // 指标记录失败不能改变代理请求的业务结果。
    }
}

void ObservabilityStore::record_error(
    std::string_view category,
    std::string_view detail)
{
    recent_errors_.push_back({
        {"sequence", next_error_sequence_++},
        {"time", timestamp_utc()},
        {"category", std::string(category)},
        {"detail", std::string(detail)}});
    trim_bounded_records();
}

nlohmann::json ObservabilityStore::snapshot() const
{
    nlohmann::json status_codes = nlohmann::json::object();
    std::array<std::uint64_t, 6> status_classes{};
    std::vector<std::pair<int, std::uint64_t>> ordered_status(
        status_codes_.begin(), status_codes_.end());
    std::sort(ordered_status.begin(), ordered_status.end());
    for (const auto& [code, count] : ordered_status) {
        status_codes[std::to_string(code)] = count;
        if (code >= 100 && code < 600) {
            status_classes[static_cast<std::size_t>(code / 100)] += count;
        }
    }

    nlohmann::json histogram = nlohmann::json::array();
    for (std::size_t index = 0; index < kLatencyUpperBoundsMs.size(); ++index) {
        nlohmann::json item{{"count", latency_buckets_[index]}};
        if (kLatencyUpperBoundsMs[index] ==
            std::numeric_limits<std::uint64_t>::max()) {
            item["le_ms"] = nullptr;
        } else {
            item["le_ms"] = kLatencyUpperBoundsMs[index];
        }
        histogram.push_back(std::move(item));
    }

    std::vector<std::pair<std::string, RouteCounters>> ordered_routes(
        routes_.begin(), routes_.end());
    std::sort(ordered_routes.begin(), ordered_routes.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    nlohmann::json routes = nlohmann::json::array();
    for (const auto& [id, counters] : ordered_routes) {
        routes.push_back({
            {"id", id},
            {"requests", counters.requests},
            {"errors", counters.errors}});
    }

    std::vector<UpstreamCounters> ordered_upstreams;
    ordered_upstreams.reserve(upstreams_.size());
    for (const auto& [key, counters] : upstreams_) {
        static_cast<void>(key);
        ordered_upstreams.push_back(counters);
    }
    std::sort(ordered_upstreams.begin(), ordered_upstreams.end(),
        [](const UpstreamCounters& left, const UpstreamCounters& right) {
            if (left.id != right.id) return left.id < right.id;
            if (left.address != right.address) return left.address < right.address;
            return left.port < right.port;
        });
    nlohmann::json upstreams = nlohmann::json::array();
    for (const UpstreamCounters& counters : ordered_upstreams) {
        upstreams.push_back({
            {"id", counters.id},
            {"address", counters.address},
            {"port", counters.port},
            {"attempts", counters.attempts},
            {"successes", counters.successes},
            {"failures", counters.failures},
            {"timeouts", counters.timeouts}});
    }

    return {
        {"requests", {
            {"total", request_count_},
            {"response_bytes", response_bytes_},
            {"status_codes", std::move(status_codes)},
            {"status_classes", {
                {"1xx", status_classes[1]},
                {"2xx", status_classes[2]},
                {"3xx", status_classes[3]},
                {"4xx", status_classes[4]},
                {"5xx", status_classes[5]}}}}},
        {"latency_ms", {
            {"samples", request_count_},
            {"min", request_count_ == 0 ? 0 : latency_min_ms_},
            {"average", request_count_ == 0 ? 0 : latency_sum_ms_ / request_count_},
            {"max", latency_max_ms_},
            {"p50", percentile(0.50)},
            {"p95", percentile(0.95)},
            {"p99", percentile(0.99)},
            {"approximate", true},
            {"histogram", std::move(histogram)}}},
        {"routes", std::move(routes)},
        {"upstreams", std::move(upstreams)},
        {"recent_errors", recent_errors_},
        {"slow_requests", slow_requests_},
        {"settings", {
            {"refresh_interval_ms", config_.refresh_interval_ms},
            {"recent_error_limit", config_.recent_error_limit},
            {"slow_request_threshold_ms", config_.slow_request_threshold_ms},
            {"slow_request_limit", config_.slow_request_limit}}}};
}

std::string ObservabilityStore::timestamp_utc()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm broken_down{};
    ::gmtime_r(&now, &broken_down);
    std::ostringstream output;
    output << std::put_time(&broken_down, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string ObservabilityStore::upstream_key(
    std::string_view address,
    std::uint16_t port)
{
    return std::string(address) + ":" + std::to_string(port);
}

std::uint64_t ObservabilityStore::percentile(double ratio) const noexcept
{
    if (request_count_ == 0) {
        return 0;
    }
    const auto rank = static_cast<std::uint64_t>(
        std::ceil(ratio * static_cast<double>(request_count_)));
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < latency_buckets_.size(); ++index) {
        cumulative += latency_buckets_[index];
        if (cumulative >= rank) {
            return kLatencyUpperBoundsMs[index] ==
                std::numeric_limits<std::uint64_t>::max()
                ? latency_max_ms_ : kLatencyUpperBoundsMs[index];
        }
    }
    return latency_max_ms_;
}

void ObservabilityStore::trim_bounded_records() noexcept
{
    while (recent_errors_.size() > config_.recent_error_limit) {
        recent_errors_.pop_front();
    }
    while (slow_requests_.size() > config_.slow_request_limit) {
        slow_requests_.pop_front();
    }
}

} // namespace edgegate::runtime
// AI-CODE-END: S9-OBSERVABILITY-IMPLEMENTATION
