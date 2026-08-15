#include "edgegate/net/byte_buffer.h"

// AI-CODE-BEGIN: S4-BYTE-BUFFER-IMPLEMENTATION
#include <stdexcept>

namespace edgegate::net {

ByteBuffer::ByteBuffer(std::size_t max_size) : max_size_(max_size)
{
    if (max_size_ == 0) {
        throw std::invalid_argument("ByteBuffer max_size must be positive");//直接拒绝创建没有容量的缓冲区
    }
}

bool ByteBuffer::append(std::string_view bytes)
{
    if (bytes.size() > writable_capacity()) {//先判断加入后是否超过硬上限
        return false;
    }

    // 先回收已经 consume() 的前缀(使用compact方法)，避免底层字符串不断保留无效空间。
    if (read_offset_ != 0 && storage_.size() + bytes.size() > max_size_) {
        compact();//逻辑可写容量足够，但 storage_ 仍保留已消费前缀，直接追加会超过物理长度限制，因此先删除无效前缀
    }
    storage_.append(bytes.data(), bytes.size());
    return true;
}

bool ByteBuffer::consume(std::size_t count) noexcept
{
    if (count > readable_size()) {//要求消费的数量超过可读字节
        return false;
    }

    read_offset_ += count;//移动下标代表处理过了
    if (read_offset_ == storage_.size()) {
        clear();//全部消费完clear
    } else if (read_offset_ >= 4096 && read_offset_ * 2 >= storage_.size()) {
        compact();//已消费前缀至少 4096 字节，并且占 storage_ 至少一半(4096 是减少频繁 erase() 的工程折中)
    }
    //不执行if会保留已消费字节，避免频繁搬动字符串
    return true;
}

void ByteBuffer::clear() noexcept
{
    storage_.clear();
    read_offset_ = 0;
}

std::string_view ByteBuffer::readable_view() const noexcept
{   //返回的string_view需要给出起始地址storage_.data() + read_offset_和长度readable_size()
    return {storage_.data() + read_offset_, readable_size()};
}

std::size_t ByteBuffer::readable_size() const noexcept
{
    return storage_.size() - read_offset_;//代表还没有消费的字节数量
}

std::size_t ByteBuffer::writable_capacity() const noexcept
{
    return max_size_ - readable_size();//返回还能安全加入多少有效字节,不减 storage_.size()，因为已消费前缀属于可复用空间
}

bool ByteBuffer::empty() const noexcept
{
    return readable_size() == 0;//没有还没有消费的字节则为空
}

std::size_t ByteBuffer::max_size() const noexcept
{
    return max_size_;
}

void ByteBuffer::compact()
{
    storage_.erase(0, read_offset_);//真正删除已经消费的前缀，把未消费字节移动到字符串开头
    read_offset_ = 0;
}

} // namespace edgegate::net
// AI-CODE-END: S4-BYTE-BUFFER-IMPLEMENTATION
/*ByteBuffer 在调用链中的作用
recv() 得到但尚未处理
→ input_

send() 尚未接受
→ output_

成功处理或发送多少
→ consume() 多少
*/
