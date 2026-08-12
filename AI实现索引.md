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

## 贡献边界

- AI：实现、重构、测试设计、Linux 构建联调、故障分析和文档整理。
- 用户：确定项目方向和范围，提出关键质疑，选择方案，审阅思路和验证结果。
- 简历或面试表述必须说明这是 AI 辅助开发项目，不把 AI 直接实现虚构成用户独立手写。
