#include "edgegate/net/byte_buffer.h"

// AI-CODE-BEGIN: S4-BYTE-BUFFER-TESTS
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using edgegate::net::ByteBuffer;

// 测试：写入、消费部分数据后再次写入，缓冲区应保留未消费字节并复用已经释放的容量。
TEST(ByteBufferTest, AppendsConsumesAndReusesCapacity)
{
    ByteBuffer buffer(8);

    EXPECT_TRUE(buffer.append("abcd"));
    EXPECT_EQ(buffer.readable_view(), "abcd");
    EXPECT_EQ(buffer.writable_capacity(), 4U);

    EXPECT_TRUE(buffer.consume(3));
    EXPECT_EQ(buffer.readable_view(), "d");
    EXPECT_TRUE(buffer.append("efghijk"));
    EXPECT_EQ(buffer.readable_view(), "defghijk");
    EXPECT_EQ(buffer.writable_capacity(), 0U);
}

// 测试：新数据超过缓冲区容量时应拒绝写入，并保持原有可读数据和长度不变。
TEST(ByteBufferTest, RejectsOverflowWithoutChangingExistingBytes)
{
    ByteBuffer buffer(5);
    ASSERT_TRUE(buffer.append("abc"));

    EXPECT_FALSE(buffer.append("def"));
    EXPECT_EQ(buffer.readable_view(), "abc");
    EXPECT_EQ(buffer.readable_size(), 3U);
}

// 测试：要求消费的字节数超过当前可读量时应失败，且现有数据不能被破坏。
TEST(ByteBufferTest, RejectsConsumingMoreThanReadableBytes)
{
    ByteBuffer buffer(4);
    ASSERT_TRUE(buffer.append("ab"));

    EXPECT_FALSE(buffer.consume(3));
    EXPECT_EQ(buffer.readable_view(), "ab");
}

// 测试：全部可读字节被消费后缓冲区应恢复为空，并可重新使用全部容量。
TEST(ByteBufferTest, ClearsWhenAllBytesAreConsumed)
{
    ByteBuffer buffer(4);
    ASSERT_TRUE(buffer.append("abcd"));
    ASSERT_TRUE(buffer.consume(4));

    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.writable_capacity(), 4U);
    EXPECT_TRUE(buffer.append("xy"));
    EXPECT_EQ(buffer.readable_view(), "xy");
}

// 测试：最大容量配置为 0 没有实际意义，构造缓冲区时应抛出参数错误。
TEST(ByteBufferTest, RequiresPositiveMaximum)
{
    EXPECT_THROW(static_cast<void>(ByteBuffer(0)), std::invalid_argument);
}

} // namespace
// AI-CODE-END: S4-BYTE-BUFFER-TESTS
