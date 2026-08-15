#include "edgegate/net/event_loop.h"

// AI-CODE-BEGIN: S4-EVENT-LOOP-IMPLEMENTATION
#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include <sys/epoll.h>

namespace edgegate::net {

namespace {
/*整个文件只做这件事
add()
把 fd 注册给内核，并保存 fd → EventHandler 映射

run_once()
调用 epoll_wait() 等待一批就绪 fd

epoll_wait() 返回
根据 fd 找到 EventHandler

调用 on_event()
由具体子类执行 accept()/recv()/send()

remove()
取消注册并安全销毁处理对象
*/
constexpr int kMaximumEventsPerWait = 64;//一次 epoll_wait() 最多取回 64 个事件

std::system_error system_error_from_errno(const char* operation)
{   //三部分组合成 C++ 异常,仅返回的是异常对象，还没有抛出
    return {errno, std::generic_category(), operation};
}

} // namespace

EventLoop::EventLoop() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC))//构造函数的成员初始化列表，初始化后epoll_fd_ 这个 UniqueFd 接管它
{//epoll_fd_不代表网络连接他只是edgegate操作内核epoll对象的句柄，EPOLL_CLOEXEC表示 EdgeGate 以后执行 exec() 启动其他程序时，不把这个 fd 泄漏给新程序
    if (!epoll_fd_) {
        throw system_error_from_errno("epoll_create1");
    }
}

void EventLoop::add(std::unique_ptr<EventHandler> handler)
{
    if (!handler || handler->fd() < 0) {//拒绝空 unique_ptr和管理无效 fd 的对象
        throw std::invalid_argument("EventLoop::add requires a valid handler");
    }

    const int registered_fd = handler->fd();//先保存 fd，因为后面会把 handler 移走
    if (handlers_.find(registered_fd) != handlers_.end()) {//查找这个fd在handlers池子内是否存在，如果该fd以有对应处理器则不等号成立，拒绝重复添加
        throw std::invalid_argument("EventLoop::add received duplicate fd");
    }
    /*结构原型
    struct epoll_event{
    uint32_t events;
    epoll_data_t data;
    };
    */
    epoll_event event{};//构造事件结构体epoll_event
    event.events = handler->interests();//保存该事件的关注
    event.data.fd = registered_fd;//内核中该事件会保存fd，但不增加fd引用计数
    if (::epoll_ctl(//调用 epoll_ctl()
            epoll_fd_.get(),//操作哪个 epoll 实例
            EPOLL_CTL_ADD,//本次要添加
            registered_fd,//添加哪个 Socket fd
            &event) == -1) {//关注哪些事件
        throw system_error_from_errno("epoll_ctl ADD");
    }//这里不抛异常则说明该事件已成功注册到epoll中

    try {
        handlers_.emplace(registered_fd, std::move(handler));//使handlers_ 独占拥有 handler
    } catch (...) {
        static_cast<void>(::epoll_ctl(//捕获任何异常
            epoll_fd_.get(), EPOLL_CTL_DEL, registered_fd, nullptr));//把刚注册进入epoll的fd删掉，不然会导致handlers里面没有fd对应的handle，但是epoll有对应fd，map 插入失败时撤销刚完成的 epoll 注册，使“内核已注册”和“程序中存在处理对象”保持一致，再继续抛出原异常
        throw;
    }
}

bool EventLoop::modify(int fd, std::uint32_t interests) noexcept//修改某个已注册 fd 关注的事件
{
    if (handlers_.find(fd) == handlers_.end() ||//fd 不存在
        pending_removals_.find(fd) != pending_removals_.end()) {//或者fd 已经等待删除
        return false;
    }
    //合法时重新准备
    epoll_event event{};
    event.events = interests;
    event.data.fd = fd;
    return ::epoll_ctl(
               epoll_fd_.get(), EPOLL_CTL_MOD, fd, &event) == 0;
}

bool EventLoop::remove(int fd) noexcept
{
    const auto found = handlers_.find(fd);//先查找该fd
    if (found == handlers_.end()) {
        return false;
    }

    static_cast<void>(//让内核不再监控这个 fd
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr));

    if (dispatching_) {
        /*
         * 当前回调可能正在执行该对象的成员函数，不能立刻 delete。
         * 保留对象到本轮事件全部分派完，也使 fd 在此期间不会被复用。
         */
        pending_removals_.insert(fd);//不能立即从 handlers_ 删除对象
    } else {
        handlers_.erase(found);
    }
    return true;
}

int EventLoop::run_once(int timeout_ms)
{
    std::array<epoll_event, kMaximumEventsPerWait> events{};//建立一个固定大小的数组，供内核写入就绪事件
    int ready = -1;//保存 epoll_wait() 返回的事件数量

    do {
        ready = ::epoll_wait(//调用 epoll_wait(),返回值>0为就绪事件数量，=0等待超时，<0系统调用失败
            epoll_fd_.get(),//等待哪个 epoll 实例
            events.data(),//内核把结果写到哪里
            static_cast<int>(events.size()),//数组最多接收几个事件
            timeout_ms);//最长等待多久
    } while (ready == -1 && errno == EINTR);

    if (ready == -1) {
        throw system_error_from_errno("epoll_wait");
    }
    //开始分派
    dispatching_ = true;//设置dispatching_ = true代表正在调用一批对象的 on_event()；现在可能正在执行某个处理器的成员函数，不能立即销毁对象
    for (int index = 0; index < ready; ++index) {//遍历就绪事件
        const int ready_fd = events[static_cast<std::size_t>(index)].data.fd;//取得内核带回来的 fd
        const auto found = handlers_.find(ready_fd);//查找对象
        if (found == handlers_.end() ||
            pending_removals_.find(ready_fd) != pending_removals_.end()) {
            continue;//对象不存在或已经等待删除就跳过
        }

        found->second->on_event(
            *this,
            events[static_cast<std::size_t>(index)].events);
            /*
            found->second          map 中的 unique_ptr<EventHandler>
            ->on_event             调用实际子类重写的虚函数
            *this                  当前 EventLoop 对象本身，以引用传入
            events[index].events   本次实际发生的事件
            */
    }
    dispatching_ = false;//跳出循环后不再执行某个处理器的成员函数，可以删除
    finish_pending_removals();//现在所有 on_event() 都已经返回，可以真正删除待删除对象
    return ready;//内核返回的就绪事件数，不一定等于真正调用 on_event() 的次数，因为某些事件可能因对象已待删除而被跳过
}

void EventLoop::run()
{
    stop_requested_ = false;
    while (!stop_requested_) {
        static_cast<void>(run_once(-1));
    }
}

void EventLoop::stop() noexcept
{
    stop_requested_ = true;
}

bool EventLoop::contains(int fd) const noexcept
{
    return handlers_.find(fd) != handlers_.end() &&//handlers_ 中存在并且不在 pending_removals_ 中在被称为包含
           pending_removals_.find(fd) == pending_removals_.end();
}

std::size_t EventLoop::handler_count() const noexcept
{
    return handlers_.size() - pending_removals_.size();//待删除对象虽然暂时还活着，但逻辑上已不属于有效处理器
}

void EventLoop::finish_pending_removals() noexcept
{
    for (int fd : pending_removals_) {//依次取出 pending_removals_ 中每个 fd
        handlers_.erase(fd);//从 map 删除对象
    }
    pending_removals_.clear();//清空待删除集合
}

} // namespace edgegate::net
// AI-CODE-END: S4-EVENT-LOOP-IMPLEMENTATION
