// AI-CODE-BEGIN: S4-REACTOR-DEMO
#include "edgegate/net/reactor_echo_server.h"

#include <exception>
#include <iostream>

int main()
{
    try {
        edgegate::net::ReactorEchoServer server("127.0.0.1", 18081);
        std::cout
            << "EdgeGate stage-4 Reactor echo demo listening on 127.0.0.1:"
            << server.port() << '\n'
            << "Send raw bytes with: nc 127.0.0.1 18081\n";
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "reactor demo failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
// AI-CODE-END: S4-REACTOR-DEMO
