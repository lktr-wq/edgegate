#pragma once

// AI-CODE-BEGIN: S4-UNIQUE-FD
#include <unistd.h>

namespace edgegate::net {

/*
 * 对象独占拥有一个 Linux 文件描述符。
 * 销毁时自动 close()，因此函数中途 return 或抛出异常也不会遗忘
 * 它不可复制，但可以移动，保证同一 fd 始终只有一个所有者。
 */
class UniqueFd {
public:
    UniqueFd() noexcept = default;//要求编译器自动生成这个无参构造函数
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}//explicit禁止隐式类型转换 带参构造函数
    /*有参构造函数的作用
    输入：一个裸 fd。
    处理：把 fd 保存到成员。
    输出：由当前对象独占管理这个 fd。
    */
    ~UniqueFd()
    {
        reset();//析构时重新设置该对象拥有的fd,由于无参传入默认值-1代表析构后不再接管fd，reset函数如有参数会接管新fd
    }

    UniqueFd(const UniqueFd&) = delete;//删除复制构造函数，禁止UniqueFd second(first);方法赋值
    UniqueFd& operator=(const UniqueFd&) = delete;//删除复制赋值运算符，禁止second = first方法赋值

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}//允许移动构造

    UniqueFd& operator=(UniqueFd&& other) noexcept//UniqueFd&&：右值引用，用来接收将被移动的对象
    {//other：原所有者
        if (this != &other) {
            reset(other.release());
        }
        return *this;
        /*
        other.release() 交出新 fd
        → reset() 关闭当前对象的旧 fd
        → 当前对象接管新 fd
        */
    }

    // [[nodiscard]]：提醒调用者不要忽略返回值；忽略时编译器通常产生警告
    [[nodiscard]] int get() const noexcept//只查看 fd，不转移所有权
    {
        return fd_;
    }

    [[nodiscard]] bool valid() const noexcept//判断当前是否拥有有效 fd
    {
        return fd_ >= 0;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    int release() noexcept//交出 fd，并把自己改成 -1。调用者从此必须负责关闭返回的 fd
    {
        const int released = fd_;
        fd_ = -1;
        return released;
    }

    //owned.reset();       // 关闭旧 fd，变成无资源状态
    //owned.reset(new_fd); // 关闭旧 fd，接管 new_fd
    void reset(int replacement = -1) noexcept
    {
        if (fd_ >= 0) {
            /*
             * Linux 下 close() 被 EINTR 打断时也不能盲目重试：fd 号码可能
             * 已被别处复用，重试反而可能关闭一个无关的新 fd。
             */
            static_cast<void>(::close(fd_));//::close()表示不调用类内成员函数，直接使用libc的系统调用封装的close，防止本类名字冲突
        }
        fd_ = replacement;
    }

private:
    int fd_{-1};//-1 表示“当前没有有效 fd”
};

} // namespace edgegate::net
// AI-CODE-END: S4-UNIQUE-FD
/*
UniqueFd 在调用链上所起的作用：
EventLoop 销毁 EventHandler
→ EchoConnection 析构
→ socket_ 析构
→ UniqueFd 自动 close(client_fd)
*/