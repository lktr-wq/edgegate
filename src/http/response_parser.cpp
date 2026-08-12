#include "edgegate/http/response_parser.h"

// AI-CODE-BEGIN: S3-RESPONSE-PARSER-IMPLEMENTATION
#include <limits>
#include <utility>

namespace edgegate::http {
namespace {

bool is_token_character(char character) noexcept
{
    const auto value = static_cast<unsigned char>(character);
    if ((value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }

    switch (character) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

bool is_field_value_character(char character) noexcept
{
    const auto value = static_cast<unsigned char>(character);
    return value == '\t' || (value >= 0x20 && value != 0x7f);
}

std::string_view trim_optional_whitespace(std::string_view value) noexcept
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

char to_ascii_lower(char character) noexcept
{
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character + ('a' - 'A'));
    }
    return character;
}

bool ascii_equals_ignore_case(
    std::string_view left,
    std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (to_ascii_lower(left[index]) != to_ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

std::optional<std::size_t> parse_decimal_size(std::string_view value) noexcept
{
    if (value.empty()) {
        return std::nullopt;
    }

    std::size_t result = 0;
    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();

    for (char character : value) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::size_t digit =
            static_cast<std::size_t>(character - '0');
        if (result > (maximum - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

std::optional<std::size_t> parse_hex_size(std::string_view value) noexcept
{
    if (value.empty()) {
        return std::nullopt;
    }

    std::size_t result = 0;
    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();

    for (char character : value) {
        unsigned int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned int>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned int>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<unsigned int>(character - 'A' + 10);
        } else {
            return std::nullopt;
        }

        if (result > (maximum - digit) / 16) {
            return std::nullopt;
        }
        result = result * 16 + digit;
    }
    return result;
}

} // namespace

ResponseParser::ResponseParser(
    std::string request_method,
    std::size_t max_header_size,
    std::size_t max_body_size)
    : request_method_(std::move(request_method)),
      max_header_size_(max_header_size),
      max_body_size_(max_body_size)
{
}

ResponseParseError ResponseParser::parse_status_line(
    std::string_view status_line)
{
    const std::size_t first_space = status_line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
        return ResponseParseError::kInvalidStatusLine;
    }

    const std::string_view version = status_line.substr(0, first_space);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        return ResponseParseError::kUnsupportedHttpVersion;
    }

    if (status_line.size() < first_space + 4) {
        return ResponseParseError::kInvalidStatusCode;
    }

    const std::string_view code = status_line.substr(first_space + 1, 3);
    if (code[0] < '0' || code[0] > '9' ||
        code[1] < '0' || code[1] > '9' ||
        code[2] < '0' || code[2] > '9') {
        return ResponseParseError::kInvalidStatusCode;
    }

    const int parsed_code =
        (code[0] - '0') * 100 +
        (code[1] - '0') * 10 +
        (code[2] - '0');
    if (parsed_code < 100 || parsed_code > 599) {
        return ResponseParseError::kInvalidStatusCode;
    }

    std::string_view reason;
    if (status_line.size() > first_space + 4) {
        if (status_line[first_space + 4] != ' ') {
            return ResponseParseError::kInvalidStatusLine;
        }
        reason = status_line.substr(first_space + 5);
        for (char character : reason) {
            if (!is_field_value_character(character)) {
                return ResponseParseError::kInvalidStatusLine;
            }
        }
    }

    version_.assign(version.data(), version.size());
    status_code_ = parsed_code;
    reason_phrase_.assign(reason.data(), reason.size());
    return ResponseParseError::kNone;
}

ResponseParseError ResponseParser::parse_header_line(
    std::string_view line,
    std::vector<HeaderField>& destination,
    bool trailer)
{
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
        return trailer
            ? ResponseParseError::kInvalidTrailer
            : ResponseParseError::kMalformedHeaderLine;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return trailer
            ? ResponseParseError::kInvalidTrailer
            : ResponseParseError::kMalformedHeaderLine;
    }

    const std::string_view name = line.substr(0, colon);
    const std::string_view value = trim_optional_whitespace(
        line.substr(colon + 1));

    if (name.empty()) {
        return trailer
            ? ResponseParseError::kInvalidTrailer
            : ResponseParseError::kInvalidHeaderName;
    }

    for (char character : name) {
        if (!is_token_character(character)) {
            return trailer
                ? ResponseParseError::kInvalidTrailer
                : ResponseParseError::kInvalidHeaderName;
        }
    }

    for (char character : value) {
        if (!is_field_value_character(character)) {
            return trailer
                ? ResponseParseError::kInvalidTrailer
                : ResponseParseError::kInvalidHeaderValue;
        }
    }

    if (trailer &&
        (ascii_equals_ignore_case(name, "Content-Length") ||
         ascii_equals_ignore_case(name, "Transfer-Encoding") ||
         ascii_equals_ignore_case(name, "Host"))) {
        return ResponseParseError::kInvalidTrailer;
    }

    destination.push_back(HeaderField{std::string(name), std::string(value)});
    return ResponseParseError::kNone;
}

ResponseParseError ResponseParser::parse_header_fields(
    std::size_t header_end)
{
    // 没有任何 Header 时，header_end 指向状态行末尾的 CRLF，
    // header_fields_start_ 位于这两个字节之后，所以两者相差 2。
    if (header_end + 2 == header_fields_start_) {
        return ResponseParseError::kNone;
    }
    if (header_end < header_fields_start_) {
        return ResponseParseError::kMalformedHeaderLine;
    }

    std::size_t line_start = header_fields_start_;
    while (line_start < header_end) {
        const std::size_t line_end = buffer_.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end > header_end) {
            return ResponseParseError::kMalformedHeaderLine;
        }

        const std::string_view line(
            buffer_.data() + line_start,
            line_end - line_start);
        const auto line_error = parse_header_line(line, headers_, false);
        if (line_error != ResponseParseError::kNone) {
            return line_error;
        }

        if (line_end == header_end) {
            break;
        }
        line_start = line_end + 2;
    }
    return ResponseParseError::kNone;
}

ResponseParseError ResponseParser::select_body_mode()
{
    std::optional<std::size_t> parsed_content_length;
    bool has_transfer_encoding = false;

    for (const HeaderField& header : headers_) {
        if (ascii_equals_ignore_case(header.name, "Content-Length")) {
            const auto length = parse_decimal_size(header.value);
            if (!length.has_value()) {
                return ResponseParseError::kInvalidContentLength;
            }
            if (parsed_content_length.has_value() &&
                *parsed_content_length != *length) {
                return ResponseParseError::kConflictingContentLength;
            }
            parsed_content_length = *length;
        }

        if (ascii_equals_ignore_case(header.name, "Transfer-Encoding")) {
            if (!ascii_equals_ignore_case(header.value, "chunked")) {
                return ResponseParseError::kUnsupportedTransferEncoding;
            }
            has_transfer_encoding = true;
        }
    }

    if (has_transfer_encoding && parsed_content_length.has_value()) {
        return ResponseParseError::kConflictingMessageFraming;
    }

    const bool no_body =
        ascii_equals_ignore_case(request_method_, "HEAD") ||
        (status_code_ >= 100 && status_code_ < 200) ||
        status_code_ == 204 || status_code_ == 304;

    if (no_body) {
        body_mode_ = ResponseBodyMode::kNoBody;
        message_bytes_ = header_bytes_;
        return ResponseParseError::kNone;
    }

    if (has_transfer_encoding) {
        body_mode_ = ResponseBodyMode::kChunked;
        chunk_position_ = header_bytes_;
        return ResponseParseError::kNone;
    }

    if (parsed_content_length.has_value()) {
        content_length_ = *parsed_content_length;
        if (content_length_ > max_body_size_) {
            return ResponseParseError::kBodyTooLarge;
        }
        if (content_length_ >
            std::numeric_limits<std::size_t>::max() - header_bytes_) {
            return ResponseParseError::kInvalidContentLength;
        }
        body_mode_ = ResponseBodyMode::kContentLength;
        message_bytes_ = header_bytes_ + content_length_;
        return ResponseParseError::kNone;
    }

    body_mode_ = ResponseBodyMode::kCloseDelimited;
    return ResponseParseError::kNone;
}

ResponseParseResult ResponseParser::parse_chunked_body()
{
    for (;;) {
        if (chunk_state_ == ChunkState::kSizeLine) {
            const std::size_t line_end = buffer_.find("\r\n", chunk_position_);
            if (line_end == std::string::npos) {
                // 块长度行也不能无限增长，否则慢速恶意上游可持续占用内存。
                if (buffer_.size() - chunk_position_ >= max_header_size_) {
                    status_ = ParseStatus::kError;
                    error_ = ResponseParseError::kInvalidChunkSize;
                }
                return {status_, error_};
            }

            const std::string_view line(
                buffer_.data() + chunk_position_,
                line_end - chunk_position_);
            const std::size_t semicolon = line.find(';');
            const std::string_view size_text = line.substr(0, semicolon);

            if (semicolon != std::string_view::npos) {
                const std::string_view extension = line.substr(semicolon + 1);
                for (char character : extension) {
                    if (!is_field_value_character(character)) {
                        status_ = ParseStatus::kError;
                        error_ = ResponseParseError::kInvalidChunkSize;
                        return {status_, error_};
                    }
                }
            }

            const auto size = parse_hex_size(size_text);
            if (!size.has_value()) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kInvalidChunkSize;
                return {status_, error_};
            }

            current_chunk_size_ = *size;
            chunk_position_ = line_end + 2;

            if (current_chunk_size_ == 0) {
                chunk_state_ = ChunkState::kTrailers;
                trailers_start_ = chunk_position_;
            } else {
                if (decoded_chunked_body_.size() > max_body_size_ ||
                    current_chunk_size_ >
                    max_body_size_ - decoded_chunked_body_.size()) {
                    status_ = ParseStatus::kError;
                    error_ = ResponseParseError::kBodyTooLarge;
                    return {status_, error_};
                }
                chunk_state_ = ChunkState::kData;
            }
        }

        if (chunk_state_ == ChunkState::kData) {
            constexpr std::size_t maximum =
                std::numeric_limits<std::size_t>::max();
            if (chunk_position_ > maximum - 2 ||
                current_chunk_size_ > maximum - chunk_position_ - 2) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kInvalidChunkSize;
                return {status_, error_};
            }

            const std::size_t data_end = chunk_position_ + current_chunk_size_;
            if (buffer_.size() < data_end + 2) {
                return {status_, error_};
            }

            if (buffer_.compare(data_end, 2, "\r\n") != 0) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kInvalidChunkTerminator;
                return {status_, error_};
            }

