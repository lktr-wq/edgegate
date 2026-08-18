<!-- AI-CODE-BEGIN: S12-RELEASE-NOTES -->
# EdgeGate v1.0.0 发布说明

发布日期：2026-08-18

## 产品定位

EdgeGate v1.0.0 是面向 Linux 的 C++17 HTTP/1.1 反向代理。它直接使用非阻塞 Socket、LT `epoll`、`timerfd`、`signalfd` 和 Unix Socket，实现了从网络事件循环、HTTP 增量解析到代理状态机、运行管理和可观测性的完整闭环。

## v1.0.0 能力

- Host 精确/通配匹配和最长路径前缀路由。
- 健康上游轮询、主动健康检查、故障摘除和安全 GET/HEAD 重试。
- 普通请求、`Content-Length` 请求体、顺序 Keep-Alive。
- 无正文、`Content-Length`、chunked 和关闭定界响应。
- 双向流式转发、容量限制、高低水位背压和分阶段超时。
- YAML 配置完整校验和失败保留旧配置的原子 reload。
- `edgegatectl` 状态查询、reload、drain 和 stop。
- 有界异步轮转日志、JSON 指标和本机只读 Dashboard。
- CMake 安装、低权限 systemd 服务、示例配置和故障排查文档。

## 明确限制

- 单 Reactor 网络线程，不宣称多核水平扩展能力。
- 上游地址只支持数字 IPv4，不支持 DNS 域名。
- 不支持 TLS、HTTP/2、CONNECT、WebSocket、chunked 请求体、请求流水线、缓存和通用 TCP 代理。
- Dashboard 默认只监听本机且只读，不提供认证后的远程配置页面。
- 当前性能证据来自同一台小型虚拟机的 localhost 场景，不能直接当作生产互联网容量。

## 发布证据

已验证环境：Ubuntu 22.04.5、Linux 6.8.0-136-generic、G++ 11.4、CMake 3.22，虚拟机为 2 vCPU、3.8 GiB 内存。

- 阶段 12 的全新 Debug、ASan/UBSan（含泄漏检查）和 Release 构建均为 `128/128`，严格编译零警告；其中新增 1 项发布元数据门禁。
- 不含 `.git`、旧构建目录和测试产物的最终源码快照再次完成 Release 构建、128 项测试和隔离安装；临时端口上的代理、Dashboard 与管理 stop 真实进程链路均通过。
- 100 并发、100,000 请求：全部 HTTP 200，0 请求失败，0 代理 5xx；8623.29 QPS，P50/P95/P99 为 10.988/15.521/18.931 ms。
- 故障矩阵覆盖畸形/超限请求、慢客户端、客户端提前退出、上游崩溃、全部不健康、无效 reload、日志启动失败和 fd 回收。
- 连续 12 小时完成 124,410,000 请求，全部 HTTP 200；RSS 首尾中位数 8804/8808 KiB，fd 12/12，线程 2/2，没有观察到持续增长。

完整测试边界、原始产物相对路径和缺陷闭环见 [阶段 11 压测与加固报告](docs/stage11-report.md)。

## 实现与掌握边界

阶段 6 起主要代码和测试由 AI 主导实现，源码以 `AI-CODE-BEGIN/END` 标记并由 `AI实现索引.md` 建立索引。项目作者负责方向、范围、关键方案选择、问题质疑、证据审核和真实环境验收。该项目可以证明作者参与了完整工程决策与验证，但不应表述为所有代码均由作者独立手写或已经逐行掌握。

## 后续候选

v1.0.0 之后可按真实需求分别评估多 Reactor/多线程、异步 DNS、TLS 终止和更完整的协议兼容性；这些能力不属于本次发布承诺。
<!-- AI-CODE-END: S12-RELEASE-NOTES -->
