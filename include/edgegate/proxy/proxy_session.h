#pragma once

// AI-CODE-BEGIN: S5-PROXY-SESSION-API
#include "edgegate/http/request_parser.h"
#include "edgegate/http/response_parser.h"
#include "edgegate/net/byte_buffer.h"
#include "edgegate/net/event_loop.h"
#include "edgegate/net/unique_fd.h"
#include "edgegate/routing/route_table.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
// 一个代理会话长期对应一条客户端连接，并在处理每次请求时临时建立一条上游连接，同时保存协调两端的共同状态
namespace edgegate::proxy {

enum class EndpointRole {//当前事件来自哪一端
    kClient,//当前事件来自客户端fd
    kUpstream//当前事件来自上游fd
    /*
    由他提供ProxyEndpoint中role_的取值，根据ProxyEndpoint.role_就可以判断收到epoll报告后具体该干什么
    客户端可读 → 读取HTTP请求
    客户端可写 → 返回HTTP响应
    上游可写   → 连接上游或发送请求
    上游可读   → 读取HTTP响应
    */
};

enum class ProxyState {//站在代理的视角看任务推进到哪一步了
    kReadingRequest,//读取并解析客户端请求
    kConnectingUpstream,//非阻塞连接上游
    kSendingRequest,//向上游发送完整请求
    kReadingResponse,//读取并解析上游响应
    kSendingResponse,//把响应发送给客户端
    kClosed//关闭
    /*一条正常请求的状态变化是：
    kReadingRequest
    读取并解析客户端请求
            ↓
    kConnectingUpstream
    非阻塞连接上游
            ↓
    kSendingRequest
    向上游发送完整请求
            ↓
    kReadingResponse
    读取并解析上游响应
            ↓
    kSendingResponse
    把响应发送给客户端
            ↓
    若保持连接：回到 kReadingRequest
    若需要关闭：进入 kClosed

    更具体而言：一次请求怎样穿过ProxySession
    客户端 fd 出现 EPOLLIN
    → on_event(kClient)
    → read_client()
    → RequestParser 得到完整请求
    → 请求放进 to_upstream_
    → connect_upstream()
            ↓
    非阻塞连接上游
    → 暂时返回 EINPROGRESS
    → 上游 fd 订阅 EPOLLOUT
    → epoll 报告连接过程有结果
    → finish_upstream_connect()
    → getsockopt(SO_ERROR) 判断成功或失败
            ↓
    连接成功
    → write_upstream()
    → 请求全部发完
    → 创建 ResponseParser
    → 上游 fd 改为订阅 EPOLLIN
            ↓
    上游响应到达
    → read_upstream()
    → ResponseParser 判断完整响应
    → finish_response()
    → 完整响应放进 to_client_
    → 关闭本次上游连接
    → write_client()
            ↓
    响应全部发完
    → 需要关闭：close_session()
    → 可以 Keep-Alive：重置解析器，回到 kReadingRequest
        */
};

struct ProxyConfig {//保存代理状态机真正会执行的配置
    std::string upstream_address{"127.0.0.1"};// 上游IP
    std::uint16_t upstream_port{19080};// 上游端口
    std::size_t max_header_size{8192};// Header容量上限
    std::size_t max_request_body_size{1024 * 1024};// 请求体上限
    std::size_t max_response_body_size{8 * 1024 * 1024};// 响应体上限
    // AI-CODE-BEGIN: S6-PROXY-ROUTING-API
    // 为空时保留阶段5“固定上游”模式；正式程序使用共享路由表。
    std::shared_ptr<edgegate::routing::RouteTable> route_table;
    // AI-CODE-END: S6-PROXY-ROUTING-API
};

struct ProxyStats {
    std::atomic<std::uint64_t> accepted{0};//接受过多少客户端连接
    std::atomic<std::uint64_t> completed_requests{0};//完成过多少请求
    std::atomic<std::uint64_t> client_errors{0};//客户端请求或连接错误
    std::atomic<std::uint64_t> upstream_errors{0};//连接上游、发送请求或响应解析错误
    std::atomic<std::uint64_t> active_sessions{0};//当前还存活的代理会话数
   // 使用 atomic保证安全读取单个计数值
};

/*
两个 ProxyEndpoint：分别注册客户端fd和上游fd
一个 ProxySession：保存两个fd共同使用的代理状态
EventLoop::handlers_
│
├── unique_ptr<ProxyEndpoint：客户端端点>
│   ├── UniqueFd：拥有客户端 fd
│   └── shared_ptr ────────────────────────────┐
│                                              ↓
│                                          ProxySession
│                                              ↑
└── unique_ptr<ProxyEndpoint：上游端点>        |
    ├── UniqueFd：拥有上游 fd                  |
    └── shared_ptr ────────────────────────────┘
*/
class ProxySession;

class ProxyEndpoint final : public edgegate::net::EventHandler {
public:
    ProxyEndpoint(
        edgegate::net::UniqueFd socket,// 真正拥有这一端的fd
        EndpointRole role,// 客户端端点还是上游端点
        std::shared_ptr<ProxySession> session);// 所属代理会话

