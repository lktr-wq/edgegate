#include "edgegate/net/reactor_echo_server.h"

// AI-CODE-BEGIN: S4-REACTOR-SERVER-IMPLEMENTATION
#include "edgegate/net/unique_fd.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::net {

namespace {

constexpr int kListenBacklog = 128;//kListenBacklog = 128 表示允许一定数量已经到达、尚未被程序 accept() 取走的连接排队

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

class ListenerHandler final : public EventHandler {//把监听 Socket 包装成一个 EventHandler，这样监听 fd 也能交给 EventLoop 管理
public:
    ListenerHandler(
        UniqueFd listener,
        std::shared_ptr<ReactorStats> stats,
        BufferWatermarks watermarks)
        : listener_(std::move(listener)),//监听 Socket 的所有权
          stats_(std::move(stats)),//与服务器、客户端连接共享的统计数据
          watermarks_(watermarks)//以后创建每个 EchoConnection 时使用的缓冲区水位
    {
    }

    int fd() const noexcept override
    {
        return listener_.get();//告诉 EventLoop我负责处理的就是这个监听 fd
    }

    std::uint32_t interests() const noexcept override
    {
        return EPOLLIN;//内核的已完成连接队列中至少有一条连接，可以调用 accept4() 取出
    }

    void on_event(EventLoop& loop, std::uint32_t events) noexcept override
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {//监听 Socket 自己出错
            ++stats_->socket_errors;
            loop.stop();
            return;
        }

        for (;;) {//不断接收新连接
            const int accepted_fd = ::accept4(//一次 accept4() 只取出一个
                listener_.get(),//从哪个监听 Socket 接连接
                //两个 nullptr当前不关心客户端的 IP 和端口
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC);//SOCK_NONBLOCK：新客户端 Socket 直接成为非阻塞 Socket;SOCK_CLOEXEC：新 fd 不被其他程序意外继承

            if (accepted_fd >= 0) {//为刚被 accept4() 取出的已连接 fd 创建 EchoConnection
                UniqueFd accepted(accepted_fd);//UniqueFd接管fd所有权
                try {//EventLoop注册客户端fd
                    loop.add(std::make_unique<EchoConnection>(//std::make_unique<EchoConnection>创建EchoConnection
                        std::move(accepted), stats_, watermarks_));
                    ++stats_->accepted_connections;
                } catch (...) {
                    ++stats_->registration_errors;//如果创建或注册 EchoConnection 失败，记录一次注册错误。由于 fd 已经交给 UniqueFd，失败路径也不会泄漏 fd
                }
                continue;
            }

            if (errno == EINTR) {//被信号打断
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {//监听 Socket 是非阻塞的，而且当前连接队列已经被取空
                return;
            }
            //其他错误才执行
            ++stats_->socket_errors;
            return;
        }
    }

private:
    UniqueFd listener_;
    std::shared_ptr<ReactorStats> stats_;
    BufferWatermarks watermarks_;
};

UniqueFd create_listener(
    const std::string& bind_address,
    std::uint16_t requested_port,//调用者请求绑定的端口，可以是具体值，也可以是 0
    std::uint16_t& actual_port)//内核最终分配的真实端口，通过引用参数带回调用者
    /*创建监听 Socket工作流程
    输入：127.0.0.1、请求端口
            ↓
    创建 Socket
    设置非阻塞和地址复用
    填写 IP、端口
    bind() 绑定地址
    listen() 变成监听 Socket
    查询实际端口
                ↓
    输出：拥有监听 fd 的 UniqueFd
    */
{
    UniqueFd listener(::socket(//创建非阻塞 Socket
        AF_INET,//使用 IPv4
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        //SOCK_STREAM：使用 TCP 字节流。
        //SOCK_NONBLOCK：创建时直接设为非阻塞。
        //SOCK_CLOEXEC：以后如果进程执行其他程序，不把这个 fd 意外继承过去
        0));
    if (!listener) {//socket() 失败会返回 -1
        throw system_error_from_errno("socket listener");
    }

    const int reuse_address = 1;
    if (::setsockopt(//允许快速重新绑定地址,可以减少因为旧连接残留而导致的Address already in use
            listener.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1) {
        throw system_error_from_errno("setsockopt SO_REUSEADDR");
    }

    //准备 IP 和端口
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(requested_port);
    if (::inet_pton(
            AF_INET,
            bind_address.c_str(),
            &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 bind address");
    }

    if (::bind(//让 Socket 占用指定的本机 IP 和端口
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        throw system_error_from_errno("bind listener");
    }
    if (::listen(listener.get(), kListenBacklog) == -1) {//把普通 TCP Socket 变成监听 Socket，允许客户端连接
        throw system_error_from_errno("listen");
    }

    sockaddr_in bound_address{};
    socklen_t bound_size = sizeof(bound_address);
    if (::getsockname(//查询真实端口
            listener.get(),
            reinterpret_cast<sockaddr*>(&bound_address),
            &bound_size) == -1) {
        throw system_error_from_errno("getsockname");
    }
    actual_port = ntohs(bound_address.sin_port);//转换后带回给actual_port
    return listener;//交出监听 fd
}

} // namespace

ReactorEchoServer::ReactorEchoServer(//服务器创建
    std::string bind_address,
    std::uint16_t port,
    BufferWatermarks watermarks)
    : stats_(std::make_shared<ReactorStats>())//成员初始化阶段创建共享统计对象；loop_ 已按声明顺序先完成构造
{   //2.创建监听Socket及ListenerHandler，监听fd注册
    UniqueFd listener = create_listener(bind_address, port, port_);
    loop_.add(std::make_unique<ListenerHandler>(//ReactorEchoServer 正在构造过程中，它内部的 EventLoop 已经构造完成，因此构造函数可以使用它
        std::move(listener), stats_, watermarks));
    /*
    开始构造 ReactorEchoServer
        ↓
    先构造它的成员变量(C++成员始终按照头文件中的声明顺序构造，不按照初始化列表中的书写顺序构造)
            ↓
    loop_ 构造完成
    stats_ 构造完成
    port_ 初始化
            ↓
    进入 ReactorEchoServer 构造函数体
            ↓
    创建监听 Socket
            ↓
    使用已经存在的 loop_.add() 注册监听 fd
            ↓
    构造函数结束
            ↓
    ReactorEchoServer 对象才算完整创建
    */
}

int ReactorEchoServer::run_once(int timeout_ms)
{
    return loop_.run_once(timeout_ms);
}

void ReactorEchoServer::run()
{
    loop_.run();
}

void ReactorEchoServer::stop() noexcept
{
    loop_.stop();
}

std::uint16_t ReactorEchoServer::port() const noexcept
{
    return port_;
}

const std::shared_ptr<ReactorStats>& ReactorEchoServer::stats() const noexcept
{
    return stats_;
}

std::size_t ReactorEchoServer::handler_count() const noexcept
{
    return loop_.handler_count();
}

} // namespace edgegate::net
// AI-CODE-END: S4-REACTOR-SERVER-IMPLEMENTATION
