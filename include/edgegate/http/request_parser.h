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
    kNone,
    kHeaderTooLarge,
    kBodyTooLarge,
    kInvalidRequestLine,
    kUnsupportedHttpVersion,
    kMalformedHeaderLine,
    kInvalidHeaderName,
    kInvalidHeaderValue,
    kMissingHost,
    kDuplicateHost,
    kInvalidContentLength,
    kConflictingContentLength,
    kUnsupportedTransferEncoding,
    kUnsupportedExpectation,
    kPipeliningNotSupported
};

struct ParseResult {
    ParseStatus status;
    ParseError error;
};

/*
 * 增量解析一条受控 HTTP/1.1 请求。
 *
 * 调用者可以把任意大小的数据分片交给 consume()。解析器保存尚未
 * 完成的字节，直到得到请求行、全部 Header 以及 Content-Length
 * 指定的正文。当前明确拒绝 chunked 请求体、Expect: 100-continue
 * 和请求流水线。
 */
class RequestParser {
public:
    explicit RequestParser(
        std::size_t max_header_size = 8192,
        std::size_t max_body_size = 1024 * 1024);

    ParseResult consume(std::string_view chunk);

    std::size_t buffered_bytes() const noexcept;
    std::size_t header_bytes() const noexcept;
    std::size_t message_bytes() const noexcept;
    std::size_t content_length() const noexcept;

    std::string_view method() const noexcept;
    std::string_view target() const noexcept;
    std::string_view version() const noexcept;
    std::string_view body() const noexcept;

    // AI-CODE-BEGIN: S5-REQUEST-RAW-ACCESS
    // 返回一条已经完成的原始请求，供代理按真实字节转发给上游。
    std::string_view raw_message() const noexcept;
    // AI-CODE-END: S5-REQUEST-RAW-ACCESS

    const std::vector<HeaderField>& headers() const noexcept;

    std::optional<std::string_view> header_value(
        std::string_view name) const noexcept;

private:
    ParseError parse_request_line(std::string_view request_line);
    ParseError parse_header_fields(std::size_t header_end);
    ParseError parse_header_line(std::string_view header_line);
    ParseError validate_message_headers();

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
