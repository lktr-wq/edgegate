#include "edgegate/http/message_rewriter.h"

// AI-CODE-BEGIN: S6-HEADER-REWRITER-TESTS
#include <string>

#include <gtest/gtest.h>

namespace {

// 测试：请求改写应保留业务 Header，删除客户端伪造的转发信息和连接专用字段。
TEST(MessageRewriterTest, RebuildsTrustedForwardingHeaders)
{
    edgegate::http::RequestParser parser;
    const auto result = parser.consume(
        "GET /api HTTP/1.1\r\n"
        "Host: api.example.test\r\n"
        "User-Agent: test-client\r\n"
        "Connection: keep-alive, X-Remove-Me, Host\r\n"
        "X-Remove-Me: secret\r\n"
        "Content-Length: 0\r\n"
        "Content-Length: 0\r\n"
        "X-Forwarded-For: 1.2.3.4\r\n\r\n");
    ASSERT_EQ(result.status, edgegate::http::ParseStatus::kMessageComplete);

    const std::string rewritten =
        edgegate::http::rewrite_request_for_upstream(parser, "127.0.0.1");
    EXPECT_NE(rewritten.find("Host: api.example.test\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("User-Agent: test-client\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-For: 127.0.0.1\r\n"),
              std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-Host: api.example.test\r\n"),
              std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-Proto: http\r\n"),
              std::string::npos);
    EXPECT_NE(rewritten.find("Connection: close\r\n"), std::string::npos);
    const std::string host_line = "\r\nHost: api.example.test\r\n";
    EXPECT_EQ(rewritten.find(host_line), rewritten.rfind(host_line));
    const std::string length_line = "\r\nContent-Length: 0\r\n";
    EXPECT_EQ(rewritten.find(length_line), rewritten.rfind(length_line));
    EXPECT_EQ(rewritten.find("1.2.3.4"), std::string::npos);
    EXPECT_EQ(rewritten.find("X-Remove-Me"), std::string::npos);
}

// 测试：原因短语为空时也要保留状态码后的必需空格，使重写结果仍是合法状态行。
TEST(MessageRewriterTest, KeepsStatusLineShapeWithoutReasonPhrase)
{
    edgegate::http::ResponseParser parser("GET");
    const auto result = parser.consume(
        "HTTP/1.1 200 \r\nContent-Length: 0\r\n\r\n");
    ASSERT_EQ(result.status, edgegate::http::ParseStatus::kMessageComplete);
    EXPECT_EQ(
        edgegate::http::rewrite_response_for_client(parser, true),
        "HTTP/1.1 200 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

// 测试：chunked 上游响应已被完整解析后，应转成明确长度并隔离上游 Connection。
TEST(MessageRewriterTest, ConvertsChunkedResponseToContentLength)
{
    edgegate::http::ResponseParser parser("GET");
    const auto result = parser.consume(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n");
    ASSERT_EQ(result.status, edgegate::http::ParseStatus::kMessageComplete);

    const std::string rewritten =
        edgegate::http::rewrite_response_for_client(parser, false);
    EXPECT_EQ(rewritten,
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Connection: keep-alive\r\n\r\nhello");
}

} // namespace
// AI-CODE-END: S6-HEADER-REWRITER-TESTS
