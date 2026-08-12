#include "edgegate/http/request_parser.h"

// AI-CODE-BEGIN: S3-REQUEST-PARSER-IMPLEMENTATION
#include <limits>

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
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

bool is_target_character(char character) noexcept
{
    const auto value = static_cast<unsigned char>(character);
    return value > 0x20 && value != 0x7f;
}

bool is_header_value_character(char character) noexcept
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

} // namespace

RequestParser::RequestParser(
    std::size_t max_header_size,
    std::size_t max_body_size)
    : max_header_size_(max_header_size),
      max_body_size_(max_body_size)
{
}

ParseError RequestParser::parse_request_line(
    std::string_view request_line)
{
    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
        return ParseError::kInvalidRequestLine;
    }

    const std::size_t second_space =
        request_line.find(' ', first_space + 1);

    if (second_space == std::string_view::npos ||
        second_space == first_space + 1 ||
        request_line.find(' ', second_space + 1) !=
            std::string_view::npos) {
        return ParseError::kInvalidRequestLine;
    }

    const std::string_view method = request_line.substr(0, first_space);
    const std::string_view target = request_line.substr(
        first_space + 1,
        second_space - first_space - 1);
    const std::string_view version = request_line.substr(second_space + 1);

    if (version.empty()) {
        return ParseError::kInvalidRequestLine;
    }

    for (char character : method) {
        if (!is_token_character(character)) {
            return ParseError::kInvalidRequestLine;
        }
    }

    for (char character : target) {
        if (!is_target_character(character)) {
            return ParseError::kInvalidRequestLine;
        }
    }

    if (version != "HTTP/1.1") {
        return ParseError::kUnsupportedHttpVersion;
    }

    method_.assign(method.data(), method.size());
    target_.assign(target.data(), target.size());
    version_.assign(version.data(), version.size());
    return ParseError::kNone;
}

ParseError RequestParser::parse_header_line(
    std::string_view header_line)
{
    if (header_line.empty() ||
        header_line.front() == ' ' ||
        header_line.front() == '\t') {
        return ParseError::kMalformedHeaderLine;
    }

    const std::size_t colon = header_line.find(':');
    if (colon == std::string_view::npos) {
        return ParseError::kMalformedHeaderLine;
    }

    const std::string_view name = header_line.substr(0, colon);
    const std::string_view value = trim_optional_whitespace(
        header_line.substr(colon + 1));

    if (name.empty()) {
        return ParseError::kInvalidHeaderName;
    }

    for (char character : name) {
        if (!is_token_character(character)) {
            return ParseError::kInvalidHeaderName;
        }
    }

    for (char character : value) {
        if (!is_header_value_character(character)) {
            return ParseError::kInvalidHeaderValue;
        }
    }

    headers_.push_back(
        HeaderField{std::string(name), std::string(value)});
    return ParseError::kNone;
}

ParseError RequestParser::parse_header_fields(std::size_t header_end)
{
    if (header_end + 2 == header_fields_start_) {
        return ParseError::kNone;
    }

    if (header_end < header_fields_start_) {
        return ParseError::kMalformedHeaderLine;
    }

    std::size_t line_start = header_fields_start_;
    while (line_start < header_end) {
        const std::size_t line_end = buffer_.find("\r\n", line_start);

        if (line_end == std::string::npos || line_end > header_end) {
            return ParseError::kMalformedHeaderLine;
        }

        const std::string_view header_line(
            buffer_.data() + line_start,
            line_end - line_start);

        const ParseError line_error = parse_header_line(header_line);
        if (line_error != ParseError::kNone) {
            return line_error;
        }

        if (line_end == header_end) {
            break;
        }
        line_start = line_end + 2;
    }

    return ParseError::kNone;
}

