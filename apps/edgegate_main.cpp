#include <iostream>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <string>
#include <cstdio>

//Linux/POSIX 头文件，并不存在于 Windows 开发环境
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    constexpr int kListenPort =18080;// 18080：curl 连接 EdgeGate
    constexpr int kUpstreamPort =19080;//19080：EdgeGate 连接测试上游
    constexpr int kListenBacklog = 8;// 已完成 TCP 连接、但还没有被 accept() 取走的连接队列上限提示值。
    constexpr std::size_t kReadChunkSize =1024;// 每次调用 recv() 时，最多尝试读取 1024 字节。
    constexpr std::size_t kMaxHeaderSize =8192;// 当前阶段允许接收的最大 HTTP Header 大小。防止客户端一直发送数据，导致 request 无限增长。
    /*
    * 创建一个阻塞式 TCP 监听 Socket。
    *
    * 成功：返回监听 Socket 的文件描述符。
    * 失败：关闭已经创建的 Socket，然后返回 -1。
    */
    int create_listener()
    {
        const int listen_fd=socket(AF_INET,SOCK_STREAM,0);
        if(listen_fd==-1){
            return -1;
        }
        /*
        * 允许程序退出并重新启动后，较快地重新绑定同一个端口。
        * 否则测试过程中可能遇到“Address already in use”。
        */
        const int reuse_address=1;
        if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse_address,sizeof(reuse_address))==-1){
            close(listen_fd);
            return -1;
        }
        sockaddr_in listen_address{};// 准备要绑定的 IPv4 地址和端口。
        listen_address.sin_family=AF_INET;
        /*
        * 设置监听端口。
        * htons() 把本机使用的整数格式转换成网络传输规定的字节顺序。
        */
        listen_address.sin_port=htons(static_cast<std::uint16_t>(kListenPort));
        /*
        * INADDR_LOOPBACK 表示 127.0.0.1。
        * 当前学习阶段只允许 Ubuntu 本机访问 EdgeGate。
        */
        listen_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        /*
        * 把 Socket 绑定到 127.0.0.1:18080。
        *
        * bind() 接受通用的 sockaddr 指针，
        * 所以需要把 sockaddr_in 指针转换成 sockaddr 指针。
        */
        if(bind(listen_fd,reinterpret_cast<const sockaddr*>(&listen_address),sizeof(listen_address))==-1){
            close(listen_fd);
            return -1;
        }
        if(listen(listen_fd,kListenBacklog)==-1){//把 Socket 从普通 Socket 转换为监听 Socket
            close(listen_fd);
            return -1;
        }
        return listen_fd;
    }
    /*
    * 创建一个阻塞式 TCP Socket，并连接测试上游。
    *
    * 连接目标：127.0.0.1:19080。
    *
    * 成功：返回已经连接上游的 Socket fd。
    * 失败：关闭已经创建的 Socket，保留 errno，然后返回 -1。
    */
    int create_upstream_connection(){
        const int upstream_fd=socket(AF_INET,SOCK_STREAM,0);
        if(upstream_fd==-1){
            return -1;
        }
        sockaddr_in upstream_address{};// 准备测试上游的 IPv4 地址。
        upstream_address.sin_family=AF_INET;
        upstream_address.sin_port=htons(static_cast<std::uint16_t>(kUpstreamPort));// 设置测试上游端口 19080。
        upstream_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); // 设置测试上游地址 127.0.0.1。
        /*
        * connect() 在当前阻塞 Socket 中会一直等待，
        * 直到 TCP 连接成功或者确认连接失败。
        *
        * 成功返回 0，失败返回 -1 并设置 errno。
        */
       if(connect(
                upstream_fd,
                reinterpret_cast<const sockaddr*>(&upstream_address),
                sizeof(upstream_address))==-1
        ){
         /*
         * connect() 失败时，errno 保存失败原因。
         *
         * 由于接下来还要调用 close()，
         * 所以先保存 errno，避免错误原因被后续操作改变。
         */
        const int connect_error=errno;
        close(upstream_fd);
        errno=connect_error; // 恢复 connect() 的原始错误原因，交给调用者输出。
        return -1;
        }
        return upstream_fd;
    }
    /*
    * 把 length 个字节完整发送到指定 Socket。
    *
    * 参数：
    * fd     ：接收数据的 Socket。
    * data   ：待发送数据的起始地址。
    * length ：需要发送的总字节数。
    *
    * 成功：返回 true。
    * 失败：输出具体错误并返回 false。
    */
    bool send_all(int fd,const char* data,std::size_t length){
        std::size_t total_sent=0; // 记录目前已经成功发送了多少字节。
        while(total_sent<length){// 只要还没有发送完，就继续调用 send()。
            ssize_t sent =-1;
            do{
             /*
             * data + total_sent：
             * 从“尚未发送部分”的第一个字节开始发送。
             *
             * length - total_sent：
             * 本次最多发送剩余的字节数。
             *
             * MSG_NOSIGNAL：
             * 如果上游已经关闭连接，不让 SIGPIPE 信号直接终止进程，
             * 而是让 send() 返回 -1，并通过 errno 报告错误。
             */
            sent=send(fd,data+total_sent,length-total_sent,MSG_NOSIGNAL);
            }while(sent==-1&&errno==EINTR);
            /*
            * send() 返回 -1，表示 Linux 发送操作失败。
            * errno 中保存具体原因，所以这里使用 perror()。
            */
           if(sent==-1){
            std::perror("send");
            return false;
           }
           /*
            * 当前仍有数据要发送，但 send() 没有发送任何字节。
            * 这是程序根据返回值判断出的异常情况，不读取 errno。
            */
           if(sent==0){
            std::cerr<<"send() returned 0 before all bytes were sent\n";
            return false;
           }
           total_sent+=static_cast<std::size_t>(sent);
        }
        return true;
    }
    /*
    * 持续读取上游响应，并把每次读到的字节完整发送给客户端。
    *
    * 当前阶段使用“关闭定界”：
    * 上游关闭连接、recv() 返回 0 时，认为响应传输结束。
    *
    * 成功：返回 true。
    * 失败：输出错误原因并返回 false。
    */
    bool relay_upstream_response(int upstream_fd,int client_fd){
        /*
        * response_buffer 只保存某一次 recv() 读到的响应数据。
        * 响应可能被 TCP 拆成多次到达，因此需要循环读取。
        */
       std::array<char,kReadChunkSize> response_buffer{};
       std::size_t total_forwarded =0;// 记录本次一共向客户端转发了多少响应字节。
       for(;;){
            ssize_t received =-1;
            /*
            * 从上游连接读取一部分响应。
            *
            * received > 0：本次收到的真实字节数。
            * received == 0：上游正常关闭连接，响应结束。
            * received == -1：recv() 调用失败。
            */
            do{
                received=recv(upstream_fd,response_buffer.data(),response_buffer.size(),0);
            }while(received==-1&&errno==EINTR);
             /*
            * Linux recv() 返回 -1，并通过 errno 保存错误原因，
            * 所以这里使用 perror()。
            */
           if(received==-1){
            std::perror("recv upstream");
            return false;
           }
           /*
            * recv() 返回 0 不是系统调用失败，
            * 而是上游正常关闭了这条 TCP 连接。
            *
            * 当前阶段把这个事件当作响应结束标志。
            */
           if(received==0){
            break;
           }
           const std::size_t received_size=static_cast<std::size_t>(received);
            /*
            * 把本次从上游收到的字节完整发送给客户端。
            *
            * 这里不能直接调用一次 send()，
            * 因为向客户端发送时同样可能发生短写。
            */
           if(!send_all(client_fd,response_buffer.data(),received_size)){
            return false;
           }
           total_forwarded+=received_size;// 只累计本次真正读取并成功转发的字节数。
       }
        /*
        * 只有recv()返回0、整个响应读取结束后，
        * 才输出总转发量并返回成功。
        */
        std::cout<<"Forwarded "<<total_forwarded<<" response bytes to client\n";
        return true;
    }
}

