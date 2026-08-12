#pragma once

// AI-CODE-BEGIN: S4-UNIQUE-FD
#include <unistd.h>

namespace edgegate::net {

/*
 * 独占拥有一个 Linux 文件描述符。
 *
 * 对象销毁时自动 close()，因此函数中途 return 或抛出异常也不会遗忘
 * 关闭 fd。它不可复制，但可以移动，保证同一 fd 始终只有一个所有者。
 */
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd()
    {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return fd_ >= 0;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    int release() noexcept
    {
        const int released = fd_;
        fd_ = -1;
        return released;
    }

    void reset(int replacement = -1) noexcept
    {
        if (fd_ >= 0) {
            /*
             * Linux 下 close() 被 EINTR 打断时也不能盲目重试：fd 号码可能
             * 已被别处复用，重试反而可能关闭一个无关的新 fd。
             */
            static_cast<void>(::close(fd_));
        }
        fd_ = replacement;
    }

private:
    int fd_{-1};
};

} // namespace edgegate::net
// AI-CODE-END: S4-UNIQUE-FD
