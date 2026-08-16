#include "edgegate/config/edgegate_config.h"

// AI-CODE-BEGIN: S6-YAML-CONFIG-TESTS
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

namespace {

class TemporaryYaml {
public:
    explicit TemporaryYaml(const std::string& contents)
        : path_("/tmp/edgegate-config-" + std::to_string(::getpid()) + "-" +
                std::to_string(++sequence_) + ".yaml")
    {
        std::ofstream output(path_);
        output << contents;
        if (!output) {
            throw std::runtime_error("failed to write temporary YAML");
        }
    }

    ~TemporaryYaml() { static_cast<void>(::unlink(path_.c_str())); }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    static int sequence_;
    std::string path_;
};

int TemporaryYaml::sequence_ = 0;

const char* kValidYaml = R"yaml(
listen:
  address: 127.0.0.1
  port: 18080
limits:
  max_header_size: 4096
  max_request_body_size: 10000
  max_response_body_size: 20000
upstream_pools:
  - id: api
    endpoints:
      - id: a
        address: 127.0.0.1
        port: 19081
      - id: b
        address: 127.0.0.1
        port: 19082
routes:
  - id: api-route
    host: api.example.test
    path_prefix: /api
    upstream_pool: api
)yaml";

// 测试：合法 YAML 应解析监听、容量和池引用，并把同一池的两个节点交给路由。
TEST(EdgeGateConfigTest, LoadsAndResolvesUpstreamPool)
{
    TemporaryYaml yaml(kValidYaml);
    const auto config = edgegate::config::load_edgegate_config(yaml.path());
    EXPECT_EQ(config.listen_address, "127.0.0.1");
    EXPECT_EQ(config.listen_port, 18080);
    EXPECT_EQ(config.max_header_size, 4096U);
    ASSERT_EQ(config.routes.size(), 1U);
    ASSERT_EQ(config.routes[0].upstreams.size(), 2U);
    EXPECT_EQ(config.routes[0].upstreams[1].port, 19082);
}

// 测试：拼错字段名时不能静默忽略，错误信息应指出未知字段及所在文件。
TEST(EdgeGateConfigTest, RejectsUnknownKeyWithLocation)
{
    std::string yaml_text(kValidYaml);
    yaml_text.replace(yaml_text.find("max_header_size"), 15, "max_header_bytes");
    TemporaryYaml yaml(yaml_text);
    try {
        static_cast<void>(edgegate::config::load_edgegate_config(yaml.path()));
        FAIL() << "configuration should have been rejected";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(yaml.path()), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("unknown configuration key"),
                  std::string::npos);
    }
}

// 测试：上游只能使用数字 IPv4，域名应在启动解析阶段得到明确拒绝原因。
TEST(EdgeGateConfigTest, RejectsDnsUpstreamAddress)
{
    std::string yaml_text(kValidYaml);
    yaml_text.replace(yaml_text.find("127.0.0.1", yaml_text.find("endpoints")),
                      9,
                      "backend.local");
    TemporaryYaml yaml(yaml_text);
    EXPECT_THROW(
        {
            try {
                static_cast<void>(edgegate::config::load_edgegate_config(yaml.path()));
            } catch (const std::runtime_error& error) {
                EXPECT_NE(std::string(error.what()).find("numeric IPv4"),
                          std::string::npos);
                throw;
            }
        },
        std::runtime_error);
}

// 测试：路由引用不存在的上游池时，应在启动阶段拒绝而不是运行后返回 502。
TEST(EdgeGateConfigTest, RejectsUnknownPoolReference)
{
    std::string yaml_text(kValidYaml);
    const std::size_t reference = yaml_text.rfind("upstream_pool: api");
    yaml_text.replace(reference, std::string("upstream_pool: api").size(),
                      "upstream_pool: missing");
    TemporaryYaml yaml(yaml_text);
    EXPECT_THROW(
        {
            try {
                static_cast<void>(edgegate::config::load_edgegate_config(yaml.path()));
            } catch (const std::runtime_error& error) {
                EXPECT_NE(std::string(error.what()).find("unknown upstream pool"),
                          std::string::npos);
                throw;
            }
        },
        std::runtime_error);
}

