#pragma once

// AI-CODE-BEGIN: S4-EVENT-LOOP-API
#include "edgegate/net/unique_fd.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

/*
在阻塞socket下的流程：
1. 服务端：创建 listen_fd  → bind → listen。
 listen_fd 代表内核中的监听 Socket；TCP 三次握手和连接队列由 Linux 内核维护，EdgeGate 用户代码不直接参与握手。
2.客户端：socket() 得到客户端自己的 socket fd，然后调用 connect() 连接 EdgeGate。
3. 三次握手整个过程由操作系统内核完成，用户态程序不参与握手；握手完成之后，这条已经建立好的连接，放入服务端内核的全连接队列。
重点：此时还没有调用accept！连接已经在内核存在了，客户端connect就已经返回成功。哪怕服务端代码暂时没跑accept，连接也躺在内核队列排队。
4. 服务端用户代码调用  accept(listen_fd) 系统调用从全连接队列头部取出一条已经完成握手的连接，生成全新的 client_fd 交给应用层。如果队列为空，阻塞版accept就卡住休眠，等待新连接进队列。
5. 之后：客户端fd ↔ 服务端client_fd，双向recv/send； listen_fd 继续留在那里，持续接待下一批新客户端，不参与这条连接的数据传输。

客户端：可能运行在另一台电脑，也可能是本机的 curl。
Socket、TCP 缓冲区和 epoll 的底层实现：位于运行 EdgeGate 的 Linux 内核。
EventLoop、EventHandler 和 ByteBuffer：位于 EdgeGate 进程的用户空间。
epoll 不是单独部署的服务器，也不运行在客户端上，它是 EdgeGate 所在 Linux 系统提供的内核能力

客户端和 fd 的关系
客户端执行：connect(edgegate_ip, 18080);EdgeGate 的监听 Socket 首先有一个 fd,如listen_fd = 3
连接到达后，EdgeGate 调用client_fd = accept(listen_fd, ...);可能得到client_fd = 7
EdgeGate 进程内的关系是：
fd 3 → 监听端口 18080
fd 7 → 客户端 A 的 TCP 连接

什么叫listen_fd 的“可读”:有新connect进入listen_fd 的等待队列,那listen_fd就值得epoll报告有新的待处理事务
则epoll_wait() 返回 listen_fd + EPOLLIN->EventLoop 调度监听处理器调用 accept()->得到新的 client_fd

什么叫client_fd的“可读”：客户端在发送新的数据到达 EdgeGate 内核接收缓冲区后，EdgeGate 进程中代表这条连接的 client_fd 变为可读的，该事件值得被epoll报告；client_fd 可读不仅可能表示有数据，也可能表示对端关闭或连接发生错误；最终要看 recv() 的返回值。
则epoll_wait() 返回 fd + EPOLLIN->调用这个对象的 on_event()->on_event() 调用 recv()

什么叫client_fd“可写”：EdgeGate 本地内核发送缓冲区有空间，send() 有机会接收更多字节。只有应用层 ByteBuffer 存在待发送数据时，EdgeGate 才有必要订阅 EPOLLOUT。

epoll 负责报告
EventLoop 负责分派
EventHandler 子类负责处理

EventHandler 表达自己关注哪些事件
→ EventLoop 主动调用 epoll_ctl()，把 fd 和事件注册给内核
→ 当前线程调用 epoll_wait()，进入等待
→ 内核返回已经就绪的 fd 和实际发生的事件
→ EventLoop 根据 fd 查找 EventHandler
→ 当前线程调用 handler->on_event()
→ 子类执行 recv()/send()/accept()
*/
namespace edgegate::net {
/*线程接收内核返回信息调度自身内部对象
当前线程进入 epoll_wait()
→ 内核返回就绪 fd
→ 当前线程调用对象的 on_event()
*/
class EventLoop;

/*
 * Reactor 中“一个 fd 对应的处理对象”。
 * EventLoop 只负责发现和分派事件，具体 recv()/send()/accept() 由实现类完成。
 */
class EventHandler {
public:
    virtual ~EventHandler() = default;//虚析构函数,virtual允许通过父类指针正确调用子类析构函数，= default析构函数的普通工作由编译器生成
    //纯虚函数,父类只规定接口，不提供具体实现
    [[nodiscard]] virtual int fd() const noexcept = 0;//你管理哪个 fd
    [[nodiscard]] virtual std::uint32_t interests() const noexcept = 0;//你希望 epoll 关注什么事件
    /*
    interests() 返回的是事件位组合，例如：
    EPOLLIN                 // 关注可读
    EPOLLOUT                // 关注可写
    EPOLLIN | EPOLLOUT      // 同时关注可读和可写
    */
    virtual void on_event(EventLoop& loop, std::uint32_t events) noexcept = 0;//事件发生后你怎么处理
    //输入：当前epoll实例和内核实际返回的事件
};

class EventLoop {
public:
    EventLoop();//创建一个 epoll 实例

    //禁止复制
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    //添加处理器
    void add(std::unique_ptr<EventHandler> handler);//std::unique_ptr 表示独占所有权
    //修改和删除
    [[nodiscard]] bool modify(//modify() 用于改变某个 fd 关注的事件
        int fd,
        std::uint32_t interests) noexcept;
    [[nodiscard]] bool remove(int fd) noexcept;//从 epoll 中取消 fd→ 删除 fd 对应的处理对象

    // 等待并分派一批事件。timeout_ms 与 epoll_wait() 语义一致。
    /*
    timeout_ms 的典型值：
    -1：一直等待，直到有事件
    0：不等待，立即检查
    >0：最多等待指定毫秒数
    */
    int run_once(int timeout_ms);//run_once(100);即最多等待 100 毫秒，只处理一批事件
    void run();//循环调用 run_once(-1)，直到 stop_requested_ 变成 true
    void stop() noexcept;//和run()配套使用

    //查询
    [[nodiscard]] bool contains(int fd) const noexcept;//判断当前 fd 是否仍有有效处理器
    [[nodiscard]] std::size_t handler_count() const noexcept;//返回当前有效处理器数量

private:
    void finish_pending_removals() noexcept;// 本轮事件分派结束后，真正删除 pending_removals_ 中的处理对象

    UniqueFd epoll_fd_;//保存 epoll_create1() 创建的 epoll fd，对象销毁时自动关闭
    std::unordered_map<int, std::unique_ptr<EventHandler>> handlers_;//std::unordered_map是哈希表，建立fd和EventHandler的映射关系，EventHandler是独占所有权的智能指针，handlers_是EventLoop的成员容器，用于接管EventHandler
    std::unordered_set<int> pending_removals_;
    bool dispatching_{false};//表示当前是否正在调用一批对象的 on_event()
    bool stop_requested_{false};//是否收到停止事件循环的请求
};

} // namespace edgegate::net
// AI-CODE-END: S4-EVENT-LOOP-API
