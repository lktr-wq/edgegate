#include "edgegate/http/chunked_stream_decoder.h"

// AI-CODE-BEGIN: S7-CHUNKED-STREAM-DECODER-IMPLEMENTATION
#include <algorithm>
#include <cctype>
#include <limits>

namespace edgegate::http {

namespace {

bool token_character(char character) noexcept
{
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 ||
           std::string_view("!#$%&'*+-.^_`|~").find(character) !=
               std::string_view::npos;
}

int hex_value(char character) noexcept
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

std::string lowercase(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    return result;
}

} // namespace

ChunkedStreamDecoder::ChunkedStreamDecoder(
    std::size_t max_decoded_body_size,
    std::size_t max_line_size)
    : max_decoded_body_size_(max_decoded_body_size),
      max_line_size_(max_line_size)
{
}

ChunkedStreamStatus ChunkedStreamDecoder::consume(std::string_view bytes) noexcept
{
    if (state_ == State::kComplete) {
        return bytes.empty() ? ChunkedStreamStatus::kComplete
                             : ChunkedStreamStatus::kError;
    }
    if (state_ == State::kError) {
        return ChunkedStreamStatus::kError;
    }

    std::size_t position = 0;
    while (position < bytes.size() && state_ != State::kComplete &&
           state_ != State::kError) {
        if (state_ == State::kData) {
            const std::size_t count =
                std::min(chunk_remaining_, bytes.size() - position);
            position += count;
            chunk_remaining_ -= count;
            if (chunk_remaining_ == 0) {
                state_ = State::kDataCr;
            }
            continue;
        }

        const char character = bytes[position++];
        if (state_ == State::kDataCr) {
            if (character != '\r') {
                fail();
            } else {
                state_ = State::kDataLf;
            }
        } else if (state_ == State::kDataLf) {
            if (character != '\n') {
                fail();
            } else {
                state_ = State::kSizeLine;
            }
        } else if (!append_line_character(character)) {
            fail();
        }
    }

    if (state_ == State::kComplete && position != bytes.size()) {
        fail();
    }
    return status();
}

ChunkedStreamStatus ChunkedStreamDecoder::status() const noexcept
{
    if (state_ == State::kComplete) {
        return ChunkedStreamStatus::kComplete;
    }
    if (state_ == State::kError) {
        return ChunkedStreamStatus::kError;
    }
    return ChunkedStreamStatus::kNeedMoreData;
}

std::size_t ChunkedStreamDecoder::decoded_body_bytes() const noexcept
{
    return decoded_body_bytes_;
}

bool ChunkedStreamDecoder::append_line_character(char character) noexcept
{
    if (line_saw_cr_) {
        line_saw_cr_ = false;
        if (character != '\n') {
            return false;
        }
        const bool valid = state_ == State::kSizeLine
            ? finish_size_line()
            : finish_trailer_line();
        line_.clear();
        return valid;
    }
    if (character == '\r') {
        line_saw_cr_ = true;
        return true;
    }
    if (character == '\n' || line_.size() >= max_line_size_) {
        return false;
    }
    line_.push_back(character);
    return true;
}

bool ChunkedStreamDecoder::finish_size_line() noexcept
{
    const std::size_t semicolon = line_.find(';');
    const std::size_t number_size =
        semicolon == std::string::npos ? line_.size() : semicolon;
    std::string_view number(line_.data(), number_size);
    if (number.empty()) {
        return false;
    }
    std::size_t parsed = 0;
    for (char character : number) {
        const int digit = hex_value(character);
        if (digit < 0 || parsed >
            (std::numeric_limits<std::size_t>::max() -
             static_cast<std::size_t>(digit)) / 16U) {
            return false;
        }
        parsed = parsed * 16U + static_cast<std::size_t>(digit);
    }
    if (parsed > max_decoded_body_size_ - decoded_body_bytes_) {
        return false;
    }
    if (parsed == 0) {
        state_ = State::kTrailerLine;
        return true;
    }
    decoded_body_bytes_ += parsed;
    chunk_remaining_ = parsed;
    state_ = State::kData;
    return true;
}

bool ChunkedStreamDecoder::finish_trailer_line() noexcept
{
    if (line_.empty()) {
        state_ = State::kComplete;
        return true;
    }
    const std::size_t colon = line_.find(':');
    if (colon == 0 || colon == std::string::npos ||
        !std::all_of(line_.begin(), line_.begin() +
                     static_cast<std::ptrdiff_t>(colon), token_character)) {
        return false;
    }
    const std::string name = lowercase(std::string_view(line_).substr(0, colon));
    return name != "content-length" && name != "transfer-encoding" &&
           name != "host";
}

void ChunkedStreamDecoder::fail() noexcept
{
    state_ = State::kError;
}

} // namespace edgegate::http
// AI-CODE-END: S7-CHUNKED-STREAM-DECODER-IMPLEMENTATION