ParseError RequestParser::validate_message_headers()
{
    std::size_t host_count = 0;
    std::optional<std::size_t> parsed_content_length;

    for (const HeaderField& header : headers_) {
        if (ascii_equals_ignore_case(header.name, "Host")) {
            ++host_count;
            if (header.value.empty()) {
                return ParseError::kMissingHost;
            }
        }

        if (ascii_equals_ignore_case(header.name, "Transfer-Encoding")) {
            return ParseError::kUnsupportedTransferEncoding;
        }

        if (ascii_equals_ignore_case(header.name, "Expect")) {
            return ParseError::kUnsupportedExpectation;
        }

        if (ascii_equals_ignore_case(header.name, "Content-Length")) {
            const auto length = parse_decimal_size(header.value);
            if (!length.has_value()) {
                return ParseError::kInvalidContentLength;
            }

            if (parsed_content_length.has_value() &&
                *parsed_content_length != *length) {
                return ParseError::kConflictingContentLength;
            }
            parsed_content_length = *length;
        }
    }

    if (host_count == 0) {
        return ParseError::kMissingHost;
    }
    if (host_count > 1) {
        return ParseError::kDuplicateHost;
    }

    content_length_ = parsed_content_length.value_or(0);
    if (content_length_ > max_body_size_) {
        return ParseError::kBodyTooLarge;
    }

    if (content_length_ >
        std::numeric_limits<std::size_t>::max() - header_bytes_) {
        return ParseError::kInvalidContentLength;
    }

    message_bytes_ = header_bytes_ + content_length_;
    return ParseError::kNone;
}

ParseResult RequestParser::consume(std::string_view chunk)
{
    if (status_ != ParseStatus::kNeedMoreData) {
        return {status_, error_};
    }

    if (!chunk.empty()) {
        buffer_.append(chunk.data(), chunk.size());
    }

    if (!request_line_parsed_) {
        const std::size_t request_line_end = buffer_.find("\r\n");
        if (request_line_end == std::string::npos) {
            if (buffer_.size() >= max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ParseError::kHeaderTooLarge;
            }
            return {status_, error_};
        }

        const std::string_view request_line(
            buffer_.data(),
            request_line_end);
        error_ = parse_request_line(request_line);

        if (error_ != ParseError::kNone) {
            status_ = ParseStatus::kError;
            return {status_, error_};
        }

        header_fields_start_ = request_line_end + 2;
        request_line_parsed_ = true;
    }

    if (!headers_parsed_) {
        const std::size_t header_end = buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (buffer_.size() >= max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ParseError::kHeaderTooLarge;
            }
            return {status_, error_};
        }

        header_bytes_ = header_end + 4;
        if (header_bytes_ > max_header_size_) {
            status_ = ParseStatus::kError;
            error_ = ParseError::kHeaderTooLarge;
            return {status_, error_};
        }

        error_ = parse_header_fields(header_end);
        if (error_ == ParseError::kNone) {
            error_ = validate_message_headers();
        }
        if (error_ != ParseError::kNone) {
            status_ = ParseStatus::kError;
            return {status_, error_};
        }

        headers_parsed_ = true;
    }

    if (buffer_.size() < message_bytes_) {
        return {status_, error_};
    }

    if (buffer_.size() > message_bytes_) {
        status_ = ParseStatus::kError;
        error_ = ParseError::kPipeliningNotSupported;
        return {status_, error_};
    }

    status_ = ParseStatus::kMessageComplete;
    return {status_, error_};
}

std::size_t RequestParser::buffered_bytes() const noexcept
{
    return buffer_.size();
}

std::size_t RequestParser::header_bytes() const noexcept
{
    return header_bytes_;
}

std::size_t RequestParser::message_bytes() const noexcept
{
    return message_bytes_;
}

std::size_t RequestParser::content_length() const noexcept
{
    return content_length_;
}

std::string_view RequestParser::method() const noexcept
{
    return {method_.data(), method_.size()};
}

std::string_view RequestParser::target() const noexcept
{
    return {target_.data(), target_.size()};
}

std::string_view RequestParser::version() const noexcept
{
    return {version_.data(), version_.size()};
}

std::string_view RequestParser::body() const noexcept
{
    if (status_ != ParseStatus::kMessageComplete || content_length_ == 0) {
        return {};
    }
    return {buffer_.data() + header_bytes_, content_length_};
}

const std::vector<HeaderField>& RequestParser::headers() const noexcept
{
    return headers_;
}

std::optional<std::string_view> RequestParser::header_value(
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
// AI-CODE-END: S3-REQUEST-PARSER-IMPLEMENTATION
