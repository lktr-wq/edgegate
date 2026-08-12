#include "edgegate/net/event_loop.h"

// AI-CODE-BEGIN: S4-EVENT-LOOP-IMPLEMENTATION
#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include <sys/epoll.h>

namespace edgegate::net {

namespace {

constexpr int kMaximumEventsPerWait = 64;

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

} // namespace

EventLoop::EventLoop() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC))
{
    if (!epoll_fd_) {
        throw system_error_from_errno("epoll_create1");
    }
}

void EventLoop::add(std::unique_ptr<EventHandler> handler)
{
    if (!handler || handler->fd() < 0) {
        throw std::invalid_argument("EventLoop::add requires a valid handler");
    }

    const int registered_fd = handler->fd();
    if (handlers_.find(registered_fd) != handlers_.end()) {
        throw std::invalid_argument("EventLoop::add received duplicate fd");
    }

    epoll_event event{};
    event.events = handler->interests();
    event.data.fd = registered_fd;
    if (::epoll_ctl(
            epoll_fd_.get(),
            EPOLL_CTL_ADD,
            registered_fd,
            &event) == -1) {
        throw system_error_from_errno("epoll_ctl ADD");
    }

    try {
        handlers_.emplace(registered_fd, std::move(handler));
    } catch (...) {
        static_cast<void>(::epoll_ctl(
            epoll_fd_.get(), EPOLL_CTL_DEL, registered_fd, nullptr));
        throw;
    }
}

bool EventLoop::modify(int fd, std::uint32_t interests) noexcept
{
    if (handlers_.find(fd) == handlers_.end() ||
        pending_removals_.find(fd) != pending_removals_.end()) {
        return false;
    }

    epoll_event event{};
    event.events = interests;
    event.data.fd = fd;
    return ::epoll_ctl(
               epoll_fd_.get(), EPOLL_CTL_MOD, fd, &event) == 0;
}

bool EventLoop::remove(int fd) noexcept
{
    const auto found = handlers_.find(fd);
    if (found == handlers_.end()) {
        return false;
    }

    static_cast<void>(
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr));

    if (dispatching_) {
        /*
         * 当前回调可能正在执行该对象的成员函数，不能立刻 delete。
         * 保留对象到本轮事件全部分派完，也使 fd 在此期间不会被复用。
         */
        pending_removals_.insert(fd);
    } else {
        handlers_.erase(found);
    }
    return true;
}

int EventLoop::run_once(int timeout_ms)
{
    std::array<epoll_event, kMaximumEventsPerWait> events{};
    int ready = -1;

    do {
        ready = ::epoll_wait(
            epoll_fd_.get(),
            events.data(),
            static_cast<int>(events.size()),
            timeout_ms);
    } while (ready == -1 && errno == EINTR);

    if (ready == -1) {
        throw system_error_from_errno("epoll_wait");
    }

    dispatching_ = true;
    for (int index = 0; index < ready; ++index) {
        const int ready_fd = events[static_cast<std::size_t>(index)].data.fd;
        const auto found = handlers_.find(ready_fd);
        if (found == handlers_.end() ||
            pending_removals_.find(ready_fd) != pending_removals_.end()) {
            continue;
        }

        found->second->on_event(
            *this,
            events[static_cast<std::size_t>(index)].events);
    }
    dispatching_ = false;
    finish_pending_removals();
    return ready;
}

void EventLoop::run()
{
    stop_requested_ = false;
    while (!stop_requested_) {
        static_cast<void>(run_once(-1));
    }
}

void EventLoop::stop() noexcept
{
    stop_requested_ = true;
}

bool EventLoop::contains(int fd) const noexcept
{
    return handlers_.find(fd) != handlers_.end() &&
           pending_removals_.find(fd) == pending_removals_.end();
}

std::size_t EventLoop::handler_count() const noexcept
{
    return handlers_.size() - pending_removals_.size();
}

void EventLoop::finish_pending_removals() noexcept
{
    for (int fd : pending_removals_) {
        handlers_.erase(fd);
    }
    pending_removals_.clear();
}

} // namespace edgegate::net
// AI-CODE-END: S4-EVENT-LOOP-IMPLEMENTATION
