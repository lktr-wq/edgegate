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
    //状态行错误
    kInvalidStatusLine,
    kUnsupportedHttpVersion,
    kInvalidStatusCode,
    //Header与消息边界错误
    kMalformedHeaderLine,
    kInvalidHeaderName,
    kInvalidHeaderValue,
    kInvalidContentLength,
    kConflictingContentLength,
    kConflictingMessageFraming,
    kUnsupportedTransferEncoding,
    //chunked错误
    kInvalidChunkSize,
    kInvalidChunkTerminator,
    kInvalidTrailer,
    //连接提前关闭
    kUnexpectedEof
};
//响应行(HTTP版本(version) 状态码(status_code) 原因短语(reason_phrase)) \r\n
//响应头(header) \r\n\r\n
//响应体(body)
enum class ResponseBodyMode {// 本项目支持的请求通过 Header 结束位置和 Content-Length 确定边界；HTTP 响应则需要根据状态码、原请求方法和 Header，从以下4种正文边界中选择一种。
    kNoBody,//本项目识别：原请求方法为 HEAD。状态码为 1xx。状态码为 204。状态码为 304
    //Header 结束时，响应也立即结束。
    kContentLength,//收够指定字节数body结束
    kChunked,//，响应体为chunked，需读到大小为 0 的最后一个块及 Trailer 结尾
    kCloseDelimited//上游关闭 TCP 连接
};

struct ResponseParseResult {
    ParseStatus status;//当前解析状态是什么？
    ResponseParseError error;//如果失败，具体是什么错误？
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
        std::string request_method = "GET",//原请求方法，默认GET,响应需要记住原请求方法因为解析HEAD响应时仅从响应状态行看不出他是HEAD请求
        std::size_t max_header_size = 8192,// Header最多8192字节
        std::size_t max_body_size = 8 * 1024 * 1024);// Body最多8MiB

    ResponseParseResult consume(std::string_view chunk);//输入本次从上游收到的一批字节
    ResponseParseResult notify_eof();//通知解析器上游连接已经关闭
    // 当解析器仍处于 kNeedMoreData 时，只有 kCloseDelimited 能把 EOF 当作正常结束；
    // Header 未完成或其他正文模式尚未完成时收到 EOF，应报告 kUnexpectedEof。
    // 如果响应此前已经完成，notify_eof() 只返回原来的完成状态。

    //查询接口
    int status_code() const noexcept;//200、404、503等
    std::string_view version() const noexcept;//HTTP/1.0或HTTP/1.1
    std::string_view reason_phrase() const noexcept;//OK、Not Found等
    ResponseBodyMode body_mode() const noexcept;//四种正文模式之一

    std::size_t header_bytes() const noexcept;//Header占多少字节
    std::size_t message_bytes() const noexcept;//当前响应本身占多少字节
    std::size_t buffered_bytes() const noexcept;//实际累计了多少字节
    std::size_t remaining_bytes() const noexcept;//当前响应完成后还多出多少字节
    //这是请求解析器和响应解析器的重要区别。请求解析器当前明确拒绝流水线，所以多余字节报错。但上游连接上可能连续返回信息响应和最终响应，或者缓冲区中已经带入下一条响应的字节，因此响应解析器需要告诉调用者：当前响应用了前N字节后面还剩M字节，由调用者处理
    std::string_view body() const noexcept;//解码后的正文
    std::string_view raw_message() const noexcept;//原始响应字节

    const std::vector<HeaderField>& headers() const noexcept;//普通Header
    const std::vector<HeaderField>& trailers() const noexcept;//chunked最后的Trailer

    std::optional<std::string_view> header_value(
        std::string_view name) const noexcept;

private:
    enum class ChunkState {
        kSizeLine,//正在等待十六进制块长度 + \r\n
        kData,//已经知道当前块有多长;正在等待指定长度的数据 + \r\n
        kTrailers// 已读到0块；逐行读取零个或多个Trailer，直到最终空行\r\n
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
    ResponseBodyMode body_mode_{ResponseBodyMode::kCloseDelimited};//构造对象时的默认占位值，不表示解析器一开始就已经确定响应属于关闭定界

    std::vector<HeaderField> headers_;
    std::vector<HeaderField> trailers_;
    std::string decoded_chunked_body_;//保存已经解码出来的真正正文

    ChunkState chunk_state_{ChunkState::kSizeLine};//保存当前chunked状态机，初始状态是等待第一行块长度
    std::size_t chunk_position_{0};//下一次应该从buffer_的哪个位置继续解释chunked数据;当 select_body_mode() 选择chunked时，它会被设为chunk_position_ = header_bytes_;从Header结束后的第一个字节开始
    std::size_t trailers_start_{0};//记录Trailer区域从哪里开始，用来限制整个Trailer区域的大小
    std::size_t current_chunk_size_{0};//保存当前块声明的数据长度

    ParseStatus status_{ParseStatus::kNeedMoreData};
    ResponseParseError error_{ResponseParseError::kNone};
};

} // namespace edgegate::http
// AI-CODE-END: S3-RESPONSE-PARSER-API