            decoded_chunked_body_.append(
                buffer_.data() + chunk_position_,
                current_chunk_size_);
            chunk_position_ = data_end + 2;
            chunk_state_ = ChunkState::kSizeLine;
            continue;
        }

        if (chunk_state_ == ChunkState::kTrailers) {
            const std::size_t line_end = buffer_.find("\r\n", chunk_position_);
            if (line_end == std::string::npos) {
                if (buffer_.size() - trailers_start_ >= max_header_size_) {
                    status_ = ParseStatus::kError;
                    error_ = ResponseParseError::kHeaderTooLarge;
                }
                return {status_, error_};
            }

            if (line_end + 2 - trailers_start_ > max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kHeaderTooLarge;
                return {status_, error_};
            }

            if (line_end == chunk_position_) {
                message_bytes_ = line_end + 2;
                status_ = ParseStatus::kMessageComplete;
                return {status_, error_};
            }

            const std::string_view trailer_line(
                buffer_.data() + chunk_position_,
                line_end - chunk_position_);
            error_ = parse_header_line(trailer_line, trailers_, true);
            if (error_ != ResponseParseError::kNone) {
                status_ = ParseStatus::kError;
                return {status_, error_};
            }
            chunk_position_ = line_end + 2;
        }
    }
}

ResponseParseResult ResponseParser::parse_body()
{
    switch (body_mode_) {
    case ResponseBodyMode::kNoBody:
        status_ = ParseStatus::kMessageComplete;
        return {status_, error_};

    case ResponseBodyMode::kContentLength:
        if (buffer_.size() >= message_bytes_) {
            status_ = ParseStatus::kMessageComplete;
        }
        return {status_, error_};

    case ResponseBodyMode::kChunked:
        return parse_chunked_body();

    case ResponseBodyMode::kCloseDelimited:
        if (buffer_.size() - header_bytes_ > max_body_size_) {
            status_ = ParseStatus::kError;
            error_ = ResponseParseError::kBodyTooLarge;
        }
        return {status_, error_};
    }

    status_ = ParseStatus::kError;
    error_ = ResponseParseError::kInvalidStatusLine;
    return {status_, error_};
}

