#pragma once

// AI-CODE-BEGIN: S3-RESPONSE-PARSER-API
#include "edgegate/http/message_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edgegate::http {

enum class ResponseParseError {
    kNone,
    kHeaderTooLarge,
    kBodyTooLarge,
    kInvalidStatusLine,
    kUnsupportedHttpVersion,
    kInvalidStatusCode,
    kMalformedHeaderLine,
    kInvalidHeaderName,
    kInvalidHeaderValue,
    kInvalidContentLength,
    kConflictingContentLength,
    kConflictingMessageFraming,
    kUnsupportedTransferEncoding,
    kInvalidChunkSize,
    kInvalidChunkTerminator,
    kInvalidTrailer,
    kUnexpectedEof
};

enum class ResponseBodyMode {
    kNoBody,
    kContentLength,
    kChunked,
    kCloseDelimited
};

struct ResponseParseResult {
    ParseStatus status;
    ResponseParseError error;
};

/*
 * 增量解析一条 HTTP/1.x 响应。
 *
 * request_method 用来识别 HEAD 响应。关闭定界响应只有在调用
 * notify_eof() 后才能完成，因为“上游关闭连接”本身就是结束标志。
 */
class ResponseParser {
public:
    explicit ResponseParser(
        std::string request_method = "GET",
        std::size_t max_header_size = 8192,
        std::size_t max_body_size = 8 * 1024 * 1024);

    ResponseParseResult consume(std::string_view chunk);
    ResponseParseResult notify_eof();

    int status_code() const noexcept;
    std::string_view version() const noexcept;
    std::string_view reason_phrase() const noexcept;
    ResponseBodyMode body_mode() const noexcept;

    std::size_t header_bytes() const noexcept;
    std::size_t message_bytes() const noexcept;
    std::size_t buffered_bytes() const noexcept;
    std::size_t remaining_bytes() const noexcept;

    std::string_view body() const noexcept;
    std::string_view raw_message() const noexcept;

    const std::vector<HeaderField>& headers() const noexcept;
    const std::vector<HeaderField>& trailers() const noexcept;

    std::optional<std::string_view> header_value(
        std::string_view name) const noexcept;

private:
    enum class ChunkState {
        kSizeLine,
        kData,
        kTrailers
    };

    ResponseParseError parse_status_line(std::string_view status_line);
    ResponseParseError parse_header_fields(std::size_t header_end);
    ResponseParseError parse_header_line(
        std::string_view line,
        std::vector<HeaderField>& destination,
        bool trailer);
    ResponseParseError select_body_mode();
    ResponseParseResult parse_body();
    ResponseParseResult parse_chunked_body();

    std::string request_method_;
    std::size_t max_header_size_;
    std::size_t max_body_size_;
    std::size_t max_message_size_;
    std::string buffer_;

    bool status_line_parsed_{false};
    bool headers_parsed_{false};
    std::size_t header_fields_start_{0};
    std::size_t header_bytes_{0};
    std::size_t message_bytes_{0};
    std::size_t content_length_{0};

    int status_code_{0};
    std::string version_;
    std::string reason_phrase_;
    ResponseBodyMode body_mode_{ResponseBodyMode::kCloseDelimited};

    std::vector<HeaderField> headers_;
    std::vector<HeaderField> trailers_;
    std::string decoded_chunked_body_;

    ChunkState chunk_state_{ChunkState::kSizeLine};
    std::size_t chunk_position_{0};
    std::size_t trailers_start_{0};
    std::size_t current_chunk_size_{0};

    ParseStatus status_{ParseStatus::kNeedMoreData};
    ResponseParseError error_{ResponseParseError::kNone};
};

} // namespace edgegate::http
// AI-CODE-END: S3-RESPONSE-PARSER-API
