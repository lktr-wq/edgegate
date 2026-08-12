#include "edgegate/http/request_parser.h"

// AI-CODE-BEGIN: S3-REQUEST-PARSER-TESTS
#include <algorithm>
#include <random>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using edgegate::http::ParseError;
using edgegate::http::ParseStatus;
using edgegate::http::RequestParser;

const std::string kSimpleGet =
    "GET / HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "\r\n";

TEST(HttpRequestParserTest, CompletesSimpleGetFromOneChunk)
{
    RequestParser parser;
    const auto result = parser.consume(kSimpleGet);

    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(result.error, ParseError::kNone);
    EXPECT_EQ(parser.header_bytes(), kSimpleGet.size());
    EXPECT_EQ(parser.message_bytes(), kSimpleGet.size());
    EXPECT_EQ(parser.body(), "");
}

TEST(HttpRequestParserTest, NeedsMoreDataForIncompleteHeader)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n");

    EXPECT_EQ(result.status, ParseStatus::kNeedMoreData);
    EXPECT_EQ(result.error, ParseError::kNone);
    EXPECT_EQ(parser.header_bytes(), 0U);
}

TEST(HttpRequestParserTest, ParsesRequestLineBeforeHeaderCompletes)
{
    RequestParser parser;
    const auto result = parser.consume("POST /submit HTTP/1.1\r\n");

    EXPECT_EQ(result.status, ParseStatus::kNeedMoreData);
    EXPECT_EQ(parser.method(), "POST");
    EXPECT_EQ(parser.target(), "/submit");
    EXPECT_EQ(parser.version(), "HTTP/1.1");
}

TEST(HttpRequestParserTest, FindsHeaderEndAcrossChunks)
{
    RequestParser parser;
    EXPECT_EQ(
        parser.consume(
            "GET / HTTP/1.1\r\n"
            "Host: example.com\r\n\r").status,
        ParseStatus::kNeedMoreData);

    EXPECT_EQ(
        parser.consume("\n").status,
        ParseStatus::kMessageComplete);
}

TEST(HttpRequestParserTest, CompletesByteByByte)
{
    RequestParser parser;
    ParseStatus status = ParseStatus::kNeedMoreData;

    for (std::size_t index = 0; index < kSimpleGet.size(); ++index) {
        const auto result = parser.consume(
            std::string_view(kSimpleGet.data() + index, 1));
        status = result.status;

        if (index + 1 < kSimpleGet.size()) {
            EXPECT_EQ(status, ParseStatus::kNeedMoreData);
        }
    }

    EXPECT_EQ(status, ParseStatus::kMessageComplete);
}

TEST(HttpRequestParserTest, CompletesAtEveryTwoChunkSplit)
{
    for (std::size_t split = 0; split <= kSimpleGet.size(); ++split) {
        RequestParser parser;
        const std::string_view input(kSimpleGet);

        parser.consume(input.substr(0, split));
        const auto result = parser.consume(input.substr(split));

        EXPECT_EQ(result.status, ParseStatus::kMessageComplete)
            << "split=" << split;
    }
}

TEST(HttpRequestParserTest, CompletesUnderDeterministicRandomFragmentation)
{
    const std::string request =
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "hello world";

    std::mt19937 generator(20260813U);
    std::uniform_int_distribution<std::size_t> chunk_size(1, 7);

    for (int round = 0; round < 100; ++round) {
        RequestParser parser;
        std::size_t offset = 0;
        ParseStatus status = ParseStatus::kNeedMoreData;

        while (offset < request.size()) {
            const std::size_t count = std::min(
                chunk_size(generator),
                request.size() - offset);
            status = parser.consume(
                std::string_view(request.data() + offset, count)).status;
            offset += count;
        }

        EXPECT_EQ(status, ParseStatus::kMessageComplete)
            << "round=" << round;
        EXPECT_EQ(parser.body(), "hello world");
    }
}

TEST(HttpRequestParserTest, RejectsOversizedHeader)
{
    RequestParser parser(16);
    const auto result = parser.consume(std::string(16, 'A'));

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kHeaderTooLarge);
}

TEST(HttpRequestParserTest, AcceptsHeaderExactlyAtLimit)
{
    RequestParser parser(kSimpleGet.size());
    const auto result = parser.consume(kSimpleGet);

    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(result.error, ParseError::kNone);
}

TEST(HttpRequestParserTest, RejectsMissingRequestTarget)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET  HTTP/1.1\r\nHost: example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidRequestLine);
}

TEST(HttpRequestParserTest, RejectsInvalidMethodCharacter)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GE(T / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidRequestLine);
}

TEST(HttpRequestParserTest, RejectsControlCharacterInTarget)
{
    RequestParser parser;
    std::string request = "GET /bad";
    request.push_back('\x01');
    request += "path HTTP/1.1\r\nHost: example.com\r\n\r\n";

    const auto result = parser.consume(request);
    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidRequestLine);
}

TEST(HttpRequestParserTest, RejectsUnsupportedHttpVersion)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kUnsupportedHttpVersion);
}

