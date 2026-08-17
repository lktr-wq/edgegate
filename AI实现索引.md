# EdgeGate AI 实现索引

> 用途：记录由 AI 直接写入或实质重构的代码、测试、构建配置和脚本。源码中的 `AI-CODE-BEGIN/END` 与本文件的标记编号对应。
>
> 既有代码若只是用户根据示范亲手输入，不追溯标记为 AI 直接实现；从 2026-08-13 协作方式变更后开始登记。

## 标记规则

~~~text
// AI-CODE-BEGIN: 标记编号
由 AI 直接写入或重构的 C/C++ 代码
// AI-CODE-END: 标记编号

# AI-CODE-BEGIN: 标记编号
由 AI 直接写入或重构的 CMake/Shell 配置
# AI-CODE-END: 标记编号
~~~

标记按连续代码块设置，不在每一行重复添加。若 AI 对既有函数进行实质重构，则整个重构后的函数进入标记范围。

## 实现记录

| 标记编号 | 阶段 | 文件/范围 | AI 直接完成内容 | 用户复核重点 | 验证/提交 |
|---|---|---|---|---|---|
| `S3-HTTP-COMMON-TYPES` | 3 | `include/edgegate/http/message_types.h` | 请求、响应解析器共用的完成状态和 Header 字段类型 | 为什么“需要更多数据”不是错误 | Ubuntu Debug 与 Sanitizer 测试通过 |
| `S3-REQUEST-PARSER-API` | 3 | `include/edgegate/http/request_parser.h` | 请求解析器接口、错误分类、容量限制和结果访问接口 | 输入、输出和明确拒绝项 | Ubuntu Debug 与 Sanitizer 测试通过 |
| `S3-REQUEST-PARSER-IMPLEMENTATION` | 3 | `src/http/request_parser.cpp` | 请求行、Header、Host、`Content-Length` 正文和流水线拒绝逻辑 | 任意分片为何得到相同结果 | Ubuntu Debug 与 Sanitizer 测试通过 |
| `S3-REQUEST-PARSER-TESTS` | 3 | `tests/http_request_parser_test.cpp` | 请求单次/逐字节/全部二分点/随机分片及错误输入测试 | 测试是否覆盖计划中的受控范围 | 28 项通过 |
| `S3-RESPONSE-PARSER-API` | 3 | `include/edgegate/http/response_parser.h` | 响应解析器接口、四种正文定界模式和错误分类 | EOF 为什么有时代表完成、有时代表错误 | Ubuntu Debug 与 Sanitizer 测试通过 |
| `S3-RESPONSE-PARSER-IMPLEMENTATION` | 3 | `src/http/response_parser.cpp` | 无正文、固定长度、chunked、关闭定界解析及 Trailer 处理 | 四类结束条件和缓冲区上限 | Ubuntu Debug 与 Sanitizer 测试通过 |
| `S3-RESPONSE-PARSER-TESTS` | 3 | `tests/http_response_parser_test.cpp` | 响应分片、四类定界、EOF、chunk、Trailer 和错误输入测试 | 原始消息与解码正文的区别 | 16 项通过 |
| `S3-RESPONSE-PARSER-BUILD` | 3 | `CMakeLists.txt` | 将响应实现加入 `edgegate_core` | 正式程序和测试是否复用同一实现 | 严格编译零警告 |
| `S3-RESPONSE-PARSER-TEST-BUILD` | 3 | `CMakeLists.txt` | 将响应测试加入 GoogleTest/CTest 目标 | CTest 是否真实发现全部用例 | 总计 45/45 通过 |
| `S4-UNIQUE-FD` | 4 | `include/edgegate/net/unique_fd.h` | fd 独占所有权、移动语义和析构自动关闭 | 为什么 fd 不能复制所有权 | 普通与 Sanitizer 测试通过 |
| `S4-BYTE-BUFFER-API` / `S4-BYTE-BUFFER-IMPLEMENTATION` | 4 | `include/edgegate/net/byte_buffer.h`、`src/net/byte_buffer.cpp` | 有容量上限、可消费前缀的字节缓冲区 | 未发送字节如何保留 | 5 项测试通过 |
| `S4-EVENT-LOOP-API` / `S4-EVENT-LOOP-IMPLEMENTATION` | 4 | `include/edgegate/net/event_loop.h`、`src/net/event_loop.cpp` | LT `epoll` 注册、修改、删除、等待和安全延迟销毁 | epoll 只报告 fd，Reactor 如何分派对象 | 3 项测试通过 |
| `S4-ECHO-CONNECTION-API` / `S4-ECHO-CONNECTION-IMPLEMENTATION` | 4 | `include/edgegate/net/echo_connection.h`、`src/net/echo_connection.cpp` | 非阻塞读写循环、`EAGAIN`、部分写、半关闭和高低水位背压 | 暂停读与继续写的条件 | 集成测试通过 |
| `S4-REACTOR-SERVER-API` / `S4-REACTOR-SERVER-IMPLEMENTATION` | 4 | `include/edgegate/net/reactor_echo_server.h`、`src/net/reactor_echo_server.cpp` | 非阻塞监听、`accept4` 循环和连接注册 | LT 模式为何要循环到 `EAGAIN` | 集成测试通过 |
| `S4-BYTE-BUFFER-TESTS` / `S4-UNIQUE-FD-TESTS` / `S4-EVENT-LOOP-TESTS` | 4 | 三份基础设施测试 | 容量、所有权、事件分派和回调内安全移除 | 测试与实现契约是否一致 | 11 项通过 |
| `S4-REACTOR-INTEGRATION-TESTS` | 4 | `tests/reactor_echo_integration_test.cpp` | 顺序消息、24 并发、慢连接隔离、512 KiB 背压和 50 次 fd 回收 | 为什么 Echo 只验证网络发动机 | 5 项通过 |
| `S4-REACTOR-DEMO` / `S4-REACTOR-DEMO-BUILD` | 4 | `apps/reactor_demo_main.cpp`、`CMakeLists.txt` | 可运行的阶段 4 Echo 验收程序 | 它不是最终代理产品 | Ubuntu 构建通过 |
| `S4-REACTOR-BUILD` / `S4-REACTOR-TEST-BUILD` | 4 | `CMakeLists.txt` | Reactor 源码、线程库和 GoogleTest 目标接入 | 严格编译与测试发现 | 全量 CTest 61/61 通过 |
| `S5-REQUEST-RAW-ACCESS` | 5 | 请求解析器头文件与实现 | 暴露已完成请求的精确原始字节 | 转发为什么不能重新拼字符串猜长度 | 请求与代理回归通过 |
| `S5-PROXY-SESSION-API` / `S5-PROXY-SESSION-IMPLEMENTATION` | 5 | `include/edgegate/proxy/proxy_session.h`、`src/proxy/proxy_session.cpp` | 客户端/上游端点共享会话、非阻塞 connect、请求响应状态迁移、错误响应和顺序 Keep-Alive | 两个 fd 如何属于同一请求 | 10 项代理集成测试通过 |
| `S5-PROXY-SERVER-API` / `S5-PROXY-SERVER-IMPLEMENTATION` | 5 | `include/edgegate/proxy/proxy_server.h`、`src/proxy/proxy_server.cpp` | 非阻塞代理监听和会话创建 | Listener 与 Session 的职责边界 | 并发与 fd 回收通过 |
| `S5-PROXY-DEMO` / `S5-PROXY-DEMO-BUILD` | 5 | `apps/proxy_demo_main.cpp`、`CMakeLists.txt` | `18082 -> 19080` 可运行阶段验收程序 | 尚不是最终可配置产品入口 | Ubuntu 严格构建通过 |
| `S5-PROXY-INTEGRATION-TESTS` / `S5-PROXY-TEST-BUILD` | 5 | `tests/proxy_integration_test.cpp`、`CMakeLists.txt` | 真实 TCP 上游、四类响应、POST、HEAD、Keep-Alive、半关闭、并发、400/502 和 fd 回收 | 状态机失败路径是否有证据 | 10/10；全量 Sanitizer 73/73 |
| `S5-PROXY-STATE-MACHINE-BUILD` | 5 | `CMakeLists.txt` | 将代理会话与服务器加入核心库 | 正式目标是否复用同一实现 | 全量 CTest 73/73 |
| `S6-ROUTE-TABLE-API` / `S6-ROUTE-TABLE-IMPLEMENTATION` | 6 | `include/edgegate/routing/route_table.h`、`src/routing/route_table.cpp` | Host 语法/端口规范化、精确/通配优先级、最长路径前缀、健康上游轮询 | 路由优先级是否符合产品预期 | 普通与 Sanitizer 11/11 |
| `S6-ROUTE-TABLE-TESTS` / `S6-ROUTE-TABLE-TEST-BUILD` | 6 | `tests/route_table_test.cpp`、`CMakeLists.txt` | 匹配优先级、Host语法、端口、查询串、轮询、无健康节点和配置冲突测试 | 字面前缀与通配语义 | 11/11 |
| `S6-ROUTE-TABLE-BUILD` | 6 | `CMakeLists.txt` | 将纯 C++ 路由核心加入核心库 | YAML 接入前后是否复用同一规则 | 严格编译零警告 |
| `S6-YAML-CONFIG-API` / `S6-YAML-CONFIG-IMPLEMENTATION` / `S6-YAML-CONFIG-TESTS` | 6 | `include/edgegate/config/edgegate_config.h`、`src/config/edgegate_config.cpp`、配置测试 | YAML 加载、未知字段、类型/范围、IPv4、池引用和路由语义校验 | 为什么配置只能包含已经生效的能力 | 三套构建全量通过 |
| `S6-HEADER-REWRITER-API` / `S6-HEADER-REWRITER-IMPLEMENTATION` / `S6-HEADER-REWRITER-TESTS` | 6 | Header 改写头文件、实现和测试 | 隔离两侧连接 Header、重建可信 `X-Forwarded-*`、响应改为明确长度 | 为什么反代不能原样复制 Connection | 三套构建全量通过 |
| `S6-PROXY-ROUTING-API` / `S6-PROXY-ROUTING-STATE` | 6 | `proxy_session.h` | 共享路由表、客户端地址与当轮上游状态 | 固定上游模式和路由模式如何共存 | 旧阶段测试保持通过 |
| `S6-ROUTED-PROXY-INTEGRATION-TESTS` | 6 | `tests/proxy_integration_test.cpp` | 三节点轮询、可信转发头、404、503 和客户端 Keep-Alive | 错误码为何在连接上游前确定 | 3 项新增代理测试通过 |
| `S6-CONFIGURED-SERVICE-MAIN` / `S6-TEST-BACKEND` / `S6-TEST-BACKEND-BUILD` | 6 | 正式服务入口、测试后端和 CMake | `edgegate <yaml>` 与三个可识别后端 | 正式入口与阶段 Demo 的边界 | 真实进程链路通过 |
| `S6-EXAMPLE-CONFIG` / `S6-YAML-DEPENDENCY` | 6 | `config/edgegate.yaml`、`CMakeLists.txt` | 示例监听、容量、三节点池、精确/通配路由和 yaml-cpp 接入 | 配置字段是否真的生效 | Debug/Sanitizer/Release 94/94 |
| `S7-RELIABLE-PROXY-SERVER-API` / `S7-RELIABLE-PROXY-SERVER-IMPLEMENTATION` | 7 | `include/edgegate/proxy/reliable_proxy_server.h`、`src/proxy/reliable_proxy_server.cpp` | 双向流式会话、跨端背压、timerfd 超时、GET/HEAD 重试、主动/被动健康检查 | 两个方向何时暂停/恢复，失败能否再发错误响应 | 三构建 110/110 |
| `S7-CHUNKED-STREAM-DECODER-API` / `IMPLEMENTATION` / `TESTS` | 7 | chunked 流式校验器及测试 | 不缓存完整正文的 chunk size/data/Trailer 状态机和总量限制 | 原始 chunk framing 与解码长度的区别 | 4 项逐字节/错误测试通过 |
| `S7-STREAMING-HEADER-REWRITER-*` / `S7-STREAMING-RESPONSE-METADATA` | 7 | Header 改写与响应解析器元数据 | 只重建头部，保留 CL/chunked/close 三种流式边界 | 为什么不能继续调用整包重写函数 | 新代理集成测试通过 |
| `S7-RETRY-AND-HEALTH-ROUTING` / `S7-RETRY-AND-SHARED-HEALTH-ROUTE-TESTS` | 7 | 路由表及测试 | 排除已尝试节点、按物理地址共享健康状态、去重探测节点 | 重试为何不能再次选回失败节点 | 路由全量回归通过 |
| `S7-RELIABILITY-CONFIG-*` / `S7-RELIABILITY-EXAMPLE-CONFIG` / `TESTS` | 7 | 配置模型、YAML、示例和测试 | 水位、六类超时、健康周期/阈值/路径及关系校验 | 配置字段是否进入不可变运行快照 | 配置全量回归通过 |
| `S7-RELIABLE-PROXY-INTEGRATION-TESTS` / `S7-RELIABLE-PROXY-TEST-BUILD` | 7 | 新代理集成测试和 CMake | 4 MiB 下行背压、512 KiB 上传、重试、部分响应故障、chunked/关闭定界、504、健康恢复 | 指标是否证明机制真实发生 | 8 项专项集成测试通过 |
| `S7-FORMAL-SERVICE-SWITCH` | 7 | `apps/edgegate_service_main.cpp` | 正式入口切换到阶段7服务器，保留旧阶段对照实现 | 配置快照如何进入运行时 | 三后端 a/b/c/a/b/c |
| `S8-MANAGEMENT-AND-LOGGING-CONFIG-*` | 8 | 配置模型、YAML、示例和配置测试 | 管理 Socket、排空期限、日志目录/级别/轮转参数及完整校验 | 哪些字段可热更，哪些必须重启 | 三构建 117/117 |
| `S8-MANAGEMENT-SERVER-API` / `IMPLEMENTATION` | 8 | `include/edgegate/runtime/management_server.h`、`src/runtime/management_server.cpp` | 非阻塞 Unix Listener、0600权限、一行JSON命令/响应、短读写和安全清理 | 管理fd与代理fd如何共享EventLoop | 集成与真实进程通过 |
| `S8-EDGEGATECTL` / `S8-EDGEGATECTL-LINK` | 8 | `apps/edgegatectl_main.cpp`、`CMakeLists.txt` | 单次命令CLI、Unix连接、JSON协议、人类输出和0/1/2/3退出码 | 为什么不做常驻Shell | 真实七类命令通过 |
| `S8-RUNTIME-CONTROL-*` / `S8-SESSION-DRAIN-API` | 8 | `src/proxy/reliable_proxy_server.cpp` | running/draining/drained/stopping状态、查询、排空期限、幂等stop与管理分派 | Listener停止与Session完成的区别 | drain/超时/stop通过 |
| `S8-IN-FLIGHT-CONFIG-SNAPSHOT` / `S8-NEXT-REQUEST-TIMEOUT-SNAPSHOT` | 8 | `src/proxy/reliable_proxy_server.cpp` | reload时固定在途路由与超时，新请求读取新配置 | 原子切换不等于修改所有旧对象 | 合法/无效/不可变reload通过 |
| `S8-SIGNAL-CONTROL-*` / `S8-SERVICE-SIGNAL-SETUP` | 8 | signal_control头源与正式入口 | 屏蔽SIGTERM/SIGINT，以signalfd接入epoll并复用平滑停止 | 为什么异步信号处理器不做复杂C++工作 | 真实SIGTERM退出0 |
| `S8-RUNTIME-LOGGER-*` / `S8-ACCESS-LOG-*` | 8 | runtime_logger头源与可靠代理 | 8192条有界异步队列、三类轮转日志、访问/错误/管理记录和敏感字段边界 | 网络线程为何不等待磁盘 | 日志生成与轮转测试通过 |
| `S8-RUNTIME-MANAGEMENT-INTEGRATION-TESTS` / `S8-RUNTIME-TEST-BUILD` / `S8-RUNTIME-BUILD` | 8 | `tests/runtime_management_integration_test.cpp`、`CMakeLists.txt` | 运行时源码接入，以及Socket权限、查询、reload原子性、排空超时和轮转日志测试 | 结果与内部状态证据是否同时存在 | 7项新增测试；全量117/117 |
| `S8-CLI-PROCESS-ACCEPTANCE` | 8 | `tests/stage8_cli_acceptance.sh` | 三后端正式进程、CLI、curl、reload/drain/stop/SIGTERM可复现验收和清理 | 单元测试与真实进程验收的边界 | `STAGE8_CLI_ACCEPTANCE=PASS` |
| `S9-DASHBOARD-CONFIG-*` / `S9-DASHBOARD-EXAMPLE-CONFIG` | 9 | 配置模型、YAML、示例和配置测试 | 独立本机监听、刷新周期、最近错误与慢请求阈值/上限及范围校验 | 为什么监听参数需重启、统计参数可热更 | 三构建 125/125 |
| `S9-OBSERVABILITY-API` / `S9-OBSERVABILITY-IMPLEMENTATION` | 9 | `include/edgegate/runtime/observability.h`、`src/runtime/observability.cpp` | 状态码、路由、上游聚合，13桶延迟与近似分位数，有界错误/慢请求和双重查询参数脱敏 | 聚合指标与诊断样本为何分开保存 | 单元、集成和隐私验收通过 |
| `S9-DASHBOARD-SERVER-API` / `S9-DASHBOARD-SERVER-IMPLEMENTATION` | 9 | Dashboard 头源文件 | 非阻塞 TCP Listener、受控只读 HTTP、内嵌原生页面/CSS/JS、JSON 和安全响应头 | 独立端口为何不与代理路由混用 | GET/HEAD、404/405和真实页面资源通过 |
| `S9-OBSERVABILITY-RUNTIME-*` / `S9-DASHBOARD-SNAPSHOT` / `S9-COMPLETE-REQUEST-METRICS` | 9 | `src/proxy/reliable_proxy_server.cpp` | 请求真正发完后统一记账、路由/上游结果、最近错误、健康转换和版本化 Dashboard 快照 | 为什么不能在“上游刚返回”时算请求完成 | 真实200/404及上游聚合一致 |
| `S9-OBSERVABILITY-TESTS` / `S9-DASHBOARD-SERVER-TESTS` / `S9-DASHBOARD-PROXY-INTEGRATION-TEST` | 9 | 三份测试及 CMake | 分位桶、有界淘汰、隐私、HTTP只读行为和实时代理指标的自动验证 | 测试是否同时覆盖数字与接口边界 | 8项新增测试；全量125/125 |
| `S9-TEST-BACKEND-DELAY*` / `S9-DASHBOARD-PROCESS-ACCEPTANCE` | 9 | 测试后端与真实进程脚本 | 可控慢上游及页面、JSON、状态码、路由/上游、慢请求、脱敏、只读端到端验收 | 慢请求证据是否由真实耗时产生 | `STAGE9_DASHBOARD_ACCEPTANCE=PASS` |
| `S10-INSTALL-DIRECTORIES` / `S10-PRODUCT-INSTALL` | 10 | `CMakeLists.txt` | GNU 安装目录、二进制/演示后端/配置/systemd/文档/许可证安装规则和隔离安装 CTest | 为什么示例配置不直接覆盖 `/etc` | 三构建 `126/126` |
| `S10-PRODUCTION-CONFIG` | 10 | `config/edgegate.production.yaml` | `/run` Socket、`/var/log` 日志、本机监听和三后端的安装版安全起点 | 开发 `/tmp` 配置与正式目录的区别 | 隔离安装与真实服务通过 |
| `S10-SYSTEMD-UNIT` / `S10-SYSUSERS` / `S10-TMPFILES` | 10 | `packaging/` | 低权限用户、运行/日志目录、systemd 启停/reload/重启和文件系统/capability 加固 | 为什么服务不以 root 运行 | enabled/active，uid/gid 999 |
| `S10-SERVICE-INSTALLER` | 10 | `scripts/edgegate-service-install.in` | 创建用户和目录、仅首次复制配置、daemon-reload 和可选 enable-now | 重复安装为何不能覆盖生产配置 | `CONFIG_NO_OVERWRITE=PASS` |
| `S10-README` / `S10-USER-GUIDE` / `S10-ARCHITECTURE` / `S10-TROUBLESHOOTING` | 10 | README 和 `docs/` | 从干净构建到 systemd 展示的新手路径、配置/CLI、内部数据链和按现象排障 | 文档命令是否与真实安装一致 | 安装路径和实机命令已验证 |
| `S10-INSTALL-LAYOUT-TEST` / `S10-SYSTEMD-ACCEPTANCE` | 10 | 两份阶段 10 Shell 脚本 | DESTDIR 无 root 布局验收，以及真实权限、轮询、CLI、Dashboard、reload/restart/SIGTERM 验收 | `active` 与应用就绪为何不等价 | `STAGE10_SYSTEMD_ACCEPTANCE=PASS` |
| 无源码标记（标准 MIT 文本） | 10 | `LICENSE` | 按用户确认使用 `lkdr` 与 2026 的标准 MIT License | 发布前再核对版权主体 | 已纳入安装布局 |

## 贡献边界

- AI：实现、重构、测试设计、Linux 构建联调、故障分析和文档整理。
- 用户：确定项目方向和范围，提出关键质疑，选择方案，审阅思路和验证结果。
- 简历或面试表述必须说明这是 AI 辅助开发项目，不把 AI 直接实现虚构成用户独立手写。