// AI-CODE-BEGIN: S7-RELIABILITY-CONFIG-TESTS
// 测试：阶段7的缓冲水位、超时和健康检查字段应从YAML进入配置快照。
TEST(EdgeGateConfigTest, LoadsReliabilitySettings)
{
    std::string yaml_text(kValidYaml);
    const std::size_t pools = yaml_text.find("upstream_pools:");
    yaml_text.insert(pools, R"yaml(
stream_buffer:
  capacity: 32768
  high_watermark: 24576
  low_watermark: 8192
timeouts:
  client_header_ms: 101
  upstream_connect_ms: 102
  upstream_header_ms: 103
  io_idle_ms: 104
  request_total_ms: 105
  keep_alive_idle_ms: 106
health_check:
  interval_ms: 201
  timeout_ms: 202
  failure_threshold: 2
  success_threshold: 3
  path: /ready
)yaml");
    TemporaryYaml yaml(yaml_text);
    const auto config = edgegate::config::load_edgegate_config(yaml.path());
    EXPECT_EQ(config.stream_buffer.capacity, 32768U);
    EXPECT_EQ(config.stream_buffer.high_watermark, 24576U);
    EXPECT_EQ(config.stream_buffer.low_watermark, 8192U);
    EXPECT_EQ(config.timeouts.upstream_header_ms, 103U);
    EXPECT_EQ(config.health_check.failure_threshold, 2U);
    EXPECT_EQ(config.health_check.path, "/ready");
}

// 测试：低水位、高水位和容量的顺序不合理时，应在服务启动前拒绝配置。
TEST(EdgeGateConfigTest, RejectsInvalidStreamWatermarks)
{
    std::string yaml_text(kValidYaml);
    yaml_text.insert(yaml_text.find("upstream_pools:"), R"yaml(
stream_buffer:
  capacity: 4096
  high_watermark: 1024
  low_watermark: 2048
)yaml");
    TemporaryYaml yaml(yaml_text);
    EXPECT_THROW(
        static_cast<void>(edgegate::config::load_edgegate_config(yaml.path())),
        std::runtime_error);
}
// AI-CODE-END: S7-RELIABILITY-CONFIG-TESTS

// AI-CODE-BEGIN: S8-MANAGEMENT-LOGGING-CONFIG-TESTS
// 测试：阶段8的本机管理通道、排空期限和日志轮转参数应进入完整配置快照。
TEST(EdgeGateConfigTest, LoadsManagementAndLoggingSettings)
{
    std::string yaml_text(kValidYaml);
    yaml_text.insert(yaml_text.find("upstream_pools:"), R"yaml(
management:
  enabled: true
  socket_path: /tmp/edgegate-test.sock
  drain_timeout_ms: 4321
logging:
  enabled: true
  directory: /tmp/edgegate-test-logs
  level: warn
  max_file_size: 65536
  max_files: 3
)yaml");
    TemporaryYaml yaml(yaml_text);
    const auto config = edgegate::config::load_edgegate_config(yaml.path());
    EXPECT_TRUE(config.management.enabled);
    EXPECT_EQ(config.management.socket_path, "/tmp/edgegate-test.sock");
    EXPECT_EQ(config.management.drain_timeout_ms, 4321U);
    EXPECT_TRUE(config.logging.enabled);
    EXPECT_EQ(config.logging.directory, "/tmp/edgegate-test-logs");
    EXPECT_EQ(config.logging.level, "warn");
    EXPECT_EQ(config.logging.max_file_size, 65536U);
    EXPECT_EQ(config.logging.max_files, 3U);
}

// 测试：Unix Socket 使用相对路径会让启动目录影响控制程序连接位置，必须拒绝。
TEST(EdgeGateConfigTest, RejectsRelativeManagementSocketPath)
{
    std::string yaml_text(kValidYaml);
    yaml_text.insert(yaml_text.find("upstream_pools:"), R"yaml(
management:
  enabled: true
  socket_path: relative/edgegate.sock
)yaml");
    TemporaryYaml yaml(yaml_text);
    EXPECT_THROW(
        static_cast<void>(edgegate::config::load_edgegate_config(yaml.path())),
        std::runtime_error);
}

// 测试：日志级别拼写错误时应在 reload/启动前失败，而不是默默退回 info。
TEST(EdgeGateConfigTest, RejectsUnknownLoggingLevel)
{
    std::string yaml_text(kValidYaml);
    yaml_text.insert(yaml_text.find("upstream_pools:"), R"yaml(
logging:
  enabled: true
  directory: /tmp/edgegate-test-logs
  level: verbose
)yaml");
    TemporaryYaml yaml(yaml_text);
    EXPECT_THROW(
        static_cast<void>(edgegate::config::load_edgegate_config(yaml.path())),
        std::runtime_error);
}
// AI-CODE-END: S8-MANAGEMENT-LOGGING-CONFIG-TESTS

} // namespace
// AI-CODE-END: S6-YAML-CONFIG-TESTS
