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

#创建程序目标名称 = edgegate 源文件 = apps/edgegate_main.cpp
add_executable(
    edgegate
    apps/edgegate_main.cpp
)


add_executable(
    edgegatectl
    apps/edgegatectl_main.cpp
)

#启用测试
enable_testing()

add_executable(
    edgegate_smoke
    tests/smoke_test.cpp
)

#注册测试,当执行 ctest 时，运行 edgegate_smoke。它返回 0 就判定通过，返回非 0 就判定失败
add_test(
    NAME edgegate_smoke
    COMMAND edgegate_smoke
)