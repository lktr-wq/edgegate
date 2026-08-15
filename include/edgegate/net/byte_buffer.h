#pragma once

// AI-CODE-BEGIN: S4-BYTE-BUFFER-API
#include <cstddef>
#include <string>
#include <string_view>

/*
不能只使用系统自带的 Socket 缓冲区。内核缓冲区和 ByteBuffer 负责的是两层不同工作

接收时：网卡→ 内核 Socket 接收缓冲区→ recv()→ 应用层 ByteBuffer/HTTP 解析器

接收时为什么需要应用缓冲区？
客户端发送：
GET / HTTP/1.1\r\n
Host: example.com\r\n
\r\n

而第一次 recv() 只取得：
GET / HTTP/1.1\r\n
Hos

调用 recv() 后，这些字节已经从内核接收缓冲区取出来了。内核不会替应用程序记住：
这是半条 HTTP 请求。
已经解析到请求行还是 Header。
下一批数据应该与哪批旧数据拼接。
Header 或 Body 是否超过项目限制。
因此应用程序必须保存这批未解析完成的字节，等待下一次 recv()。

理论上可以一直不调用 recv()，把数据留在内核中，但这样也无法正常解析内容；内核缓冲区满后还会阻止对端继续发送。使用 MSG_PEEK 反复偷看也会造成重复扫描和复制，不适合作为正常解析方案。

发送时：应用层 ByteBuffer→ send()→ 内核 Socket 发送缓冲区→ 网卡
发送时更无法只靠内核缓冲区

假设应用程序准备发送：
abcdefgh
但内核发送缓冲区只剩 3 字节空间：
ssize_t sent = send(fd, data, 8, 0);
可能返回：
sent == 3

此时：
内核已经接管：abc
内核完全不知道：defgh
没有被 send() 接受的 defgh 仍然只能由应用程序保存

必须：
ByteBuffer 保存 "defgh"
→ 等 epoll 再次报告可写
→ 再调用 send()
→ 成功发送多少，就 consume() 多少
*/
namespace edgegate::net {

/*
 * 保存“已经收到但还没处理完”或“还没发送完”的字节。
 * max_size 是硬上限，防止慢连接让进程无限占用内存。
 */
// ByteBuffer 同时可以作为：
// 输入缓冲区：已经 recv()，但还没有处理完
// 输出缓冲区：准备 send()，但还没有发送完
class ByteBuffer {
public:
    explicit ByteBuffer(std::size_t max_size);//防止整数被偷偷转换成缓冲区
    //ByteBuffer buffer = 8;  禁止
    //ByteBuffer buffer(8);   允许

    //修改缓冲区
    [[nodiscard]] bool append(std::string_view bytes);//向末尾加入字节。返回 false 表示容量不足，它不会破坏原有数据
    [[nodiscard]] bool consume(std::size_t count) noexcept;//标记前 count 个可读字节已经处理,返回 false 表示想消费的数量超过现有可读字节
    void clear() noexcept;//清空全部可读数据

    //查询状态
    [[nodiscard]] std::string_view readable_view() const noexcept;// 当前还没消费的字节
    [[nodiscard]] std::size_t readable_size() const noexcept;// 未消费字节数量
    [[nodiscard]] std::size_t writable_capacity() const noexcept;// 还能加入多少字节
    [[nodiscard]] bool empty() const noexcept;// 是否没有可读字节
    [[nodiscard]] std::size_t max_size() const noexcept;// 配置的硬上限

private:
    void compact();//满足特定条件时已消费前缀

    std::size_t max_size_;//限制单连接允许保存的最大有效字节数
    std::string storage_;//保存物理字节
    std::size_t read_offset_{0};//标记可读数据从哪里开始
};

} // namespace edgegate::net
// AI-CODE-END: S4-BYTE-BUFFER-API
