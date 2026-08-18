#最低 CMake 版本
cmake_minimum_required(VERSION 3.22)

#项目名：EdgeGate
#版本：1.0.0
#使用语言：C++
project(
    EdgeGate
    VERSION 1.0.0
    LANGUAGES CXX
)

# AI-CODE-BEGIN: S10-INSTALL-DIRECTORIES
# GNUInstallDirs 统一 Linux 安装目录名：默认前缀 /usr/local 下，
# 程序进入 bin，服务资产进入 lib，文档和示例进入 share。
include(GNUInstallDirs)
# AI-CODE-END: S10-INSTALL-DIRECTORIES

# AI-CODE-BEGIN: S6-YAML-DEPENDENCY
# yaml-cpp 只负责 YAML 语法树；字段、范围和引用关系仍由 EdgeGate 校验。
find_package(yaml-cpp REQUIRED)
# AI-CODE-BEGIN: S8-RUNTIME-DEPENDENCIES
# JSON 用于本机管理协议，spdlog 用于有界异步轮转日志。
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
# AI-CODE-END: S8-RUNTIME-DEPENDENCIES
# AI-CODE-END: S6-YAML-DEPENDENCY

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
    # 阶段7对 chunked 正文逐段校验，不再为了判断终点缓存完整正文。
    src/http/chunked_stream_decoder.cpp
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
    # 新服务器承载阶段7流式转发；旧 ProxyServer 保留供阶段5/6对照。
    src/proxy/reliable_proxy_server.cpp
)
# AI-CODE-END: S5-PROXY-STATE-MACHINE-BUILD

# AI-CODE-BEGIN: S6-ROUTE-TABLE-BUILD
target_sources(
    edgegate_core
    PRIVATE
    src/config/edgegate_config.cpp
    src/http/message_rewriter.cpp
    src/routing/route_table.cpp
    # AI-CODE-BEGIN: S8-RUNTIME-BUILD
    src/runtime/runtime_logger.cpp
    src/runtime/management_server.cpp
    src/runtime/signal_control.cpp
    # AI-CODE-END: S8-RUNTIME-BUILD
    # AI-CODE-BEGIN: S9-OBSERVABILITY-BUILD
    src/runtime/observability.cpp
    src/runtime/dashboard_server.cpp
    # AI-CODE-END: S9-OBSERVABILITY-BUILD
)
# AI-CODE-END: S6-ROUTE-TABLE-BUILD

