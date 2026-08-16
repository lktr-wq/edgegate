#include "edgegate/proxy/proxy_session.h"
#include "edgegate/http/message_rewriter.h"

// AI-CODE-BEGIN: S5-PROXY-SESSION-IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace edgegate::proxy {

namespace {

constexpr std::size_t kIoChunkSize = 16 * 1024;
constexpr std::size_t kHeaderRewriteReserve = 1024;

std::size_t checked_capacity(
    std::size_t header_size,
    std::size_t body_size)
{
    if (body_size > std::numeric_limits<std::size_t>::max() - header_size) {
        throw std::invalid_argument("proxy buffer capacity overflow");
    }
    return header_size + body_size;
}

char ascii_lower(char character) noexcept
{
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character + ('a' - 'A'));
    }
    return character;
}

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

bool header_contains_token(
    std::optional<std::string_view> value,
    std::string_view expected) noexcept
{
    if (!value.has_value()) {
        return false;
    }

    std::string_view remaining = *value;
    while (!remaining.empty()) {
        const std::size_t comma = remaining.find(',');
        std::string_view token = remaining.substr(0, comma);
        while (!token.empty() &&
               (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() &&
               (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (equals_ignore_case(token, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

} // namespace

ProxyEndpoint::ProxyEndpoint(
    edgegate::net::UniqueFd socket,
    EndpointRole role,
    std::shared_ptr<ProxySession> session)
    : socket_(std::move(socket)),
      role_(role),
      session_(std::move(session))
{
    if (!socket_ || !session_) {
        throw std::invalid_argument("ProxyEndpoint requires socket and session");
    }
}

int ProxyEndpoint::fd() const noexcept
{
    return socket_.get();
}

std::uint32_t ProxyEndpoint::interests() const noexcept
{
    return session_->interests(role_);
}

void ProxyEndpoint::on_event(
    edgegate::net::EventLoop& loop,
    std::uint32_t events) noexcept
{
    session_->on_event(loop, role_, events);
}

ProxySession::ProxySession(
    int client_fd,
    std::string client_address,
    ProxyConfig config,
    std::shared_ptr<ProxyStats> stats)
    //初始化列表
    : client_fd_(client_fd),//client_fd_只是记录fd数字，不负责关闭;真正负责关闭客户端fd的是ProxyEndpoint::socket_
      client_address_(std::move(client_address)),
      config_(std::move(config)),
      stats_(std::move(stats)),
      request_parser_(//创建请求解析器
          config_.max_header_size,
          config_.max_request_body_size),
      to_upstream_(checked_capacity(//额外空间用于重新生成的 X-Forwarded-* Header
          checked_capacity(config_.max_header_size, config_.max_request_body_size),
          kHeaderRewriteReserve)),
      to_client_(checked_capacity(//额外空间用于重新生成的 Content-Length/Connection Header
          checked_capacity(config_.max_header_size, config_.max_response_body_size),
          kHeaderRewriteReserve))
{
    if (client_fd_ < 0 || client_address_.empty() || !stats_) {
        throw std::invalid_argument("ProxySession requires client fd and stats");
    }
    ++stats_->active_sessions;//构造成功
}

ProxySession::~ProxySession()
{
    --stats_->active_sessions;// 最后一份持有 Session 的 shared_ptr 被释放后，Session 析构并减少活动会话数
}

std::uint32_t ProxySession::interests(EndpointRole role) const noexcept
{
    if (state_ == ProxyState::kClosed) {
        return 0;
    }
    /*对于客户端所关注的事件：
    | 当前条件 | 关注事件 | 原因 |
    | 客户端尚未关闭发送方向 | `EPOLLRDHUP` | 检测客户端半关闭,当客户端发送FIN时会报告 |
    | `kReadingRequest` | `EPOLLIN` | 等客户端请求字节 |
    | `to_client_` 非空 | `EPOLLOUT` | 有响应需要继续发送 |

    代理正在等待上游响应时客户端端点此时不订阅 EPOLLIN，因为项目不支持请求流水线，不能在上一个响应完成前继续读取下一条请求
    */
    std::uint32_t events = 0;
    if (role == EndpointRole::kClient) {
        if (!client_read_closed_) {
            events |= EPOLLRDHUP;
        }
        if (state_ == ProxyState::kReadingRequest && !client_read_closed_) {
            events |= EPOLLIN;
        }
        if (!to_client_.empty()) {
            events |= EPOLLOUT;
        }
        return events;
    }

    /*对于上游所关注的事件：
    | 当前状态 | 关注事件 | 原因 |
    | 始终 | `EPOLLRDHUP` | 检测上游关闭发送方向 |
    | `kConnectingUpstream` | `EPOLLOUT` | 等待非阻塞连接产生结果 |
    | `kSendingRequest` | `EPOLLOUT` | 继续发送尚未写完的请求 |
    | `kReadingResponse` | `EPOLLIN` | 读取上游响应 |
    */
    events |= EPOLLRDHUP;
    if (state_ == ProxyState::kConnectingUpstream ||
        state_ == ProxyState::kSendingRequest) {
        events |= EPOLLOUT;
    }//同一个 EPOLLOUT 在不同状态下含义不同;不能只看事件，还必须结合 state_
    if (state_ == ProxyState::kReadingResponse) {
        events |= EPOLLIN;
    }
    return events;
}

void ProxySession::on_event(//总调度函数,根据：哪一端 + 什么事件 + 当前状态,调用相应函数
    edgegate::net::EventLoop& loop,//当前事件循环，用于注册、修改或删除客户端/上游Endpoint
    EndpointRole role,//哪一端有事件
    std::uint32_t events) noexcept//具体是什么事件
    //state_：代理当前进行到哪一步这是ProxySession自己具备的
{
    if (state_ == ProxyState::kClosed) {//如果Session之前已经关闭，本次事件可能是同一轮 epoll_wait() 中残留的通知，不再处理
        return;
    }

    if (role == EndpointRole::kClient) {//先处理客户端fd发生的事件
        /*
        客户端Endpoint触发on_event()
                ↓
        客户端Socket错误？
        是 → 关闭Session
                ↓ 否
        正在等请求且可读？
        是 → read_client()，状态可能改变
                ↓
        Session是否已关闭？
        是 → 结束
                ↓ 否
        客户端是否半关闭？
        是 → 标记以后不再读
            → 当前请求完整：继续完成响应
            → 当前请求不完整：关闭Session
                ↓
        客户端是否完全挂断？
        是 → 关闭Session
                ↓
        客户端可写且存在待发响应？
        是 → write_client()
                ↓
        把最新关注事件同步给epoll
        */
        if ((events & EPOLLERR) != 0U) {//连接不可继续，关闭整个Session
            ++stats_->client_errors;
            close_session(loop);
            return;
        }

        if ((events & EPOLLIN) != 0U &&//可读且代理确实在等待客户端请求
            state_ == ProxyState::kReadingRequest) {
            read_client(loop);
            /*
            read_client()内部会一直 recv()，可能产生多种结果
            请求还不完整→ 保持 kReadingRequest

            请求完整→ connect_upstream()→ 变成 kConnectingUpstream 或 kSendingRequest

            客户端提前关闭→ close_session()→ 变成 kClosed

            请求错误→ 准备400/413→ 变成 kSendingResponse；如果错误响应立即发完，也可能直接 kClosed
            */
        }
        if (state_ == ProxyState::kClosed) {// read_client() 可能因EOF、Socket错误或错误响应已经发送完而关闭Session；
        // 如果已关闭，不能继续处理本轮的半关闭、挂断和可写事件。
            return;
        }

        if ((events & EPOLLRDHUP) != 0U) {// 收到EPOLLRDHUP表示客户端关闭了发送方向，以后不会再有新字节；
            // 当前请求可能完整，也可能被截断，必须结合read_client()后的state_判断。
            client_read_closed_ = true;//记录客户端发送方向已经关闭，使interests()以后不再订阅客户端EPOLLIN
            close_after_response_ = true;//设置自己在发完本次响应后也该关闭客户端连接,因为客户端已经不能再通过这条连接发送下一条请求
            if (state_ == ProxyState::kReadingRequest) {//判断客户端关闭发送方向以后，这条请求到底完整了没有？
                close_session(loop);//先处理了read_client()后只有在请求不完整就半关闭情况下此时状态才仍是kReadingRequest+(events & EPOLLRDHUP) != 0U，这种情况下直接关闭对话
                return;
            }
        }
        if ((events & EPOLLHUP) != 0U) {//连接完全挂断，关闭Session
            close_session(loop);
            return;
        }

        if ((events & EPOLLOUT) != 0U && !to_client_.empty()) {//客户端fd现在可写且to_client_ 中确实存在尚未发送的响应时向客户端发送响应
            write_client(loop);
        }
        refresh_interests(loop);//根据刚才可能改变的状态重新计算关注事件
        return;
    }

    //进入这段代码时已经确定role == EndpointRole::kUpstream,因为所有role==EndpointRole::kClient的分支都会在上一个if中返回
    //调用非阻塞 connect()时得到了errno == EINPROGRESS;状态会变成kConnectingUpstream
    if (state_ == ProxyState::kConnectingUpstream &&
        (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
        /*
        上游fd出现以下任意事件：
        EPOLLOUT：连接过程产生结果
        EPOLLERR：连接过程出现错误
        EPOLLHUP：连接已经挂断
        都只能说明：非阻塞连接过程结束了，但暂时不知道成功还是失败
        */
        finish_upstream_connect(loop);//它会使用：getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)读取准确结果
        //连接成功和连接失败的处理逻辑都在finish_upstream_connect内部
    } else if ((events & EPOLLERR) != 0U) {//如果当前不是“正在连接上游”，但上游fd出现了错误，就直接按上游运行错误处理
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
    }//使用 else if，避免连接阶段的同一个 EPOLLERR 被处理两次

    if (state_ == ProxyState::kSendingRequest &&//只有同时满足：当前正在发送请求+上游fd现在可写
        (events & EPOLLOUT) != 0U) {
        write_upstream(loop);//才把to_upstream_里的请求字节送进上游Socket
        /*write_upstream内部会处理多种情况，可能导致状态变化
        全部发送成功
        to_upstream_清空
        → 创建ResponseParser
        → state_ = kReadingResponse

        只发送了一部分
        成功多少就consume()多少
        → 继续send()

        本地发送缓冲区满
        send()返回EAGAIN
        → 未发送部分继续留在to_upstream_
        → state_仍为kSendingRequest
        → interests(kUpstream)继续返回EPOLLOUT
        → 等下一次可写通知

        发送失败
        记录upstream_errors
        → 返回502
        */
    }

    if (state_ == ProxyState::kReadingResponse &&//必须处于kReadingResponse
        (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0U) {//且发生下列任意事件
        /*
        EPOLLIN:上游响应字节已经进入EdgeGate本地内核接收缓冲区，可以调用 recv()
        EPOLLRDHUP:上游关闭了发送方向。
        这不代表立刻丢弃响应，因为FIN前面可能还有最后一批响应字节
        EPOLLHUP:上游连接挂断，也进入 read_upstream()，让 recv()和解析器判断：
            响应是否已经完整；
            是否属于合法的关闭定界响应；
            是否在固定长度/chunked尚未完成时提前关闭

        若合法结束内部调用finish_response(loop)；提前关闭则返回：502 Bad Gateway
        */
        read_upstream(loop);
    }
    /*
    这三段不能使用一个else-if链，因为调用一个函数后，state_可能立即改变
    */
    refresh_interests(loop);
}

ProxyState ProxySession::state() const noexcept
{
    return state_;
}

void ProxySession::read_client(edgegate::net::EventLoop& loop) noexcept
{   /*
    输入：客户端fd中当前可读取的字节
    输出:
    请求暂时不完整 → 保持kReadingRequest，等待下一次EPOLLIN
    请求完整       → 放入to_upstream_，开始连接上游
    请求错误       → 准备400或413
    客户端关闭/错误 → 关闭Session
    */
    std::array<char, kIoChunkSize> bytes{};//临时接收数组(临时容器,会被覆盖)
    for (;;) {//一个 EPOLLIN可能对应很多已经到达的字节，而一次 recv()不保证全部取完,使用无限循环直到发生：
            /*请求完整；
            协议错误；
            对端关闭；
            Socket错误；
            当前数据已经读空，得到 EAGAIN*/
        const ssize_t received =
            ::recv(client_fd_, bytes.data(), bytes.size(), 0);//client_fd_：从客户端连接读取 bytes.data()：把数据写到临时数组开头 bytes.size()：本次最多读取16 KiB
        if (received > 0) {//收到真实字节
            const auto result = request_parser_.consume(std::string_view(//RequestParser会把本次真实字节复制到自己内部保存
                bytes.data(), static_cast<std::size_t>(received)));
            if (result.status == edgegate::http::ParseStatus::kError) {
                ++stats_->client_errors;
                const int status =
                    result.error == edgegate::http::ParseError::kBodyTooLarge ||
                    result.error == edgegate::http::ParseError::kHeaderTooLarge
                    ? 413//容量超限映射为413
                    : 400;//其他请求错误映射为400
                queue_error_response(
                    loop,
                    status,
                    status == 413 ? "Payload Too Large" : "Bad Request");
                return;//queue_error_response()已经把状态切换成kSendingResponse并准备向客户端发送错误响应。此时不能继续读取或连接上游
            }
            if (result.status ==//请求完整
                edgegate::http::ParseStatus::kMessageComplete) {
                close_after_response_ = request_wants_close();//检查客户端是否要求关闭
                std::string upstream_request;
                if (config_.route_table) {
                    const auto route = config_.route_table->lookup(
                        *request_parser_.header_value("Host"),
                        request_parser_.target());
                    if (route.status == edgegate::routing::RouteLookupStatus::kNoRoute) {
                        queue_error_response(loop, 404, "Not Found");
                        return;
                    }
                    if (route.status ==
                        edgegate::routing::RouteLookupStatus::kNoHealthyUpstream) {
                        queue_error_response(loop, 503, "Service Unavailable");
                        return;
                    }
                    selected_upstream_address_ = route.upstream->address;
                    selected_upstream_port_ = route.upstream->port;
                    upstream_request = edgegate::http::rewrite_request_for_upstream(
                        request_parser_, client_address_);
                } else {
                    selected_upstream_address_ = config_.upstream_address;
                    selected_upstream_port_ = config_.upstream_port;
                    upstream_request.assign(request_parser_.raw_message());
                }
                if (!to_upstream_.append(upstream_request)) {//把完整请求放入上游输出缓冲区
                    ++stats_->client_errors;
                    queue_error_response(loop, 413, "Payload Too Large");
                    return;
                }
                connect_upstream(loop);//启动上游连接
                return;
            }
            continue;//排除错误和完成后，剩余情况自然就是需要更多数据执行continue的状态只能是kNeedMoreData
            //回到循环顶部，再调用一次 recv()
        }

        if (received == 0) {
            client_read_closed_ = true;
            close_session(loop);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        ++stats_->client_errors;
        close_session(loop);
        return;
    }
}

void ProxySession::connect_upstream(edgegate::net::EventLoop& loop) noexcept
{
    edgegate::net::UniqueFd socket(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0));
    if (!socket) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(selected_upstream_port_);
    if (::inet_pton(
            AF_INET,
            selected_upstream_address_.c_str(),
            &address.sin_addr) != 1) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    const int result = ::connect(
        socket.get(),
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
    if (result == 0) {
        state_ = ProxyState::kSendingRequest;
    } else if (errno == EINPROGRESS) {
        state_ = ProxyState::kConnectingUpstream;
    } else {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    const int descriptor = socket.get();
    upstream_fd_ = descriptor;
    try {
        loop.add(std::make_unique<ProxyEndpoint>(
            std::move(socket),
            EndpointRole::kUpstream,
            shared_from_this()));
    } catch (...) {
        upstream_fd_.reset();
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    if (state_ == ProxyState::kSendingRequest) {
        write_upstream(loop);
    }
}

void ProxySession::finish_upstream_connect(
    edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(
            *upstream_fd_,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &length) == -1 ||
        socket_error != 0) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    state_ = ProxyState::kSendingRequest;
    write_upstream(loop);
}

void ProxySession::write_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value()) {
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    while (!to_upstream_.empty()) {
        const std::string_view pending = to_upstream_.readable_view();
        const ssize_t sent = ::send(
            *upstream_fd_,
            pending.data(),
            pending.size(),
            MSG_NOSIGNAL);
        if (sent > 0) {
            static_cast<void>(
                to_upstream_.consume(static_cast<std::size_t>(sent)));
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    response_parser_.emplace(
        std::string(request_parser_.method()),
        config_.max_header_size,
        config_.max_response_body_size);
    state_ = ProxyState::kReadingResponse;
}

void ProxySession::read_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (!upstream_fd_.has_value() || !response_parser_.has_value()) {
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    std::array<char, kIoChunkSize> bytes{};
    for (;;) {
        const ssize_t received =
            ::recv(*upstream_fd_, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            const auto result = response_parser_->consume(std::string_view(
                bytes.data(), static_cast<std::size_t>(received)));
            if (result.status == edgegate::http::ParseStatus::kError) {
                ++stats_->upstream_errors;
                queue_error_response(loop, 502, "Bad Gateway");
                return;
            }
            if (result.status ==
                edgegate::http::ParseStatus::kMessageComplete) {
                if (response_parser_->remaining_bytes() != 0) {
                    ++stats_->upstream_errors;
                    queue_error_response(loop, 502, "Bad Gateway");
                    return;
                }
                finish_response(loop);
                return;
            }
            continue;
        }

        if (received == 0) {
            const auto result = response_parser_->notify_eof();
            if (result.status ==
                edgegate::http::ParseStatus::kMessageComplete) {
                finish_response(loop);
            } else {
                ++stats_->upstream_errors;
                queue_error_response(loop, 502, "Bad Gateway");
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }
}

void ProxySession::write_client(edgegate::net::EventLoop& loop) noexcept
{
    while (!to_client_.empty()) {
        const std::string_view pending = to_client_.readable_view();
        const ssize_t sent = ::send(
            client_fd_, pending.data(), pending.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            static_cast<void>(
                to_client_.consume(static_cast<std::size_t>(sent)));
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ++stats_->client_errors;
        close_session(loop);
        return;
    }

    if (state_ != ProxyState::kSendingResponse) {
        return;
    }
    if (close_after_response_ || client_read_closed_) {
        close_session(loop);
        return;
    }

    request_parser_ = edgegate::http::RequestParser(
        config_.max_header_size,
        config_.max_request_body_size);
    response_parser_.reset();
    close_after_response_ = false;
    state_ = ProxyState::kReadingRequest;
}

void ProxySession::finish_response(edgegate::net::EventLoop& loop) noexcept
{
    std::string client_response;
    if (config_.route_table) {
        // 上游是否关闭只影响“上游—EdgeGate”连接；客户端连接由客户端请求决定。
        client_response = edgegate::http::rewrite_response_for_client(
            *response_parser_, close_after_response_ || client_read_closed_);
    } else {
        close_after_response_ =
            close_after_response_ || response_requires_close();
        client_response.assign(response_parser_->raw_message());
    }

    if (!to_client_.append(client_response)) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
        return;
    }

    ++stats_->completed_requests;
    close_upstream(loop);
    state_ = ProxyState::kSendingResponse;
    write_client(loop);
}

void ProxySession::queue_error_response(
    edgegate::net::EventLoop& loop,
    int status,
    const char* reason) noexcept
{
    close_upstream(loop);
    to_upstream_.clear();
    to_client_.clear();

    const std::string body =
        std::to_string(status) + " " + reason + "\n";
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

    if (!to_client_.append(response)) {
        close_session(loop);
        return;
    }
    close_after_response_ = true;
    state_ = ProxyState::kSendingResponse;
    write_client(loop);
}

void ProxySession::close_upstream(edgegate::net::EventLoop& loop) noexcept
{
    if (upstream_fd_.has_value()) {
        static_cast<void>(loop.remove(*upstream_fd_));
        upstream_fd_.reset();
    }
}

void ProxySession::close_session(edgegate::net::EventLoop& loop) noexcept
{
    if (state_ == ProxyState::kClosed) {
        return;
    }
    state_ = ProxyState::kClosed;
    close_upstream(loop);
    static_cast<void>(loop.remove(client_fd_));
}

void ProxySession::refresh_interests(
    edgegate::net::EventLoop& loop) noexcept
{
    if (state_ == ProxyState::kClosed) {
        return;
    }
    if (!loop.modify(client_fd_, interests(EndpointRole::kClient))) {
        close_session(loop);
        return;
    }
    if (upstream_fd_.has_value() &&
        !loop.modify(*upstream_fd_, interests(EndpointRole::kUpstream))) {
        ++stats_->upstream_errors;
        queue_error_response(loop, 502, "Bad Gateway");
    }
}

bool ProxySession::request_wants_close() const noexcept
{
    return header_contains_token(
        request_parser_.header_value("Connection"), "close");
}

bool ProxySession::response_requires_close() const noexcept
{
    if (response_parser_->body_mode() ==
        edgegate::http::ResponseBodyMode::kCloseDelimited) {
        return true;
    }
    const auto connection = response_parser_->header_value("Connection");
    if (header_contains_token(connection, "close")) {
        return true;
    }
    if (response_parser_->version() == "HTTP/1.0") {
        return !header_contains_token(connection, "keep-alive");
    }
    return false;
}

} // namespace edgegate::proxy
// AI-CODE-END: S5-PROXY-SESSION-IMPLEMENTATION
