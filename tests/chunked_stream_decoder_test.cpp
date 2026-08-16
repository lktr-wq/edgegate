#include "edgegate/http/chunked_stream_decoder.h"

// AI-CODE-BEGIN: S7-CHUNKED-STREAM-DECODER-TESTS
#include <gtest/gtest.h>

namespace {

using edgegate::http::ChunkedStreamDecoder;
using edgegate::http::ChunkedStreamStatus;

// 测试：chunked 边界被拆到任意单字节位置时，流式校验器仍能找到 0 块终点。
TEST(ChunkedStreamDecoderTest, CompletesByteByByteWithoutBufferingBody)
{
    const std::string wire =
        "5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nX-Trace: ok\r\n\r\n";
    ChunkedStreamDecoder decoder(32);
    for (std::size_t index = 0; index < wire.size(); ++index) {
        const auto status = decoder.consume(
            std::string_view(wire.data() + index, 1));
        EXPECT_NE(status, ChunkedStreamStatus::kError);
    }
    EXPECT_EQ(decoder.status(), ChunkedStreamStatus::kComplete);
    EXPECT_EQ(decoder.decoded_body_bytes(), 11U);
}

// 测试：声明长度与实际分块终止符不一致时必须报错，不能把歧义字节转成下一块。
TEST(ChunkedStreamDecoderTest, RejectsMalformedChunkTerminator)
{
    ChunkedStreamDecoder decoder(32);
    EXPECT_EQ(decoder.consume("3\r\nabcX\n"), ChunkedStreamStatus::kError);
}

// 测试：累计解码正文超过产品上限时稳定拒绝，即使单个块本身并不大。
TEST(ChunkedStreamDecoderTest, EnforcesDecodedBodyLimit)
{
    ChunkedStreamDecoder decoder(5);
    EXPECT_EQ(decoder.consume("3\r\nabc\r\n3\r\n"),
              ChunkedStreamStatus::kError);
}

// 测试：Trailer 不允许重新声明 Content-Length 等消息边界字段。
TEST(ChunkedStreamDecoderTest, RejectsFramingFieldInTrailer)
{
    ChunkedStreamDecoder decoder(32);
    EXPECT_EQ(decoder.consume("0\r\nContent-Length: 1\r\n\r\n"),
              ChunkedStreamStatus::kError);
}

} // namespace
// AI-CODE-END: S7-CHUNKED-STREAM-DECODER-TESTS
