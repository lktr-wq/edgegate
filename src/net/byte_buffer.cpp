#include "edgegate/net/byte_buffer.h"

// AI-CODE-BEGIN: S4-BYTE-BUFFER-IMPLEMENTATION
#include <stdexcept>

namespace edgegate::net {

ByteBuffer::ByteBuffer(std::size_t max_size) : max_size_(max_size)
{
    if (max_size_ == 0) {
        throw std::invalid_argument("ByteBuffer max_size must be positive");
    }
}

bool ByteBuffer::append(std::string_view bytes)
{
    if (bytes.size() > writable_capacity()) {
        return false;
    }

    // 先回收已经 consume() 的前缀，避免底层字符串不断保留无效空间。
    if (read_offset_ != 0 && storage_.size() + bytes.size() > max_size_) {
        compact();
    }

    storage_.append(bytes.data(), bytes.size());
    return true;
}

bool ByteBuffer::consume(std::size_t count) noexcept
{
    if (count > readable_size()) {
        return false;
    }

    read_offset_ += count;
    if (read_offset_ == storage_.size()) {
        clear();
    } else if (read_offset_ >= 4096 && read_offset_ * 2 >= storage_.size()) {
        compact();
    }
    return true;
}

void ByteBuffer::clear() noexcept
{
    storage_.clear();
    read_offset_ = 0;
}

std::string_view ByteBuffer::readable_view() const noexcept
{
    return {storage_.data() + read_offset_, readable_size()};
}

std::size_t ByteBuffer::readable_size() const noexcept
{
    return storage_.size() - read_offset_;
}

std::size_t ByteBuffer::writable_capacity() const noexcept
{
    return max_size_ - readable_size();
}

bool ByteBuffer::empty() const noexcept
{
    return readable_size() == 0;
}

std::size_t ByteBuffer::max_size() const noexcept
{
    return max_size_;
}

void ByteBuffer::compact()
{
    storage_.erase(0, read_offset_);
    read_offset_ = 0;
}

} // namespace edgegate::net
// AI-CODE-END: S4-BYTE-BUFFER-IMPLEMENTATION