# PUBLIC表示：
# edgegate_core自己和链接它的目标都能找到include目录。
target_include_directories(
    edgegate_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    edgegate_core
    PUBLIC
    yaml-cpp
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# 正式 edgegate 已在阶段6切换为读取 YAML 的非阻塞代理入口。
add_executable(
    edgegate
    apps/edgegate_service_main.cpp
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

# AI-CODE-BEGIN: S8-EDGEGATECTL-LINK
target_link_libraries(edgegatectl PRIVATE nlohmann_json::nlohmann_json)
# AI-CODE-END: S8-EDGEGATECTL-LINK

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

# AI-CODE-BEGIN: S6-TEST-BACKEND-BUILD
add_executable(
    edgegate_test_backend
    apps/test_backend_main.cpp
)
# AI-CODE-END: S6-TEST-BACKEND-BUILD

# AI-CODE-BEGIN: S11-LOAD-GENERATOR-BUILD
# 阶段11压测器站在客户端视角逐条校验响应，并输出精确延迟分位数。
# 它是验收工具，不安装为产品命令。
add_executable(
    edgegate_loadgen
    apps/loadgen_main.cpp
)
target_link_libraries(
    edgegate_loadgen
    PRIVATE
    nlohmann_json::nlohmann_json
)
# AI-CODE-END: S11-LOAD-GENERATOR-BUILD

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
    tests/chunked_stream_decoder_test.cpp
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

# AI-CODE-BEGIN: S11-LOAD-GENERATOR-THREADS
# 每个并发客户端由一个阻塞线程模拟；EdgeGate 服务本身仍是单 Reactor。
target_link_libraries(edgegate_loadgen PRIVATE Threads::Threads)
# AI-CODE-END: S11-LOAD-GENERATOR-THREADS

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

# AI-CODE-BEGIN: S7-RELIABLE-PROXY-TEST-BUILD
add_executable(
    edgegate_reliable_proxy_tests
    tests/reliable_proxy_integration_test.cpp
    # AI-CODE-BEGIN: S8-RUNTIME-TEST-BUILD
    tests/runtime_management_integration_test.cpp
    # AI-CODE-END: S8-RUNTIME-TEST-BUILD
    # AI-CODE-BEGIN: S9-OBSERVABILITY-TEST-BUILD
    tests/observability_test.cpp
    tests/dashboard_server_test.cpp
    # AI-CODE-END: S9-OBSERVABILITY-TEST-BUILD
)
target_link_libraries(
    edgegate_reliable_proxy_tests
    PRIVATE
    edgegate_core
    GTest::gtest_main
    Threads::Threads
)
gtest_discover_tests(edgegate_reliable_proxy_tests)
# AI-CODE-END: S7-RELIABLE-PROXY-TEST-BUILD

# AI-CODE-BEGIN: S6-ROUTE-TABLE-TEST-BUILD
add_executable(
    edgegate_route_tests
    tests/route_table_test.cpp
    tests/edgegate_config_test.cpp
    tests/message_rewriter_test.cpp
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

# AI-CODE-BEGIN: S10-PRODUCT-INSTALL
# systemd 单元和管理脚本需要写入实际安装路径，因此先由 CMake
# 生成最终文件。更改 CMAKE_INSTALL_PREFIX 时，程序路径会同步更新。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging")
configure_file(
    packaging/systemd/edgegate.service.in
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/edgegate.service"
    @ONLY
)
configure_file(
    scripts/edgegate-service-install.in
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/edgegate-service-install"
    @ONLY
)

# 正式对外命令只有代理服务和管理 CLI。
install(
    TARGETS edgegate edgegatectl
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# 测试后端用于 README 的可重复演示，放到 libexec 而不伪装成产品命令。
install(
    TARGETS edgegate_test_backend
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/edgegate"
)

# 安装只读示例；真正的 /etc/edgegate/edgegate.yaml 由管理脚本
# “不存在时才创建”，避免升级覆盖用户配置。
install(
    FILES config/edgegate.production.yaml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/edgegate/examples"
    RENAME edgegate.yaml
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/edgegate.service"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
)
install(
    FILES packaging/sysusers/edgegate.conf
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/sysusers.d"
)
install(
    FILES packaging/tmpfiles/edgegate.conf
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/tmpfiles.d"
)
install(
    PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/packaging/edgegate-service-install"
    DESTINATION "${CMAKE_INSTALL_SBINDIR}"
)

install(
    FILES README.md
        RELEASE_NOTES.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
install(
    FILES
        docs/user-guide.md
        docs/architecture.md
        docs/troubleshooting.md
        docs/stage11-report.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
)
install(
    FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/edgegate"
)

# 这个 CTest 将安装到临时根目录，验证布局而不修改真实系统。
find_program(EDGEGATE_BASH_EXECUTABLE bash REQUIRED)
add_test(
    NAME stage10_install_layout
    COMMAND
        "${EDGEGATE_BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stage10_install_layout_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
)
# AI-CODE-END: S10-PRODUCT-INSTALL

# AI-CODE-BEGIN: S12-RELEASE-METADATA-TEST
# 发布门禁检查版本、文档、可移植路径和仓库快照卫生。
# 它不替代 C++ 单元/集成测试，而是阻止“代码通过但发布包信息错误”。
add_test(
    NAME stage12_release_metadata
    COMMAND
        "${EDGEGATE_BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stage12_release_acceptance.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
)
# AI-CODE-END: S12-RELEASE-METADATA-TEST
