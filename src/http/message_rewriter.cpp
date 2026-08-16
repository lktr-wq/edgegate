#include "edgegate/http/message_rewriter.h"

// AI-CODE-BEGIN: S6-HEADER-REWRITER-IMPLEMENTATION
#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace edgegate::http {

namespace {

std::string lowercase(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character + ('a' - 'A'));
        }
        return character;
    });
    return result;
}

void add_connection_tokens(
    std::unordered_set<std::string>& removed,
    std::string_view value)
{
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        std::string_view token = value.substr(0, comma);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (!token.empty()) {
            removed.insert(lowercase(token));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
}

std::unordered_set<std::string> hop_by_hop_headers(
    const std::vector<HeaderField>& headers)
{
    std::unordered_set<std::string> removed{
        "connection",
        "proxy-connection",
        "keep-alive",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade"};
    for (const HeaderField& header : headers) {
        if (lowercase(header.name) == "connection") {
            add_connection_tokens(removed, header.value);
        }
    }
    return removed;
}

void append_header(std::string& message, std::string_view name, std::string_view value)
{
    message.append(name.data(), name.size());
    message += ": ";
    message.append(value.data(), value.size());
    message += "\r\n";
}

} // namespace

std::string rewrite_request_for_upstream(
    const RequestParser& request,
    std::string_view client_address)
{
    const auto removed = hop_by_hop_headers(request.headers());
    std::string message;
    message.reserve(request.raw_message().size() + 256);
    message.append(request.method());
    message += ' ';
    message.append(request.target());
    message += ' ';
    message.append(request.version());
    message += "\r\n";

    for (const HeaderField& header : request.headers()) {
        const std::string name = lowercase(header.name);
        if (removed.count(name) != 0U ||
            name == "host" ||
            name == "content-length" ||
            name == "x-forwarded-for" ||
            name == "x-forwarded-host" ||
            name == "x-forwarded-proto") {
            continue;
        }
        append_header(message, header.name, header.value);
    }

    // Host 和 Content-Length 由已通过校验的解析结果各生成一次，避免客户端
    // 通过 Connection: Host 或重复长度字段让上游看到歧义。
    append_header(message, "Host", *request.header_value("Host"));
    if (request.header_value("Content-Length").has_value()) {
        append_header(
            message,
            "Content-Length",
            *request.header_value("Content-Length"));
    }
    append_header(message, "X-Forwarded-For", client_address);
    append_header(message, "X-Forwarded-Host", *request.header_value("Host"));
    append_header(message, "X-Forwarded-Proto", "http");
    append_header(message, "Connection", "close");
    message += "\r\n";
    if (!request.body().empty()) {
        message.append(request.body().data(), request.body().size());
    }
    return message;
}

std::string rewrite_response_for_client(
    const ResponseParser& response,
    bool close_client_connection)
{
    auto removed = hop_by_hop_headers(response.headers());
    removed.insert("content-length");

    std::string message;
    message.reserve(response.raw_message().size() + 128);
    message.append(response.version());
    message += ' ';
    message += std::to_string(response.status_code());
    message += ' ';
    if (!response.reason_phrase().empty()) {
        message.append(response.reason_phrase());
    }
    message += "\r\n";

    for (const HeaderField& header : response.headers()) {
        if (removed.count(lowercase(header.name)) == 0U) {
            append_header(message, header.name, header.value);
        }
    }

    if (response.body_mode() != ResponseBodyMode::kNoBody) {
        append_header(message, "Content-Length", std::to_string(response.body().size()));
    } else if (response.header_value("Content-Length").has_value()) {
        append_header(message, "Content-Length", *response.header_value("Content-Length"));
    }
    append_header(
        message,
        "Connection",
        close_client_connection ? "close" : "keep-alive");
    message += "\r\n";
    if (response.body_mode() != ResponseBodyMode::kNoBody &&
        !response.body().empty()) {
        message.append(response.body().data(), response.body().size());
    }
    return message;
}

} // namespace edgegate::http
// AI-CODE-END: S6-HEADER-REWRITER-IMPLEMENTATION
