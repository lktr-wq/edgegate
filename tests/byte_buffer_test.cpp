#include "edgegate/net/byte_buffer.h"

// AI-CODE-BEGIN: S4-BYTE-BUFFER-TESTS
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using edgegate::net::ByteBuffer;

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

TEST(ByteBufferTest, RejectsOverflowWithoutChangingExistingBytes)
{
    ByteBuffer buffer(5);
    ASSERT_TRUE(buffer.append("abc"));

    EXPECT_FALSE(buffer.append("def"));
    EXPECT_EQ(buffer.readable_view(), "abc");
    EXPECT_EQ(buffer.readable_size(), 3U);
}

TEST(ByteBufferTest, RejectsConsumingMoreThanReadableBytes)
{
    ByteBuffer buffer(4);
    ASSERT_TRUE(buffer.append("ab"));

    EXPECT_FALSE(buffer.consume(3));
    EXPECT_EQ(buffer.readable_view(), "ab");
}

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

TEST(ByteBufferTest, RequiresPositiveMaximum)
{
    EXPECT_THROW(static_cast<void>(ByteBuffer(0)), std::invalid_argument);
}

} // namespace
// AI-CODE-END: S4-BYTE-BUFFER-TESTS
