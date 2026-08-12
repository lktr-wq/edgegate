#pragma once

// AI-CODE-BEGIN: S4-EVENT-LOOP-API
#include "edgegate/net/unique_fd.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace edgegate::net {

class EventLoop;

/*
 * Reactor 中“一个 fd 对应的处理对象”。
 * EventLoop 只负责发现和分派事件，具体 recv()/send()/accept() 由实现类完成。
 */
class EventHandler {
public:
    virtual ~EventHandler() = default;
    [[nodiscard]] virtual int fd() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t interests() const noexcept = 0;
    virtual void on_event(EventLoop& loop, std::uint32_t events) noexcept = 0;
};

class EventLoop {
public:
    EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add(std::unique_ptr<EventHandler> handler);
    [[nodiscard]] bool modify(
        int fd,
        std::uint32_t interests) noexcept;
    [[nodiscard]] bool remove(int fd) noexcept;

    // 等待并分派一批事件。timeout_ms 与 epoll_wait() 语义一致。
    int run_once(int timeout_ms);
    void run();
    void stop() noexcept;

    [[nodiscard]] bool contains(int fd) const noexcept;
    [[nodiscard]] std::size_t handler_count() const noexcept;

private:
    void finish_pending_removals() noexcept;

    UniqueFd epoll_fd_;
    std::unordered_map<int, std::unique_ptr<EventHandler>> handlers_;
    std::unordered_set<int> pending_removals_;
    bool dispatching_{false};
    bool stop_requested_{false};
};

} // namespace edgegate::net
// AI-CODE-END: S4-EVENT-LOOP-API
