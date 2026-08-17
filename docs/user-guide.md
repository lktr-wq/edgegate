<!-- AI-CODE-BEGIN: S10-USER-GUIDE -->
# EdgeGate 用户手册

## 1. 运行时文件

| 路径 | 用途 | 权限边界 |
| --- | --- | --- |
| `/usr/local/bin/edgegate` | 代理服务 | root 安装，普通用户可执行 |
| `/usr/local/bin/edgegatectl` | 单次管理 CLI | 通常配合 `sudo` 访问 Socket |
| `/etc/edgegate/edgegate.yaml` | 正式配置 | `root:edgegate`，`0640` |
| `/run/edgegate/edgegate.sock` | 本机管理通道 | 只有服务用户和 root 可访问 |
| `/var/log/edgegate/` | 访问/错误/管理日志 | `edgegate` 用户可写 |

`/run` 是内存中的运行时目录，重启 Ubuntu 后会重建；`/var/log` 保存需要轮转的持久日志。

## 2. YAML 配置阅读方法

建议按以下顺序理解：

1. `listen`：客户端连到哪个地址和端口。
2. `upstream_pools`：请求最终可以发给哪些后端。
3. `routes`：哪类 Host 和路径使用哪个上游池。
4. `limits`/`stream_buffer`/`timeouts`：单个请求和连接可以占用多少资源。
5. `health_check`：如何判断上游能否接收请求。
6. `management`/`logging`/`dashboard`：如何管理和观察服务。

## 3. 主要配置项

### `listen`

- `address`：代理监听地址。`127.0.0.1` 只接受 Ubuntu 本机请求；`0.0.0.0` 代表所有 IPv4 网卡，修改前必须同时审查防火墙和上游权限。
- `port`：监听端口，v1 不支持 TLS，不要将它误当 HTTPS 端口。

### `limits`

- `max_header_size`：请求或响应 Header 的最大字节数。
- `max_request_body_size`：请求体上限，超限返回 `413`。
- `max_response_body_size`：上游响应体上限。

这些上限不等于程序会一次性分配同等大小内存，而是消息接受的产品边界。

### `stream_buffer`

- `capacity`：单个转发方向的最大待发字节。
- `high_watermark`：到达后暂停读取来源 Socket。
- `low_watermark`：待发数据降低到该值后恢复读取。

必须满足 `low_watermark < high_watermark <= capacity`。

### `timeouts`

- `client_header_ms`：客户端发完 Header 的最长时间。
- `upstream_connect_ms`：连接上游的最长时间。
- `upstream_header_ms`：等待上游响应 Header 的最长时间。
- `io_idle_ms`：数据转发期间无进展的最长时间。
- `request_total_ms`：单次请求总时间上限。
- `keep_alive_idle_ms`：Keep-Alive 连接等待下一个请求的时间。

### `health_check`

EdgeGate 定期请求 `path`。连续失败达到 `failure_threshold` 后摘除上游；摘除后连续成功达到 `success_threshold` 才恢复。这种双阈值避免上游在健康/不健康之间频繁抖动。

### `routes` 和 `upstream_pools`

一条路由的选择次序：

1. Host 精确匹配优先于 `*.example.com` 通配。
2. 确定 Host 后，选择最长的字面路径前缀。
3. 在该路由的健康上游中轮询。

查询串不参与路由匹配。没有路由返回 `404`；路由存在但没有健康上游返回 `503`。

### `management`、`logging` 和 `dashboard`

- systemd 安装版的 Socket 应保持 `/run/edgegate/edgegate.sock`。
- 日志目录应保持 `/var/log/edgegate`，程序按大小和个数轮转。
- Dashboard v1 只允许 `127.0.0.1`，只读且不保存全量请求历史。

## 4. `edgegatectl` 命令

| 命令 | 作用 | 是否改变服务状态 |
| --- | --- | --- |
| `status` | 查看状态、启动时间和活跃会话 | 否 |
| `routes` | 查看已生效路由 | 否 |
| `upstreams` | 查看上游健康状态 | 否 |
| `stats --json` | 输出机器可读指标 | 否 |
| `reload` | 完整校验并原子切换配置 | 是 |
| `drain` | 停止接收新连接，等待现有请求 | 是 |
| `stop` | 排空后停止服务 | 是 |

通用形式：

```bash
sudo edgegatectl --socket /run/edgegate/edgegate.sock <command>
```

systemd 运行时，常规停止建议使用 `sudo systemctl stop edgegate`，由 systemd 发送 SIGTERM 并监督最终退出。

## 5. 安全重载流程

```bash
sudo cp /etc/edgegate/edgegate.yaml /etc/edgegate/edgegate.yaml.bak
sudoedit /etc/edgegate/edgegate.yaml
sudo systemctl reload edgegate
sudo edgegatectl --socket /run/edgegate/edgegate.sock status
```

如果 reload 失败，先查看 `journalctl` 和 `/var/log/edgegate/error.log`，不要重启服务来“碰运气”：重启会让非法配置变成启动失败，而原子 reload 会保留旧配置。
<!-- AI-CODE-END: S10-USER-GUIDE -->
