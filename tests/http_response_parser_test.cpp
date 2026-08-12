#include "edgegate/http/response_parser.h"

// AI-CODE-BEGIN: S3-RESPONSE-PARSER-TESTS
#include <algorithm>
#include <random>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using edgegate::http::ParseStatus;
using edgegate::http::ResponseBodyMode;
using edgegate::http::ResponseParseError;
using edgegate::http::ResponseParser;

const std::string kFixedResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "hello";

const std::string kChunkedResponse =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "4\r\nWiki\r\n"
    "5\r\npedia\r\n"
    "0\r\n\r\n";

TEST(HttpResponseParserTest, ParsesContentLengthResponse)
{
    ResponseParser parser;
    const auto result = parser.consume(kFixedResponse);

    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(result.error, ResponseParseError::kNone);
    EXPECT_EQ(parser.status_code(), 200);
    EXPECT_EQ(parser.version(), "HTTP/1.1");
    EXPECT_EQ(parser.reason_phrase(), "OK");
    EXPECT_EQ(parser.body_mode(), ResponseBodyMode::kContentLength);
    EXPECT_EQ(parser.body(), "hello");
    EXPECT_EQ(parser.raw_message(), kFixedResponse);
    ASSERT_TRUE(parser.header_value("content-type").has_value());
    EXPECT_EQ(*parser.header_value("CONTENT-TYPE"), "text/plain");
}

TEST(HttpResponseParserTest, WaitsForCompleteContentLengthBody)
{
    ResponseParser parser;
    const auto first = parser.consume(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe");

    EXPECT_EQ(first.status, ParseStatus::kNeedMoreData);
    EXPECT_EQ(parser.body_mode(), ResponseBodyMode::kContentLength);

    const auto second = parser.consume("llo");
    EXPECT_EQ(second.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.body(), "hello");
}

TEST(HttpResponseParserTest, CompletesFixedResponseAtEveryTwoChunkSplit)
{
    for (std::size_t split = 0; split <= kFixedResponse.size(); ++split) {
        ResponseParser parser;
        const std::string_view input(kFixedResponse);

        parser.consume(input.substr(0, split));
        const auto result = parser.consume(input.substr(split));

        EXPECT_EQ(result.status, ParseStatus::kMessageComplete)
            << "split=" << split;
        EXPECT_EQ(parser.body(), "hello") << "split=" << split;
    }
}

TEST(HttpResponseParserTest, LeavesNextResponseBytesForCaller)
{
    ResponseParser parser;
    const std::string next = "HTTP/1.1 204 No Content\r\n\r\n";
    const auto result = parser.consume(kFixedResponse + next);

    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.message_bytes(), kFixedResponse.size());
    EXPECT_EQ(parser.remaining_bytes(), next.size());
    EXPECT_EQ(parser.raw_message(), kFixedResponse);
}

TEST(HttpResponseParserTest, RecognizesResponsesThatMustNotHaveBody)
{
    ResponseParser head("HEAD");
    auto result = head.consume(
        "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\nnext");
    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(head.body_mode(), ResponseBodyMode::kNoBody);
    EXPECT_EQ(head.body(), "");
    EXPECT_EQ(head.remaining_bytes(), 4U);

    for (const int code : {100, 199, 204, 304}) {
        ResponseParser parser;
        const std::string response =
            "HTTP/1.1 " + std::to_string(code) + " Test\r\n\r\n";
        result = parser.consume(response);
        EXPECT_EQ(result.status, ParseStatus::kMessageComplete)
            << "status=" << code;
        EXPECT_EQ(parser.body_mode(), ResponseBodyMode::kNoBody)
            << "status=" << code;
    }
}

TEST(HttpResponseParserTest, UsesConnectionCloseAsMessageBoundary)
{
    ResponseParser parser;
    const auto received = parser.consume(
        "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhello");

    EXPECT_EQ(received.status, ParseStatus::kNeedMoreData);
    EXPECT_EQ(parser.body_mode(), ResponseBodyMode::kCloseDelimited);

    const auto eof = parser.notify_eof();
    EXPECT_EQ(eof.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.body(), "hello");
}

