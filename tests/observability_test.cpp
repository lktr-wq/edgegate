#include "edgegate/runtime/observability.h"

// AI-CODE-BEGIN: S9-OBSERVABILITY-TESTS
#include <string>

#include <gtest/gtest.h>

namespace {

using edgegate::runtime::ObservabilityStore;

// 测试：固定延迟桶能给出近似分位数，并同时累计状态码、路由和上游结果。
TEST(ObservabilityTest, AggregatesBoundedRequestMetrics)
{
    edgegate::config::DashboardConfig config;
    config.slow_request_threshold_ms = 10;
    config.slow_request_limit = 2;
    ObservabilityStore store(config);
    const edgegate::routing::UpstreamEndpoint upstream{
        "backend-a", "127.0.0.1", 19081, true};

    store.record_upstream_result(upstream, true, false);
    store.record_upstream_result(upstream, false, true);
    store.record_request("GET", "/fast", "api", &upstream, 200, 100, 1, 1);
    store.record_request("GET", "/warm", "api", &upstream, 200, 200, 8, 1);
    store.record_request("GET", "/slow-a", "api", &upstream, 502, 300, 20, 2);
    store.record_request("GET", "/slow-b", "api", &upstream, 504, 400, 900, 2);
    store.record_request(
        "GET", "/slow-c?token=secret", "api", &upstream, 200, 500, 600, 1);

    const auto snapshot = store.snapshot();
    EXPECT_EQ(snapshot["requests"]["total"], 5);
    EXPECT_EQ(snapshot["requests"]["status_codes"]["200"], 3);
    EXPECT_EQ(snapshot["requests"]["status_classes"]["5xx"], 2);
    EXPECT_EQ(snapshot["latency_ms"]["p50"], 25);
    EXPECT_EQ(snapshot["latency_ms"]["p95"], 1000);
    EXPECT_TRUE(snapshot["latency_ms"]["approximate"]);
    ASSERT_EQ(snapshot["routes"].size(), 1U);
    EXPECT_EQ(snapshot["routes"][0]["errors"], 2);
    ASSERT_EQ(snapshot["upstreams"].size(), 1U);
    EXPECT_EQ(snapshot["upstreams"][0]["attempts"], 2);
    EXPECT_EQ(snapshot["upstreams"][0]["timeouts"], 1);
    ASSERT_EQ(snapshot["slow_requests"].size(), 2U);
    EXPECT_EQ(snapshot["slow_requests"][0]["path"], "/slow-b");
    EXPECT_EQ(snapshot["slow_requests"][1]["path"], "/slow-c");
    EXPECT_EQ(snapshot.dump().find("secret"), std::string::npos);
}

// 测试：最近错误和慢请求达到上限后淘汰最旧项，热更新更小上限也立即收缩。
TEST(ObservabilityTest, KeepsOnlyNewestBoundedRecords)
{
    edgegate::config::DashboardConfig config;
    config.recent_error_limit = 2;
    config.slow_request_threshold_ms = 1;
    config.slow_request_limit = 3;
    ObservabilityStore store(config);
    store.record_error("first", "one");
    store.record_error("second", "two");
    store.record_error("third", "three");
    store.record_request("GET", "/a", "route", nullptr, 404, 10, 2, 0);
    store.record_request("GET", "/b", "route", nullptr, 404, 10, 3, 0);

    auto snapshot = store.snapshot();
    ASSERT_EQ(snapshot["recent_errors"].size(), 2U);
    EXPECT_EQ(snapshot["recent_errors"][0]["category"], "second");

    config.recent_error_limit = 1;
    config.slow_request_limit = 1;
    store.reconfigure(config);
    snapshot = store.snapshot();
    ASSERT_EQ(snapshot["recent_errors"].size(), 1U);
    EXPECT_EQ(snapshot["recent_errors"][0]["category"], "third");
    ASSERT_EQ(snapshot["slow_requests"].size(), 1U);
    EXPECT_EQ(snapshot["slow_requests"][0]["path"], "/b");
}

} // namespace
// AI-CODE-END: S9-OBSERVABILITY-TESTS
