#pragma once

// AI-CODE-BEGIN: S3-REQUEST-PARSER-API
#include "edgegate/http/message_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edgegate::http {

enum class ParseError {
    kNone,//无误
    kHeaderTooLarge,//Header 太大
    kBodyTooLarge,//
    kInvalidRequestLine,//请求行格式错误
    kUnsupportedHttpVersion,
    kMalformedHeaderLine,
    kInvalidHeaderName,
    kInvalidHeaderValue,
    kMissingHost,//缺少 Host
    kDuplicateHost,//Host 重复
    kInvalidContentLength,//Content-Length 不是合法数字
    kConflictingContentLength,
    kUnsupportedTransferEncoding,//使用了不支持的请求体编码
    kUnsupportedExpectation,
    kPipeliningNotSupported//一次传入了多条流水线请求
};

struct ParseResult {
    ParseStatus status;//解析进行到什么状态？
    ParseError error;//如果失败，具体为什么？
};

/*
 * 增量解析一条受控 HTTP/1.1 请求。
 * 调用者可以把任意大小的数据分片交给 consume()。
 * 解析器保存尚未完成的字节，直到得到请求行、全部 Header 以及 Content-Length指定的正文。
 * 当前明确拒绝 chunked 请求体、Expect: 100-continue和请求流水线。
 */
class RequestParser {
public:
    explicit RequestParser(
        std::size_t max_header_size = 8192,
        std::size_t max_body_size = 1024 * 1024);//1 MiB

    ParseResult consume(std::string_view chunk);//输入本次收到的一批字节,输出：当前解析状态 + 具体错误

    std::size_t buffered_bytes() const noexcept;//目前实际累计了多少字节
    std::size_t header_bytes() const noexcept;//Header 完整时一共占多少字节
    std::size_t message_bytes() const noexcept;//完整请求应该占多少字节
    std::size_t content_length() const noexcept;//请求体应有多少字节

    std::string_view method() const noexcept;//查询GET、POST 等方法
    std::string_view target() const noexcept;//查询/、/submit 等请求目标
    std::string_view version() const noexcept;//HTTP/1.1
    std::string_view body() const noexcept;//查询请求体

    // AI-CODE-BEGIN: S5-REQUEST-RAW-ACCESS
    // 返回一条已经完成的原始请求，供代理按真实字节转发给上游。
    std::string_view raw_message() const noexcept;
    // AI-CODE-END: S5-REQUEST-RAW-ACCESS

    const std::vector<HeaderField>& headers() const noexcept;

    std::optional<std::string_view> header_value(//查询 Header
        std::string_view name) const noexcept;

private:
    ParseError parse_request_line(std::string_view request_line);//解析请求行
    ParseError parse_header_fields(std::size_t header_end);//遍历所有 Header 行
    ParseError parse_header_line(std::string_view header_line);//检查单独一行是不是“名称: 值”
    ParseError validate_message_headers();//检查所有 Header 放在一起后是否合理
    /*
    解析器对象内部保存：
    容量上限
    原始字节 buffer_
    各类字节数量
    请求行和 Header 是否已经解析
    当前 status 和 error
    method、target、version、headers
    */
    std::size_t max_header_size_;
    std::size_t max_body_size_;
    std::size_t max_message_size_;
    std::string buffer_;

    std::size_t header_fields_start_{0};
    std::size_t header_bytes_{0};
    std::size_t message_bytes_{0};
    std::size_t content_length_{0};

    bool request_line_parsed_{false};
    bool headers_parsed_{false};

    ParseStatus status_{ParseStatus::kNeedMoreData};
    ParseError error_{ParseError::kNone};

    std::string method_;
    std::string target_;
    std::string version_;
    std::vector<HeaderField> headers_;
};

} // namespace edgegate::http
// AI-CODE-END: S3-REQUEST-PARSER-API
