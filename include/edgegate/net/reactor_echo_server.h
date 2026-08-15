#pragma once

// AI-CODE-BEGIN: S4-REACTOR-SERVER-API
#include "edgegate/net/echo_connection.h"
#include "edgegate/net/event_loop.h"

#include <cstdint>
#include <memory>
#include <string>

namespace edgegate::net {

class ReactorEchoServer {
public:
    ReactorEchoServer(//构造服务器
        std::string bind_address,//服务器监听的本机地址
        std::uint16_t port,//监听端口
        BufferWatermarks watermarks = {});//每条客户端连接使用的背压高低水位,不传时，使用 BufferWatermarks 的默认值

    //运行控制
    int run_once(int timeout_ms);
    void run();
    void stop() noexcept;

    //查询服务器状态
    [[nodiscard]] std::uint16_t port() const noexcept;//返回服务器最终绑定的端口
    [[nodiscard]] const std::shared_ptr<ReactorStats>& stats() const noexcept;//返回共享统计数据，例如连接数量、收发字节数和错误数量
    [[nodiscard]] std::size_t handler_count() const noexcept;//EventLoop 当前管理多少个事件处理对象
    //刚启动、还没有客户端时，通常至少有一个 Handler——监听 Socket 对应的 ListenerHandler

private:
    EventLoop loop_;//服务器的事件循环
    std::shared_ptr<ReactorStats> stats_;//监听器和所有客户端连接共同修改的统计对象，所以用 shared_ptr 共享
    std::uint16_t port_{0};//服务器实际绑定的端口,阶段4的可运行验收服务器。端口传 0 时由内核选择空闲端口，便于测试
};

} // namespace edgegate::net
// AI-CODE-END: S4-REACTOR-SERVER-API
