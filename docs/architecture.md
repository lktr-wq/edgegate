<!-- AI-CODE-BEGIN: S10-ARCHITECTURE -->
# EdgeGate 架构说明

## 1. 一次请求的完整路径

```text
1. client fd 可读
       ↓
2. EventLoop 从 epoll_wait() 得到 fd 和事件
       ↓
3. ReliableProxySession 读入有界 ByteBuffer
       ↓
4. RequestParser 在任意 TCP 分片下增量确认消息边界
       ↓
5. RouteTable 按 Host + 最长路径前缀选择路由
       ↓
6. 从健康上游中轮询，非阻塞 connect()
       ↓
7. 改写逐跳 Header，将请求流式发往上游
       ↓
8. ResponseParser 识别响应边界，边读边写回客户端
       ↓
9. 客户端实际收到完整响应后记录日志和指标
```

`epoll` 不是线程调度器。它只告诉 EdgeGate：哪个 fd 现在读或写更可能立即取得进展。`EventLoop` 再把该 fd 分派给持有它的连接对象。

## 2. 主要模块

### 网络层

- `UniqueFd`：RAII 拥有 fd，对象销毁时自动 `close()`。
- `ByteBuffer`：保存短读、短写之间尚未处理的字节，容量有上限。
- `EventLoop`：管理 epoll 注册、修改、删除和事件分派。

### HTTP 层

- `RequestParser`：请求行、Header、`Content-Length` 请求体。
- `ResponseParser`：状态行、Header 以及四种响应定界方式。
- `ChunkedStreamDecoder`：流式检查 chunk 长度和 Trailer，不因为 chunked 就缓存全部正文。
- `MessageRewriter`：移除不能端到端转发的逐跳 Header，重建可信的转发信息。

### 代理和可靠性层

- `ReliableProxySession`：把客户端和上游两个 fd 组成一个状态机。
- `RouteTable`：保存配置快照、路由优先级和上游健康状态。
- `timerfd`：把连接、请求、空闲和健康检查超时放进同一个事件循环。
- 高/低水位：待发数据太多时暂停源端读取，防止慢接收方拖垮内存。

### 运行时层

- `signalfd`：将 SIGTERM/SIGINT 变成 EventLoop 可处理的 fd 事件。
- `ManagementServer`：只接受本机 Unix Socket 上的单行 JSON 命令。
- `RuntimeLogger`：异步访问、错误和管理日志，文件大小有上限。
- `ObservabilityStore`：固定延迟桶、状态码、路由/上游聚合和有界慢请求摘要。
- `DashboardServer`：独立本机 HTTP 监听器，只有静态页面和只读 JSON。

## 3. 关键状态和资源所有权

每个代理会话至少经历：读取请求、连接上游、发送请求、读取响应、写回客户端、回收/等待 Keep-Alive。状态决定某个 fd 此刻对 `EPOLLIN` 还是 `EPOLLOUT` 感兴趣。

fd 只由 RAII 对象拥有。EventLoop 保存事件处理对象，删除对象后 fd 自动关闭。这使正常完成、对端中断、超时和异常路径使用同一套资源回收规则。

## 4. 配置热重载

```text
edgegatectl reload
  → 服务重新读取 YAML
  → 完整语法、范围、引用关系校验
  → 构建新 RouteTable/可观测限制
  → 原子替换配置快照
```

监听地址、管理 Socket 和 Dashboard 端口在 v1 中不支持热变更，因为它们涉及重建底层监听 fd。尝试改动时 reload 会失败，旧快照保留。

## 5. systemd 边界

systemd 以 `edgegate` 低权限用户运行程序，文件系统默认只读，只允许写入 `/run/edgegate` 和 `/var/log/edgegate`。服务没有 Linux capability，因此不能监听小于 1024 的特权端口。

这些限制不是反向代理功能，而是“即使程序出错，也尽量减少可修改的系统范围”。
<!-- AI-CODE-END: S10-ARCHITECTURE -->