ResponseParseResult ResponseParser::consume(std::string_view chunk)
{
    if (status_ != ParseStatus::kNeedMoreData) {
        return {status_, error_};
    }

    if (!chunk.empty()) {
        buffer_.append(chunk.data(), chunk.size());
    }

    if (!status_line_parsed_) {
        const std::size_t status_line_end = buffer_.find("\r\n");
        if (status_line_end == std::string::npos) {
            if (buffer_.size() >= max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kHeaderTooLarge;
            }
            return {status_, error_};
        }

        error_ = parse_status_line(
            std::string_view(buffer_.data(), status_line_end));
        if (error_ != ResponseParseError::kNone) {
            status_ = ParseStatus::kError;
            return {status_, error_};
        }

        header_fields_start_ = status_line_end + 2;
        status_line_parsed_ = true;
    }

    if (!headers_parsed_) {
        const std::size_t header_end = buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (buffer_.size() >= max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kHeaderTooLarge;
            }
            return {status_, error_};
        }

        header_bytes_ = header_end + 4;
        if (header_bytes_ > max_header_size_) {
            status_ = ParseStatus::kError;
            error_ = ResponseParseError::kHeaderTooLarge;
            return {status_, error_};
        }

        error_ = parse_header_fields(header_end);
        if (error_ == ResponseParseError::kNone) {
            error_ = select_body_mode();
        }
        if (error_ != ResponseParseError::kNone) {
            status_ = ParseStatus::kError;
            return {status_, error_};
        }
        headers_parsed_ = true;
    }

    return parse_body();
}

