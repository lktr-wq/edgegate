#pragma once

// AI-CODE-BEGIN: S4-ECHO-CONNECTION-API
#include "edgegate/net/byte_buffer.h"
#include "edgegate/net/event_loop.h"
#include "edgegate/net/unique_fd.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace edgegate::net {

struct ReactorStats {//对数字的读取和修改是原子的，避免事件循环线程更新指标时，测试或其他线程同时读取造成数据竞争
    std::atomic<std::uint64_t> accepted_connections{0}; // 一共接受过多少连接
    std::atomic<std::uint64_t> active_connections{0};// 当前还活着多少连接
    std::atomic<std::uint64_t> closed_connections{0}; // 已关闭多少连接
    std::atomic<std::uint64_t> bytes_received{0};// 接收字节数
    std::atomic<std::uint64_t> bytes_sent{0};// 发送字节数
    std::atomic<std::uint64_t> read_would_block{0};// recv() 遇到 EAGAIN 的次数
    std::atomic<std::uint64_t> write_would_block{0};// send() 遇到 EAGAIN 的次数
    std::atomic<std::uint64_t> read_pauses{0}; // 因背压暂停读取的次数
    std::atomic<std::uint64_t> socket_errors{0}; // Socket 错误数
    std::atomic<std::uint64_t> registration_errors{0};// 注册到 EventLoop 失败次数
};//这里的原子只保护每个计数器自身，不代表整个 EchoConnection 可以被多个线程随意同时调用

struct BufferWatermarks {//控制应用层输出缓冲区
    std::size_t maximum{256 * 1024};//256 KiB
    std::size_t high{192 * 1024};//192 KiB
    std::size_t low{64 * 1024};//64 KiB
    /*
    output_ 达到 high
    → 暂停继续读取客户端

    output_ 逐渐发送，降到 low
    → 恢复读取客户端

    output_ 绝不能超过 maximum
    → 超过说明连接无法安全继续
    */
};

/*
 * 阶段4的练习连接：收到什么字节就原样回送什么字节。
 *
 * Echo 不是最终反向代理协议，只用于单独验证非阻塞读写、EAGAIN、
 * 部分写、背压和连接回收；阶段5会复用同一事件循环组织代理状态机。
 */
class EchoConnection final : public EventHandler {//EchoConnection 是一种事件处理器继承自 EventHandler；因此 EventLoop 可以统一管理它；final不允许再从 EchoConnection 派生其他类；必须实现 fd()、interests() 和 on_event()
public:
    EchoConnection(
        UniqueFd socket,//当前客户端连接的 fd 所有权
        std::shared_ptr<ReactorStats> stats,//全部连接共享的统计对象
        BufferWatermarks watermarks = {});//当前连接的缓冲区水位
    ~EchoConnection() override;//override编译期检查是否正确重写父类虚函数

    [[nodiscard]] int fd() const noexcept override;//返回 socket_.get()，让 EventLoop 知道对象管理哪个 fd
    [[nodiscard]] std::uint32_t interests() const noexcept override;
    /*
    根据当前状态动态生成事件集合：
    连接还能读且没有因背压暂停
    → 关注 EPOLLIN

    output_ 中有未发送数据
    → 关注 EPOLLOUT

    始终关注客户端关闭发送方向
    → 关注 EPOLLRDHUP
    */
    void on_event(EventLoop& loop, std::uint32_t events) noexcept override;
    /*
    根据本次事件决定：
    是否读取。
    是否写出。
    是否记录客户端关闭。
    是否移除连接。
    是否调用 EventLoop::modify() 更新关注事件
    等总体调度
    */
private:
    void read_from_peer() noexcept;
    /*
    循环 recv()，直到发生以下情况：
    当前读空，返回 EAGAIN。
    客户端关闭，返回 0。
    输出缓冲区达到高水位。
    发生错误
    */
    void write_to_peer() noexcept;
    /*
    循环 send()，每次成功后从 output_ 消费对应字节，直到发生以下情况：
    全部发送完成。
    内核发送缓冲区满，返回 EAGAIN。
    发生错误
    */
    void refresh_backpressure_state() noexcept;// 当读取已暂停且 output_ 降到低水位时，恢复读取

    UniqueFd socket_;//独占客户端 Socket。连接对象销毁时自动关闭
    std::shared_ptr<ReactorStats> stats_;//共享的运行指标
    BufferWatermarks watermarks_;//保存当前连接的最大、高、低水位
    ByteBuffer input_;//已经 recv() 但尚未完成应用处理的字节
    ByteBuffer output_;//准备回给客户端但尚未被 send() 接受的字节
    bool peer_closed_{false};//为true则客户端已经关闭发送方向，不会再有新数据，但现有 output_ 仍可能需要发完
    bool fatal_error_{false};//为true则发生不可恢复错误，连接需要删除
    bool read_paused_{false};//为true则积压达到高水位暂时不再关注 EPOLLIN
};

} // namespace edgegate::net
/*EchoConnection总状态转换
正常状态
EPOLLIN → recv → input_ → output_

output_ 有数据
→ 关注 EPOLLOUT
→ send → consume output_

output_ 达到 high
→ read_paused_ = true
→ 暂停关注 EPOLLIN

output_ 降到 low
→ read_paused_ = false
→ 恢复关注 EPOLLIN

客户端关闭且 output_ 已清空
或发生 fatal_error_
→ EventLoop::remove(fd)
→ EchoConnection 析构
→ UniqueFd 关闭 Socket
*/
// AI-CODE-END: S4-ECHO-CONNECTION-API
