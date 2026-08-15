#include "edgegate/net/echo_connection.h"

// AI-CODE-BEGIN: S4-ECHO-CONNECTION-IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::net {

namespace {

constexpr std::size_t kReadChunkSize = 16 * 1024;//单次读取大小,constexpr代表编译器就确定大小
//每次最多从内核读取 16 KiB
} // namespace

EchoConnection::EchoConnection(
    UniqueFd socket,
    std::shared_ptr<ReactorStats> stats,
    BufferWatermarks watermarks)
    : socket_(std::move(socket)),//接管客户端 Socket 所有权
      stats_(std::move(stats)),//共享统计对象
      watermarks_(watermarks),//保存当前水位配置
      input_(kReadChunkSize),//最大保存 16 KiB
      output_(watermarks.maximum)// 最大保存 maximum 字节
{
    if (!socket_ || !stats_) {//拒绝无效 Socket和空统计对象
        throw std::invalid_argument(
            "EchoConnection requires a socket and shared stats");
    }
    if (watermarks_.low >= watermarks_.high ||//合法关系必须是low < high <= maximum
        watermarks_.high > watermarks_.maximum) {
        throw std::invalid_argument("invalid buffer watermarks");
    }

    ++stats_->active_connections;//只有构造函数全部检查通过后，才把活动连接加一
}//输出：一个可以注册到 EventLoop 的连接对象

EchoConnection::~EchoConnection()
{
    --stats_->active_connections;
    ++stats_->closed_connections;
}//成员随后析构→ socket_ 关闭客户端 fd

int EchoConnection::fd() const noexcept
{
    return socket_.get();//把客户端 fd 交给 EventLoop
}

std::uint32_t EchoConnection::interests() const noexcept
{   //EPOLLRDHUP是基础事件
    std::uint32_t events = EPOLLRDHUP;//初始值始终关注客户端关闭发送方向，即当对方发送fin包时，epoll_wait返回时带上此标记通知我
    if (!peer_closed_ && !read_paused_) {//还能继续读取就要关注EPOLLIN
        events |= EPOLLIN;//按位或并赋值
    }
    if (!output_.empty()) {//如果有待发送数据就要关注EPOLLOUT
        events |= EPOLLOUT;
    }
    return events;
    /*
    正常且无待发送数据
    → EPOLLRDHUP | EPOLLIN

    正常且有待发送数据
    → EPOLLRDHUP | EPOLLIN | EPOLLOUT

    背压暂停且有待发送数据
    → EPOLLRDHUP | EPOLLOUT

    客户端已关闭但仍有待发送数据
    → EPOLLRDHUP | EPOLLOUT
    */
}

void EchoConnection::on_event(
    EventLoop& loop,
    std::uint32_t events) noexcept
{
    if ((events & EPOLLERR) != 0U) {// events & EPOLLERR 非零，说明本次事件集合包含 Socket 错误，进入该 if 分支
        ++stats_->socket_errors;
        fatal_error_ = true;
    }

    // 先读完已经到达的数据，再处理 RDHUP，避免丢掉 FIN 前的最后一批字节。这是因为客户端发完最后一批字节后可能会紧接着发送FIN；导致同一次epoll同时包含EPOLLIN | EPOLLRDHUP，不能因为有EPOLLRDHUP就放弃读取
    if (!fatal_error_ && (events & EPOLLIN) != 0U) {//只有没有致命错误并且本次包含 EPOLLIN 才读取
        read_from_peer();
    }

    if ((events & (EPOLLRDHUP | EPOLLHUP)) != 0U) {// 先把 EPOLLRDHUP 和 EPOLLHUP 合成掩码；按位与结果非零，说明至少发生了一种关闭/挂断事件
        peer_closed_ = true;
    }

    // 新读到的数据可以立刻尝试发送，不必额外等待下一轮 EPOLLOUT。
    if (!fatal_error_ && !output_.empty()) {
        write_to_peer();
    }
    /*
    如果内核发送缓冲区有空间
    → 立即发送，减少一次事件循环等待

    如果没有空间
    → send() 返回 EAGAIN
    → output_ 保留数据
    → interests() 加入 EPOLLOUT
    */
    refresh_backpressure_state();//刚才 write_to_peer() 可能已经让输出缓冲区从高水位降到低水位，因此可以恢复读取

    if (fatal_error_ || (peer_closed_ && output_.empty())) {//发生不可恢复错误或客户端不会再发送新数据并且应回送的数据已经全部发送完成时应该关闭连接
        static_cast<void>(loop.remove(fd()));
        return;
    }

    if (!loop.modify(fd(), interests())) {//连接继续存活时根据最新状态重新决定是否关注 EPOLLIN/EPOLLOUT，这里modify成功不是一定要改变，而是根据当前interst能成功更新，modify失败代表epoll_ctl调用失败，这代表了发生严重错误
        ++stats_->registration_errors;//修改失败时记录错误删除链接
        static_cast<void>(loop.remove(fd()));
    }
}

