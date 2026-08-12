#最低 CMake 版本
cmake_minimum_required(VERSION 3.22)

#项目名：EdgeGate
#版本：0.1.0
#使用语言：C++
project(
    EdgeGate
    VERSION 0.1.0
    LANGUAGES CXX
)

#使用 C++17。
#编译器必须支持 C++17，不能自动降级。
#不使用编译器私有扩展，尽量保持标准 C++。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#只有 GCC 或 Clang 才加入这三个参数，避免以后使用 MSVC 时参数不兼容
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# 创建EdgeGate核心静态库。
# HTTP解析器等可复用代码放在该库中。
add_library(
    edgegate_core
    STATIC
    src/http/request_parser.cpp
)

# AI-CODE-BEGIN: S3-RESPONSE-PARSER-BUILD
# 阶段3新增响应解析器；它和请求解析器共同组成可复用的HTTP核心库。
target_sources(
    edgegate_core
    PRIVATE
    src/http/response_parser.cpp
)
# AI-CODE-END: S3-RESPONSE-PARSER-BUILD

# AI-CODE-BEGIN: S4-REACTOR-BUILD
# 阶段4网络基础：有界字节缓冲、LT epoll事件循环、连接和验收Echo服务。
target_sources(
    edgegate_core
    PRIVATE
    src/net/byte_buffer.cpp
    src/net/event_loop.cpp
    src/net/echo_connection.cpp
    src/net/reactor_echo_server.cpp
)
# AI-CODE-END: S4-REACTOR-BUILD

# AI-CODE-BEGIN: S5-PROXY-STATE-MACHINE-BUILD
target_sources(
    edgegate_core
    PRIVATE
    src/proxy/proxy_session.cpp
    src/proxy/proxy_server.cpp
)
# AI-CODE-END: S5-PROXY-STATE-MACHINE-BUILD

# AI-CODE-BEGIN: S6-ROUTE-TABLE-BUILD
target_sources(
    edgegate_core
    PRIVATE
    src/routing/route_table.cpp
)
# AI-CODE-END: S6-ROUTE-TABLE-BUILD

# PUBLIC表示：
# edgegate_core自己和链接它的目标都能找到include目录。
target_include_directories(
    edgegate_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

#创建程序目标名称 = edgegate 源文件 = apps/edgegate_main.cpp
add_executable(
    edgegate
    apps/edgegate_main.cpp
)

# 正式程序链接核心库。
target_link_libraries(
    edgegate
    PRIVATE
    edgegate_core
)

# 创建管理程序。
add_executable(
    edgegatectl
    apps/edgegatectl_main.cpp
)

# AI-CODE-BEGIN: S4-REACTOR-DEMO-BUILD
# 独立演示程序不替换阶段2代理，避免学习基线和Reactor验收相互混淆。
add_executable(
    edgegate_reactor_demo
    apps/reactor_demo_main.cpp
)
target_link_libraries(
    edgegate_reactor_demo
    PRIVATE
    edgegate_core
)
# AI-CODE-END: S4-REACTOR-DEMO-BUILD

# AI-CODE-BEGIN: S5-PROXY-DEMO-BUILD
add_executable(
    edgegate_proxy_demo
    apps/proxy_demo_main.cpp
)
target_link_libraries(
    edgegate_proxy_demo
    PRIVATE
    edgegate_core
)
# AI-CODE-END: S5-PROXY-DEMO-BUILD

#启用CTest
enable_testing()

# 保留阶段1的冒烟测试。
add_executable(
    edgegate_smoke
    tests/smoke_test.cpp
)

#注册测试,当执行 ctest 时，运行 edgegate_smoke。它返回 0 就判定通过，返回非 0 就判定失败
add_test(
    NAME edgegate_smoke
    COMMAND edgegate_smoke
)

# 查找Ubuntu中已经安装的GoogleTest。
find_package(GTest REQUIRED)

# 引入gtest_discover_tests()命令。
include(GoogleTest)

# 创建HTTP请求解析器测试程序。
add_executable(
    edgegate_http_parser_tests
    tests/http_request_parser_test.cpp
)

# AI-CODE-BEGIN: S3-RESPONSE-PARSER-TEST-BUILD
# 把响应解析测试编入同一个GoogleTest程序，由CTest统一发现。
target_sources(
    edgegate_http_parser_tests
    PRIVATE
    tests/http_response_parser_test.cpp
)
# AI-CODE-END: S3-RESPONSE-PARSER-TEST-BUILD

# 测试程序同时链接解析器实现和GoogleTest入口。
target_link_libraries(
    edgegate_http_parser_tests
    PRIVATE
    edgegate_core
    GTest::gtest_main
)

# 自动发现TEST(...)定义的测试用例并注册到CTest。
gtest_discover_tests(
    edgegate_http_parser_tests
)

# AI-CODE-BEGIN: S4-REACTOR-TEST-BUILD
find_package(Threads REQUIRED)

add_executable(
    edgegate_reactor_tests
    tests/byte_buffer_test.cpp
    tests/unique_fd_test.cpp
    tests/event_loop_test.cpp
    tests/reactor_echo_integration_test.cpp
)
target_link_libraries(
    edgegate_reactor_tests
    PRIVATE
    edgegate_core
    GTest::gtest_main
    Threads::Threads
)
gtest_discover_tests(
    edgegate_reactor_tests
)
# AI-CODE-END: S4-REACTOR-TEST-BUILD

# AI-CODE-BEGIN: S5-PROXY-TEST-BUILD
add_executable(
    edgegate_proxy_tests
    tests/proxy_integration_test.cpp
)
target_link_libraries(
    edgegate_proxy_tests
    PRIVATE
    edgegate_core
    GTest::gtest_main
    Threads::Threads
)
gtest_discover_tests(
    edgegate_proxy_tests
)
# AI-CODE-END: S5-PROXY-TEST-BUILD

# AI-CODE-BEGIN: S6-ROUTE-TABLE-TEST-BUILD
add_executable(
    edgegate_route_tests
    tests/route_table_test.cpp
)
target_link_libraries(
    edgegate_route_tests
    PRIVATE
    edgegate_core
    GTest::gtest_main
)
gtest_discover_tests(
    edgegate_route_tests
)
# AI-CODE-END: S6-ROUTE-TABLE-TEST-BUILD