TEST(HttpResponseParserTest, RejectsPrematureEof)
{
    ResponseParser before_headers;
    before_headers.consume("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
    auto result = before_headers.notify_eof();
    EXPECT_EQ(result.error, ResponseParseError::kUnexpectedEof);

    ResponseParser during_fixed_body;
    during_fixed_body.consume(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe");
    result = during_fixed_body.notify_eof();
    EXPECT_EQ(result.error, ResponseParseError::kUnexpectedEof);
}

TEST(HttpResponseParserTest, DecodesChunkedResponse)
{
    ResponseParser parser;
    const auto result = parser.consume(kChunkedResponse);

    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(result.error, ResponseParseError::kNone);
    EXPECT_EQ(parser.body_mode(), ResponseBodyMode::kChunked);
    EXPECT_EQ(parser.body(), "Wikipedia");
    EXPECT_EQ(parser.raw_message(), kChunkedResponse);
}

TEST(HttpResponseParserTest, DecodesChunkedResponseByteByByte)
{
    ResponseParser parser;
    ParseStatus status = ParseStatus::kNeedMoreData;

    for (std::size_t index = 0; index < kChunkedResponse.size(); ++index) {
        status = parser.consume(
            std::string_view(kChunkedResponse.data() + index, 1)).status;
        if (index + 1 < kChunkedResponse.size()) {
            EXPECT_EQ(status, ParseStatus::kNeedMoreData)
                << "index=" << index;
        }
    }

    EXPECT_EQ(status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.body(), "Wikipedia");
}

TEST(HttpResponseParserTest, HandlesDeterministicRandomChunkBoundaries)
{
    std::mt19937 generator(20260813U);
    std::uniform_int_distribution<std::size_t> chunk_size(1, 9);

    for (int round = 0; round < 100; ++round) {
        ResponseParser parser;
        std::size_t offset = 0;
        ParseStatus status = ParseStatus::kNeedMoreData;

        while (offset < kChunkedResponse.size()) {
            const std::size_t count = std::min(
                chunk_size(generator),
                kChunkedResponse.size() - offset);
            status = parser.consume(std::string_view(
                kChunkedResponse.data() + offset,
                count)).status;
            offset += count;
        }

        EXPECT_EQ(status, ParseStatus::kMessageComplete)
            << "round=" << round;
        EXPECT_EQ(parser.body(), "Wikipedia") << "round=" << round;
    }
}

TEST(HttpResponseParserTest, AcceptsChunkExtensionAndTrailer)
{
    ResponseParser parser;
    const auto result = parser.consume(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5;name=value\r\nhello\r\n"
        "0\r\nX-Checksum: abc123\r\n\r\n");

    ASSERT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.body(), "hello");
    ASSERT_EQ(parser.trailers().size(), 1U);
    EXPECT_EQ(parser.trailers()[0].name, "X-Checksum");
    EXPECT_EQ(parser.trailers()[0].value, "abc123");
}

TEST(HttpResponseParserTest, RejectsInvalidChunkSyntax)
{
    ResponseParser bad_size;
    auto result = bad_size.consume(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "xyz\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidChunkSize);

    ResponseParser bad_terminator;
    result = bad_terminator.consume(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabcXX");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidChunkTerminator);

    ResponseParser forbidden_trailer;
    result = forbidden_trailer.consume(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nContent-Length: 3\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidTrailer);
}

TEST(HttpResponseParserTest, ValidatesStatusLine)
{
    ResponseParser bad_version;
    auto result = bad_version.consume("HTTP/2 200 OK\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kUnsupportedHttpVersion);

    ResponseParser non_digit;
    result = non_digit.consume("HTTP/1.1 2O0 OK\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidStatusCode);

    ResponseParser out_of_range;
    result = out_of_range.consume("HTTP/1.1 999 Bad\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidStatusCode);

    ResponseParser missing_separator;
    result = missing_separator.consume("HTTP/1.1 200OK\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidStatusLine);
}

TEST(HttpResponseParserTest, ValidatesHeadersAndFraming)
{
    ResponseParser no_colon;
    auto result = no_colon.consume(
        "HTTP/1.1 200 OK\r\nContent-Length 3\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kMalformedHeaderLine);

    ResponseParser bad_name;
    result = bad_name.consume(
        "HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidHeaderName);

    ResponseParser conflicting_lengths;
    result = conflicting_lengths.consume(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 3\r\nContent-Length: 4\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kConflictingContentLength);

    ResponseParser invalid_length;
    result = invalid_length.consume(
        "HTTP/1.1 200 OK\r\nContent-Length: 3x\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kInvalidContentLength);

    ResponseParser conflicting_framing;
    result = conflicting_framing.consume(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kConflictingMessageFraming);

    ResponseParser unsupported_transfer_encoding;
    result = unsupported_transfer_encoding.consume(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");
    EXPECT_EQ(
        result.error,
        ResponseParseError::kUnsupportedTransferEncoding);
}

TEST(HttpResponseParserTest, EnforcesHeaderAndBodyLimits)
{
    ResponseParser header_limited("GET", 16, 1024);
    auto result = header_limited.consume(std::string(16, 'A'));
    EXPECT_EQ(result.error, ResponseParseError::kHeaderTooLarge);

    ResponseParser fixed_body_limited("GET", 8192, 4);
    result = fixed_body_limited.consume(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kBodyTooLarge);

    ResponseParser chunked_body_limited("GET", 8192, 4);
    result = chunked_body_limited.consume(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n");
    EXPECT_EQ(result.error, ResponseParseError::kBodyTooLarge);

    ResponseParser close_body_limited("GET", 8192, 4);
    result = close_body_limited.consume(
        "HTTP/1.0 200 OK\r\n\r\nhello");
    EXPECT_EQ(result.error, ResponseParseError::kBodyTooLarge);
}

TEST(HttpResponseParserTest, RejectsWireBytesBeforeGrowingPastAbsoluteLimit)
{
    ResponseParser parser("GET", 32, 4);
    const auto result = parser.consume(std::string(37, 'x'));

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ResponseParseError::kBodyTooLarge);
    EXPECT_EQ(parser.buffered_bytes(), 0U);
}

TEST(HttpResponseParserTest, KeepsTerminalResultStable)
{
    ResponseParser parser;
    const auto complete = parser.consume(kFixedResponse);
    const std::size_t buffered = parser.buffered_bytes();
    const auto repeated = parser.consume("ignored");

    EXPECT_EQ(complete.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(repeated.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(parser.buffered_bytes(), buffered);
}

} // namespace
// AI-CODE-END: S3-RESPONSE-PARSER-TESTS
