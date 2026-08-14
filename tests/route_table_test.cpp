#include "edgegate/routing/route_table.h"

// AI-CODE-BEGIN: S6-ROUTE-TABLE-TESTS
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using edgegate::routing::RouteDefinition;
using edgegate::routing::RouteLookupStatus;
using edgegate::routing::RouteTable;
using edgegate::routing::UpstreamEndpoint;

UpstreamEndpoint upstream(std::string id, std::uint16_t port)
{
    return {std::move(id), "127.0.0.1", port, true};
}

RouteDefinition route(
    std::string id,
    std::string host,
    std::string path,
    std::vector<UpstreamEndpoint> upstreams)
{
    return {
        std::move(id),
        std::move(host),
        std::move(path),
        std::move(upstreams)};
}

// 测试：Host 匹配应忽略字母大小写和请求 Host 中附带的端口号。
TEST(RouteTableTest, MatchesHostCaseInsensitivelyAndIgnoresPort)
{
    RouteTable table({route(
        "api", "api.example.com", "/", {upstream("one", 9001)})});

    const auto result = table.lookup("API.Example.COM:18080", "/users");
    ASSERT_EQ(result.status, RouteLookupStatus::kMatched);
    EXPECT_EQ(result.route_id, "api");
    EXPECT_EQ(result.upstream->id, "one");
}

// 测试：精确 Host 和通配 Host 同时匹配时，应优先选择精确 Host 路由。
TEST(RouteTableTest, ExactHostBeatsWildcardHost)
{
    RouteTable table({
        route("wild", "*.example.com", "/api", {upstream("wild", 9001)}),
        route("exact", "api.example.com", "/", {upstream("exact", 9002)})});

    const auto result = table.lookup("api.example.com", "/api/items");
    ASSERT_EQ(result.status, RouteLookupStatus::kMatched);
    EXPECT_EQ(result.route_id, "exact");
}

// 测试：多个通配 Host 都匹配时，应选择后缀更长、范围更具体的路由。
TEST(RouteTableTest, MostSpecificWildcardSuffixWins)
{
    RouteTable table({
        route("broad", "*.example.com", "/", {upstream("broad", 9001)}),
        route(
            "narrow",
            "*.api.example.com",
            "/",
            {upstream("narrow", 9002)})});

    const auto result = table.lookup("v1.api.example.com", "/");
    ASSERT_EQ(result.status, RouteLookupStatus::kMatched);
    EXPECT_EQ(result.route_id, "narrow");
}

// 测试：`*.example.com` 只能匹配其子域名，不能匹配裸域名 `example.com`。
TEST(RouteTableTest, WildcardDoesNotMatchBareSuffix)
{
    RouteTable table({route(
        "wild", "*.example.com", "/", {upstream("one", 9001)})});

    EXPECT_EQ(
        table.lookup("example.com", "/").status,
        RouteLookupStatus::kNoRoute);
}

// 测试：同一 Host 下应选择最长路径前缀，且查询字符串不能参与路径匹配。
TEST(RouteTableTest, LongestPathPrefixWinsAndQueryIsIgnored)
{
    RouteTable table({
        route("root", "api.example.com", "/", {upstream("root", 9001)}),
        route("api", "api.example.com", "/api", {upstream("api", 9002)}),
        route(
            "users",
            "api.example.com",
            "/api/users",
            {upstream("users", 9003)})});

    const auto result =
        table.lookup("api.example.com.", "/api/users/42?full=true");
    ASSERT_EQ(result.status, RouteLookupStatus::kMatched);
    EXPECT_EQ(result.route_id, "users");
}

// 测试：当前路由采用字面前缀规则，因此 `/api` 也会匹配以 `/api` 开头的 `/apix`。
TEST(RouteTableTest, UsesLiteralPrefixSemantics)
{
    RouteTable table({route(
        "api", "api.example.com", "/api", {upstream("one", 9001)})});

    EXPECT_EQ(
        table.lookup("api.example.com", "/apix").status,
        RouteLookupStatus::kMatched);
}