void EchoConnection::read_from_peer() noexcept
{
    std::array<char, kReadChunkSize> chunk{};//一次 recv() 使用的 16 KiB临时数组

    for (;;) {
        const std::size_t capacity = std::min(// 取单次临时数组容量和 output_ 可写容量的较小值，决定本次最多读取多少字节
            chunk.size(), output_.writable_capacity());
        if (capacity == 0) {//输出缓冲区完全没空间不再调用 recv()，让字节暂时留在内核接收缓冲区，由 TCP 背压逐渐限制客户端发送
            read_paused_ = true;
            ++stats_->read_pauses;
            return;
        }

        const ssize_t received = ::recv(
            socket_.get(), chunk.data(), capacity, 0);//创建socket的时候加入了SOCK_NONBLOCK，因此不会无限等待
        if (received > 0) {//received > 0表示真实收到字节数
            const std::size_t count = static_cast<std::size_t>(received);
            stats_->bytes_received.fetch_add(count);//更新统计

            const std::string_view bytes(chunk.data(), count);//只观察本次真实收到的字节
            if (!input_.append(bytes) ||//放入input_
                !output_.append(input_.readable_view()) ||//原样复制到 output_
                !input_.consume(input_.readable_size())) {//input_ 标记全部处理完成
                fatal_error_ = true;//三个操作只要有一个失败
                ++stats_->socket_errors;
                return;
            }

            if (output_.readable_size() >= watermarks_.high) {//达到高水位
                read_paused_ = true;
                ++stats_->read_pauses;
                return;
            }
            continue;
        }

        if (received == 0) {//客户端关闭发送方向，后续不会再有字节
            peer_closed_ = true;
            return;
        }

        if (errno == EINTR) {//系统调用被信号打断，重新 recv()
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {//当前内核接收缓冲区已经读空，本轮读取工作完成
            ++stats_->read_would_block;
            return;
        }

        fatal_error_ = true;//执行到这里即发生其他错误
        ++stats_->socket_errors;
        return;
    }
}

void EchoConnection::write_to_peer() noexcept
{
    while (!output_.empty()) {//只要输出缓冲区还有数据
        const std::string_view pending = output_.readable_view();//取得未发送部分
        const ssize_t sent = ::send(
            socket_.get(),
            pending.data(),
            pending.size(),
            MSG_NOSIGNAL);//禁止发送SIGPIPE信号，让 send() 正常返回

        if (sent > 0) {
            const std::size_t count = static_cast<std::size_t>(sent);//内核只接收了多少，就消费多少
            static_cast<void>(output_.consume(count));
            stats_->bytes_sent.fetch_add(count);//更新统计
            continue;
        }

        if (sent == -1 && errno == EINTR) {//被信号打断，重新 send()
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {//本地内核发送缓冲区暂时没有空间;没有发送完的字节继续留在 output_。随后 interests() 会加入 EPOLLOUT，等待可写事件
            ++stats_->write_would_block;
            return;
        }

        fatal_error_ = true;//包括不可恢复错误以及异常的 send() == 0，都标记为 fatal_error_
        ++stats_->socket_errors;
        return;
    }
}

void EchoConnection::refresh_backpressure_state() noexcept
{
    if (read_paused_ && output_.readable_size() <= watermarks_.low) {//同时满足之前确实因为背压暂停并且积压已经降到低水位恢复读取
        read_paused_ = false;
    }
}

} // namespace edgegate::net
// AI-CODE-END: S4-ECHO-CONNECTION-IMPLEMENTATION
