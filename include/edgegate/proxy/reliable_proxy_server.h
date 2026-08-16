#pragma once

// AI-CODE-BEGIN: S7-RELIABLE-PROXY-SERVER-API
#include "edgegate/config/edgegate_config.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace edgegate::proxy {

/*
 * 阶段7统计量既用于最终 Dashboard，也用于当前测试确认：代理确实发生过
 * 暂停/恢复、重试、超时和健康状态切换，而不只是“最后碰巧返回成功”。
 */
struct ReliableProxyStats {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> completed_requests{0};
    std::atomic<std::uint64_t> client_errors{0};
    std::atomic<std::uint64_t> upstream_errors{0};
    std::atomic<std::uint64_t> active_sessions{0};
    std::atomic<std::uint64_t> backpressure_pauses{0};
    std::atomic<std::uint64_t> backpressure_resumes{0};
    std::atomic<std::uint64_t> upstream_timeouts{0};
    std::atomic<std::uint64_t> retries{0};
    std::atomic<std::uint64_t> health_failures{0};
    std::atomic<std::uint64_t> health_transitions{0};
};

/*
 * 正式阶段7/8服务。构造时接收一份完整、已校验的配置快照；阶段8只有
 * reload 命令会重新读取 YAML，并且完整校验成功后才原子切换。
 */
class ReliableProxyServer {
public:
    // AI-CODE-BEGIN: S8-RELOAD-CONFIG-PATH
    // config_path 仅由正式服务传入，reload 才知道应重新读取哪个 YAML；
    // 阶段 7 测试仍可只传内存配置，因此保留空路径默认值。
    explicit ReliableProxyServer(
        edgegate::config::EdgeGateConfig config,
        std::string config_path = {});
    // AI-CODE-END: S8-RELOAD-CONFIG-PATH
    ~ReliableProxyServer();

    ReliableProxyServer(const ReliableProxyServer&) = delete;
    ReliableProxyServer& operator=(const ReliableProxyServer&) = delete;

    int run_once(int timeout_ms);
    void run();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::shared_ptr<ReliableProxyStats>& stats() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace edgegate::proxy
// AI-CODE-END: S7-RELIABLE-PROXY-SERVER-API
