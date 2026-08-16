#pragma once

// AI-CODE-BEGIN: S8-SIGNAL-CONTROL-API
#include "edgegate/net/event_loop.h"

#include <functional>
#include <memory>

namespace edgegate::runtime {

using TerminationSignalHandler = std::function<void(
    edgegate::net::EventLoop&,
    int)>;

/*
 * 在启动工作线程和 EventLoop 之前屏蔽 SIGTERM/SIGINT，使它们不再异步
 * 打断任意一行 C++ 代码，而是统一进入 signalfd，作为普通 epoll 事件处理。
 */
void block_termination_signals();

[[nodiscard]] std::unique_ptr<edgegate::net::EventHandler>
make_termination_signal_handler(TerminationSignalHandler handler);

} // namespace edgegate::runtime
// AI-CODE-END: S8-SIGNAL-CONTROL-API
