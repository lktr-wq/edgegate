#pragma once

// AI-CODE-BEGIN: S4-BYTE-BUFFER-API
#include <cstddef>
#include <string>
#include <string_view>

namespace edgegate::net {

/*
 * 保存“已经收到但还没处理完”或“还没发送完”的字节。
 * max_size 是硬上限，防止慢连接让进程无限占用内存。
 */
class ByteBuffer {
public:
    explicit ByteBuffer(std::size_t max_size);

    [[nodiscard]] bool append(std::string_view bytes);
    [[nodiscard]] bool consume(std::size_t count) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::string_view readable_view() const noexcept;
    [[nodiscard]] std::size_t readable_size() const noexcept;
    [[nodiscard]] std::size_t writable_capacity() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t max_size() const noexcept;

private:
    void compact();

    std::size_t max_size_;
    std::string storage_;
    std::size_t read_offset_{0};
};

} // namespace edgegate::net
// AI-CODE-END: S4-BYTE-BUFFER-API
