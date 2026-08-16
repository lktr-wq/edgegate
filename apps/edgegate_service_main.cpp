#include "edgegate/config/edgegate_config.h"
#include "edgegate/proxy/reliable_proxy_server.h"
#include "edgegate/runtime/signal_control.h"

// AI-CODE-BEGIN: S6-CONFIGURED-SERVICE-MAIN
#include <exception>
#include <iostream>
#include <string>
#include <utility>

int main(int argc, char* argv[])
{
    const std::string config_path =
        argc > 1 ? argv[1] : "config/edgegate.yaml";

    try {
        // AI-CODE-BEGIN: S8-SERVICE-SIGNAL-SETUP
        // 先屏蔽异步终止信号，再由服务内部 signalfd 把信号交给 EventLoop。
        edgegate::runtime::block_termination_signals();
        // AI-CODE-END: S8-SERVICE-SIGNAL-SETUP
        auto loaded = edgegate::config::load_edgegate_config(config_path);
        const std::string listen_address = loaded.listen_address;
        const std::size_t route_count = loaded.routes.size();

        // AI-CODE-BEGIN: S7-FORMAL-SERVICE-SWITCH
        // 正式 edgegate 从阶段6整包代理切换到阶段7流式、超时和健康检查实现。
        edgegate::proxy::ReliableProxyServer server(
            std::move(loaded), config_path);
        // AI-CODE-END: S7-FORMAL-SERVICE-SWITCH

        std::cout << "EdgeGate listening on "
                  << listen_address << ':' << server.port()
                  << " with " << route_count << " routes"
                  << std::endl;
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "EdgeGate startup failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
// AI-CODE-END: S6-CONFIGURED-SERVICE-MAIN
