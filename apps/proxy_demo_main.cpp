// AI-CODE-BEGIN: S5-PROXY-DEMO
#include "edgegate/proxy/proxy_server.h"

#include <exception>
#include <iostream>

int main()
{
    try {
        edgegate::proxy::ProxyConfig config;
        config.upstream_address = "127.0.0.1";
        config.upstream_port = 19080;

        edgegate::proxy::ProxyServer server("127.0.0.1", 18082, config);
        std::cout
            << "EdgeGate stage-5 proxy listening on 127.0.0.1:"
            << server.port() << " -> 127.0.0.1:"
            << config.upstream_port << '\n';
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "proxy demo failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
// AI-CODE-END: S5-PROXY-DEMO
