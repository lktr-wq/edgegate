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

} // namespace
// AI-CODE-END: S6-YAML-CONFIG-TESTS
