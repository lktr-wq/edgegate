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

## 贡献边界

- AI：实现、重构、测试设计、Linux 构建联调、故障分析和文档整理。
- 用户：确定项目方向和范围，提出关键质疑，选择方案，审阅思路和验证结果。
- 简历或面试表述必须说明这是 AI 辅助开发项目，不把 AI 直接实现虚构成用户独立手写。
