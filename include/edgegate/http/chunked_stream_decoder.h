#pragma once

// AI-CODE-BEGIN: S7-CHUNKED-STREAM-DECODER-API
#include <cstddef>
#include <string>
#include <string_view>

namespace edgegate::http {

enum class ChunkedStreamStatus {
    kNeedMoreData,
    kComplete,
    kError
};

/*
 * 增量检查 chunked 正文的边界，但不保存完整正文，也不改变原始字节。
 * ProxySession 可以把原始 chunk framing 直接转发，同时用本对象防止畸形
 * chunk、超大解码正文或缺少终止 0 块悄悄穿过代理。
 */
class ChunkedStreamDecoder {
public:
    explicit ChunkedStreamDecoder(
        std::size_t max_decoded_body_size,
        std::size_t max_line_size = 8192);

    ChunkedStreamStatus consume(std::string_view bytes) noexcept;
    [[nodiscard]] ChunkedStreamStatus status() const noexcept;
    [[nodiscard]] std::size_t decoded_body_bytes() const noexcept;

private:
    enum class State {
        kSizeLine,
        kData,
        kDataCr,
        kDataLf,
        kTrailerLine,
        kComplete,
        kError
    };

    bool finish_size_line() noexcept;
    bool finish_trailer_line() noexcept;
    bool append_line_character(char character) noexcept;
    void fail() noexcept;

    std::size_t max_decoded_body_size_;
    std::size_t max_line_size_;
    std::size_t decoded_body_bytes_{0};
    std::size_t chunk_remaining_{0};
    std::string line_;
    bool line_saw_cr_{false};
    State state_{State::kSizeLine};
};

} // namespace edgegate::http
// AI-CODE-END: S7-CHUNKED-STREAM-DECODER-API