// 测试：同一路由应只在健康上游之间轮询，并按顺序循环选择可用节点。
TEST(RouteTableTest, RoundRobinsOnlyAcrossHealthyUpstreams)
{
    RouteTable table({route(
        "api",
        "api.example.com",
        "/",
        {upstream("one", 9001),
         upstream("two", 9002),
         upstream("three", 9003)})});

    EXPECT_EQ(table.lookup("api.example.com", "/").upstream->id, "one");
    EXPECT_EQ(table.lookup("api.example.com", "/").upstream->id, "two");
    EXPECT_EQ(table.lookup("api.example.com", "/").upstream->id, "three");
    EXPECT_EQ(table.lookup("api.example.com", "/").upstream->id, "one");

    ASSERT_TRUE(table.set_upstream_health("api", "two", false));
    EXPECT_EQ(table.lookup("api.example.com", "/").upstream->id, "three");
    EXPECT_EQ(table.lookup("api.example.com", "/").upstream->id, "one");
}

// 测试：Host 无匹配路由与路由存在但全部上游不健康，应返回两种不同状态。
TEST(RouteTableTest, DistinguishesNoRouteFromNoHealthyUpstream)
{
    auto only = upstream("one", 9001);
    only.healthy = false;
    RouteTable table({route(
        "api", "api.example.com", "/", {std::move(only)})});

    EXPECT_EQ(
        table.lookup("other.example.com", "/").status,
        RouteLookupStatus::kNoRoute);
    const auto unhealthy = table.lookup("api.example.com", "/");
    EXPECT_EQ(unhealthy.status, RouteLookupStatus::kNoHealthyUpstream);
    EXPECT_EQ(unhealthy.route_id, "api");
    EXPECT_FALSE(unhealthy.upstream.has_value());
}

// 测试：空路由 ID、非法 Host/路径、重复定义和无上游等错误配置应在构造时拒绝。
TEST(RouteTableTest, RejectsInvalidAndDuplicateDefinitions)
{
    EXPECT_THROW(
        RouteTable({route("", "example.com", "/", {upstream("u", 1)})}),
        std::invalid_argument);
    EXPECT_THROW(
        RouteTable({route("r", "*bad.example.com", "/", {upstream("u", 1)})}),
        std::invalid_argument);
    EXPECT_THROW(
        RouteTable({route("r", "example.com", "api", {upstream("u", 1)})}),
        std::invalid_argument);
    EXPECT_THROW(
        RouteTable({
            route("one", "example.com", "/", {upstream("u1", 1)}),
            route("two", "EXAMPLE.COM", "/", {upstream("u2", 2)})}),
        std::invalid_argument);
}

// 测试：更新不存在的路由或上游健康状态应失败，并且不能改变原有有效路由。
TEST(RouteTableTest, ReportsMissingHealthTargetWithoutMutation)
{
    RouteTable table({route(
        "api", "api.example.com", "/", {upstream("one", 9001)})});

    EXPECT_FALSE(table.set_upstream_health("missing", "one", false));
    EXPECT_FALSE(table.set_upstream_health("api", "missing", false));
    EXPECT_EQ(
        table.lookup("api.example.com", "/").status,
        RouteLookupStatus::kMatched);
}

// 测试：配置或查询中的非法 Host 语法、标签格式和端口应被拒绝或判定为无路由。
TEST(RouteTableTest, RejectsInvalidHostSyntaxAndPort)
{
    EXPECT_THROW(
        RouteTable({route(
            "space", "bad host.example", "/", {upstream("u", 9001)})}),
        std::invalid_argument);
    EXPECT_THROW(
        RouteTable({route(
            "label", "bad..example", "/", {upstream("u", 9001)})}),
        std::invalid_argument);

    RouteTable table({route(
        "api", "api.example.com", "/", {upstream("u", 9001)})});
    EXPECT_EQ(
        table.lookup("api.example.com:70000", "/").status,
        RouteLookupStatus::kNoRoute);
    EXPECT_EQ(
        table.lookup("bad host.example", "/").status,
        RouteLookupStatus::kNoRoute);
}

} // namespace
// AI-CODE-END: S6-ROUTE-TABLE-TESTS
