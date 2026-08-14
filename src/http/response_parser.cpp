#include "edgegate/http/response_parser.h"

// AI-CODE-BEGIN: S3-RESPONSE-PARSER-IMPLEMENTATION
#include <limits>
#include <stdexcept>
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

std::optional<std::size_t> parse_hex_size(std::string_view value) noexcept// 把十六进制文本表示的块长度解析成size_t数值。
{
    if (value.empty()) {//空字符串非法
        return std::nullopt;
    }

    std::size_t result = 0;
    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();

    for (char character : value) {//一个字符一个字符转换
        unsigned int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned int>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned int>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<unsigned int>(character - 'A' + 10);
        } else {
            return std::nullopt;//非十六进制字符，返回std::nullopt
        }

        if (result > (maximum - digit) / 16) {//提前防止溢出,确认结果不会超过std::size_t最大值
            return std::nullopt;
        }
        result = result * 16 + digit;//累积多位十六进制
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
      max_body_size_(max_body_size),
      max_message_size_(0)
{
    if (max_body_size_ >
        std::numeric_limits<std::size_t>::max() - max_header_size_) {
        throw std::invalid_argument("response parser size limit overflow");
    }
    max_message_size_ = max_header_size_ + max_body_size_;
}

ResponseParseError ResponseParser::parse_status_line(
    std::string_view status_line)//输入是已经去掉末尾\r\n的状态行
{
    /*
    输出：
    kNone                    解析成功(成功时还保存：version_,status_code_,reason_phrase_)
    kInvalidStatusLine       整体格式错误
    kUnsupportedHttpVersion  HTTP版本不支持
    kInvalidStatusCode       状态码错误
    */
    const std::size_t first_space = status_line.find(' ');//寻找第一个空格,把HTTP版本与状态码分开
    if (first_space == std::string_view::npos || first_space == 0) {
        return ResponseParseError::kInvalidStatusLine;// 找不到第一个空格，说明版本和状态码之间没有分隔；first_space == 0 表示状态行以空格开头，HTTP版本部分为空。
    }

    const std::string_view version = status_line.substr(0, first_space);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {//请求解析器只接受HTTP/1.1，但响应解析器允许HTTP/1.0
        return ResponseParseError::kUnsupportedHttpVersion;
    }

    if (status_line.size() < first_space + 4) {//保证至少存在三位状态码,4指代1个空格和三位状态码
        return ResponseParseError::kInvalidStatusCode;
    }

    const std::string_view code = status_line.substr(first_space + 1, 3);//状态码必须是三位数字,逐个判断是否在'0'到'9'之间
    if (code[0] < '0' || code[0] > '9' ||
        code[1] < '0' || code[1] > '9' ||
        code[2] < '0' || code[2] > '9') {
        return ResponseParseError::kInvalidStatusCode;
    }

    const int parsed_code =//把三个字符变成整数后保证状态码在100~599范围内
        (code[0] - '0') * 100 +
        (code[1] - '0') * 10 +
        (code[2] - '0');
    if (parsed_code < 100 || parsed_code > 599) {
        return ResponseParseError::kInvalidStatusCode;
    }
    //解析原因短语
    std::string_view reason;//先建立空的std::string_view reason,这意味着原因短语可以不存在
    if (status_line.size() > first_space + 4) {
        if (status_line[first_space + 4] != ' ') {
            return ResponseParseError::kInvalidStatusLine;
        }
        reason = status_line.substr(first_space + 5);
        for (char character : reason) {//随后检查原因短语中不能有不允许的控制字符
            if (!is_field_value_character(character)) {
                return ResponseParseError::kInvalidStatusLine;
            }
        }
    }
    //全部合法才保存
    version_.assign(version.data(), version.size());
    status_code_ = parsed_code;
    reason_phrase_.assign(reason.data(), reason.size());
    return ResponseParseError::kNone;
}

ResponseParseError ResponseParser::parse_header_line(
    std::string_view line,//当前要解析的一行
    std::vector<HeaderField>& destination,//解析成功后保存到哪个容器;普通Header →headers_  Trailer→ trailers_
    bool trailer)//说明当前行是不是Trailer
    //Trailer是chunked响应最后可以在0块后附带一些Header形式的补充字段;长得像Header但位置在chunked正文结束块之后
{
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {//拒绝空行和折叠行
        return trailer//条件 ? 条件成立时的结果 : 条件不成立时的结果
            ? ResponseParseError::kInvalidTrailer//如果是Trailer，返回kInvalidTrailer
            : ResponseParseError::kMalformedHeaderLine;//普通Header，返回kMalformedHeaderLine
    }

    const std::size_t colon = line.find(':');//寻找冒号
    if (colon == std::string_view::npos) {//找不到冒号时,根据他是trailer还是普通header返回不同错误
        return trailer
            ? ResponseParseError::kInvalidTrailer
            : ResponseParseError::kMalformedHeaderLine;
    }
    //执行到这找到冒号
    const std::string_view name = line.substr(0, colon);//name  = 冒号左边
    const std::string_view value = trim_optional_whitespace(//value = 冒号右边，并去掉两端空格或制表符
        line.substr(colon + 1));

    //检查字段名,名称不能为空，也只能包含HTTP token允许的字符
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
    //检查字段值
    for (char character : value) {
        if (!is_field_value_character(character)) {
            return trailer
                ? ResponseParseError::kInvalidTrailer
                : ResponseParseError::kInvalidHeaderValue;
        }
    }
    // Trailer 不能在正文结束后重新影响消息边界或路由语义：
    // Content-Length、Transfer-Encoding 影响消息定界，Host 影响目标主机或路由。
    if (trailer &&
        (ascii_equals_ignore_case(name, "Content-Length") ||
         ascii_equals_ignore_case(name, "Transfer-Encoding") ||
         ascii_equals_ignore_case(name, "Host"))) {
        return ResponseParseError::kInvalidTrailer;
    }

    destination.push_back(HeaderField{std::string(name), std::string(value)});//保存结果
    return ResponseParseError::kNone;
}

ResponseParseError ResponseParser::parse_header_fields(//输入header_end表示\r\n\r\n中第一个\r的位置。
    std::size_t header_end)
{
    // 没有任何 Header 时，header_end 指向状态行末尾的\r\n，
    // header_fields_start_ 位于这两个字节之后，所以两者相差 2。
    if (header_end + 2 == header_fields_start_) {//没有任何Header
        return ResponseParseError::kNone;
    }
    if (header_end < header_fields_start_) {
        return ResponseParseError::kMalformedHeaderLine;
    }
    /*
    循环执行：从line_start开始寻找\r\n
    → 截出一行
    → parse_header_line(line, headers_, false)
    → 移动到下一行
    */
    std::size_t line_start = header_fields_start_;
    while (line_start < header_end) {
        const std::size_t line_end = buffer_.find("\r\n", line_start);//开始找
        if (line_end == std::string::npos || line_end > header_end) {//没找到
            return ResponseParseError::kMalformedHeaderLine;
        }

        const std::string_view line(//找到后先截出来
            buffer_.data() + line_start,
            line_end - line_start);
        const auto line_error = parse_header_line(line, headers_, false);//单独处理这一行
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

/*
它的输入是此前解析后保存在对象中的：
request_method_
status_code_
headers_
header_bytes_

输出是：
body_mode_
message_bytes_
或具体错误
*/
ResponseParseError ResponseParser::select_body_mode()
{
    std::optional<std::size_t> parsed_content_length;//parsed_content_length记录当前是否看到合法Content-Length，如果看到记录他的的数值
    bool has_transfer_encoding = false;//记录是否看到 Transfer-Encoding: chunked

    for (const HeaderField& header : headers_) {//遍历全部 Header
        if (ascii_equals_ignore_case(header.name, "Content-Length")) {//处理 Content-Length
            const auto length = parse_decimal_size(header.value);//把字符串数字转成整数得到当前值
            if (!length.has_value()) {
                return ResponseParseError::kInvalidContentLength;//无法转换返回kInvalidContentLength(如85kad)
            }
            if (parsed_content_length.has_value() &&//以前是否出现过
                *parsed_content_length != *length) {//旧值和当前值是否不同
                return ResponseParseError::kConflictingContentLength;//出现多个不同ContentLength值,返回kConflictingContentLength
            }
            parsed_content_length = *length;//比较通过后才更新
            //重点是比较发生在赋值之前
        }

        if (ascii_equals_ignore_case(header.name, "Transfer-Encoding")) {//处理 Transfer-Encoding
            if (!ascii_equals_ignore_case(header.value, "chunked")) {//当前只支持chunked
                return ResponseParseError::kUnsupportedTransferEncoding;//值不是 chunked，返回kUnsupportedTransferEncoding
            }
            has_transfer_encoding = true;
        }
    }

    if (has_transfer_encoding && parsed_content_length.has_value()) {//如果一条响应同时包含长度定界和chunked定界为冲突,因为两种边界同时出现，解析器不猜测相信哪一个
        return ResponseParseError::kConflictingMessageFraming;//返回kConflictingMessageFraming
    }
    //四种模式的选择优先级
    const bool no_body =// Header 的长度和定界信息校验通过后，四种正文模式中优先判断是否为无正文响应。
        ascii_equals_ignore_case(request_method_, "HEAD") ||
        (status_code_ >= 100 && status_code_ < 200) ||
        status_code_ == 204 || status_code_ == 304;

    if (no_body) {
        body_mode_ = ResponseBodyMode::kNoBody;
        message_bytes_ = header_bytes_;//无正文是Header字节数等于消息总字节数
        return ResponseParseError::kNone;
    }

    if (has_transfer_encoding) {//chunked优先级次之
        body_mode_ = ResponseBodyMode::kChunked;
        chunk_position_ = header_bytes_;//chunk_position_ 指向Header结束后，第一个chunk长度行的起点
        return ResponseParseError::kNone;
    }

    if (parsed_content_length.has_value()) {//固定长度再次之
        content_length_ = *parsed_content_length;
        if (content_length_ > max_body_size_) {
            return ResponseParseError::kBodyTooLarge;
        }
        if (content_length_ >
            std::numeric_limits<std::size_t>::max() - header_bytes_) {
            return ResponseParseError::kInvalidContentLength;
        }
        body_mode_ = ResponseBodyMode::kContentLength;
        message_bytes_ = header_bytes_ + content_length_;//消息总字节数等于响应头字节数加响应体长度
        return ResponseParseError::kNone;
    }

    body_mode_ = ResponseBodyMode::kCloseDelimited;//前三种都不满足时只能等待上游关闭连接
    return ResponseParseError::kNone;
}

ResponseParseResult ResponseParser::parse_chunked_body()
{   /*该函数形如
    for (;;) {
        if (状态是kSizeLine) {
            ...
        }

        if (状态是kData) {
            ...
        }

        if (状态是kTrailers) {
            ...
        }
    }
    它不是一定会死循环，因为内部会在以下情况返回：
    数据不够      → NeedMoreData
    解析错误      → Error
    消息完成      → MessageComplete

    如果当前缓冲区已经包含多个完整块，它会在同一次调用中连续处理，不必每解析一块就返回
    */
    for (;;) {
        if (chunk_state_ == ChunkState::kSizeLine) {
            const std::size_t line_end = buffer_.find("\r\n", chunk_position_);//寻找长度行结尾找到\r\n后，line_end指向\r
            if (line_end == std::string::npos) {//如果找不到\r\n，说明可能只收到了5或者5\r，通常返回kNeedMoreData
                // 但块长度行也不能无限增长，否则慢速恶意上游可持续占用内存。
                if (buffer_.size() - chunk_position_ >= max_header_size_) {
                    status_ = ParseStatus::kError;
                    error_ = ResponseParseError::kInvalidChunkSize;//防止恶意上游无限发送长度字符
                }
                return {status_, error_};
            }

            const std::string_view line(//找到后截取长度行，可能只有长度或者还带扩展(当前项目不使用扩展中的业务信息，只检查它不包含非法控制字符)
                buffer_.data() + chunk_position_,
                line_end - chunk_position_);
            const std::size_t semicolon = line.find(';');//找分号分号前面是长度，分号后面是拓展，没有拓展不会报错
            const std::string_view size_text = line.substr(0, semicolon);

            if (semicolon != std::string_view::npos) {
                const std::string_view extension = line.substr(semicolon + 1);
                for (char character : extension) {
                    if (!is_field_value_character(character)) {//检查扩展字符
                        status_ = ParseStatus::kError;
                        error_ = ResponseParseError::kInvalidChunkSize;
                        return {status_, error_};
                    }
                }
            }

            const auto size = parse_hex_size(size_text);//解析十六进制长度
            if (!size.has_value()) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kInvalidChunkSize;
                return {status_, error_};
            }

            current_chunk_size_ = *size;//保存长度
            chunk_position_ = line_end + 2;//移动位置,+2跳过长度行末尾的\r\n

            if (current_chunk_size_ == 0) {//遇到长度为0的块后进入Trailer状态
                chunk_state_ = ChunkState::kTrailers;
                trailers_start_ = chunk_position_;
            } else {
                if (decoded_chunked_body_.size() > max_body_size_ ||
                    current_chunk_size_ >
                    max_body_size_ - decoded_chunked_body_.size()) {//先检查已经解码的正文长度 + 当前块长度是否超过max_body_size_
                    status_ = ParseStatus::kError;
                    error_ = ResponseParseError::kBodyTooLarge;
                    return {status_, error_};
                }
                chunk_state_ = ChunkState::kData;//合法非0块进入Data状态
            }
        }

        if (chunk_state_ == ChunkState::kData) {
            constexpr std::size_t maximum =
                std::numeric_limits<std::size_t>::max();
            if (chunk_position_ > maximum - 2 ||
                current_chunk_size_ > maximum - chunk_position_ - 2) {//如果正文起点 + 当前块长度 + 结尾两个CRLF字节超过std::size_t上限，返回kInvalidChunkSize
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kInvalidChunkSize;
                return {status_, error_};
            }

            const std::size_t data_end = chunk_position_ + current_chunk_size_;// data_end 指向正文后一位，也就是当前块结尾 CRLF 应该开始的位置。
            if (buffer_.size() < data_end + 2) {//必须连块结尾\r\n一起收到，才能确认这个块格式完整
                return {status_, error_};
            }

            if (buffer_.compare(data_end, 2, "\r\n") != 0) {//正文后面必须是\r\n
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kInvalidChunkTerminator;
                return {status_, error_};
            }

            decoded_chunked_body_.append(//保存解码正文,只复制真正正文
                buffer_.data() + chunk_position_,
                current_chunk_size_);
            chunk_position_ = data_end + 2;//准备下一块重新回到kSizeLine
            chunk_state_ = ChunkState::kSizeLine;
            continue;
        }

        if (chunk_state_ == ChunkState::kTrailers) {//已经读到0\r\n,之后可能有Trailer也可能没有
            const std::size_t line_end = buffer_.find("\r\n", chunk_position_);//寻找当前可能存在的Trailer行结尾
            if (line_end == std::string::npos) {
                if (buffer_.size() - trailers_start_ >= max_header_size_) {
                    status_ = ParseStatus::kError;
                    error_ = ResponseParseError::kHeaderTooLarge;//超过Header上限返回kHeaderTooLarge
                }
                return {status_, error_};//还没找到，返回kNeedMoreData
            }

            if (line_end + 2 - trailers_start_ > max_header_size_) {//检查从trailers_start_到当前行结束的总长度是否超过限制
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kHeaderTooLarge;
                return {status_, error_};
            }

            if (line_end == chunk_position_) {//表示当前位置立刻就是\r\n，当前行为空
                message_bytes_ = line_end + 2;//message_bytes_只包括当前响应
                status_ = ParseStatus::kMessageComplete;
                return {status_, error_};
            }

            const std::string_view trailer_line(//不是空行，截取当前Trailer
                buffer_.data() + chunk_position_,
                line_end - chunk_position_);
            error_ = parse_header_line(trailer_line, trailers_, true);//保存到trailers_,按Trailer规则检查和分类错误
            if (error_ != ResponseParseError::kNone) {
                status_ = ParseStatus::kError;
                return {status_, error_};
            }
            chunk_position_ = line_end + 2;//移动到下一行
        }
    }
}

ResponseParseResult ResponseParser::parse_body()
{
    switch (body_mode_) {//模式已经由 select_body_mode() 决定。它只回答：按照当前模式，现在收到的数据足够让响应完成了吗？
    case ResponseBodyMode::kNoBody:
        status_ = ParseStatus::kMessageComplete;//能执行到这个函数说明header是完整的，无正文时header完整响应就完整
        return {status_, error_};

    case ResponseBodyMode::kContentLength:
        if (buffer_.size() >= message_bytes_) {//使用>=是因为允许用remaining_bytes将响应该消息后的剩余字节留给下一个响应
            status_ = ParseStatus::kMessageComplete;// 缓冲区字节数大于或等于当前消息总长度时，当前响应完整；多出的字节由 remaining_bytes() 留给调用者。
        }
        return {status_, error_};

    case ResponseBodyMode::kChunked:
        return parse_chunked_body();//把工作交给 chunked 状态机

    case ResponseBodyMode::kCloseDelimited:
        if (buffer_.size() - header_bytes_ > max_body_size_) {//只检查正文是否超限,没有超限时仍然保持kNeedMoreData
            status_ = ParseStatus::kError;
            error_ = ResponseParseError::kBodyTooLarge;
        }//它必须等待 notify_eof()
        return {status_, error_};
    }

    status_ = ParseStatus::kError;
    error_ = ResponseParseError::kInvalidStatusLine;
    return {status_, error_};
}

ResponseParseResult ResponseParser::consume(std::string_view chunk)
{
    if (status_ != ParseStatus::kNeedMoreData) {//终态保持不变
        return {status_, error_};
    }

    if (!chunk.empty()) {//保存本次输入,先检查原有 buffer 大小 + 新 chunk 大小;看是否超过max_message_size_没有超过才追加
        if (buffer_.size() > max_message_size_ ||
            chunk.size() > max_message_size_ - buffer_.size()) {
            status_ = ParseStatus::kError;
            error_ = ResponseParseError::kBodyTooLarge;
            return {status_, error_};
        }
        buffer_.append(chunk.data(), chunk.size());
    }

    if (!status_line_parsed_) {//查找第一个\r\n
        const std::size_t status_line_end = buffer_.find("\r\n");
        if (status_line_end == std::string::npos) {
            if (buffer_.size() >= max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kHeaderTooLarge;//如果已经达到 Header 容量上限，转为kHeaderTooLarge
            }
            return {status_, error_};//找不到说明状态行还没收完整：返回 kNeedMoreData
        }

        error_ = parse_status_line(//找到后调用，使用状态行解析函数解析
            std::string_view(buffer_.data(), status_line_end));
        if (error_ != ResponseParseError::kNone) {
            status_ = ParseStatus::kError;
            return {status_, error_};
        }

        header_fields_start_ = status_line_end + 2;
        status_line_parsed_ = true;//成功后下次 consume() 不会重复解析状态行。
    }

    if (!headers_parsed_) {
        const std::size_t header_end = buffer_.find("\r\n\r\n");//查找\r\n\r\n
        if (header_end == std::string::npos) {
            if (buffer_.size() >= max_header_size_) {
                status_ = ParseStatus::kError;
                error_ = ResponseParseError::kHeaderTooLarge;
            }
            return {status_, error_};
        }
        //下面是找到了才会执行
        header_bytes_ = header_end + 4;//计算 Header 总字节数
        if (header_bytes_ > max_header_size_) {
            status_ = ParseStatus::kError;
            error_ = ResponseParseError::kHeaderTooLarge;
            return {status_, error_};
        }

        error_ = parse_header_fields(header_end);//逐行解析 Header，判断每一行 Header 的格式是否合法？
        if (error_ == ResponseParseError::kNone) {
            error_ = select_body_mode();//根据 Header 和状态码选择正文模式，只有能正确解析才进入模式选择
        }
        if (error_ != ResponseParseError::kNone) {
            status_ = ParseStatus::kError;
            return {status_, error_};
        }
        headers_parsed_ = true;//记录 Header 已解析
    }

    return parse_body();//完成状态行和 Header 后，consume() 自己不继续写四套判断，而是交给 parse_body()
}

ResponseParseResult ResponseParser::notify_eof()
{
    if (status_ != ParseStatus::kNeedMoreData) {//终态不变
        return {status_, error_};
    }

    if (!headers_parsed_) {//Header还没完成就关闭
        status_ = ParseStatus::kError;
        error_ = ResponseParseError::kUnexpectedEof;//返回kUnexpectedEof
        return {status_, error_};
    }

    if (body_mode_ != ResponseBodyMode::kCloseDelimited) {// 当前响应仍未完成，却在非关闭定界模式下收到 EOF
        status_ = ParseStatus::kError;
        error_ = ResponseParseError::kUnexpectedEof;//返回kUnexpectedEof
        return {status_, error_};
    }

    if (buffer_.size() - header_bytes_ > max_body_size_) {//关闭定界正文超限
        status_ = ParseStatus::kError;
        error_ = ResponseParseError::kBodyTooLarge;//超限返回kBodyTooLarge
        return {status_, error_};
    }

    message_bytes_ = buffer_.size();//执行到这说明正常完成关闭定界响应
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