TEST(HttpRequestParserTest, ParsesHeadersAndIgnoresNameCase)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n");

    ASSERT_EQ(result.status, ParseStatus::kMessageComplete);
    ASSERT_EQ(parser.headers().size(), 2U);

    const auto host = parser.header_value("hOsT");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "example.com");

    const auto type = parser.header_value("CONTENT-TYPE");
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, "text/plain");
}

TEST(HttpRequestParserTest, TrimsHeaderWhitespaceAndKeepsValueColon)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Trace: \t abc:123 \t\r\n"
        "\r\n");

    ASSERT_EQ(result.status, ParseStatus::kMessageComplete);
    const auto value = parser.header_value("x-trace");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "abc:123");
}

TEST(HttpRequestParserTest, ParsesHeaderAcrossChunks)
{
    RequestParser parser;
    parser.consume("GET / HTTP/1.1\r\nHo");
    parser.consume("st: exam");
    const auto result = parser.consume("ple.com\r\n\r\n");

    ASSERT_EQ(result.status, ParseStatus::kMessageComplete);
    ASSERT_TRUE(parser.header_value("host").has_value());
    EXPECT_EQ(*parser.header_value("host"), "example.com");
}

TEST(HttpRequestParserTest, RejectsHeaderWithoutColon)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\nHost example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kMalformedHeaderLine);
}

TEST(HttpRequestParserTest, RejectsInvalidHeaderName)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\nBad Header: value\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidHeaderName);
}

TEST(HttpRequestParserTest, RejectsControlCharacterInHeaderValue)
{
    RequestParser parser;
    std::string request =
        "GET / HTTP/1.1\r\nHost: example.com\r\nX-Test: ok";
    request.push_back('\x01');
    request += "bad\r\n\r\n";

    const auto result = parser.consume(request);
    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidHeaderValue);
}

TEST(HttpRequestParserTest, RejectsFoldedHeaderLine)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Test: first\r\n"
        " second\r\n"
        "\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kMalformedHeaderLine);
}

TEST(HttpRequestParserTest, RequiresExactlyOneNonEmptyHost)
{
    RequestParser missing_host;
    auto result = missing_host.consume("GET / HTTP/1.1\r\n\r\n");
    EXPECT_EQ(result.error, ParseError::kMissingHost);

    RequestParser empty_host;
    result = empty_host.consume("GET / HTTP/1.1\r\nHost: \r\n\r\n");
    EXPECT_EQ(result.error, ParseError::kMissingHost);

    RequestParser duplicate_host;
    result = duplicate_host.consume(
        "GET / HTTP/1.1\r\n"
        "Host: one.example\r\n"
        "host: two.example\r\n"
        "\r\n");
    EXPECT_EQ(result.error, ParseError::kDuplicateHost);
}

TEST(HttpRequestParserTest, WaitsForContentLengthBodyAcrossChunks)
{
    RequestParser parser;
    const auto first = parser.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "he");

    EXPECT_EQ(first.status, ParseStatus::kNeedMoreData);
    EXPECT_EQ(parser.content_length(), 5U);

    const auto second = parser.consume("llo");
    EXPECT_EQ(second.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.body(), "hello");
    EXPECT_EQ(parser.message_bytes(), parser.buffered_bytes());
}

TEST(HttpRequestParserTest, AcceptsZeroAndRepeatedIdenticalContentLength)
{
    RequestParser zero;
    auto result = zero.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);

    RequestParser repeated;
    result = repeated.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 3\r\n"
        "content-length: 3\r\n\r\n"
        "abc");
    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(repeated.body(), "abc");
}

TEST(HttpRequestParserTest, RejectsInvalidOrConflictingContentLength)
{
    RequestParser invalid;
    auto result = invalid.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 3x\r\n\r\n");
    EXPECT_EQ(result.error, ParseError::kInvalidContentLength);

    RequestParser conflicting;
    result = conflicting.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 3\r\n"
        "Content-Length: 4\r\n\r\n");
    EXPECT_EQ(result.error, ParseError::kConflictingContentLength);
}

TEST(HttpRequestParserTest, RejectsBodyOverConfiguredLimit)
{
    RequestParser parser(8192, 4);
    const auto result = parser.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kBodyTooLarge);
}

TEST(HttpRequestParserTest, RejectsUnsupportedRequestFeatures)
{
    RequestParser chunked;
    auto result = chunked.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    EXPECT_EQ(result.error, ParseError::kUnsupportedTransferEncoding);

    RequestParser expect;
    result = expect.consume(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    EXPECT_EQ(result.error, ParseError::kUnsupportedExpectation);
}

TEST(HttpRequestParserTest, RejectsPipelinedBytesInSameChunk)
{
    RequestParser parser;
    const auto result = parser.consume(kSimpleGet + kSimpleGet);

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kPipeliningNotSupported);
}

TEST(HttpRequestParserTest, KeepsTerminalResultStable)
{
    RequestParser parser;
    const auto complete = parser.consume(kSimpleGet);
    const std::size_t buffered = parser.buffered_bytes();
    const auto repeated = parser.consume("ignored");

    EXPECT_EQ(complete.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(repeated.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.buffered_bytes(), buffered);
}

} // namespace
// AI-CODE-END: S3-REQUEST-PARSER-TESTS
