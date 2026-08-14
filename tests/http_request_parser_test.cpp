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

// 测试：一次传入完整的 GET 请求时，解析器应立即得到一条完整且无正文的请求。
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

// 测试：请求头还缺少结尾空行时，解析器应报告“还需要更多数据”，而不是误判为完整请求。
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

// 测试：即使完整请求头尚未到达，只要请求行已经收全，就应先解析出方法、目标和 HTTP 版本。
TEST(HttpRequestParserTest, ParsesRequestLineBeforeHeaderCompletes)
{
    RequestParser parser;
    const auto result = parser.consume("POST /submit HTTP/1.1\r\n");

    EXPECT_EQ(result.status, ParseStatus::kNeedMoreData);
    // 当前只收到完整请求行，还没有收到 Header 结束标志，所以仍需更多数据。
    // 即使请求没有 body，也必须等 Header 解析和校验通过后才能判定完整。
    EXPECT_EQ(parser.method(), "POST");
    EXPECT_EQ(parser.target(), "/submit");
    EXPECT_EQ(parser.version(), "HTTP/1.1");
}

// 测试：标志请求头结束的 \r\n\r\n 被拆到两次接收中时，解析器仍应正确识别边界。
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

// 测试：把请求每次只喂入 1 字节，最终结果应与一次性传入完整请求相同。
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

// 测试：穷举所有“两段式分片”位置，确保 TCP 在任何位置拆包都不会影响解析结果。
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

// 测试：把带正文的 POST 请求随机切成许多小块，重复 100 次后都应正确还原正文。
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

// 测试：请求头达到配置上限却仍未形成完整请求头时，应拒绝并报告请求头过大。
TEST(HttpRequestParserTest, RejectsOversizedHeader)
{
    RequestParser parser(16);
    const auto result = parser.consume(std::string(16, 'A'));

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kHeaderTooLarge);
}

// 测试：完整请求头的大小刚好等于配置上限时仍应接受，验证边界没有少算 1 字节。
TEST(HttpRequestParserTest, AcceptsHeaderExactlyAtLimit)
{
    RequestParser parser(kSimpleGet.size());
    const auto result = parser.consume(kSimpleGet);

    EXPECT_EQ(result.status, ParseStatus::kMessageComplete);
    EXPECT_EQ(result.error, ParseError::kNone);
}

// 测试：请求行缺少请求目标（例如路径 /）时，应判定请求行格式错误。
TEST(HttpRequestParserTest, RejectsMissingRequestTarget)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET  HTTP/1.1\r\nHost: example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidRequestLine);
}

// 测试：HTTP 方法名含有不允许的字符“(”时，应判定请求行格式错误。
TEST(HttpRequestParserTest, RejectsInvalidMethodCharacter)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GE(T / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidRequestLine);
}

// 测试：请求目标中含有控制字符时，应拒绝该请求，防止把不可见非法字节当作路径。
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

// 测试：本项目只接受 HTTP/1.1，因此收到 HTTP/1.0 时应返回“不支持的版本”。
TEST(HttpRequestParserTest, RejectsUnsupportedHttpVersion)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kUnsupportedHttpVersion);
}

// 测试：解析器应保存多个 Header，并且查询 Header 名时不区分英文字母大小写。
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

// 测试：Header 值两端的空格和制表符应被去掉，但值内部的冒号必须保留。
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

// 测试：一行 Header 被拆到三次输入中时，解析器仍应拼接并得到完整的 Host 值。
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

// 测试：Header 行缺少分隔名称和值的冒号时，应报告 Header 行格式错误。
TEST(HttpRequestParserTest, RejectsHeaderWithoutColon)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\nHost example.com\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kMalformedHeaderLine);
}

// 测试：Header 名中包含空格等非法字符时，应报告 Header 名非法。
TEST(HttpRequestParserTest, RejectsInvalidHeaderName)
{
    RequestParser parser;
    const auto result = parser.consume(
        "GET / HTTP/1.1\r\nBad Header: value\r\n\r\n");

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kInvalidHeaderName);
}

// 测试：Header 值中夹入不允许的控制字符时，应拒绝该请求。
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

// 测试：以空格开头、试图接续上一行的旧式折叠 Header 不在项目支持范围内，应明确拒绝。
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

// 测试：HTTP/1.1 请求必须有且只能有一个非空 Host；分别验证缺失、空值和重复三种错误。
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

// 测试：Content-Length 声明 5 字节但首批正文只有 2 字节时，应等待剩余 3 字节再完成。
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

// 测试：Content-Length 为 0 应有效；重复出现且数值相同的 Content-Length 也应有效。
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

// 测试：Content-Length 含非数字字符或多个值互相冲突时，应分别返回对应错误。
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

// 测试：声明的正文长度超过配置上限时，即使正文尚未到达也应立即拒绝。
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

// 测试：单次输入已超过“请求头上限 + 正文上限”时，应在扩充内部缓冲区前拒绝数据。
TEST(HttpRequestParserTest, RejectsWireBytesBeforeGrowingPastAbsoluteLimit)
{
    RequestParser parser(32, 4);
    const auto result = parser.consume(std::string(37, 'x'));

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kBodyTooLarge);
    EXPECT_EQ(parser.buffered_bytes(), 0U);
}

// 测试：项目不支持 chunked 请求体和 Expect: 100-continue，遇到时应返回明确错误。
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

// 测试：同一次输入中连续放入两条请求属于 HTTP 流水线，本项目不支持并应明确拒绝。
TEST(HttpRequestParserTest, RejectsPipelinedBytesInSameChunk)
{
    RequestParser parser;
    const auto result = parser.consume(kSimpleGet + kSimpleGet);

    EXPECT_EQ(result.status, ParseStatus::kError);
    EXPECT_EQ(result.error, ParseError::kPipeliningNotSupported);
}

// 测试：请求已经解析完成后再次喂入数据，解析器应保持原完成结果且不再修改缓冲区。
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