int main()
{
    const int listen_fd=create_listener();//创建监听 Socket，准备接收 curl 发起的 TCP 连接。
    if(listen_fd==-1){
        std::perror("create_listener");
        return 1;
    }
    std::cout << "EdgeGate listening on 127.0.0.1:" << kListenPort <<std::endl;
    /*
     * accept() 会阻塞在这里。
     * 在客户端连接到来之前，当前线程不会继续向下执行。
     */
    int client_fd=-1;
    for(;;){
        client_fd=accept(listen_fd,nullptr,nullptr);
        /*
        * EINTR 表示 accept() 被信号临时打断。
        * 监听 Socket 本身没有出错，因此重新调用 accept()。
        */
        if(client_fd==-1&&errno==EINTR){
                continue;
        }
        break;
    }
    if(client_fd==-1){
        std::perror("accept");
        close(listen_fd);
        return 1;
    }
    /*
     * buffer 只保存某一次 recv() 收到的数据。
     * request 保存多次 recv() 拼接后的完整 HTTP Header。
     */
    std::array<char,kReadChunkSize> buffer{};
    std::string request;
    /*
     * HTTP Header 以连续四个字符 \r\n\r\n 结束。
     *
     * 因为一次 recv() 不保证收到完整 Header，
     * 所以需要循环接收，直到找到结束标志。
     */
    while(request.find("\r\n\r\n")==std::string::npos){
        /*
         * 如果已经收满 8192 字节还没发现结束标志，
         * 就拒绝继续接收，避免内存无限增长。
         */
        if(request.size()>=kMaxHeaderSize){
            std::cerr << "HTTP Header exceeds "<<kMaxHeaderSize<<" bytes\n";
            close(client_fd);
            close(listen_fd);
            return 1;
        }
        const std::size_t remaining_capacity=kMaxHeaderSize-request.size();// 计算距离 8192 字节上限还剩多少空间。
         /*
         * 本次读取量不能超过：
         * 1. 临时数组 buffer 的容量；
         * 2. HTTP Header 剩余的允许容量。
         */
        const std::size_t receive_capacity=std::min(buffer.size(),remaining_capacity);
        ssize_t received =-1;
        /*
         * recv() 返回本次真正收到的字节数。
         *
         * received > 0：收到这些字节。
         * received == 0：客户端关闭了连接。
         * received == -1：发生错误。
         */
        do{
            received=recv(client_fd,buffer.data(),receive_capacity,0);
        }while(received==-1&&errno==EINTR);
        if(received==-1){
            std::perror("recv");
            close(client_fd);
            close(listen_fd);
            return 1;
        }
        if(received==0){
            std::cerr<<"Client closed before sending a complete Header\n";
            close(client_fd);
            close(listen_fd);
            return 1;
        }
        /*
         * 只追加 recv() 真正收到的 received 个字节。
         *
         * 不能使用 buffer 的总容量，也不能依赖 '\0'，
         * 因为 TCP 提供的是带长度的字节流。
         */
        request.append(buffer.data(),static_cast<std::size_t>(received));
    }
    std::cout<<"Received "<<request.size()<<" bytes:\n";
    /*
     * write(地址, 长度) 按明确长度输出。
     * 它不会寻找 '\0'，因此符合网络字节流的处理方式。
     */
    std::cout.write(request.data(),static_cast<std::streamsize>(request.size()));
    std::cout<<'\n';
    /*
    * 客户端请求已经接收完整。
    * 现在主动连接 127.0.0.1:19080 测试上游。
    */
   const int upstream_fd=create_upstream_connection();
   if(upstream_fd==-1){
    /*
     * create_upstream_connection() 已经保留了失败时的 errno，
     * 因此这里可以输出 Linux 提供的具体错误原因。
     */
    std::perror("create_upstream_connection");
    close(client_fd);
    close(listen_fd);
    return 1;
   }
   std::cout<<"Connected to upstream 127.0.0.1: "<<kUpstreamPort<<'\n';
   /*
    * 把客户端请求按真实长度完整发送给上游。
    *
    * request.data() 是请求字节的起始地址。
    * request.size() 是请求的实际字节数。
    */
   if(!send_all(upstream_fd,request.data(),request.size())){
    close(upstream_fd);
    close(client_fd);
    close(listen_fd);
    return 1;
   }
   std::cout<<"Forwarded "<<request.size()<<" bytes to upstream\n";
   /*
    * 请求已经完整发送给上游。
    *
    * 接下来阻塞等待上游响应，并把响应完整发送给客户端。
    */
   if(!relay_upstream_response(upstream_fd,client_fd)){
    close(upstream_fd);
    close(client_fd);
    close(listen_fd);
    return 1;
   }
   /*
    * 上游已关闭连接，说明当前响应传输结束。
    *
    * EdgeGate随后关闭客户端连接，
    * curl便能确认本次响应不会再有更多数据。
    */
    close(upstream_fd);
    close(client_fd);
    close(listen_fd);
    return 0;
}