ResponseParseResult ResponseParser::notify_eof()
{
    if (status_ != ParseStatus::kNeedMoreData) {
        return {status_, error_};
    }

    if (!headers_parsed_) {
        status_ = ParseStatus::kError;
        error_ = ResponseParseError::kUnexpectedEof;
        return {status_, error_};
    }

    if (body_mode_ != ResponseBodyMode::kCloseDelimited) {
        status_ = ParseStatus::kError;
        error_ = ResponseParseError::kUnexpectedEof;
        return {status_, error_};
    }

    if (buffer_.size() - header_bytes_ > max_body_size_) {
        status_ = ParseStatus::kError;
        error_ = ResponseParseError::kBodyTooLarge;
        return {status_, error_};
    }

    message_bytes_ = buffer_.size();
    status_ = ParseStatus::kMessageComplete;
    return {status_, error_};
}

int ResponseParser::status_code() const noexcept
{
    return status_code_;
}

std::string_view ResponseParser::version() const noexcept
{
    return {version_.data(), version_.size()};
}

std::string_view ResponseParser::reason_phrase() const noexcept
{
    return {reason_phrase_.data(), reason_phrase_.size()};
}

ResponseBodyMode ResponseParser::body_mode() const noexcept
{
    return body_mode_;
}

std::size_t ResponseParser::header_bytes() const noexcept
{
    return header_bytes_;
}

std::size_t ResponseParser::message_bytes() const noexcept
{
    return message_bytes_;
}

std::size_t ResponseParser::buffered_bytes() const noexcept
{
    return buffer_.size();
}

std::size_t ResponseParser::remaining_bytes() const noexcept
{
    if (status_ != ParseStatus::kMessageComplete ||
        buffer_.size() < message_bytes_) {
        return 0;
    }
    return buffer_.size() - message_bytes_;
}

std::string_view ResponseParser::body() const noexcept
{
    if (status_ != ParseStatus::kMessageComplete) {
        return {};
    }
    if (body_mode_ == ResponseBodyMode::kChunked) {
        return {decoded_chunked_body_.data(), decoded_chunked_body_.size()};
    }
    if (body_mode_ == ResponseBodyMode::kNoBody) {
        return {};
    }
    return {buffer_.data() + header_bytes_, message_bytes_ - header_bytes_};
}

std::string_view ResponseParser::raw_message() const noexcept
{
    if (status_ != ParseStatus::kMessageComplete) {
        return {};
    }
    return {buffer_.data(), message_bytes_};
}

const std::vector<HeaderField>& ResponseParser::headers() const noexcept
{
    return headers_;
}

const std::vector<HeaderField>& ResponseParser::trailers() const noexcept
{
    return trailers_;
}

std::optional<std::string_view> ResponseParser::header_value(
    std::string_view name) const noexcept
{
    for (const HeaderField& header : headers_) {
        if (ascii_equals_ignore_case(header.name, name)) {
            return std::string_view(header.value.data(), header.value.size());
        }
    }
    return std::nullopt;
}

} // namespace edgegate::http
// AI-CODE-END: S3-RESPONSE-PARSER-IMPLEMENTATION