    [[nodiscard]] int fd() const noexcept override;//返回当前端点的 Socket fd
    [[nodiscard]] std::uint32_t interests() const noexcept override;//询问共享 Session：当前这一端应该关注什么事件
    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override;//把角色和实际事件交给共享 Session

private:
    edgegate::net::UniqueFd socket_;
    EndpointRole role_;
    std::shared_ptr<ProxySession> session_;// 两个 Endpoint 保存指向同一 Session 的 shared_ptr，共享其生命周期；最后一份 shared_ptr 释放后 Session 才析构
};

//继承自enable_shared_from_this允许已经由 shared_ptr 管理的 Session 通过 shared_from_this()
//安全获得另一份共享同一引用计数的 shared_ptr，供上游 Endpoint 保存
//它只能在对象已经被 shared_ptr 管理以后使用，要先std::make_shared<ProxySession>()创建，再在处理请求时调用 shared_from_this()
class ProxySession final : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(
        int client_fd,
        std::string client_address,
        ProxyConfig config,
        std::shared_ptr<ProxyStats> stats);
    ~ProxySession();

    [[nodiscard]] std::uint32_t interests(EndpointRole role) const noexcept;
    void on_event(
        edgegate::net::EventLoop& loop,
        EndpointRole role,
        std::uint32_t events) noexcept;

    [[nodiscard]] ProxyState state() const noexcept;

private:
    void read_client(edgegate::net::EventLoop& loop) noexcept;
    void connect_upstream(edgegate::net::EventLoop& loop) noexcept;
    void finish_upstream_connect(edgegate::net::EventLoop& loop) noexcept;
    void write_upstream(edgegate::net::EventLoop& loop) noexcept;
    void read_upstream(edgegate::net::EventLoop& loop) noexcept;
    void write_client(edgegate::net::EventLoop& loop) noexcept;

    void finish_response(edgegate::net::EventLoop& loop) noexcept;
    void queue_error_response(
        edgegate::net::EventLoop& loop,
        int status,
        const char* reason) noexcept;
    void close_upstream(edgegate::net::EventLoop& loop) noexcept;
    void close_session(edgegate::net::EventLoop& loop) noexcept;
    void refresh_interests(edgegate::net::EventLoop& loop) noexcept;

    [[nodiscard]] bool request_wants_close() const noexcept;
    [[nodiscard]] bool response_requires_close() const noexcept;

    //只是fd号码，实际所有权在各自 ProxyEndpoint::socket_ 中
    int client_fd_;//客户端fd从会话建立时就存在
    std::optional<int> upstream_fd_;//上游fd只有解析完请求、开始连接上游后才存在，所以用 optional

    // AI-CODE-BEGIN: S6-PROXY-ROUTING-STATE
    std::string client_address_;//用于生成可信的 X-Forwarded-For
    std::string selected_upstream_address_;//本轮路由选中的上游 IPv4
    std::uint16_t selected_upstream_port_{0};//本轮路由选中的上游端口
    // AI-CODE-END: S6-PROXY-ROUTING-STATE

    ProxyConfig config_;
    std::shared_ptr<ProxyStats> stats_;
    ProxyState state_{ProxyState::kReadingRequest};

    edgegate::http::RequestParser request_parser_;//请求解析器从会话建立时就需要
    std::optional<edgegate::http::ResponseParser> response_parser_;//响应解析器暂时不存在，因为构造它时需要知道原请求方法。解析完请求以后才能正确创建响应解析器
    edgegate::net::ByteBuffer to_upstream_;
    edgegate::net::ByteBuffer to_client_;
    bool close_after_response_{false};//本次响应发完后是否关闭客户端连接
    bool client_read_closed_{false};//客户端是否已经关闭自己的发送方向。它可能仍然保留接收方向，继续等待代理返回响应
};

} // namespace edgegate::proxy
// AI-CODE-END: S5-PROXY-SESSION-API
