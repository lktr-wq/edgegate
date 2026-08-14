#include "edgegate/net/unique_fd.h"

// AI-CODE-BEGIN: S4-UNIQUE-FD-TESTS
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

using edgegate::net::UniqueFd;

// 测试：UniqueFd 离开作用域析构时，应自动关闭它唯一拥有的文件描述符。
TEST(UniqueFdTest, ClosesOwnedDescriptorAtScopeExit)
{
    int descriptor = -1;
    {
        UniqueFd owned(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        ASSERT_TRUE(owned.valid());
        descriptor = owned.get();
        EXPECT_NE(::fcntl(descriptor, F_GETFD), -1);
    }

    errno = 0;
    EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

// 测试：移动构造应把 fd 的唯一所有权转给目标对象，源对象随后变为无效。
TEST(UniqueFdTest, MoveTransfersSingleOwnership)
{
    UniqueFd source(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(source);
    const int descriptor = source.get();

    UniqueFd destination(std::move(source));
    EXPECT_FALSE(source.valid());
    EXPECT_EQ(destination.get(), descriptor);
    EXPECT_NE(::fcntl(descriptor, F_GETFD), -1);
}

// 测试：调用 release() 后对象应放弃所有权且不再自动关闭 fd，由调用者负责关闭。
TEST(UniqueFdTest, ReleaseStopsAutomaticClose)
{
    UniqueFd owned(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(owned);
    const int descriptor = owned.release();

    EXPECT_FALSE(owned.valid());
    EXPECT_NE(::fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(::close(descriptor), 0);
}

} // namespace
// AI-CODE-END: S4-UNIQUE-FD-TESTS
