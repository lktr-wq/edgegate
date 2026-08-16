#include "edgegate/config/edgegate_config.h"
#include "edgegate/proxy/proxy_server.h"
#include "edgegate/routing/route_table.h"

// AI-CODE-BEGIN: S6-CONFIGURED-SERVICE-MAIN
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

int main(int argc, char* argv[])
{
    const std::string config_path =
        argc > 1 ? argv[1] : "config/edgegate.yaml";

    try {
        const auto loaded = edgegate::config::load_edgegate_config(config_path);

        edgegate::proxy::ProxyConfig proxy_config;
        proxy_config.max_header_size = loaded.max_header_size;
        proxy_config.max_request_body_size = loaded.max_request_body_size;
        proxy_config.max_response_body_size = loaded.max_response_body_size;
        proxy_config.route_table =
            std::make_shared<edgegate::routing::RouteTable>(loaded.routes);

        edgegate::proxy::ProxyServer server(
            loaded.listen_address,
            loaded.listen_port,
            std::move(proxy_config));

        std::cout << "EdgeGate listening on "
                  << loaded.listen_address << ':' << server.port()
                  << " with " << loaded.routes.size() << " routes"
                  << std::endl;
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "EdgeGate startup failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
// AI-CODE-END: S6-CONFIGURED-SERVICE-MAIN
