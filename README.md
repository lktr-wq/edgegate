<!-- AI-CODE-BEGIN: S10-README -->
# EdgeGate

EdgeGate 是一个使用 C++17 和 Linux POSIX API 实现的 HTTP/1.1 反向代理。它直接使用非阻塞 Socket、LT `epoll`、`timerfd`、`signalfd` 和 Unix Socket，并提供 YAML 路由、健康检查、背压、热重载、轮转日志、管理 CLI 和本机只读 Dashboard。

> 当前版本：`0.1.0`。项目实现和测试已由 AI 主导完成，作者负责产品取舍、证据审核和后续源码复核；这不等同于所有模块均已独立手写或逐行掌握。

## 1. 它解决什么问题

客户端不再直接连接每个后端，而是统一访问 EdgeGate：

```text
curl / 浏览器
       ↓ HTTP/1.1 :18080
EdgeGate：解析 → 路由 → 选择健康上游 → 流式转发
       ↓
backend-a / backend-b / backend-c
```

EdgeGate 不是 WAF，也不会自动修复后端漏洞。它提供的是统一入口、路由、故障隔离、超时、资源上限和运行观测。

## 2. 环境与能力边界

已验证环境：Ubuntu 22.04.5、G++ 11.4、CMake 3.22。EdgeGate 是 Linux 专用程序，不能在 Windows 上原生运行。

当前支持：

- HTTP/1.1 普通请求和 `Content-Length` 请求体。
- 顺序 Keep-Alive。
- 无正文、`Content-Length`、chunked 和关闭定界响应。
- Host 精确/通配匹配、最长路径前缀和健康上游轮询。

明确不支持：TLS、HTTP/2、CONNECT、WebSocket、chunked 请求体、请求流水线、缓存和通用 TCP 代理。

## 3. 从干净 Ubuntu 构建和测试

### 3.1 安装依赖

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake \
  libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libgtest-dev \
  curl jq
```

如果 `command -v curl` 显示 `/snap/bin/curl` 且每次请求都重复输出 Caution，这是 Snap 包装提示，不是 EdgeGate 错误；可执行一次 `curl.snap-acked` 关闭该提示。

### 3.2 严格 Release 构建

```bash
cd /path/to/edgegate
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure -j2
```

预期结果：编译无 `-Wall -Wextra -Wpedantic` 警告，CTest 全部通过。

## 4. 安装为 Linux 服务

### 4.1 安装文件

```bash
sudo cmake --install build-release --prefix /usr/local
```

这一步只安装程序、示例配置、systemd 单元和文档，不会自动启动服务。

### 4.2 创建低权限用户并启动

```bash
sudo /usr/local/sbin/edgegate-service-install --enable-now
sudo systemctl status edgegate --no-pager
```

管理脚本会：

1. 创建不能登录的 `edgegate` 用户。
2. 创建 `/etc/edgegate`、`/run/edgegate` 和 `/var/log/edgegate`。
3. 仅在配置不存在时创建 `/etc/edgegate/edgegate.yaml`。
4. 加载并启动 `edgegate.service`。

## 5. 启动三个可重复演示后端

以下命令在当前终端后台启动三个本机后端：

```bash
/usr/local/libexec/edgegate/edgegate_test_backend backend-a 19081 &
backend_a_pid=$!
/usr/local/libexec/edgegate/edgegate_test_backend backend-b 19082 &
backend_b_pid=$!
/usr/local/libexec/edgegate/edgegate_test_backend backend-c 19083 &
backend_c_pid=$!
```

连续请求数次，正文应在三个后端名之间轮询：

```bash
for request in 1 2 3 4 5 6; do
  curl -sS -H 'Host: api.edgegate.test' http://127.0.0.1:18080/api/demo
done
```

演示结束后只停止刚才记录的 PID：

```bash
kill "${backend_a_pid}" "${backend_b_pid}" "${backend_c_pid}"
wait "${backend_a_pid}" "${backend_b_pid}" "${backend_c_pid}" 2>/dev/null || true
```

## 6. 管理和查看

```bash
sudo edgegatectl --socket /run/edgegate/edgegate.sock status
sudo edgegatectl --socket /run/edgegate/edgegate.sock routes
sudo edgegatectl --socket /run/edgegate/edgegate.sock upstreams
sudo edgegatectl --socket /run/edgegate/edgegate.sock stats --json | jq
```

修改 `/etc/edgegate/edgegate.yaml` 后原子重载：

```bash
sudo systemctl reload edgegate
```

新配置完整校验成功才切换；失败时旧配置继续服务。

查看服务与日志：

```bash
sudo systemctl status edgegate --no-pager
sudo journalctl -u edgegate -n 50 --no-pager
sudo ls -l /var/log/edgegate
sudo tail -n 20 /var/log/edgegate/error.log
```

## 7. 在 Windows 浏览器展示 Dashboard

Dashboard 默认只监听 Ubuntu 本机 `127.0.0.1:18081`。在 Windows PowerShell 建立 SSH 隧道：

```powershell
ssh -L 18081:127.0.0.1:18081 zy@192.168.150.129
```

保持该终端连接，Windows 浏览器打开 `http://127.0.0.1:18081`。无需把 Dashboard 暴露给局域网。

## 8. 停止和后续阅读

```bash
sudo systemctl stop edgegate
sudo systemctl start edgegate
sudo systemctl restart edgegate
```

- [用户手册](docs/user-guide.md)：完整配置项和 CLI。
- [架构说明](docs/architecture.md)：从 fd 就绪到响应返回的内部链路。
- [故障排查](docs/troubleshooting.md)：按现象、证据和处理步骤定位问题。
- [项目计划](项目计划.md)：范围、里程碑和验收边界。

## 9. 许可证

MIT License，见 [LICENSE](LICENSE)。
<!-- AI-CODE-END: S10-README -->
