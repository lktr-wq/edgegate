#include "edgegate/net/event_loop.h"

// AI-CODE-BEGIN: S4-EVENT-LOOP-TESTS
#include "edgegate/net/unique_fd.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

using edgegate::net::EventHandler;
using edgegate::net::EventLoop;
using edgegate::net::UniqueFd;

class CountingHandler final : public EventHandler {
public:
    CountingHandler(UniqueFd descriptor, int& calls, std::uint64_t& value)
        : descriptor_(std::move(descriptor)), calls_(calls), value_(value)
    {
    }

    int fd() const noexcept override
    {
        return descriptor_.get();
    }

    std::uint32_t interests() const noexcept override
    {
        return EPOLLIN;
    }

    void on_event(EventLoop& loop, std::uint32_t events) noexcept override
    {
        if ((events & EPOLLIN) == 0U) {
            return;
        }

        std::uint64_t received = 0;
        if (::read(descriptor_.get(), &received, sizeof(received)) ==
            static_cast<ssize_t>(sizeof(received))) {
            ++calls_;
            value_ += received;
        }
        static_cast<void>(loop.remove(descriptor_.get()));
    }

private:
    UniqueFd descriptor_;
    int& calls_;
    std::uint64_t& value_;
};

class BorrowedHandler final : public EventHandler {
public:
    explicit BorrowedHandler(int descriptor) : descriptor_(descriptor) {}

    int fd() const noexcept override
    {
        return descriptor_;
    }

    std::uint32_t interests() const noexcept override
    {
        return EPOLLIN;
    }

    void on_event(EventLoop&, std::uint32_t) noexcept override {}

private:
    int descriptor_;
};

TEST(EventLoopTest, DispatchesReadyDescriptorAndSafelyRemovesIt)
{
    EventLoop loop;
    UniqueFd event_descriptor(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    ASSERT_TRUE(event_descriptor);
    const int registered_fd = event_descriptor.get();
    const int write_fd = ::dup(registered_fd);
    ASSERT_NE(write_fd, -1);
    UniqueFd writer(write_fd);

    int calls = 0;
    std::uint64_t total = 0;
    loop.add(std::make_unique<CountingHandler>(
        std::move(event_descriptor), calls, total));

    const std::uint64_t value = 7;
    ASSERT_EQ(
        ::write(writer.get(), &value, sizeof(value)),
        static_cast<ssize_t>(sizeof(value)));

    EXPECT_EQ(loop.run_once(100), 1);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(total, 7U);
    EXPECT_FALSE(loop.contains(registered_fd));
    EXPECT_EQ(loop.handler_count(), 0U);
}

TEST(EventLoopTest, ReturnsZeroOnTimeout)
{
    EventLoop loop;
    EXPECT_EQ(loop.run_once(0), 0);
}

TEST(EventLoopTest, RejectsDuplicateDescriptor)
{
    EventLoop loop;
    UniqueFd descriptor(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    ASSERT_TRUE(descriptor);
    const int registered_fd = descriptor.get();
    int calls = 0;
    std::uint64_t total = 0;
    loop.add(std::make_unique<CountingHandler>(
        std::move(descriptor), calls, total));

    // 第二个对象只借用编号；EventLoop 必须在接管它之前拒绝重复注册。
    EXPECT_THROW(
        loop.add(std::make_unique<BorrowedHandler>(registered_fd)),
        std::invalid_argument);
    EXPECT_EQ(loop.handler_count(), 1U);
}

} // namespace
// AI-CODE-END: S4-EVENT-LOOP-TESTS
