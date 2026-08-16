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
 * 正式阶段7服务。构造时接收一份完整、已校验的配置快照；运行期间不再
 * 读取 YAML，避免半更新配置污染正在处理的连接。
 */
class ReliableProxyServer {
public:
    explicit ReliableProxyServer(edgegate::config::EdgeGateConfig config);
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
