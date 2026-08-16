#include "edgegate/runtime/signal_control.h"

// AI-CODE-BEGIN: S8-SIGNAL-CONTROL-IMPLEMENTATION
#include "edgegate/net/unique_fd.h"

#include <cerrno>
#include <csignal>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace edgegate::runtime {

namespace {

sigset_t termination_signal_set()
{
    sigset_t signals{};
    if (::sigemptyset(&signals) == -1 ||
        ::sigaddset(&signals, SIGTERM) == -1 ||
        ::sigaddset(&signals, SIGINT) == -1) {
        throw std::system_error(errno, std::generic_category(), "build signal set");
    }
    return signals;
}

class SignalControl final : public edgegate::net::EventHandler {
public:
    SignalControl(
        edgegate::net::UniqueFd descriptor,
        TerminationSignalHandler handler)
        : descriptor_(std::move(descriptor)), handler_(std::move(handler))
    {
    }

    int fd() const noexcept override { return descriptor_.get(); }
    std::uint32_t interests() const noexcept override { return EPOLLIN; }

    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override
    {
        if ((events & EPOLLIN) == 0U) {
            return;
        }

        // 一次读空 signalfd，多个连续 stop 信号也只会让关闭流程保持幂等。
        for (;;) {
            signalfd_siginfo information{};
            const ssize_t received = ::read(
                descriptor_.get(), &information, sizeof(information));
            if (received == static_cast<ssize_t>(sizeof(information))) {
                handler_(loop, static_cast<int>(information.ssi_signo));
                continue;
            }
            if (received == -1 && errno == EINTR) continue;
            if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            return;
        }
    }

private:
    edgegate::net::UniqueFd descriptor_;
    TerminationSignalHandler handler_;
};

} // namespace

void block_termination_signals()
{
    const sigset_t signals = termination_signal_set();
    const int result = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (result != 0) {
        throw std::system_error(
            result, std::generic_category(), "pthread_sigmask");
    }
}

std::unique_ptr<edgegate::net::EventHandler>
make_termination_signal_handler(TerminationSignalHandler handler)
{
    if (!handler) {
        throw std::invalid_argument("termination signal handler is required");
    }
    const sigset_t signals = termination_signal_set();
    edgegate::net::UniqueFd descriptor(::signalfd(
        -1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!descriptor) {
        throw std::system_error(
            errno, std::generic_category(), "signalfd");
    }
    return std::make_unique<SignalControl>(
        std::move(descriptor), std::move(handler));
}

} // namespace edgegate::runtime
// AI-CODE-END: S8-SIGNAL-CONTROL-IMPLEMENTATION
